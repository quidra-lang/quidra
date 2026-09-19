#include "device_backend.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <type_traits>
#include <thread>
#include <unordered_map>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#ifdef __APPLE__
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#endif

namespace quidra::device {
namespace {

class DynamicLibrary {
public:
    DynamicLibrary() = default;
    explicit DynamicLibrary(const char* name) { open(name); }
    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;
    DynamicLibrary(DynamicLibrary&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }
    DynamicLibrary& operator=(DynamicLibrary&& other) noexcept {
        if (this == &other) return *this;
        close();
        handle_ = other.handle_;
        other.handle_ = nullptr;
        return *this;
    }
    ~DynamicLibrary() { close(); }

    bool open(const char* name) {
        close();
#ifdef _WIN32
        handle_ = static_cast<void*>(LoadLibraryA(name));
#else
        handle_ = dlopen(name, RTLD_NOW | RTLD_LOCAL);
#endif
        return handle_ != nullptr;
    }

    void* symbol(const char* name) const {
        if (!handle_) return nullptr;
#ifdef _WIN32
        return reinterpret_cast<void*>(
            GetProcAddress(static_cast<HMODULE>(handle_), name));
#else
        return dlsym(handle_, name);
#endif
    }

    explicit operator bool() const { return handle_ != nullptr; }

private:
    void close() {
        if (!handle_) return;
#ifdef _WIN32
        FreeLibrary(static_cast<HMODULE>(handle_));
#else
        dlclose(handle_);
#endif
        handle_ = nullptr;
    }

    void* handle_{};
};

template <typename T>
T load_symbol(const DynamicLibrary& library, const char* name) {
    return reinterpret_cast<T>(library.symbol(name));
}

[[maybe_unused]] std::string cuda_version_string(int value) {
    if (value <= 0) return {};
    const int major = value / 1000;
    const int minor = (value % 1000) / 10;
    return std::to_string(major) + "." + std::to_string(minor);
}

struct CudaApi {
    using CUdevice = int;
    using CUcontext = void*;
    using CUdeviceptr = std::uint64_t;
    using CUmodule = void*;
    using CUfunction = void*;
    using CUstream = void*;
    using Result = int;

    DynamicLibrary library;
    Result (*init)(unsigned){};
    Result (*device_count)(int*){};
    Result (*device_get)(CUdevice*, int){};
    Result (*device_name)(char*, int, CUdevice){};
    Result (*driver_version)(int*){};
    Result (*ctx_create)(CUcontext*, unsigned, CUdevice){};
    Result (*ctx_set_current)(CUcontext){};
    Result (*mem_alloc)(CUdeviceptr*, std::size_t){};
    Result (*mem_free)(CUdeviceptr){};
    Result (*copy_h2d)(CUdeviceptr, const void*, std::size_t){};
    Result (*copy_d2h)(void*, CUdeviceptr, std::size_t){};
    Result (*copy_d2d)(CUdeviceptr, CUdeviceptr, std::size_t){};
    Result (*memset_d8)(CUdeviceptr, unsigned char, std::size_t){};
    Result (*module_load_data)(CUmodule*, const void*){};
    Result (*module_unload)(CUmodule){};
    Result (*module_get_function)(CUfunction*, CUmodule, const char*){};
    Result (*launch_kernel)(CUfunction, unsigned, unsigned, unsigned,
                            unsigned, unsigned, unsigned, unsigned,
                            CUstream, void**, void**){};
    Result (*ctx_synchronize)(){};
    std::vector<CUcontext> contexts;
    std::mutex mutex;
    bool ready{};

    CudaApi() {
#ifdef _WIN32
        if (!library.open("nvcuda.dll")) return;
#else
        if (!library.open("libcuda.so.1") && !library.open("libcuda.so")) return;
#endif
        init = load_symbol<decltype(init)>(library, "cuInit");
        device_count = load_symbol<decltype(device_count)>(library, "cuDeviceGetCount");
        device_get = load_symbol<decltype(device_get)>(library, "cuDeviceGet");
        device_name = load_symbol<decltype(device_name)>(library, "cuDeviceGetName");
        driver_version = load_symbol<decltype(driver_version)>(library, "cuDriverGetVersion");
        ctx_create = load_symbol<decltype(ctx_create)>(library, "cuCtxCreate_v2");
        if (!ctx_create)
            ctx_create = load_symbol<decltype(ctx_create)>(library, "cuCtxCreate");
        ctx_set_current = load_symbol<decltype(ctx_set_current)>(library, "cuCtxSetCurrent");
        mem_alloc = load_symbol<decltype(mem_alloc)>(library, "cuMemAlloc_v2");
        if (!mem_alloc)
            mem_alloc = load_symbol<decltype(mem_alloc)>(library, "cuMemAlloc");
        mem_free = load_symbol<decltype(mem_free)>(library, "cuMemFree_v2");
        if (!mem_free)
            mem_free = load_symbol<decltype(mem_free)>(library, "cuMemFree");
        copy_h2d = load_symbol<decltype(copy_h2d)>(library, "cuMemcpyHtoD_v2");
        if (!copy_h2d)
            copy_h2d = load_symbol<decltype(copy_h2d)>(library, "cuMemcpyHtoD");
        copy_d2h = load_symbol<decltype(copy_d2h)>(library, "cuMemcpyDtoH_v2");
        if (!copy_d2h)
            copy_d2h = load_symbol<decltype(copy_d2h)>(library, "cuMemcpyDtoH");
        copy_d2d = load_symbol<decltype(copy_d2d)>(library, "cuMemcpyDtoD_v2");
        if (!copy_d2d)
            copy_d2d = load_symbol<decltype(copy_d2d)>(library, "cuMemcpyDtoD");
        memset_d8 = load_symbol<decltype(memset_d8)>(library, "cuMemsetD8_v2");
        if (!memset_d8)
            memset_d8 = load_symbol<decltype(memset_d8)>(library, "cuMemsetD8");
        module_load_data =
            load_symbol<decltype(module_load_data)>(library, "cuModuleLoadData");
        module_unload =
            load_symbol<decltype(module_unload)>(library, "cuModuleUnload");
        module_get_function =
            load_symbol<decltype(module_get_function)>(library, "cuModuleGetFunction");
        launch_kernel =
            load_symbol<decltype(launch_kernel)>(library, "cuLaunchKernel");
        ctx_synchronize =
            load_symbol<decltype(ctx_synchronize)>(library, "cuCtxSynchronize");
        ready = init && device_count && device_get && device_name && driver_version &&
                ctx_create && ctx_set_current && mem_alloc && mem_free &&
                copy_h2d && copy_d2h && memset_d8 && init(0) == 0;
    }

