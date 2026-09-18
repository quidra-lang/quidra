#include "device_backend.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
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

struct HipApi {
    DynamicLibrary library;
    int (*get_count)(int*){};
    int (*get_name)(char*, int, int){};
    int (*set_device)(int){};
    int (*malloc_fn)(void**, std::size_t){};
    int (*free_fn)(void*){};
    int (*memcpy_fn)(void*, const void*, std::size_t, int){};
    int (*memset_fn)(void*, int, std::size_t){};
    int (*runtime_version)(int*){};
    bool ready{};

    HipApi() {
#ifdef _WIN32
        if (!library.open("amdhip64.dll")) return;
#else
        if (!library.open("libamdhip64.so") &&
            !library.open("libamdhip64.so.7") &&
            !library.open("libamdhip64.so.6")) return;
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
        int count = 0;
        ready = get_count && set_device && malloc_fn && free_fn && memcpy_fn &&
                memset_fn && get_count(&count) == 0;
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
    int global_index{-1};
    int backend_index{-1};
    void* cuda_module{};
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
    result->global_index = index;
    result->backend_index = info->backend_index;
    result->cuda_module = module;
    return result.release();
}

void release(Module* raw) {
    if (!raw) return;
    std::unique_ptr<Module> module(raw);
    auto& api = cuda();
    std::string ignored;
    CudaApi::CUcontext context = nullptr;
    if (module->cuda_module &&
        api.current(module->backend_index, context, ignored) &&
        api.module_unload) {
        (void)api.module_unload(
            static_cast<CudaApi::CUmodule>(module->cuda_module));
    }
}

bool launch(Module* module, const char* kernel,
            LaunchDimensions grid, LaunchDimensions block,
            void** arguments, std::string& error) {
    if (!module || !module->cuda_module || !kernel || !*kernel) {
        error = "invalid NVIDIA kernel launch";
        return false;
    }
    if (grid.x == 0 || grid.y == 0 || grid.z == 0 ||
        block.x == 0 || block.y == 0 || block.z == 0) {
        error = "NVIDIA kernel launch dimensions must be nonzero";
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

} // namespace quidra::device
