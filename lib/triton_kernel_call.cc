/* // Copyright 2022 The jax_triton Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License. */

#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <hip/hip_runtime.h>
#include "pybind11/pybind11.h"
#include "pybind11/stl.h"

// TODO(cjfj): Use `Status` for error handling.
#define CHECK_ROCM(expr)                                                  \
  do {                                                                    \
    hipError_t result = (expr);                                           \
    if (result != hipSuccess) {                                           \
      const char* error_string = "unknown error";                         \
      hipDrvGetErrorString(result, &error_string);                           \
      std::cerr << "HIP call failed (" << #expr << "): " << error_string  \
                << std::endl;                                             \
      abort();                                                            \
    }                                                                     \
  } while (false)

namespace py = pybind11;

namespace jax_triton {
namespace {

constexpr uint32_t kNumThreadsPerWarp = 64;

struct hipModuleDeleter {
  void operator()(hipModule_t module) { hipModuleUnload(module); }
};

using OwnedhipModule =
    std::unique_ptr<std::remove_pointer_t<hipModule_t>, hipModuleDeleter>;

class TritonKernel {
 public:
  TritonKernel(std::string module_image, std::string kernel_name,
               uint32_t num_warps, uint32_t shared_mem_bytes)
      : module_image_(std::move(module_image)),
        kernel_name_(std::move(kernel_name)),
        block_dim_x_(num_warps * kNumThreadsPerWarp),
        shared_mem_bytes_(shared_mem_bytes) {}

  void Launch(hipStream_t stream, uint32_t grid[3], void** params) {
    hipCtx_t context;
    hipDevice_t device;
    //CHECK_ROCM(hipStreamGetCtx(stream, &context));  //BLARG
    int device_id = hipGetStreamDeviceId(stream);
    CHECK_ROCM(hipDeviceGet(&device, device_id));
    CHECK_ROCM(hipDevicePrimaryCtxRetain(&context, device));
    hipFunction_t kernel = GetFunctionForContext(context);
    CHECK_ROCM(hipModuleLaunchKernel(kernel, grid[0], grid[1], grid[2], block_dim_x_,
                              /*blockDimY=*/1, /*blockDimZ=*/1,
                              shared_mem_bytes_, stream, params,
                              /*extra=*/nullptr));
  }

 private:
  hipFunction_t GetFunctionForContext(hipCtx_t context) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = functions_.find(context);
    if (it != functions_.end()) {
      return it->second;
    }

    CHECK_ROCM(hipCtxPushCurrent(context));
    hipModule_t module;
    CHECK_ROCM(hipModuleLoadData(&module, module_image_.c_str()));
    modules_.push_back(OwnedhipModule(module, hipModuleDeleter()));
    CHECK_ROCM(hipCtxPopCurrent(nullptr));

    hipFunction_t function;
    CHECK_ROCM(hipModuleGetFunction(&function, module, kernel_name_.c_str()));
    auto [_, success] = functions_.insert({context, function});
    assert(success);

    // The maximum permitted static shared memory allocation in CUDA is 48kB,
    // but we can expose more to the kernel using dynamic shared memory.
    constexpr int kMaxStaticSharedMemBytes = 49152;
    if (shared_mem_bytes_ <= kMaxStaticSharedMemBytes) {
      return function;
    }

    // Set up dynamic shared memory.
    hipDevice_t device;
    CHECK_ROCM(hipCtxGetDevice(&device));

    int shared_optin;
    CHECK_ROCM(hipDeviceGetAttribute(
        &shared_optin, hipDeviceAttributeSharedMemPerBlockOptin,
        device));

    if (shared_optin > kMaxStaticSharedMemBytes) {
      //CHECK_ROCM(cuFuncSetCacheConfig(function, CU_FUNC_CACHE_PREFER_SHARED));
      int shared_total;
      CHECK_ROCM(hipDeviceGetAttribute(
          &shared_total,
          hipDeviceAttributeMaxSharedMemoryPerMultiprocessor, device));
      int shared_static;
      CHECK_ROCM(hipFuncGetAttribute(
          &shared_static, HIP_FUNC_ATTRIBUTE_SHARED_SIZE_BYTES, function));
      //CHECK_ROCM(cuFuncSetAttribute(
      //    function, HIP_FUNC_ATTRIBUTE_MAX_DYNAMIC_SHARED_SIZE_BYTES,
      //    shared_optin - shared_static));
    }
    return function;
  }

  std::string module_image_;
  std::string kernel_name_;
  uint32_t block_dim_x_;
  uint32_t shared_mem_bytes_;