    bool current(int backend_index, CUcontext& context, std::string& error) {
        if (!ready) {
            error = "NVIDIA CUDA Driver API is unavailable";
            return false;
        }
        std::lock_guard lock(mutex);
        int count = 0;
        if (device_count(&count) != 0 || backend_index < 0 ||
            backend_index >= count) {
            error = "NVIDIA GPU index is unavailable";
            return false;
        }
        if (contexts.size() < static_cast<std::size_t>(count))
            contexts.resize(static_cast<std::size_t>(count), nullptr);
        auto& stored = contexts[static_cast<std::size_t>(backend_index)];
        if (!stored) {
            CUdevice dev = 0;
            if (device_get(&dev, backend_index) != 0 ||
                ctx_create(&stored, 0, dev) != 0) {
                stored = nullptr;
                error = "failed to create NVIDIA CUDA context";
                return false;
            }
        }
        if (ctx_set_current(stored) != 0) {
            error = "failed to select NVIDIA CUDA context";
            return false;
        }
        context = stored;
        return true;
    }
};

CudaApi& cuda() {
    static CudaApi api;
    return api;
}

template <std::size_t N>
bool open_dnn_nvidia_library(DynamicLibrary& library,
                             const std::array<const char*, N>& names) {
    if (const char* root = std::getenv("QUIDRA_DNN_NVIDIA_LIBRARY_PATH");
        root && *root) {
        std::string prefix(root);
        if (prefix.back() != '/' && prefix.back() != '\\') prefix.push_back('/');
        for (const char* name : names) {
            const auto candidate = prefix + name;
            if (library.open(candidate.c_str())) return true;
        }
        return false;
    }

    const char* allow_system = std::getenv("QUIDRA_DNN_ALLOW_SYSTEM_NVIDIA_LIBS");
    if (!allow_system || std::string_view(allow_system) != "1") return false;
    for (const char* name : names) {
        if (library.open(name)) return true;
    }
    return false;
}


struct CublasApi {
    using Handle = void*;
    using Status = int;
    using Operation = int;
    using AtomicsMode = int;

    DynamicLibrary library;
    Status (*create)(Handle*){};
    Status (*destroy)(Handle){};
    Status (*set_atomics_mode)(Handle, AtomicsMode){};
    Status (*sgemm)(Handle, Operation, Operation, int, int, int,
                    const float*, const float*, int, const float*, int,
                    const float*, float*, int){};
    Status (*dgemm)(Handle, Operation, Operation, int, int, int,
                    const double*, const double*, int, const double*, int,
                    const double*, double*, int){};
    std::vector<Handle> handles;
    std::mutex mutex;
    bool ready{};

    CublasApi() {
#ifdef _WIN32
        constexpr std::array names{
            "cublas64_13.dll", "cublas64_12.dll", "cublas64_11.dll"};
#else
        constexpr std::array names{
            "libcublas.so.13", "libcublas.so.12", "libcublas.so.11",
            "libcublas.so"};
#endif
        if (!open_dnn_nvidia_library(library, names)) return;
        create = load_symbol<decltype(create)>(library, "cublasCreate_v2");
        destroy = load_symbol<decltype(destroy)>(library, "cublasDestroy_v2");
        set_atomics_mode =
            load_symbol<decltype(set_atomics_mode)>(library, "cublasSetAtomicsMode");
        sgemm = load_symbol<decltype(sgemm)>(library, "cublasSgemm_v2");
        dgemm = load_symbol<decltype(dgemm)>(library, "cublasDgemm_v2");
        ready = create && destroy && sgemm && dgemm;
    }

    ~CublasApi() {
        if (!destroy) return;
        for (auto handle : handles) {
            if (handle) (void)destroy(handle);
        }
    }

    Handle handle(int backend_index, std::string& error) {
        if (!ready) return nullptr;
        auto& cu = cuda();
        CudaApi::CUcontext context = nullptr;
        if (!cu.current(backend_index, context, error)) return nullptr;
        std::lock_guard lock(mutex);
        int count = 0;
        if (cu.device_count(&count) != 0 || backend_index < 0 ||
            backend_index >= count) {
            error = "NVIDIA GPU index is unavailable";
            return nullptr;
        }
        if (handles.size() < static_cast<std::size_t>(count))
            handles.resize(static_cast<std::size_t>(count), nullptr);
        auto& result = handles[static_cast<std::size_t>(backend_index)];
        if (!result && create(&result) != 0) {
            result = nullptr;
            error = "cuBLAS handle creation failed";
            return nullptr;
        }
        return result;
    }
};

CublasApi& cublas() {
    static CublasApi api;
    return api;
}


struct CudnnApi {
    using Handle = void*;
    using TensorDescriptor = void*;
    using FilterDescriptor = void*;
    using ConvolutionDescriptor = void*;
    using Status = int;

    struct FwdPerf {
        int algo{};
        int status{};
        float time{};
        std::size_t memory{};
        int determinism{};
        int math_type{};
        int reserved[3]{};
    };
    using BwdDataPerf = FwdPerf;
    using BwdFilterPerf = FwdPerf;

    DynamicLibrary library;
    Status (*create)(Handle*){};
    Status (*destroy)(Handle){};
    Status (*create_tensor)(TensorDescriptor*){};
    Status (*destroy_tensor)(TensorDescriptor){};
    Status (*set_tensor4d)(TensorDescriptor,int,int,int,int,int,int){};
    Status (*create_filter)(FilterDescriptor*){};
    Status (*destroy_filter)(FilterDescriptor){};
    Status (*set_filter4d)(FilterDescriptor,int,int,int,int,int,int){};
    Status (*create_convolution)(ConvolutionDescriptor*){};
    Status (*destroy_convolution)(ConvolutionDescriptor){};
    Status (*set_convolution2d)(ConvolutionDescriptor,int,int,int,int,int,int,int,int){};
    Status (*set_convolution_math_type)(ConvolutionDescriptor,int){};
    Status (*get_fwd_algorithms)(Handle,TensorDescriptor,FilterDescriptor,
                                 ConvolutionDescriptor,TensorDescriptor,
                                 int,int*,FwdPerf*){};
    Status (*find_fwd_algorithms)(Handle,TensorDescriptor,FilterDescriptor,
                                  ConvolutionDescriptor,TensorDescriptor,
                                  int,int*,FwdPerf*){};
    Status (*get_fwd_workspace)(Handle,TensorDescriptor,FilterDescriptor,
                                ConvolutionDescriptor,TensorDescriptor,int,
                                std::size_t*){};
    Status (*convolution_forward)(Handle,const void*,TensorDescriptor,const void*,
                                  FilterDescriptor,const void*,ConvolutionDescriptor,
                                  int,void*,std::size_t,const void*,
                                  TensorDescriptor,void*){};
    Status (*add_tensor)(Handle,const void*,TensorDescriptor,const void*,
                         const void*,TensorDescriptor,void*){};
    Status (*get_bwd_data_algorithms)(Handle,FilterDescriptor,TensorDescriptor,
                                      ConvolutionDescriptor,TensorDescriptor,
                                      int,int*,BwdDataPerf*){};
    Status (*find_bwd_data_algorithms)(Handle,FilterDescriptor,TensorDescriptor,
                                       ConvolutionDescriptor,TensorDescriptor,
                                       int,int*,BwdDataPerf*){};
    Status (*get_bwd_data_workspace)(Handle,FilterDescriptor,TensorDescriptor,
                                     ConvolutionDescriptor,TensorDescriptor,int,
                                     std::size_t*){};
    Status (*convolution_backward_data)(Handle,const void*,FilterDescriptor,const void*,
                                        TensorDescriptor,const void*,ConvolutionDescriptor,
                                        int,void*,std::size_t,const void*,
                                        TensorDescriptor,void*){};
    Status (*get_bwd_filter_algorithms)(Handle,TensorDescriptor,TensorDescriptor,
                                        ConvolutionDescriptor,FilterDescriptor,
                                        int,int*,BwdFilterPerf*){};
    Status (*find_bwd_filter_algorithms)(Handle,TensorDescriptor,TensorDescriptor,
                                         ConvolutionDescriptor,FilterDescriptor,
                                         int,int*,BwdFilterPerf*){};
    Status (*get_bwd_filter_workspace)(Handle,TensorDescriptor,TensorDescriptor,
                                       ConvolutionDescriptor,FilterDescriptor,int,
                                       std::size_t*){};
    Status (*convolution_backward_filter)(Handle,const void*,TensorDescriptor,const void*,
                                          TensorDescriptor,const void*,ConvolutionDescriptor,
                                          int,void*,std::size_t,const void*,
                                          FilterDescriptor,void*){};
    Status (*convolution_backward_bias)(Handle,const void*,TensorDescriptor,const void*,
                                        const void*,TensorDescriptor,void*){};
    std::vector<Handle> handles;
    std::mutex mutex;
    bool ready{};

    CudnnApi() {
#ifdef _WIN32
        constexpr std::array names{"cudnn64_9.dll", "cudnn64_8.dll"};
#else
        constexpr std::array names{
            "libcudnn.so.9", "libcudnn.so.8", "libcudnn.so"};
#endif
        if (!open_dnn_nvidia_library(library, names)) return;
        create=load_symbol<decltype(create)>(library,"cudnnCreate");
        destroy=load_symbol<decltype(destroy)>(library,"cudnnDestroy");
        create_tensor=load_symbol<decltype(create_tensor)>(library,"cudnnCreateTensorDescriptor");
        destroy_tensor=load_symbol<decltype(destroy_tensor)>(library,"cudnnDestroyTensorDescriptor");
        set_tensor4d=load_symbol<decltype(set_tensor4d)>(library,"cudnnSetTensor4dDescriptor");
        create_filter=load_symbol<decltype(create_filter)>(library,"cudnnCreateFilterDescriptor");
        destroy_filter=load_symbol<decltype(destroy_filter)>(library,"cudnnDestroyFilterDescriptor");
        set_filter4d=load_symbol<decltype(set_filter4d)>(library,"cudnnSetFilter4dDescriptor");
        create_convolution=load_symbol<decltype(create_convolution)>(library,"cudnnCreateConvolutionDescriptor");
        destroy_convolution=load_symbol<decltype(destroy_convolution)>(library,"cudnnDestroyConvolutionDescriptor");
        set_convolution2d=load_symbol<decltype(set_convolution2d)>(library,"cudnnSetConvolution2dDescriptor");
        set_convolution_math_type=load_symbol<decltype(set_convolution_math_type)>(library,"cudnnSetConvolutionMathType");
        get_fwd_algorithms=load_symbol<decltype(get_fwd_algorithms)>(library,"cudnnGetConvolutionForwardAlgorithm_v7");
        find_fwd_algorithms=load_symbol<decltype(find_fwd_algorithms)>(library,"cudnnFindConvolutionForwardAlgorithm");
        get_fwd_workspace=load_symbol<decltype(get_fwd_workspace)>(library,"cudnnGetConvolutionForwardWorkspaceSize");
        convolution_forward=load_symbol<decltype(convolution_forward)>(library,"cudnnConvolutionForward");
        add_tensor=load_symbol<decltype(add_tensor)>(library,"cudnnAddTensor");
        get_bwd_data_algorithms=load_symbol<decltype(get_bwd_data_algorithms)>(library,"cudnnGetConvolutionBackwardDataAlgorithm_v7");
        find_bwd_data_algorithms=load_symbol<decltype(find_bwd_data_algorithms)>(library,"cudnnFindConvolutionBackwardDataAlgorithm");
        get_bwd_data_workspace=load_symbol<decltype(get_bwd_data_workspace)>(library,"cudnnGetConvolutionBackwardDataWorkspaceSize");
        convolution_backward_data=load_symbol<decltype(convolution_backward_data)>(library,"cudnnConvolutionBackwardData");
        get_bwd_filter_algorithms=load_symbol<decltype(get_bwd_filter_algorithms)>(library,"cudnnGetConvolutionBackwardFilterAlgorithm_v7");
        find_bwd_filter_algorithms=load_symbol<decltype(find_bwd_filter_algorithms)>(library,"cudnnFindConvolutionBackwardFilterAlgorithm");
        get_bwd_filter_workspace=load_symbol<decltype(get_bwd_filter_workspace)>(library,"cudnnGetConvolutionBackwardFilterWorkspaceSize");
        convolution_backward_filter=load_symbol<decltype(convolution_backward_filter)>(library,"cudnnConvolutionBackwardFilter");
        convolution_backward_bias=load_symbol<decltype(convolution_backward_bias)>(library,"cudnnConvolutionBackwardBias");
        ready=create&&destroy&&create_tensor&&destroy_tensor&&set_tensor4d&&
              create_filter&&destroy_filter&&set_filter4d&&create_convolution&&
              destroy_convolution&&set_convolution2d&&get_fwd_algorithms&&
              get_fwd_workspace&&convolution_forward&&add_tensor&&
              get_bwd_data_algorithms&&get_bwd_data_workspace&&
              convolution_backward_data&&get_bwd_filter_algorithms&&
              get_bwd_filter_workspace&&convolution_backward_filter&&
              convolution_backward_bias;
    }

    ~CudnnApi() {
        if(!destroy)return;
        for(auto handle:handles) if(handle) (void)destroy(handle);
    }