  std::mutex mutex_;
  std::vector<OwnedhipModule> modules_;
  std::unordered_map<hipCtx_t, hipFunction_t> functions_;
};

struct TritonKernelCallBase {
  virtual ~TritonKernelCallBase() = default;
  virtual void Launch(hipStream_t stream, void** buffers) = 0;
};

class TritonKernelCall : public TritonKernelCallBase {
 public:
  TritonKernelCall(TritonKernel& kernel, uint32_t grid_0, uint32_t grid_1,
                   uint32_t grid_2,
                   std::vector<std::optional<uint64_t>> parameters,
		   std::unordered_map<size_t, size_t> zeroed_buffers)
      : kernel_(kernel),
        grid_{grid_0, grid_1, grid_2},
        parameters_(std::move(parameters)),
        zeroed_buffers_(std::move(zeroed_buffers)) {}

  void Launch(hipStream_t stream, void** buffers) override final {
    for (const auto& [i, size] : zeroed_buffers_) {
      CHECK_ROCM(hipMemsetD8Async(reinterpret_cast<hipDeviceptr_t>(buffers[i]), 0,
                                 size, stream));
    }

    std::vector<void*> params;
    params.reserve(parameters_.size());
    for (std::optional<uint64_t>& param : parameters_) {
      if (param.has_value()) {
        params.push_back(&*param);
      } else {
        params.push_back(buffers++);
      }
    }

    kernel_.Launch(stream, grid_, params.data());
  }

 private:
  TritonKernel& kernel_;
  uint32_t grid_[3];
  // Parameter values. `nullopt` values represent buffer arguments.
  std::vector<std::optional<uint64_t>> parameters_;
  // Buffers to be zeroed before kernel launch (index and size).
  std::unordered_map<size_t, size_t> zeroed_buffers_;
};

class TritonAutotunedKernelCall : public TritonKernelCallBase {
 public:
  struct Config {
    py::object kernel_call;
    std::string description;
  };

  TritonAutotunedKernelCall(std::string name, std::vector<Config> configs)
      : name_(name), configs_(configs) {}

  void Launch(hipStream_t stream, void** buffers) override {
    if (configs_.size() > 1) {
      std::cerr << "Autotuning function: " << name_ << std::endl;
      // First run a single iteration of each to config to determine how many
      // iterations to run for benchmarking.
      float best = std::numeric_limits<float>::infinity();
      for (Config& config : configs_) {
        auto& kernel_call = py::cast<TritonKernelCall&>(config.kernel_call);
        float t = Benchmark(stream, kernel_call, buffers, 1);
        std::cerr << config.description << ", ran 1 iter in " << t << " ms"
                  << std::endl;
        best = std::min(best, t);
      }

      int timed_iters =
          std::max(static_cast<int>(kBenchmarkTimeMillis / best), 1);
      std::cerr << "Benchmarking with " << timed_iters
                << " iters (target time: " << kBenchmarkTimeMillis << " ms)"
                << std::endl;

      best = std::numeric_limits<float>::infinity();
      for (Config& config : configs_) {
        auto& kernel_call = py::cast<TritonKernelCall&>(config.kernel_call);
        float t = Benchmark(stream, kernel_call, buffers, timed_iters);
        std::cerr << config.description << ", ran " << timed_iters
                  << " iters in " << t << " ms" << std::endl;

        if (t < best) {
          std::cerr << config.description << " is the new best config"
                    << std::endl;
          best = t;
          std::swap(config, configs_[0]);
        }
      }

      // Discard all but the best config.
      py::gil_scoped_acquire gil;
      configs_.erase(configs_.begin() + 1, configs_.end());
    }

    auto& kernel_call = py::cast<TritonKernelCall&>(configs_[0].kernel_call);
    kernel_call.Launch(stream, buffers);
  }

 private:
  static constexpr float kBenchmarkTimeMillis = 100.;

  float Benchmark(hipStream_t stream, TritonKernelCall& kernel_call,
                  void** buffers, int num_iterations) {
    hipEvent_t start, stop;
    CHECK_ROCM(hipEventCreateWithFlags(&start, /*Flags=*/hipEventDefault));
    CHECK_ROCM(hipEventCreateWithFlags(&stop, /*Flags=*/hipEventDefault));
    kernel_call.Launch(stream, buffers);  // Warm-up iteration.
    CHECK_ROCM(hipEventRecord(start, stream));
    for (int i = 0; i < num_iterations; ++i) {
      kernel_call.Launch(stream, buffers);
    }
    CHECK_ROCM(hipEventRecord(stop, stream));
    CHECK_ROCM(hipEventSynchronize(stop));
    float elapsed_ms;
    CHECK_ROCM(hipEventElapsedTime(&elapsed_ms, start, stop));
    CHECK_ROCM(hipEventDestroy(start));
    CHECK_ROCM(hipEventDestroy(stop));
    return elapsed_ms;
  }