    Handle handle(int backend_index,std::string& error) {
        if(!ready)return nullptr;
        auto& cu=cuda();
        CudaApi::CUcontext context=nullptr;
        if(!cu.current(backend_index,context,error))return nullptr;
        std::lock_guard lock(mutex);
        int count=0;
        if(cu.device_count(&count)!=0||backend_index<0||backend_index>=count){
            error="NVIDIA GPU index is unavailable";return nullptr;
        }
        if(handles.size()<static_cast<std::size_t>(count))
            handles.resize(static_cast<std::size_t>(count),nullptr);
        auto& result=handles[static_cast<std::size_t>(backend_index)];
        if(!result&&create(&result)!=0){
            result=nullptr;error="cuDNN handle creation failed";return nullptr;
        }
        return result;
    }
};

CudnnApi& cudnn() {
    static CudnnApi api;
    return api;
}


struct NcclApi {
    struct UniqueId { char internal[128]; };
    using Comm = void*;
    using Result = int;

    DynamicLibrary library;
    Result (*get_unique_id)(UniqueId*){};
    Result (*comm_init_rank)(Comm*, int, UniqueId, int){};
    Result (*all_reduce)(const void*, void*, std::size_t, int, int, Comm, void*){};
    Result (*comm_destroy)(Comm){};
    const char* (*get_error_string)(Result){};
    bool ready{};

    NcclApi() {
#ifdef _WIN32
        return;
#else
        constexpr std::array names{"libnccl.so.2", "libnccl.so"};
        if (!open_dnn_nvidia_library(library, names)) return;
        get_unique_id=load_symbol<decltype(get_unique_id)>(library,"ncclGetUniqueId");
        comm_init_rank=load_symbol<decltype(comm_init_rank)>(library,"ncclCommInitRank");
        all_reduce=load_symbol<decltype(all_reduce)>(library,"ncclAllReduce");
        comm_destroy=load_symbol<decltype(comm_destroy)>(library,"ncclCommDestroy");
        get_error_string=load_symbol<decltype(get_error_string)>(library,"ncclGetErrorString");
        ready=get_unique_id&&comm_init_rank&&all_reduce&&comm_destroy;
#endif
    }

    std::string message(Result result) const {
        if(get_error_string){
            if(const char* text=get_error_string(result)) return text;
        }
        return "NCCL error " + std::to_string(result);
    }
};

NcclApi& nccl() {
    static NcclApi api;
    return api;
}

struct HipApi {
    using Module = void*;
    using Function = void*;
    using Stream = void*;
    using RtcProgram = void*;

    DynamicLibrary library;
    DynamicLibrary rtc_library;
    int (*get_count)(int*){};
    int (*get_name)(char*, int, int){};
    int (*set_device)(int){};
    int (*malloc_fn)(void**, std::size_t){};
    int (*free_fn)(void*){};
    int (*memcpy_fn)(void*, const void*, std::size_t, int){};
    int (*memset_fn)(void*, int, std::size_t){};
    int (*runtime_version)(int*){};
    int (*module_load_data)(Module*, const void*){};
    int (*module_unload)(Module){};
    int (*module_get_function)(Function*, Module, const char*){};
    int (*module_launch_kernel)(Function, unsigned, unsigned, unsigned,
                                unsigned, unsigned, unsigned, unsigned,
                                Stream, void**, void**){};
    int (*device_synchronize)(){};

    int (*rtc_create_program)(RtcProgram*, const char*, const char*,
                              int, const char**, const char**){};
    int (*rtc_compile_program)(RtcProgram, int, const char**){};
    int (*rtc_get_code_size)(RtcProgram, std::size_t*){};
    int (*rtc_get_code)(RtcProgram, char*){};
    int (*rtc_get_log_size)(RtcProgram, std::size_t*){};
    int (*rtc_get_log)(RtcProgram, char*){};
    int (*rtc_destroy_program)(RtcProgram*){};

    bool ready{};
    bool compute_ready{};

    HipApi() {
#ifdef _WIN32
        if (!library.open("amdhip64.dll")) return;
        (void)rtc_library.open("hiprtc.dll");
#else
        if (!library.open("libamdhip64.so") &&
            !library.open("libamdhip64.so.7") &&
            !library.open("libamdhip64.so.6")) return;
        if (!rtc_library.open("libhiprtc.so") &&
            !rtc_library.open("libhiprtc.so.7") &&
            !rtc_library.open("libhiprtc.so.6")) {
            // Memory/transfer support remains usable; compute_ready stays false.
        }
#endif
        get_count = load_symbol<decltype(get_count)>(library, "hipGetDeviceCount");
        get_name = load_symbol<decltype(get_name)>(library, "hipDeviceGetName");
        set_device = load_symbol<decltype(set_device)>(library, "hipSetDevice");
        malloc_fn = load_symbol<decltype(malloc_fn)>(library, "hipMalloc");
        free_fn = load_symbol<decltype(free_fn)>(library, "hipFree");
        memcpy_fn = load_symbol<decltype(memcpy_fn)>(library, "hipMemcpy");
        memset_fn = load_symbol<decltype(memset_fn)>(library, "hipMemset");
        runtime_version =
            load_symbol<decltype(runtime_version)>(library, "hipRuntimeGetVersion");
        module_load_data =
            load_symbol<decltype(module_load_data)>(library, "hipModuleLoadData");
        module_unload =
            load_symbol<decltype(module_unload)>(library, "hipModuleUnload");
        module_get_function =
            load_symbol<decltype(module_get_function)>(library, "hipModuleGetFunction");
        module_launch_kernel =
            load_symbol<decltype(module_launch_kernel)>(library, "hipModuleLaunchKernel");
        device_synchronize =
            load_symbol<decltype(device_synchronize)>(library, "hipDeviceSynchronize");

        if (rtc_library) {
            rtc_create_program =
                load_symbol<decltype(rtc_create_program)>(rtc_library, "hiprtcCreateProgram");
            rtc_compile_program =
                load_symbol<decltype(rtc_compile_program)>(rtc_library, "hiprtcCompileProgram");
            rtc_get_code_size =
                load_symbol<decltype(rtc_get_code_size)>(rtc_library, "hiprtcGetCodeSize");
            rtc_get_code =
                load_symbol<decltype(rtc_get_code)>(rtc_library, "hiprtcGetCode");
            rtc_get_log_size =
                load_symbol<decltype(rtc_get_log_size)>(rtc_library, "hiprtcGetProgramLogSize");
            rtc_get_log =
                load_symbol<decltype(rtc_get_log)>(rtc_library, "hiprtcGetProgramLog");
            rtc_destroy_program =
                load_symbol<decltype(rtc_destroy_program)>(rtc_library, "hiprtcDestroyProgram");
        }

        int count = 0;
        ready = get_count && set_device && malloc_fn && free_fn && memcpy_fn &&
                memset_fn && get_count(&count) == 0;
        compute_ready = ready && module_load_data && module_unload &&
                        module_get_function && module_launch_kernel &&
                        device_synchronize && rtc_create_program &&
                        rtc_compile_program && rtc_get_code_size &&
                        rtc_get_code && rtc_destroy_program;
    }
};

HipApi& hip() {
    static HipApi api;
    return api;
}

struct BufferImpl {
    Backend backend{Backend::Nvidia};
    int global_index{-1};
    int backend_index{-1};
    std::size_t bytes{};
    void* pointer{};
    std::uint64_t cuda_pointer{};
    void* cuda_context{};
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    std::vector<unsigned char> test_data;
#endif
#ifdef __APPLE__
    id<MTLBuffer> metal_buffer{nil};
#endif
};

struct ModuleImpl {
    Backend backend{Backend::Nvidia};
    int global_index{-1};
    int backend_index{-1};
    void* cuda_module{};
    void* hip_module{};
};

std::vector<Info> enumerate_devices() {
    std::vector<Info> result;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if (const char* configured = std::getenv("QUIDRA_TEST_FAKE_GPU_COUNT")) {
        char* end = nullptr;
        const long count = std::strtol(configured, &end, 10);
        if (end == configured || (end && *end != '\0') || count < 0 || count > 16) {
            return result;
        }
        for (long i = 0; i < count; ++i) {
            result.push_back(Info{
                static_cast<int>(result.size()), Backend::Test,
                static_cast<int>(i),
                "Quidra Fake GPU " + std::to_string(i),
                "test-only", "host-backed test backend"});
        }
        return result;
    }
#endif
#ifdef __APPLE__
    @autoreleasepool {
        NSArray<id<MTLDevice>>* metal_devices = MTLCopyAllDevices();
        for (NSUInteger i = 0; i < [metal_devices count]; ++i) {
            id<MTLDevice> dev = [metal_devices objectAtIndex:i];
            Info info;
            info.index = static_cast<int>(result.size());
            info.backend = Backend::Metal;
            info.backend_index = static_cast<int>(i);
            info.name = std::string([[dev name] UTF8String]);
            info.driver = "macOS Metal driver";
            info.runtime = "Metal";
            result.push_back(std::move(info));
        }
        [metal_devices release];
    }
#else
    auto& cu = cuda();
    if (cu.ready) {
        int count = 0;
        if (cu.device_count(&count) == 0) {
            int driver = 0;
            (void)cu.driver_version(&driver);
            for (int i = 0; i < count; ++i) {
                CudaApi::CUdevice dev = 0;
                char name[256]{};
                if (cu.device_get(&dev, i) != 0) continue;
                if (cu.device_name(name, static_cast<int>(sizeof(name)), dev) != 0)
                    std::memcpy(name, "NVIDIA GPU", sizeof("NVIDIA GPU"));
                result.push_back(Info{
                    static_cast<int>(result.size()), Backend::Nvidia, i, name,
                    cuda_version_string(driver), "CUDA Driver API"});
            }
        }
    }

    auto& h = hip();
    if (h.ready) {
        int count = 0;
        if (h.get_count(&count) == 0) {
            int version = 0;
            if (h.runtime_version) (void)h.runtime_version(&version);
            for (int i = 0; i < count; ++i) {
                char name[256]{};
                if (!h.get_name ||
                    h.get_name(name, static_cast<int>(sizeof(name)), i) != 0)
                    std::memcpy(name, "AMD GPU", sizeof("AMD GPU"));
                const auto runtime = version > 0
                    ? "HIP " + std::to_string(version / 10000000) + "." +
                          std::to_string((version / 100000) % 100)
                    : "HIP";
                result.push_back(Info{
                    static_cast<int>(result.size()), Backend::Amd, i, name,
                    "AMD GPU driver", runtime});
            }
        }
    }
#endif
    return result;
}

std::atomic<int> dnn_mode_value{static_cast<int>(DnnMode::Fast)};

bool range_ok(const BufferImpl& buffer, std::size_t offset, std::size_t bytes) {
    return offset <= buffer.bytes && bytes <= buffer.bytes - offset;
}

} // namespace

struct Buffer : BufferImpl {};
struct Module : ModuleImpl {};

const std::vector<Info>& devices() {
    static const std::vector<Info> value = enumerate_devices();
    return value;
}

const Info* find(int index) {
    const auto& all = devices();
    if (index < 0 || static_cast<std::size_t>(index) >= all.size()) return nullptr;
    return &all[static_cast<std::size_t>(index)];
}

void set_dnn_mode(DnnMode mode) {
    dnn_mode_value.store(static_cast<int>(mode), std::memory_order_relaxed);
}

DnnMode dnn_mode() {
    return static_cast<DnnMode>(
        dnn_mode_value.load(std::memory_order_relaxed));
}

std::string backend_name(Backend backend) {
    switch (backend) {
        case Backend::Nvidia: return "NVIDIA";
        case Backend::Amd: return "AMD";
        case Backend::Metal: return "Metal";
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
        case Backend::Test: return "TEST";
#endif
    }
    return "unknown";
}

Buffer* allocate(int index, std::size_t bytes, std::string& error) {
    const auto* info = find(index);
    if (!info) {
        error = "gpu(" + std::to_string(index) + ") is not available";
        return nullptr;
    }
    auto buffer = std::make_unique<Buffer>();
    buffer->backend = info->backend;
    buffer->global_index = index;
    buffer->backend_index = info->backend_index;
    buffer->bytes = bytes;
    const auto physical_bytes = std::max<std::size_t>(bytes, 1);

#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if (info->backend == Backend::Test) {
        try {
            buffer->test_data.resize(physical_bytes);
        } catch (...) {
            error = "test GPU memory allocation failed";
            return nullptr;
        }
        return buffer.release();
    }
#endif

    if (info->backend == Backend::Nvidia) {
        auto& api = cuda();
        CudaApi::CUcontext context = nullptr;
        if (!api.current(info->backend_index, context, error)) return nullptr;
        CudaApi::CUdeviceptr pointer = 0;
        if (api.mem_alloc(&pointer, physical_bytes) != 0) {
            error = "NVIDIA GPU memory allocation failed";
            return nullptr;
        }
        buffer->cuda_pointer = pointer;
        buffer->cuda_context = context;
        return buffer.release();
    }

    if (info->backend == Backend::Amd) {
        auto& api = hip();
        if (!api.ready || api.set_device(info->backend_index) != 0) {
            error = "failed to select AMD GPU";
            return nullptr;
        }
        void* pointer = nullptr;
        if (api.malloc_fn(&pointer, physical_bytes) != 0) {
            error = "AMD GPU memory allocation failed";
            return nullptr;
        }
        buffer->pointer = pointer;
        return buffer.release();
    }

#ifdef __APPLE__
    if (info->backend == Backend::Metal) {
        @autoreleasepool {
            NSArray<id<MTLDevice>>* metal_devices = MTLCopyAllDevices();
            if (info->backend_index < 0 ||
                static_cast<NSUInteger>(info->backend_index) >= [metal_devices count]) {
                [metal_devices release];
                error = "Metal GPU index is unavailable";
                return nullptr;
            }
            id<MTLDevice> dev =
                [metal_devices objectAtIndex:static_cast<NSUInteger>(info->backend_index)];
            id<MTLBuffer> metal =
                [dev newBufferWithLength:physical_bytes
                                 options:MTLResourceStorageModeShared];
            [metal_devices release];
            if (!metal) {
                error = "Metal GPU memory allocation failed";
                return nullptr;
            }
            buffer->metal_buffer = metal;
            return buffer.release();
        }
    }
#endif