  std::string name_;
  // After auto-tuning, all configurations, except the best, will be discarded.
  std::vector<Config> configs_;
};

template <typename CppT, typename PyT>
uint64_t EncodeKernelParameterAs(PyT value) {
  static_assert(sizeof(CppT) <= sizeof(uint64_t));
  union {
    CppT value;
    uint64_t bits;
  } encoded;
  encoded.bits = 0;
  encoded.value = CppT(value);
  return encoded.bits;
}

uint64_t EncodeKernelParameter(py::int_ value, std::string_view dtype) {
  if ((dtype == "i1") || (dtype == "i8")) {
    return EncodeKernelParameterAs<int8_t>(value);
  } else if (dtype == "u8") {
    return EncodeKernelParameterAs<uint8_t>(value);
  } else if (dtype == "i16") {
    return EncodeKernelParameterAs<int16_t>(value);
  } else if (dtype == "u16") {
    return EncodeKernelParameterAs<uint16_t>(value);
  } else if (dtype == "i32") {
    return EncodeKernelParameterAs<int32_t>(value);
  } else if (dtype == "u32") {
    return EncodeKernelParameterAs<uint32_t>(value);
  } else if (dtype == "i64") {
    return EncodeKernelParameterAs<int64_t>(value);
  } else if (dtype == "u64") {
    return EncodeKernelParameterAs<uint64_t>(value);
  } else {
    throw std::runtime_error(std::string("unknown dtype: ") + dtype.data());
  }
}

uint64_t EncodeKernelParameter(py::float_ value, std::string_view dtype) {
  if (dtype == "fp32") {
    return EncodeKernelParameterAs<float>(value);
  } else if (dtype == "fp64") {
    return EncodeKernelParameterAs<double>(value);
  } else {
    throw std::runtime_error(std::string("unknown dtype: ") + dtype.data());
  }
}

uint64_t EncodeKernelParameter(py::bool_ value, std::string_view dtype) {
  if ((dtype == "int1") || (dtype == "B")) {
    return EncodeKernelParameterAs<bool>(value);
  } else {
    throw std::runtime_error(std::string("unknown dtype: ") + dtype.data());
  }
}

}  // namespace

void LaunchTritonKernel(hipStream_t stream, void** buffers, char* opaque,
                        size_t opaque_len) {
  assert(opaque_len == sizeof(TritonKernelCallBase*));
  TritonKernelCallBase* kernel_call;
  std::memcpy(&kernel_call, opaque, sizeof(TritonKernelCallBase*));
  kernel_call->Launch(stream, buffers);
}

PYBIND11_MODULE(triton_kernel_call_lib, m) {
  py::class_<TritonKernel>(m, "TritonKernel")
      .def(py::init<std::string, std::string, uint32_t, uint32_t>());

  py::class_<TritonKernelCall>(m, "TritonKernelCall")
      .def(py::init<TritonKernel&, uint32_t, uint32_t, uint32_t,
                    std::vector<std::optional<uint64_t>>,
                    std::unordered_map<size_t, size_t>>(),
           py::keep_alive<1, 2>())  // Ensure that the kernel lives long enough.
      .def_property_readonly("descriptor", [](TritonKernelCall& kernel_call) {
        union {
          TritonKernelCall* ptr;
          char bytes[sizeof(TritonKernelCall*)];
        } descriptor;
        descriptor.ptr = &kernel_call;
        return py::bytes(descriptor.bytes, sizeof(TritonKernelCall*));
      });

  py::class_<TritonAutotunedKernelCall>(m, "TritonAutotunedKernelCall")
      .def(py::init<>([](std::string name,
                         std::vector<std::pair<py::object, std::string>>
                             calls_and_descriptions) {
        std::vector<TritonAutotunedKernelCall::Config> configs;
        configs.reserve(calls_and_descriptions.size());
        for (auto& [kernel_call, desc] : calls_and_descriptions) {
          configs.push_back({std::move(kernel_call), std::move(desc)});
        }
        return std::make_unique<TritonAutotunedKernelCall>(std::move(name),
                                                           std::move(configs));
      }))
      .def_property_readonly(
          "descriptor", [](TritonAutotunedKernelCall& kernel_call) {
            union {
              TritonAutotunedKernelCall* ptr;
              char bytes[sizeof(TritonAutotunedKernelCall*)];
            } descriptor;
            descriptor.ptr = &kernel_call;
            return py::bytes(descriptor.bytes,
                             sizeof(TritonAutotunedKernelCall*));
          });

  m.def("get_custom_call", [] {
    return py::capsule(reinterpret_cast<void*>(&LaunchTritonKernel),
                       "xla._CUSTOM_CALL_TARGET");
  });

  m.def("encode_kernel_parameter",
        py::overload_cast<py::int_, std::string_view>(&EncodeKernelParameter));
  m.def(
      "encode_kernel_parameter",
      py::overload_cast<py::float_, std::string_view>(&EncodeKernelParameter));
  m.def("encode_kernel_parameter",
        py::overload_cast<py::bool_, std::string_view>(&EncodeKernelParameter));
}

}  // namespace jax_triton