    error = "GPU backend is unavailable";
    return nullptr;
}

void release(Buffer* raw) {
    if (!raw) return;
    std::unique_ptr<Buffer> buffer(raw);
    if (buffer->backend == Backend::Nvidia && buffer->cuda_pointer != 0) {
        auto& api = cuda();
        std::string ignored;
        CudaApi::CUcontext context = nullptr;
        if (api.current(buffer->backend_index, context, ignored))
            (void)api.mem_free(buffer->cuda_pointer);
    } else if (buffer->backend == Backend::Amd && buffer->pointer) {
        auto& api = hip();
        if (api.ready && api.set_device(buffer->backend_index) == 0)
            (void)api.free_fn(buffer->pointer);
#ifdef __APPLE__
    } else if (buffer->backend == Backend::Metal && buffer->metal_buffer) {
        [buffer->metal_buffer release];
        buffer->metal_buffer = nil;
#endif
    }
}

bool copy_from_host(Buffer* raw, std::size_t offset, const void* source,
                    std::size_t bytes, std::string& error) {
    if (!raw || (!source && bytes != 0) || !range_ok(*raw, offset, bytes)) {
        error = "invalid GPU upload range";
        return false;
    }
    if (bytes == 0) return true;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if (raw->backend == Backend::Test) {
        std::memcpy(raw->test_data.data() + offset, source, bytes);
        return true;
    }
#endif
    if (raw->backend == Backend::Nvidia) {
        auto& api = cuda();
        CudaApi::CUcontext context = nullptr;
        if (!api.current(raw->backend_index, context, error)) return false;
        if (api.copy_h2d(raw->cuda_pointer + offset, source, bytes) != 0) {
            error = "NVIDIA GPU upload failed";
            return false;
        }
        return true;
    }
    if (raw->backend == Backend::Amd) {
        auto& api = hip();
        if (!api.ready || api.set_device(raw->backend_index) != 0 ||
            api.memcpy_fn(static_cast<unsigned char*>(raw->pointer) + offset,
                          source, bytes, 1) != 0) {
            error = "AMD GPU upload failed";
            return false;
        }
        return true;
    }
#ifdef __APPLE__
    if (raw->backend == Backend::Metal) {
        std::memcpy(static_cast<unsigned char*>([raw->metal_buffer contents]) + offset,
                    source, bytes);
        return true;
    }
#endif
    error = "GPU backend upload is unavailable";
    return false;
}

bool copy_to_host(const Buffer* raw, std::size_t offset, void* destination,
                  std::size_t bytes, std::string& error) {
    if (!raw || (!destination && bytes != 0) || !range_ok(*raw, offset, bytes)) {
        error = "invalid GPU download range";
        return false;
    }
    if (bytes == 0) return true;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if (raw->backend == Backend::Test) {
        std::memcpy(destination, raw->test_data.data() + offset, bytes);
        return true;
    }
#endif
    if (raw->backend == Backend::Nvidia) {
        auto& api = cuda();
        CudaApi::CUcontext context = nullptr;
        if (!api.current(raw->backend_index, context, error)) return false;
        if (api.copy_d2h(destination, raw->cuda_pointer + offset, bytes) != 0) {
            error = "NVIDIA GPU download failed";
            return false;
        }
        return true;
    }
    if (raw->backend == Backend::Amd) {
        auto& api = hip();
        if (!api.ready || api.set_device(raw->backend_index) != 0 ||
            api.memcpy_fn(destination,
                          static_cast<unsigned char*>(raw->pointer) + offset,
                          bytes, 2) != 0) {
            error = "AMD GPU download failed";
            return false;
        }
        return true;
    }
#ifdef __APPLE__
    if (raw->backend == Backend::Metal) {
        std::memcpy(destination,
                    static_cast<unsigned char*>([raw->metal_buffer contents]) + offset,
                    bytes);
        return true;
    }
#endif
    error = "GPU backend download is unavailable";
    return false;
}

bool copy_device_to_device(
    Buffer* destination, std::size_t destination_offset,
    const Buffer* source, std::size_t source_offset,
    std::size_t bytes, std::string& error) {
    if (!destination || !source ||
        !range_ok(*destination, destination_offset, bytes) ||
        !range_ok(*source, source_offset, bytes)) {
        error = "invalid GPU device-copy range";
        return false;
    }
    if (bytes == 0) return true;
    if (destination->backend != source->backend ||
        destination->global_index != source->global_index) {
        error = "direct GPU device copy requires the same gpu(n); cross-device transfers are staged explicitly";
        return false;
    }

#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if (destination->backend == Backend::Test) {
        std::memmove(destination->test_data.data() + destination_offset,
                     source->test_data.data() + source_offset, bytes);
        return true;
    }
#endif

    if (destination->backend == Backend::Nvidia) {
        auto& api = cuda();
        CudaApi::CUcontext context = nullptr;
        if (!api.copy_d2d ||
            !api.current(destination->backend_index, context, error)) {
            if (error.empty()) error = "NVIDIA device-to-device copy is unavailable";
            return false;
        }
        if (api.copy_d2d(destination->cuda_pointer + destination_offset,
                         source->cuda_pointer + source_offset, bytes) != 0) {
            error = "NVIDIA device-to-device copy failed";
            return false;
        }
        return true;
    }
    if (destination->backend == Backend::Amd) {
        auto& api = hip();
        if (!api.ready || api.set_device(destination->backend_index) != 0 ||
            api.memcpy_fn(static_cast<unsigned char*>(destination->pointer) +
                              destination_offset,
                          static_cast<unsigned char*>(source->pointer) +
                              source_offset,
                          bytes, 3) != 0) {
            error = "AMD device-to-device copy failed";
            return false;
        }
        return true;
    }
#ifdef __APPLE__
    if (destination->backend == Backend::Metal) {
        std::memmove(
            static_cast<unsigned char*>([destination->metal_buffer contents]) +
                destination_offset,
            static_cast<unsigned char*>([source->metal_buffer contents]) +
                source_offset,
            bytes);
        return true;
    }
#endif
    error = "GPU device-to-device copy is unavailable";
    return false;
}

bool zero(Buffer* raw, std::size_t offset, std::size_t bytes,
          std::string& error) {
    if (!raw || !range_ok(*raw, offset, bytes)) {
        error = "invalid GPU memset range";
        return false;
    }
    if (bytes == 0) return true;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if (raw->backend == Backend::Test) {
        std::memset(raw->test_data.data() + offset, 0, bytes);
        return true;
    }
#endif
    if (raw->backend == Backend::Nvidia) {
        auto& api = cuda();
        CudaApi::CUcontext context = nullptr;
        if (!api.current(raw->backend_index, context, error)) return false;
        if (api.memset_d8(raw->cuda_pointer + offset, 0, bytes) != 0) {
            error = "NVIDIA GPU memset failed";
            return false;
        }
        return true;
    }
    if (raw->backend == Backend::Amd) {
        auto& api = hip();
        if (!api.ready || api.set_device(raw->backend_index) != 0 ||
            api.memset_fn(static_cast<unsigned char*>(raw->pointer) + offset,
                          0, bytes) != 0) {
            error = "AMD GPU memset failed";
            return false;
        }
        return true;
    }
#ifdef __APPLE__
    if (raw->backend == Backend::Metal) {
        std::memset(static_cast<unsigned char*>([raw->metal_buffer contents]) + offset,
                    0, bytes);
        return true;
    }
#endif
    error = "GPU backend memset is unavailable";
    return false;
}

int buffer_device(const Buffer* buffer) {
    return buffer ? buffer->global_index : -1;
}

Module* load_ptx(int index, const std::string& ptx, std::string& error) {
    const auto* info = find(index);
    if (!info) {
        error = "gpu(" + std::to_string(index) + ") is not available";
        return nullptr;
    }
    if (info->backend != Backend::Nvidia) {
        error = "PTX modules are only supported by the NVIDIA backend";
        return nullptr;
    }
    auto& api = cuda();
    if (!api.module_load_data || !api.module_unload || !api.module_get_function ||
        !api.launch_kernel || !api.ctx_synchronize) {
        error = "NVIDIA PTX module/launch API is unavailable";
        return nullptr;
    }
    CudaApi::CUcontext context = nullptr;
    if (!api.current(info->backend_index, context, error)) return nullptr;

    CudaApi::CUmodule module = nullptr;
    if (api.module_load_data(&module, ptx.c_str()) != 0 || !module) {
        error = "failed to load Quidra PTX module";
        return nullptr;
    }
    auto result = std::make_unique<Module>();
    result->backend = Backend::Nvidia;
    result->global_index = index;
    result->backend_index = info->backend_index;
    result->cuda_module = module;
    return result.release();
}


Module* load_hip_source(int index, const std::string& source, std::string& error) {
    const auto* info = find(index);
    if (!info) {
        error = "gpu(" + std::to_string(index) + ") is not available";
        return nullptr;
    }
    if (info->backend != Backend::Amd) {
        error = "HIP source modules are only supported by the AMD backend";
        return nullptr;
    }
    auto& api = hip();
    if (!api.compute_ready || api.set_device(info->backend_index) != 0) {
        error = "AMD HIP runtime compilation/launch API is unavailable";
        return nullptr;
    }

    HipApi::RtcProgram program = nullptr;
    if (api.rtc_create_program(
            &program, source.c_str(), "quidra_kernel.hip", 0, nullptr, nullptr) != 0 ||
        !program) {
        error = "failed to create Quidra HIP runtime compilation program";
        return nullptr;
    }

    const auto destroy_program = [&] {
        if (program) {
            (void)api.rtc_destroy_program(&program);
            program = nullptr;
        }
    };
    const char* options[] = {"--std=c++14"};
    const int compile_status = api.rtc_compile_program(program, 1, options);
    if (compile_status != 0) {
        error = "HIP runtime compilation failed";
        if (api.rtc_get_log_size && api.rtc_get_log) {
            std::size_t log_size = 0;
            if (api.rtc_get_log_size(program, &log_size) == 0 && log_size > 1) {
                std::string log(log_size, '\0');
                if (api.rtc_get_log(program, log.data()) == 0) {
                    while (!log.empty() && log.back() == '\0') log.pop_back();
                    if (!log.empty()) error += ": " + log;
                }
            }
        }
        destroy_program();
        return nullptr;
    }

    std::size_t code_size = 0;
    if (api.rtc_get_code_size(program, &code_size) != 0 || code_size == 0) {
        destroy_program();
        error = "HIP runtime compilation produced no code object";
        return nullptr;
    }
    std::vector<char> code(code_size);
    if (api.rtc_get_code(program, code.data()) != 0) {
        destroy_program();
        error = "failed to retrieve HIP runtime code object";
        return nullptr;
    }
    destroy_program();

    HipApi::Module module = nullptr;
    if (api.module_load_data(&module, code.data()) != 0 || !module) {
        error = "failed to load Quidra HIP module";
        return nullptr;
    }
    auto result = std::make_unique<Module>();
    result->backend = Backend::Amd;
    result->global_index = index;
    result->backend_index = info->backend_index;
    result->hip_module = module;
    return result.release();
}

void release(Module* raw) {
    if (!raw) return;
    std::unique_ptr<Module> module(raw);
    if (module->backend == Backend::Nvidia) {
        auto& api = cuda();
        std::string ignored;
        CudaApi::CUcontext context = nullptr;
        if (module->cuda_module &&
            api.current(module->backend_index, context, ignored) &&
            api.module_unload) {
            (void)api.module_unload(
                static_cast<CudaApi::CUmodule>(module->cuda_module));
        }
        return;
    }
    if (module->backend == Backend::Amd && module->hip_module) {
        auto& api = hip();
        if (api.ready && api.set_device(module->backend_index) == 0 &&
            api.module_unload) {
            (void)api.module_unload(
                static_cast<HipApi::Module>(module->hip_module));
        }
    }
}
bool launch(Module* module, const char* kernel,
            LaunchDimensions grid, LaunchDimensions block,
            void** arguments, std::string& error) {
    if (!module || !kernel || !*kernel) {
        error = "invalid GPU kernel launch";
        return false;
    }
    if (grid.x == 0 || grid.y == 0 || grid.z == 0 ||
        block.x == 0 || block.y == 0 || block.z == 0) {
        error = "GPU kernel launch dimensions must be nonzero";
        return false;
    }

    if (module->backend == Backend::Nvidia) {
        if (!module->cuda_module) {
            error = "invalid NVIDIA kernel module";
            return false;
        }
        auto& api = cuda();
        if (!api.module_get_function || !api.launch_kernel || !api.ctx_synchronize) {
            error = "NVIDIA PTX module/launch API is unavailable";
            return false;
        }
        CudaApi::CUcontext context = nullptr;
        if (!api.current(module->backend_index, context, error)) return false;

        CudaApi::CUfunction function = nullptr;
        if (api.module_get_function(
                &function, static_cast<CudaApi::CUmodule>(module->cuda_module),
                kernel) != 0 || !function) {
            error = std::string("NVIDIA PTX kernel not found: ") + kernel;
            return false;
        }
        if (api.launch_kernel(function,
                              grid.x, grid.y, grid.z,
                              block.x, block.y, block.z,
                              0, nullptr, arguments, nullptr) != 0) {
            error = std::string("NVIDIA kernel launch failed: ") + kernel;
            return false;
        }
        if (api.ctx_synchronize() != 0) {
            error = std::string("NVIDIA kernel synchronization failed: ") + kernel;
            return false;
        }
        return true;
    }

    if (module->backend == Backend::Amd) {
        if (!module->hip_module) {
            error = "invalid AMD HIP kernel module";
            return false;
        }
        auto& api = hip();
        if (!api.compute_ready || api.set_device(module->backend_index) != 0) {
            error = "AMD HIP runtime compilation/launch API is unavailable";
            return false;
        }
        HipApi::Function function = nullptr;
        if (api.module_get_function(
                &function, static_cast<HipApi::Module>(module->hip_module),
                kernel) != 0 || !function) {
            error = std::string("AMD HIP kernel not found: ") + kernel;
            return false;
        }
        if (api.module_launch_kernel(
                function, grid.x, grid.y, grid.z,
                block.x, block.y, block.z, 0, nullptr,
                arguments, nullptr) != 0) {
            error = std::string("AMD HIP kernel launch failed: ") + kernel;
            return false;
        }
        if (api.device_synchronize() != 0) {
            error = std::string("AMD HIP kernel synchronization failed: ") + kernel;
            return false;
        }
        return true;
    }

    error = "kernel module backend is not launchable";
    return false;
}


bool compute_all_reduce_sum(
    const std::vector<Buffer*>& buffers, int dtype, std::size_t count,
    std::string& error) {
    if (buffers.empty()) {
        error = "NCCL all-reduce requires at least one GPU tensor";
        return false;
    }
    if (dtype != 9 && dtype != 10) {
        error = "NCCL all-reduce requires float32 or float tensors";
        return false;
    }
    const auto width = dtype == 10 ? sizeof(float) : sizeof(double);
    if (count != 0 && width > std::numeric_limits<std::size_t>::max() / count) {
        error = "NCCL all-reduce byte size overflow";
        return false;
    }
    const auto bytes = count * width;
    for (auto* buffer : buffers) {
        if (!buffer || !range_ok(*buffer, 0, bytes)) {
            error = "invalid NCCL all-reduce buffer range";
            return false;
        }
    }
    if (buffers.size() == 1) return true;

#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    bool all_test = true;
    for (auto* buffer : buffers) all_test &= buffer->backend == Backend::Test;
    if (all_test) {
        if (dtype == 10) {
            std::vector<float> sum(count, 0.0F);
            for (auto* buffer : buffers) {
                for (std::size_t i = 0; i < count; ++i) {
                    float value{};
                    std::memcpy(&value, buffer->test_data.data() + i * sizeof(float),
                                sizeof(float));
                    sum[i] += value;
                }
            }
            for (auto* buffer : buffers)
                std::memcpy(buffer->test_data.data(), sum.data(), count * sizeof(float));
        } else {
            std::vector<double> sum(count, 0.0);
            for (auto* buffer : buffers) {
                for (std::size_t i = 0; i < count; ++i) {
                    double value{};
                    std::memcpy(&value, buffer->test_data.data() + i * sizeof(double),
                                sizeof(double));
                    sum[i] += value;
                }
            }
            for (auto* buffer : buffers)
                std::memcpy(buffer->test_data.data(), sum.data(), count * sizeof(double));
        }
        return true;
    }
#endif

    std::unordered_map<int, bool> seen_devices;
    for (auto* buffer : buffers) {
        if (buffer->backend != Backend::Nvidia) {
            error = "NCCL all-reduce requires NVIDIA gpu(n) tensors";
            return false;
        }
        if (!seen_devices.emplace(buffer->global_index, true).second) {
            error = "NCCL all-reduce requires one tensor per distinct gpu(n)";
            return false;
        }
    }

    auto& api = nccl();
    if (!api.ready) {
        error = "NCCL is unavailable; configure the DNN NVIDIA library path";
        return false;
    }
    if (buffers.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        error = "NCCL all-reduce rank count is too large";
        return false;
    }

    NcclApi::UniqueId unique{};
    const auto unique_status = api.get_unique_id(&unique);
    if (unique_status != 0) {
        error = "NCCL unique-id creation failed: " + api.message(unique_status);
        return false;
    }

    const auto ranks = static_cast<int>(buffers.size());
    std::vector<NcclApi::Comm> comms(buffers.size(), nullptr);
    std::vector<std::string> rank_errors(buffers.size());
    std::vector<std::thread> threads;
    try {
        threads.reserve(buffers.size());
        for (std::size_t rank = 0; rank < buffers.size(); ++rank) {
            threads.emplace_back([&, rank] {
                auto* buffer = buffers[rank];
                auto& cu = cuda();
                CudaApi::CUcontext context = nullptr;
                std::string context_error;
                if (!cu.current(buffer->backend_index, context, context_error)) {
                    rank_errors[rank] = context_error;
                    return;
                }
                auto status = api.comm_init_rank(
                    &comms[rank], ranks, unique, static_cast<int>(rank));
                if (status != 0) {
                    rank_errors[rank] =
                        "NCCL communicator initialization failed: " + api.message(status);
                    return;
                }
                void* pointer = reinterpret_cast<void*>(
                    static_cast<std::uintptr_t>(buffer->cuda_pointer));
                constexpr int nccl_sum = 0;
                const int nccl_dtype = dtype == 10 ? 7 : 8;
                status = api.all_reduce(
                    pointer, pointer, count, nccl_dtype, nccl_sum, comms[rank], nullptr);
                if (status == 0 && cu.ctx_synchronize)
                    status = cu.ctx_synchronize();
                if (status != 0)
                    rank_errors[rank] =
                        "NCCL all-reduce failed: " + api.message(status);
            });
        }
    } catch (const std::exception& exception) {
        for (auto& thread : threads) if (thread.joinable()) thread.join();
        for (auto comm : comms) if (comm) (void)api.comm_destroy(comm);
        error = std::string("cannot start NCCL rank: ") + exception.what();
        return false;
    }

    for (auto& thread : threads) thread.join();
    for (auto comm : comms) if (comm) (void)api.comm_destroy(comm);
    for (const auto& rank_error : rank_errors) {
        if (!rank_error.empty()) {
            error = rank_error;
            return false;
        }
    }
    return true;
}

#include "device_integer_compute.inc"
#include "device_compute.inc"
#include "device_neural_compute.inc"
#include "device_autograd_compute.inc"
#include "device_training_compute.inc"
#include "device_vision_compute.inc"

} // namespace quidra::device
