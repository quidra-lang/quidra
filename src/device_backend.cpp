#include "device_backend.hpp"
#include "quidra/project.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <functional>
#include <iterator>
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

#ifdef __APPLE__
bool synchronize_metal_backend(int backend_index, std::string& error);
bool metal_copy_from_host(Buffer* raw, std::size_t offset, const void* source,
                          std::size_t bytes, std::string& error);
bool metal_copy_to_host(const Buffer* raw, std::size_t offset, void* destination,
                        std::size_t bytes, std::string& error);
bool metal_copy_device_to_device(Buffer* destination, std::size_t destination_offset,
                                 const Buffer* source, std::size_t source_offset,
                                 std::size_t bytes, std::string& error);
bool metal_zero(Buffer* raw, std::size_t offset, std::size_t bytes,
                std::string& error);
#endif

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
    using CUevent = void*;
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
    Result (*copy_h2d_async)(CUdeviceptr, const void*, std::size_t, CUstream){};
    Result (*copy_d2d_async)(CUdeviceptr, CUdeviceptr, std::size_t, CUstream){};
    Result (*memset_d8_async)(CUdeviceptr, unsigned char, std::size_t, CUstream){};
    Result (*host_alloc)(void**, std::size_t, unsigned){};
    Result (*host_free)(void*){};
    Result (*event_create)(CUevent*, unsigned){};
    Result (*event_record)(CUevent, CUstream){};
    Result (*event_query)(CUevent){};
    Result (*event_synchronize)(CUevent){};
    Result (*event_destroy)(CUevent){};
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
        copy_h2d_async =
            load_symbol<decltype(copy_h2d_async)>(library, "cuMemcpyHtoDAsync_v2");
        if (!copy_h2d_async)
            copy_h2d_async =
                load_symbol<decltype(copy_h2d_async)>(library, "cuMemcpyHtoDAsync");
        copy_d2d_async =
            load_symbol<decltype(copy_d2d_async)>(library, "cuMemcpyDtoDAsync_v2");
        if (!copy_d2d_async)
            copy_d2d_async =
                load_symbol<decltype(copy_d2d_async)>(library, "cuMemcpyDtoDAsync");
        memset_d8_async =
            load_symbol<decltype(memset_d8_async)>(library, "cuMemsetD8Async");
        host_alloc = load_symbol<decltype(host_alloc)>(library, "cuMemHostAlloc");
        host_free = load_symbol<decltype(host_free)>(library, "cuMemFreeHost");
        event_create = load_symbol<decltype(event_create)>(library, "cuEventCreate");
        event_record = load_symbol<decltype(event_record)>(library, "cuEventRecord");
        event_query = load_symbol<decltype(event_query)>(library, "cuEventQuery");
        event_synchronize =
            load_symbol<decltype(event_synchronize)>(library, "cuEventSynchronize");
        event_destroy =
            load_symbol<decltype(event_destroy)>(library, "cuEventDestroy_v2");
        if (!event_destroy)
            event_destroy =
                load_symbol<decltype(event_destroy)>(library, "cuEventDestroy");
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

    bool current_if_created(int backend_index, CUcontext& context,
                            bool& exists, std::string& error) {
        std::lock_guard lock(mutex);
        exists = backend_index >= 0 &&
                 static_cast<std::size_t>(backend_index) < contexts.size() &&
                 contexts[static_cast<std::size_t>(backend_index)] != nullptr;
        if (!exists) {
            context = nullptr;
            return true;
        }
        auto stored = contexts[static_cast<std::size_t>(backend_index)];
        if (!ctx_set_current || ctx_set_current(stored) != 0) {
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

struct CudaResourceState {
    struct DeviceBlock {
        std::size_t bytes{};
        CudaApi::CUdeviceptr pointer{};
    };
    struct HostBlock {
        std::size_t bytes{};
        void* pointer{};
    };
    struct PendingUpload {
        int backend_index{-1};
        CudaApi::CUevent event{};
        HostBlock block;
    };
    struct PendingDevice {
        int backend_index{-1};
        CudaApi::CUevent event{};
        DeviceBlock block;
    };

    std::mutex mutex;
    std::vector<std::vector<DeviceBlock>> free_device_blocks;
    std::vector<std::vector<DeviceBlock>> retired_device_blocks;
    std::vector<std::vector<HostBlock>> free_host_blocks;
    std::vector<std::vector<HostBlock>> retired_host_blocks;
    std::vector<PendingUpload> pending_uploads;
    std::vector<PendingDevice> pending_device_blocks;
    std::vector<std::vector<CudaApi::CUmodule>> retired_modules;

    ~CudaResourceState() {
        auto& api=cuda();
        int count=0;
        if(!api.device_count||api.device_count(&count)!=0) count=0;
        for(int index=0;index<count;++index){
            std::string ignored;
            CudaApi::CUcontext context=nullptr;
            if(api.current(index,context,ignored)&&api.ctx_synchronize)
                (void)api.ctx_synchronize();
        }

        std::lock_guard lock(mutex);
        for(auto& pending:pending_uploads){
            if(pending.event&&api.event_destroy)
                (void)api.event_destroy(pending.event);
            if(pending.block.pointer&&api.host_free)
                (void)api.host_free(pending.block.pointer);
        }
        pending_uploads.clear();
        for(auto& pending:pending_device_blocks){
            if(pending.event&&api.event_destroy)
                (void)api.event_destroy(pending.event);
            if(pending.backend_index>=0){
                const auto index=static_cast<std::size_t>(pending.backend_index);
                if(free_device_blocks.size()<=index)
                    free_device_blocks.resize(index+1);
                free_device_blocks[index].push_back(pending.block);
            }
        }
        pending_device_blocks.clear();
        for(auto& blocks:free_host_blocks)
            for(auto& block:blocks)
                if(block.pointer&&api.host_free)
                    (void)api.host_free(block.pointer);
        for(auto& blocks:retired_host_blocks)
            for(auto& block:blocks)
                if(block.pointer&&api.host_free)
                    (void)api.host_free(block.pointer);
        const auto device_slots=
            std::max(free_device_blocks.size(),retired_device_blocks.size());
        for(std::size_t index=0;index<device_slots;++index){
            std::string ignored;
            CudaApi::CUcontext context=nullptr;
            if(!api.current(static_cast<int>(index),context,ignored)) continue;
            if(index<free_device_blocks.size())
                for(auto& block:free_device_blocks[index])
                    if(block.pointer&&api.mem_free)
                        (void)api.mem_free(block.pointer);
            if(index<retired_device_blocks.size())
                for(auto& block:retired_device_blocks[index])
                    if(block.pointer&&api.mem_free)
                        (void)api.mem_free(block.pointer);
        }
        for(std::size_t index=0;index<retired_modules.size();++index){
            std::string ignored;
            CudaApi::CUcontext context=nullptr;
            if(!api.current(static_cast<int>(index),context,ignored)) continue;
            for(auto module:retired_modules[index])
                if(module&&api.module_unload)
                    (void)api.module_unload(module);
        }
    }
};

CudaResourceState& cuda_resources() {
    static CudaResourceState state;
    return state;
}

void cuda_ensure_resource_slots(CudaResourceState& state,int backend_index) {
    const auto count=static_cast<std::size_t>(backend_index+1);
    if(state.free_device_blocks.size()<count)
        state.free_device_blocks.resize(count);
    if(state.retired_device_blocks.size()<count)
        state.retired_device_blocks.resize(count);
    if(state.free_host_blocks.size()<count)
        state.free_host_blocks.resize(count);
    if(state.retired_host_blocks.size()<count)
        state.retired_host_blocks.resize(count);
    if(state.retired_modules.size()<count)
        state.retired_modules.resize(count);
}

void cuda_reap_uploads(CudaApi& api,int backend_index,bool completed) {
    auto& state=cuda_resources();
    std::lock_guard lock(state.mutex);
    cuda_ensure_resource_slots(state,backend_index);
    auto out=state.pending_uploads.begin();
    for(auto it=state.pending_uploads.begin();it!=state.pending_uploads.end();++it){
        if(it->backend_index!=backend_index){
            *out++=std::move(*it);
            continue;
        }
        const bool ready=completed||
            (api.event_query&&it->event&&api.event_query(it->event)==0);
        if(!ready){
            *out++=std::move(*it);
            continue;
        }
        if(it->event&&api.event_destroy)
            (void)api.event_destroy(it->event);
        state.free_host_blocks[static_cast<std::size_t>(backend_index)]
            .push_back(it->block);
    }
    state.pending_uploads.erase(out,state.pending_uploads.end());
    if(completed){
        auto& retired=
            state.retired_host_blocks[static_cast<std::size_t>(backend_index)];
        auto& free=
            state.free_host_blocks[static_cast<std::size_t>(backend_index)];
        free.insert(free.end(),
                    std::make_move_iterator(retired.begin()),
                    std::make_move_iterator(retired.end()));
        retired.clear();
    }
}

void cuda_reap_device_blocks(CudaApi& api,int backend_index,bool completed) {
    CudaApi::CUcontext context=nullptr;
    std::string ignored;
    if(!api.current(backend_index,context,ignored)) return;
    auto& state=cuda_resources();
    std::lock_guard lock(state.mutex);
    cuda_ensure_resource_slots(state,backend_index);
    auto out=state.pending_device_blocks.begin();
    for(auto it=state.pending_device_blocks.begin();
        it!=state.pending_device_blocks.end();++it){
        if(it->backend_index!=backend_index){
            *out++=std::move(*it);
            continue;
        }
        const bool ready=completed||
            (api.event_query&&it->event&&api.event_query(it->event)==0);
        if(!ready){
            *out++=std::move(*it);
            continue;
        }
        if(it->event&&api.event_destroy)
            (void)api.event_destroy(it->event);
        state.free_device_blocks[static_cast<std::size_t>(backend_index)]
            .push_back(it->block);
    }
    state.pending_device_blocks.erase(out,state.pending_device_blocks.end());
    if(completed){
        auto& retired=
            state.retired_device_blocks[static_cast<std::size_t>(backend_index)];
        auto& free=
            state.free_device_blocks[static_cast<std::size_t>(backend_index)];
        free.insert(free.end(),
                    std::make_move_iterator(retired.begin()),
                    std::make_move_iterator(retired.end()));
        retired.clear();
    }
}

void cuda_release_free_device_blocks(CudaApi& api,int backend_index) {
    CudaApi::CUcontext context=nullptr;
    std::string ignored;
    if(!api.current(backend_index,context,ignored)||!api.mem_free) return;
    std::vector<CudaResourceState::DeviceBlock> releasable;
    {
        auto& state=cuda_resources();
        std::lock_guard lock(state.mutex);
        cuda_ensure_resource_slots(state,backend_index);
        releasable.swap(
            state.free_device_blocks[static_cast<std::size_t>(backend_index)]);
    }
    for(const auto& block:releasable)
        if(block.pointer)(void)api.mem_free(block.pointer);
}

bool cuda_take_device_block(int backend_index,std::size_t bytes,
                            CudaApi::CUdeviceptr& pointer) {
    auto& api=cuda();
    cuda_reap_device_blocks(api,backend_index,false);
    auto& state=cuda_resources();
    std::lock_guard lock(state.mutex);
    cuda_ensure_resource_slots(state,backend_index);
    auto& blocks=state.free_device_blocks[static_cast<std::size_t>(backend_index)];
    auto best=blocks.end();
    for(auto it=blocks.begin();it!=blocks.end();++it)
        if(it->bytes>=bytes&&(best==blocks.end()||it->bytes<best->bytes))
            best=it;
    if(best==blocks.end()) return false;
    pointer=best->pointer;
    blocks.erase(best);
    return true;
}

void cuda_return_device_block(int backend_index,std::size_t bytes,
                              CudaApi::CUdeviceptr pointer) {
    if(!pointer) return;
    auto& api=cuda();
    CudaApi::CUcontext context=nullptr;
    std::string ignored;
    CudaApi::CUevent event=nullptr;
    constexpr unsigned disable_timing=2;
    const bool can_track=
        api.current(backend_index,context,ignored)&&
        api.event_create&&api.event_record&&api.event_query&&api.event_destroy&&
        api.event_create(&event,disable_timing)==0&&event&&
        api.event_record(event,nullptr)==0;

    auto& state=cuda_resources();
    std::lock_guard lock(state.mutex);
    cuda_ensure_resource_slots(state,backend_index);
    const CudaResourceState::DeviceBlock block{
        std::max<std::size_t>(bytes,1),pointer};
    if(can_track){
        state.pending_device_blocks.push_back(
            CudaResourceState::PendingDevice{backend_index,event,block});
    }else{
        if(event&&api.event_destroy)(void)api.event_destroy(event);
        // No hidden wait on destruction. Without an event, explicit/final
        // synchronization is the earliest safe point at which this block can
        // re-enter the allocation pool.
        state.retired_device_blocks[static_cast<std::size_t>(backend_index)]
            .push_back(block);
    }
}

void cuda_retire_module(int backend_index,CudaApi::CUmodule module) {
    if(!module) return;
    auto& state=cuda_resources();
    std::lock_guard lock(state.mutex);
    cuda_ensure_resource_slots(state,backend_index);
    state.retired_modules[static_cast<std::size_t>(backend_index)].push_back(module);
}

void* cuda_take_host_block(CudaApi& api,int backend_index,std::size_t bytes,
                           std::size_t& capacity,std::string& error) {
    cuda_reap_uploads(api,backend_index,false);
    auto& state=cuda_resources();
    {
        std::lock_guard lock(state.mutex);
        cuda_ensure_resource_slots(state,backend_index);
        auto& blocks=state.free_host_blocks[static_cast<std::size_t>(backend_index)];
        auto best=blocks.end();
        for(auto it=blocks.begin();it!=blocks.end();++it)
            if(it->bytes>=bytes&&(best==blocks.end()||it->bytes<best->bytes))
                best=it;
        if(best!=blocks.end()){
            void* pointer=best->pointer;
            capacity=best->bytes;
            blocks.erase(best);
            return pointer;
        }
    }
    if(!api.host_alloc){
        error="CUDA pinned host allocation API is unavailable";
        return nullptr;
    }
    const auto allocation_bytes=std::max<std::size_t>(bytes,1);
    void* pointer=nullptr;
    if(api.host_alloc(&pointer,allocation_bytes,0)!=0||!pointer){
        error="CUDA pinned upload staging allocation failed";
        return nullptr;
    }
    capacity=allocation_bytes;
    return pointer;
}

void cuda_return_host_block(int backend_index,std::size_t bytes,void* pointer) {
    if(!pointer) return;
    auto& state=cuda_resources();
    std::lock_guard lock(state.mutex);
    cuda_ensure_resource_slots(state,backend_index);
    state.free_host_blocks[static_cast<std::size_t>(backend_index)]
        .push_back({std::max<std::size_t>(bytes,1),pointer});
}

void cuda_retire_host_block(int backend_index,std::size_t bytes,void* pointer) {
    if(!pointer) return;
    auto& state=cuda_resources();
    std::lock_guard lock(state.mutex);
    cuda_ensure_resource_slots(state,backend_index);
    state.retired_host_blocks[static_cast<std::size_t>(backend_index)]
        .push_back({std::max<std::size_t>(bytes,1),pointer});
}

bool cuda_copy_from_host_async(CudaApi& api,int backend_index,
                               CudaApi::CUdeviceptr destination,
                               const void* source,std::size_t bytes,
                               std::string& error) {
    if(!api.copy_h2d_async||!api.host_alloc){
        error="NVIDIA asynchronous upload support is unavailable";
        return false;
    }
    std::size_t staging_capacity=0;
    void* staging=cuda_take_host_block(
        api,backend_index,bytes,staging_capacity,error);
    if(!staging) return false;
    std::memcpy(staging,source,bytes);

    CudaApi::CUevent event=nullptr;
    constexpr unsigned disable_timing=2;
    const bool can_track=api.event_create&&api.event_record&&
                         api.event_query&&api.event_destroy&&
                         api.event_create(&event,disable_timing)==0&&event;
    if(api.copy_h2d_async(destination,staging,bytes,nullptr)!=0){
        if(event&&api.event_destroy)(void)api.event_destroy(event);
        cuda_return_host_block(backend_index,staging_capacity,staging);
        error="NVIDIA asynchronous GPU upload failed";
        return false;
    }
    if(!can_track||api.event_record(event,nullptr)!=0){
        if(event&&api.event_destroy)(void)api.event_destroy(event);
        // The copy is already queued. Never insert a hidden host wait merely to
        // reclaim staging memory; keep it alive until final GPU shutdown.
        cuda_retire_host_block(backend_index,staging_capacity,staging);
        return true;
    }
    auto& state=cuda_resources();
    std::lock_guard lock(state.mutex);
    cuda_ensure_resource_slots(state,backend_index);
    state.pending_uploads.push_back(
        CudaResourceState::PendingUpload{
            backend_index,event,{staging_capacity,staging}});
    return true;
}


struct HipApi {
    using Module = void*;
    using Function = void*;
    using Stream = void*;
    using Event = void*;
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
    int (*memcpy_async)(void*, const void*, std::size_t, int, Stream){};
    int (*memset_async)(void*, int, std::size_t, Stream){};
    int (*host_malloc)(void**, std::size_t, unsigned){};
    int (*host_free)(void*){};
    int (*event_create)(Event*){};
    int (*event_record)(Event, Stream){};
    int (*event_query)(Event){};
    int (*event_synchronize)(Event){};
    int (*event_destroy)(Event){};
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

    std::mutex active_mutex;
    std::vector<bool> active_devices;
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
        memcpy_async =
            load_symbol<decltype(memcpy_async)>(library, "hipMemcpyAsync");
        memset_async =
            load_symbol<decltype(memset_async)>(library, "hipMemsetAsync");
        host_malloc =
            load_symbol<decltype(host_malloc)>(library, "hipHostMalloc");
        host_free = load_symbol<decltype(host_free)>(library, "hipHostFree");
        event_create =
            load_symbol<decltype(event_create)>(library, "hipEventCreate");
        event_record =
            load_symbol<decltype(event_record)>(library, "hipEventRecord");
        event_query =
            load_symbol<decltype(event_query)>(library, "hipEventQuery");
        event_synchronize =
            load_symbol<decltype(event_synchronize)>(library, "hipEventSynchronize");
        event_destroy =
            load_symbol<decltype(event_destroy)>(library, "hipEventDestroy");
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

    void mark_active(int backend_index) {
        if (backend_index < 0) return;
        std::lock_guard lock(active_mutex);
        const auto index=static_cast<std::size_t>(backend_index);
        if(active_devices.size()<=index) active_devices.resize(index+1,false);
        active_devices[index]=true;
    }

    bool is_active(int backend_index) {
        if (backend_index < 0) return false;
        std::lock_guard lock(active_mutex);
        const auto index=static_cast<std::size_t>(backend_index);
        return index<active_devices.size()&&active_devices[index];
    }
};

HipApi& hip() {
    static HipApi api;
    return api;
}

struct HipResourceState {
    struct DeviceBlock { std::size_t bytes{}; void* pointer{}; };
    struct HostBlock { std::size_t bytes{}; void* pointer{}; };
    struct PendingUpload {
        int backend_index{-1};
        HipApi::Event event{};
        HostBlock block;
    };
    struct PendingDevice {
        int backend_index{-1};
        HipApi::Event event{};
        DeviceBlock block;
    };

    std::mutex mutex;
    std::vector<std::vector<DeviceBlock>> free_device_blocks;
    std::vector<std::vector<DeviceBlock>> retired_device_blocks;
    std::vector<std::vector<HostBlock>> free_host_blocks;
    std::vector<std::vector<HostBlock>> retired_host_blocks;
    std::vector<PendingUpload> pending_uploads;
    std::vector<PendingDevice> pending_device_blocks;
    std::vector<std::vector<HipApi::Module>> retired_modules;

    ~HipResourceState() {
        auto& api=hip();
        int count=0;
        if(!api.get_count||api.get_count(&count)!=0) count=0;
        for(int index=0;index<count;++index)
            if(api.set_device&&api.set_device(index)==0&&api.device_synchronize)
                (void)api.device_synchronize();

        std::lock_guard lock(mutex);
        for(auto& pending:pending_uploads){
            if(pending.event&&api.event_destroy)
                (void)api.event_destroy(pending.event);
            if(pending.block.pointer&&api.host_free)
                (void)api.host_free(pending.block.pointer);
        }
        pending_uploads.clear();
        for(auto& pending:pending_device_blocks){
            if(pending.event&&api.event_destroy)
                (void)api.event_destroy(pending.event);
            if(pending.backend_index>=0){
                const auto index=static_cast<std::size_t>(pending.backend_index);
                if(free_device_blocks.size()<=index)
                    free_device_blocks.resize(index+1);
                free_device_blocks[index].push_back(pending.block);
            }
        }
        pending_device_blocks.clear();
        for(auto& blocks:free_host_blocks)
            for(auto& block:blocks)
                if(block.pointer&&api.host_free)
                    (void)api.host_free(block.pointer);
        for(auto& blocks:retired_host_blocks)
            for(auto& block:blocks)
                if(block.pointer&&api.host_free)
                    (void)api.host_free(block.pointer);
        const auto device_slots=
            std::max(free_device_blocks.size(),retired_device_blocks.size());
        for(std::size_t index=0;index<device_slots;++index){
            if(!api.set_device||api.set_device(static_cast<int>(index))!=0) continue;
            if(index<free_device_blocks.size())
                for(auto& block:free_device_blocks[index])
                    if(block.pointer&&api.free_fn)
                        (void)api.free_fn(block.pointer);
            if(index<retired_device_blocks.size())
                for(auto& block:retired_device_blocks[index])
                    if(block.pointer&&api.free_fn)
                        (void)api.free_fn(block.pointer);
        }
        for(std::size_t index=0;index<retired_modules.size();++index){
            if(!api.set_device||api.set_device(static_cast<int>(index))!=0) continue;
            for(auto module:retired_modules[index])
                if(module&&api.module_unload)
                    (void)api.module_unload(module);
        }
    }
};

HipResourceState& hip_resources() {
    static HipResourceState state;
    return state;
}

void hip_ensure_resource_slots(HipResourceState& state,int backend_index) {
    const auto count=static_cast<std::size_t>(backend_index+1);
    if(state.free_device_blocks.size()<count)
        state.free_device_blocks.resize(count);
    if(state.retired_device_blocks.size()<count)
        state.retired_device_blocks.resize(count);
    if(state.free_host_blocks.size()<count)
        state.free_host_blocks.resize(count);
    if(state.retired_host_blocks.size()<count)
        state.retired_host_blocks.resize(count);
    if(state.retired_modules.size()<count)
        state.retired_modules.resize(count);
}

void hip_reap_uploads(HipApi& api,int backend_index,bool completed) {
    auto& state=hip_resources();
    std::lock_guard lock(state.mutex);
    hip_ensure_resource_slots(state,backend_index);
    auto out=state.pending_uploads.begin();
    for(auto it=state.pending_uploads.begin();it!=state.pending_uploads.end();++it){
        if(it->backend_index!=backend_index){
            *out++=std::move(*it);
            continue;
        }
        const bool ready=completed||
            (api.event_query&&it->event&&api.event_query(it->event)==0);
        if(!ready){
            *out++=std::move(*it);
            continue;
        }
        if(it->event&&api.event_destroy)
            (void)api.event_destroy(it->event);
        state.free_host_blocks[static_cast<std::size_t>(backend_index)]
            .push_back(it->block);
    }
    state.pending_uploads.erase(out,state.pending_uploads.end());
    if(completed){
        auto& retired=
            state.retired_host_blocks[static_cast<std::size_t>(backend_index)];
        auto& free=
            state.free_host_blocks[static_cast<std::size_t>(backend_index)];
        free.insert(free.end(),
                    std::make_move_iterator(retired.begin()),
                    std::make_move_iterator(retired.end()));
        retired.clear();
    }
}

void hip_reap_device_blocks(HipApi& api,int backend_index,bool completed) {
    if(!api.set_device||api.set_device(backend_index)!=0) return;
    auto& state=hip_resources();
    std::lock_guard lock(state.mutex);
    hip_ensure_resource_slots(state,backend_index);
    auto out=state.pending_device_blocks.begin();
    for(auto it=state.pending_device_blocks.begin();
        it!=state.pending_device_blocks.end();++it){
        if(it->backend_index!=backend_index){
            *out++=std::move(*it);
            continue;
        }
        const bool ready=completed||
            (api.event_query&&it->event&&api.event_query(it->event)==0);
        if(!ready){
            *out++=std::move(*it);
            continue;
        }
        if(it->event&&api.event_destroy)
            (void)api.event_destroy(it->event);
        state.free_device_blocks[static_cast<std::size_t>(backend_index)]
            .push_back(it->block);
    }
    state.pending_device_blocks.erase(out,state.pending_device_blocks.end());
    if(completed){
        auto& retired=
            state.retired_device_blocks[static_cast<std::size_t>(backend_index)];
        auto& free=
            state.free_device_blocks[static_cast<std::size_t>(backend_index)];
        free.insert(free.end(),
                    std::make_move_iterator(retired.begin()),
                    std::make_move_iterator(retired.end()));
        retired.clear();
    }
}

void hip_release_free_device_blocks(HipApi& api,int backend_index) {
    if(!api.set_device||api.set_device(backend_index)!=0||!api.free_fn) return;
    std::vector<HipResourceState::DeviceBlock> releasable;
    {
        auto& state=hip_resources();
        std::lock_guard lock(state.mutex);
        hip_ensure_resource_slots(state,backend_index);
        releasable.swap(
            state.free_device_blocks[static_cast<std::size_t>(backend_index)]);
    }
    for(const auto& block:releasable)
        if(block.pointer)(void)api.free_fn(block.pointer);
}

bool hip_take_device_block(int backend_index,std::size_t bytes,void*& pointer) {
    auto& api=hip();
    hip_reap_device_blocks(api,backend_index,false);
    auto& state=hip_resources();
    std::lock_guard lock(state.mutex);
    hip_ensure_resource_slots(state,backend_index);
    auto& blocks=state.free_device_blocks[static_cast<std::size_t>(backend_index)];
    auto best=blocks.end();
    for(auto it=blocks.begin();it!=blocks.end();++it)
        if(it->bytes>=bytes&&(best==blocks.end()||it->bytes<best->bytes))
            best=it;
    if(best==blocks.end()) return false;
    pointer=best->pointer;
    blocks.erase(best);
    return true;
}

void hip_return_device_block(int backend_index,std::size_t bytes,void* pointer) {
    if(!pointer) return;
    auto& api=hip();
    HipApi::Event event=nullptr;
    const bool can_track=
        api.set_device&&api.set_device(backend_index)==0&&
        api.event_create&&api.event_record&&api.event_query&&api.event_destroy&&
        api.event_create(&event)==0&&event&&api.event_record(event,nullptr)==0;

    auto& state=hip_resources();
    std::lock_guard lock(state.mutex);
    hip_ensure_resource_slots(state,backend_index);
    const HipResourceState::DeviceBlock block{
        std::max<std::size_t>(bytes,1),pointer};
    if(can_track){
        state.pending_device_blocks.push_back(
            HipResourceState::PendingDevice{backend_index,event,block});
    }else{
        if(event&&api.event_destroy)(void)api.event_destroy(event);
        state.retired_device_blocks[static_cast<std::size_t>(backend_index)]
            .push_back(block);
    }
}

void hip_retire_module(int backend_index,HipApi::Module module) {
    if(!module) return;
    auto& state=hip_resources();
    std::lock_guard lock(state.mutex);
    hip_ensure_resource_slots(state,backend_index);
    state.retired_modules[static_cast<std::size_t>(backend_index)].push_back(module);
}

void* hip_take_host_block(HipApi& api,int backend_index,std::size_t bytes,
                          std::size_t& capacity,std::string& error) {
    hip_reap_uploads(api,backend_index,false);
    auto& state=hip_resources();
    {
        std::lock_guard lock(state.mutex);
        hip_ensure_resource_slots(state,backend_index);
        auto& blocks=state.free_host_blocks[static_cast<std::size_t>(backend_index)];
        auto best=blocks.end();
        for(auto it=blocks.begin();it!=blocks.end();++it)
            if(it->bytes>=bytes&&(best==blocks.end()||it->bytes<best->bytes))
                best=it;
        if(best!=blocks.end()){
            void* pointer=best->pointer;
            capacity=best->bytes;
            blocks.erase(best);
            return pointer;
        }
    }
    if(!api.host_malloc){
        error="HIP pinned host allocation API is unavailable";
        return nullptr;
    }
    const auto allocation_bytes=std::max<std::size_t>(bytes,1);
    void* pointer=nullptr;
    if(api.host_malloc(&pointer,allocation_bytes,0)!=0||!pointer){
        error="HIP pinned upload staging allocation failed";
        return nullptr;
    }
    capacity=allocation_bytes;
    return pointer;
}

void hip_return_host_block(int backend_index,std::size_t bytes,void* pointer) {
    if(!pointer) return;
    auto& state=hip_resources();
    std::lock_guard lock(state.mutex);
    hip_ensure_resource_slots(state,backend_index);
    state.free_host_blocks[static_cast<std::size_t>(backend_index)]
        .push_back({std::max<std::size_t>(bytes,1),pointer});
}

void hip_retire_host_block(int backend_index,std::size_t bytes,void* pointer) {
    if(!pointer) return;
    auto& state=hip_resources();
    std::lock_guard lock(state.mutex);
    hip_ensure_resource_slots(state,backend_index);
    state.retired_host_blocks[static_cast<std::size_t>(backend_index)]
        .push_back({std::max<std::size_t>(bytes,1),pointer});
}

bool hip_copy_from_host_async(HipApi& api,int backend_index,
                              void* destination,const void* source,
                              std::size_t bytes,std::string& error) {
    if(!api.memcpy_async||!api.host_malloc){
        error="AMD asynchronous upload support is unavailable";
        return false;
    }
    std::size_t staging_capacity=0;
    void* staging=hip_take_host_block(
        api,backend_index,bytes,staging_capacity,error);
    if(!staging) return false;
    std::memcpy(staging,source,bytes);

    HipApi::Event event=nullptr;
    const bool can_track=api.event_create&&api.event_record&&
                         api.event_query&&api.event_destroy&&
                         api.event_create(&event)==0&&event;
    if(api.memcpy_async(destination,staging,bytes,1,nullptr)!=0){
        if(event&&api.event_destroy)(void)api.event_destroy(event);
        hip_return_host_block(backend_index,staging_capacity,staging);
        error="AMD asynchronous GPU upload failed";
        return false;
    }
    if(!can_track||api.event_record(event,nullptr)!=0){
        if(event&&api.event_destroy)(void)api.event_destroy(event);
        hip_retire_host_block(backend_index,staging_capacity,staging);
        return true;
    }
    auto& state=hip_resources();
    std::lock_guard lock(state.mutex);
    hip_ensure_resource_slots(state,backend_index);
    state.pending_uploads.push_back(
        HipResourceState::PendingUpload{
            backend_index,event,{staging_capacity,staging}});
    return true;
}

constexpr std::size_t no_validation_page=static_cast<std::size_t>(-1);
constexpr std::size_t validation_slots_per_page=4096;
constexpr std::size_t validation_page_bytes=
    validation_slots_per_page*sizeof(std::uint32_t);

struct BufferImpl {
    Backend backend{Backend::Cuda};
    int global_index{-1};
    int backend_index{-1};
    std::size_t bytes{};
    // Validation views borrow one uint32 slot from a page owned by the
    // deferred-validation arena. They never own the underlying device storage.
    std::size_t validation_page{no_validation_page};
    std::size_t validation_slot{};
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
    Backend backend{Backend::Cuda};
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
                    static_cast<int>(result.size()), Backend::Cuda, i, name,
                    cuda_version_string(driver), "CUDA Driver API"});
            }
        }
    }

    auto& h = hip();
    if (h.ready) {
        int count = 0;
        if (h.get_count(&count) == 0) {
            int hip_runtime_version = 0;
            if (h.runtime_version) (void)h.runtime_version(&hip_runtime_version);
            for (int i = 0; i < count; ++i) {
                char name[256]{};
                if (!h.get_name ||
                    h.get_name(name, static_cast<int>(sizeof(name)), i) != 0)
                    std::memcpy(name, "AMD GPU", sizeof("AMD GPU"));
                const auto runtime = hip_runtime_version > 0
                    ? "HIP " + std::to_string(hip_runtime_version / 10000000) + "." +
                          std::to_string((hip_runtime_version / 100000) % 100)
                    : "HIP";
                result.push_back(Info{
                    static_cast<int>(result.size()), Backend::Hip, i, name,
                    "AMD GPU driver", runtime});
            }
        }
    }
#endif
    return result;
}

std::atomic<int> execution_mode_value{static_cast<int>(ExecutionMode::Fast)};
std::mutex gpu_usage_mutex;
std::vector<int> used_gpu_indices;

void mark_gpu_used(int index) {
    std::lock_guard lock(gpu_usage_mutex);
    if (std::find(used_gpu_indices.begin(), used_gpu_indices.end(), index) ==
        used_gpu_indices.end()) {
        used_gpu_indices.push_back(index);
    }
}

std::vector<int> used_gpu_indices_snapshot() {
    std::lock_guard lock(gpu_usage_mutex);
    return used_gpu_indices;
}

bool range_ok(const BufferImpl& buffer, std::size_t offset, std::size_t bytes) {
    return offset <= buffer.bytes && bytes <= buffer.bytes - offset;
}

} // namespace

struct Buffer : BufferImpl {};
struct Module : ModuleImpl {};

namespace {
struct ValidationPage {
    int global_index{-1};
    Buffer* storage{};
    std::size_t used{};
    std::size_t consumed{};
    std::size_t abandoned{};
};
struct DeferredValidation {
    int global_index{-1};
    Buffer* status{};
    std::size_t page{no_validation_page};
    std::size_t slot{};
    std::string message;
};
struct DeferredValidationState {
    std::mutex mutex;
    std::vector<DeferredValidation> pending;
    std::vector<ValidationPage> pages;
};
DeferredValidationState& deferred_validation_state() {
    static auto* state=new DeferredValidationState;
    return *state;
}

Buffer* acquire_validation_status(int global_index,std::string& error) {
    const auto* info=find(global_index);
    if(!info){
        error="GPU validation requested an unavailable device";
        return nullptr;
    }
    // Metal keeps its existing per-operation status buffer because MTLBuffer
    // bindings do not expose a cheap portable sub-buffer view. CUDA/HIP use
    // shared pages, which removes the allocation storm on long async runs.
    if(info->backend!=Backend::Cuda&&info->backend!=Backend::Hip){
        auto* status=allocate(global_index,sizeof(std::uint32_t),error);
        if(!status) return nullptr;
        if(!zero(status,0,sizeof(std::uint32_t),error)){
            release(status);
            return nullptr;
        }
        return status;
    }

    auto& state=deferred_validation_state();
    std::lock_guard lock(state.mutex);
    std::size_t page_index=no_validation_page;
    for(std::size_t i=0;i<state.pages.size();++i){
        const auto& page=state.pages[i];
        if(page.global_index==global_index&&
           page.used<validation_slots_per_page){
            page_index=i;
            break;
        }
    }
    if(page_index==no_validation_page){
        auto* storage=allocate(global_index,validation_page_bytes,error);
        if(!storage) return nullptr;
        if(!zero(storage,0,validation_page_bytes,error)){
            release(storage);
            return nullptr;
        }
        page_index=state.pages.size();
        state.pages.push_back(ValidationPage{global_index,storage,0,0,0});
    }
    auto& page=state.pages[page_index];
    const auto slot=page.used++;
    const auto offset=slot*sizeof(std::uint32_t);
    auto* view=new Buffer;
    view->backend=page.storage->backend;
    view->global_index=page.storage->global_index;
    view->backend_index=page.storage->backend_index;
    view->bytes=sizeof(std::uint32_t);
    view->validation_page=page_index;
    view->validation_slot=slot;
    view->cuda_context=page.storage->cuda_context;
    if(view->backend==Backend::Cuda)
        view->cuda_pointer=page.storage->cuda_pointer+offset;
    else
        view->pointer=
            static_cast<unsigned char*>(page.storage->pointer)+offset;
    return view;
}

void abandon_validation_view(const Buffer& view) {
    if(view.validation_page==no_validation_page) return;
    auto& state=deferred_validation_state();
    std::lock_guard lock(state.mutex);
    if(view.validation_page<state.pages.size())
        ++state.pages[view.validation_page].abandoned;
}

bool raw_validation_synchronize(Buffer* status,std::string& error) {
    if(!status) return true;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if(status->backend==Backend::Test) return true;
#endif
    if(status->backend==Backend::Cuda){
        auto& api=cuda();
        CudaApi::CUcontext context=nullptr;
        if(!api.current(status->backend_index,context,error)) return false;
        if(!api.ctx_synchronize||api.ctx_synchronize()!=0){
            error="failed to synchronize NVIDIA GPU";
            return false;
        }
        cuda_reap_uploads(api,status->backend_index,true);
        cuda_reap_device_blocks(api,status->backend_index,true);
        return true;
    }
    if(status->backend==Backend::Hip){
        auto& api=hip();
        if(!api.device_synchronize||!api.set_device||
           api.set_device(status->backend_index)!=0||
           api.device_synchronize()!=0){
            error="failed to synchronize AMD GPU";
            return false;
        }
        hip_reap_uploads(api,status->backend_index,true);
        hip_reap_device_blocks(api,status->backend_index,true);
        return true;
    }
#ifdef __APPLE__
    if(status->backend==Backend::Metal)
        return synchronize_metal_backend(status->backend_index,error);
#endif
    error="GPU validation synchronization is unavailable";
    return false;
}
bool raw_validation_copy_bytes(const Buffer* status,void* destination,
                               std::size_t bytes,std::string& error) {
    if(!status||!destination||bytes>status->bytes){
        error="missing deferred GPU validation status";
        return false;
    }
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if(status->backend==Backend::Test){
        std::memcpy(destination,status->test_data.data(),bytes);
        return true;
    }
#endif
    if(status->backend==Backend::Cuda){
        auto& api=cuda();
        CudaApi::CUcontext context=nullptr;
        if(!api.current(status->backend_index,context,error)) return false;
        if(!api.copy_d2h||
           api.copy_d2h(destination,status->cuda_pointer,bytes)!=0){
            error="failed to read deferred NVIDIA GPU validation status";
            return false;
        }
        return true;
    }
    if(status->backend==Backend::Hip){
        auto& api=hip();
        if(!api.ready||!api.set_device||
           api.set_device(status->backend_index)!=0||!api.memcpy_fn||
           api.memcpy_fn(destination,status->pointer,bytes,2)!=0){
            error="failed to read deferred AMD GPU validation status";
            return false;
        }
        return true;
    }
#ifdef __APPLE__
    if(status->backend==Backend::Metal){
        std::memcpy(destination,[status->metal_buffer contents],bytes);
        return true;
    }
#endif
    error="GPU validation status read is unavailable";
    return false;
}
bool consume_deferred_validations(int global_index,std::string& error) {
    std::vector<DeferredValidation> local;
    {
        auto& state=deferred_validation_state();
        std::lock_guard lock(state.mutex);
        auto out=state.pending.begin();
        for(auto it=state.pending.begin();it!=state.pending.end();++it){
            if(it->global_index==global_index) local.push_back(std::move(*it));
            else *out++=std::move(*it);
        }
        state.pending.erase(out,state.pending.end());
    }

    std::unordered_map<std::size_t,std::size_t> page_max_slot;
    for(const auto& validation:local)
        if(validation.page!=no_validation_page){
            auto [it,inserted]=page_max_slot.emplace(
                validation.page,validation.slot);
            if(!inserted) it->second=std::max(it->second,validation.slot);
        }

    std::unordered_map<std::size_t,std::vector<std::uint32_t>> page_values;
    std::string first_error;
    for(const auto& [page_index,max_slot]:page_max_slot){
        Buffer* storage=nullptr;
        {
            auto& state=deferred_validation_state();
            std::lock_guard lock(state.mutex);
            if(page_index<state.pages.size())
                storage=state.pages[page_index].storage;
        }
        if(!storage){
            if(first_error.empty())
                first_error="missing deferred GPU validation page";
            continue;
        }
        auto& values=page_values[page_index];
        values.resize(max_slot+1);
        std::string read_error;
        if(!raw_validation_copy_bytes(
               storage,values.data(),values.size()*sizeof(std::uint32_t),
               read_error)&&first_error.empty())
            first_error=std::move(read_error);
    }

    for(auto& validation:local){
        std::uint32_t failed=0;
        std::string read_error;
        if(validation.page!=no_validation_page){
            const auto found=page_values.find(validation.page);
            if(found==page_values.end()||validation.slot>=found->second.size()){
                if(first_error.empty())
                    first_error="missing deferred GPU validation slot";
            }else{
                failed=found->second[validation.slot];
            }
        }else if(!raw_validation_copy_bytes(
                    validation.status,&failed,sizeof(failed),read_error)){
            if(first_error.empty()) first_error=std::move(read_error);
        }
        if(failed!=0&&first_error.empty())
            first_error=validation.message;
        if(validation.page==no_validation_page){
            release(validation.status);
            validation.status=nullptr;
        }
    }

    // Mark consumed arena slots and recycle a whole page only when every
    // acquired slot is either consumed or was abandoned by a failed launch.
    // The state lock stays held while zeroing so a new submit cannot acquire a
    // recycled slot before the clearing operation has been enqueued.
    {
        auto& state=deferred_validation_state();
        std::lock_guard lock(state.mutex);
        for(const auto& validation:local)
            if(validation.page!=no_validation_page&&
               validation.page<state.pages.size())
                ++state.pages[validation.page].consumed;
        for(auto& page:state.pages){
            if(page.global_index!=global_index||page.used==0||
               page.used!=page.consumed+page.abandoned)
                continue;
            std::string zero_error;
            if(!zero(page.storage,0,validation_page_bytes,zero_error)){
                if(first_error.empty()) first_error=std::move(zero_error);
                continue;
            }
            page.used=0;
            page.consumed=0;
            page.abandoned=0;
        }
    }

    if(!first_error.empty()){
        error=std::move(first_error);
        return false;
    }
    return true;
}
struct DeferredValidationExitGuard {
    ~DeferredValidationExitGuard() {
        std::vector<int> indices;
        {
            auto& state=deferred_validation_state();
            std::lock_guard lock(state.mutex);
            for(const auto& validation:state.pending)
                if(std::find(indices.begin(),indices.end(),validation.global_index)==indices.end())
                    indices.push_back(validation.global_index);
        }
        std::string first_error;
        for(int index:indices){
            Buffer* sample=nullptr;
            {
                auto& state=deferred_validation_state();
                std::lock_guard lock(state.mutex);
                for(const auto& validation:state.pending){
                    if(validation.global_index!=index) continue;
                    sample=validation.page==no_validation_page
                        ?validation.status
                        :(validation.page<state.pages.size()
                            ?state.pages[validation.page].storage:nullptr);
                    break;
                }
            }
            std::string sync_error;
            if(sample&&!raw_validation_synchronize(sample,sync_error)){
                if(first_error.empty()) first_error=std::move(sync_error);
                continue;
            }
            std::string validation_error;
            if(!consume_deferred_validations(index,validation_error)&&first_error.empty())
                first_error=std::move(validation_error);
        }
        if(!first_error.empty()){
            std::fprintf(stderr,"Quidra runtime error[GPU_ASYNC]: %s\n",first_error.c_str());
            std::fflush(stderr);
            std::_Exit(101);
        }

        std::vector<Buffer*> pages;
        {
            auto& state=deferred_validation_state();
            std::lock_guard lock(state.mutex);
            for(auto& page:state.pages)
                if(page.storage) pages.push_back(page.storage);
            state.pages.clear();
        }
        for(auto* page:pages) release(page);
    }
};
void defer_validation(Buffer* status,std::string message) {
    if(!status) return;
    static DeferredValidationExitGuard exit_guard;
    (void)exit_guard;
    auto& state=deferred_validation_state();
    if(status->validation_page!=no_validation_page){
        const auto page=status->validation_page;
        const auto slot=status->validation_slot;
        const auto global_index=status->global_index;
        {
            std::lock_guard lock(state.mutex);
            state.pending.push_back(
                DeferredValidation{
                    global_index,nullptr,page,slot,std::move(message)});
        }
        // The tiny Buffer is only a non-owning view into the page.
        delete status;
        return;
    }
    std::lock_guard lock(state.mutex);
    state.pending.push_back(
        DeferredValidation{
            status->global_index,status,no_validation_page,0,std::move(message)});
}
} // namespace

const std::vector<Info>& devices() {
    static const std::vector<Info> value = enumerate_devices();
    return value;
}

const Info* find(int index) {
    const auto& all = devices();
    if (index < 0 || static_cast<std::size_t>(index) >= all.size()) return nullptr;
    return &all[static_cast<std::size_t>(index)];
}

bool synchronize(int index, std::string& error) {
    const auto* info = find(index);
    if (!info) {
        error = "gpu(" + std::to_string(index) + ") is not available";
        return false;
    }
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if (info->backend == Backend::Test) {
        if (std::getenv("QUIDRA_TEST_FAKE_GPU_SYNC_FAIL")) {
            error = "test-only fake GPU synchronization failure";
            return false;
        }
        if (const char* configured =
                std::getenv("QUIDRA_TEST_FAKE_GPU_SYNC_FAIL_INDEX")) {
            char* end = nullptr;
            const long fail_index = std::strtol(configured, &end, 10);
            if (end != configured && end && *end == '\0' && fail_index == index) {
                error = "test-only fake GPU synchronization failure";
                return false;
            }
        }
        return consume_deferred_validations(index, error);
    }
#endif
    if (info->backend == Backend::Cuda) {
        auto& api = cuda();
        CudaApi::CUcontext context = nullptr;
        bool exists=false;
        if (!api.current_if_created(info->backend_index,context,exists,error))
            return false;
        if (!exists) return true;
        bool synchronized=false;
        if(api.event_create&&api.event_record&&api.event_synchronize&&
           api.event_destroy){
            CudaApi::CUevent marker=nullptr;
            constexpr unsigned disable_timing=2;
            if(api.event_create(&marker,disable_timing)==0&&marker){
                const auto record_status=api.event_record(marker,nullptr);
                const auto wait_status=
                    record_status==0 ? api.event_synchronize(marker) : record_status;
                (void)api.event_destroy(marker);
                if(wait_status==0) synchronized=true;
            }
        }
        if(!synchronized){
            if (!api.ctx_synchronize || api.ctx_synchronize() != 0) {
                error = "failed to synchronize NVIDIA GPU";
                return false;
            }
        }
        cuda_reap_uploads(api,info->backend_index,true);
        cuda_reap_device_blocks(api,info->backend_index,true);
        if(!consume_deferred_validations(index,error)) return false;
        return true;
    }
    if (info->backend == Backend::Hip) {
        auto& api = hip();
        if (!api.is_active(info->backend_index)) return true;
        if(!api.set_device||api.set_device(info->backend_index)!=0){
            error = "failed to synchronize AMD GPU";
            return false;
        }
        bool synchronized=false;
        if(api.event_create&&api.event_record&&api.event_synchronize&&
           api.event_destroy){
            HipApi::Event marker=nullptr;
            if(api.event_create(&marker)==0&&marker){
                const auto record_status=api.event_record(marker,nullptr);
                const auto wait_status=
                    record_status==0 ? api.event_synchronize(marker) : record_status;
                (void)api.event_destroy(marker);
                if(wait_status==0) synchronized=true;
            }
        }
        if(!synchronized){
            if (!api.device_synchronize || api.device_synchronize() != 0) {
                error = "failed to synchronize AMD GPU";
                return false;
            }
        }
        hip_reap_uploads(api,info->backend_index,true);
        hip_reap_device_blocks(api,info->backend_index,true);
        if(!consume_deferred_validations(index,error)) return false;
        return true;
    }
#ifdef __APPLE__
    if (info->backend == Backend::Metal) {
        if(!synchronize_metal_backend(info->backend_index,error)) return false;
        return consume_deferred_validations(index,error);
    }
#endif
    error = "GPU synchronization is unavailable";
    return false;
}

bool synchronize_all(std::string& error) {
    const auto indices = used_gpu_indices_snapshot();
    for (const int index : indices) {
        if (!synchronize(index, error)) return false;
    }
    return true;
}

void set_execution_mode(ExecutionMode mode) {
    execution_mode_value.store(static_cast<int>(mode), std::memory_order_relaxed);
}

ExecutionMode execution_mode() {
    return static_cast<ExecutionMode>(
        execution_mode_value.load(std::memory_order_relaxed));
}

std::string backend_display_name(Backend backend) {
    switch (backend) {
        case Backend::Cuda: return "NVIDIA";
        case Backend::Hip: return "AMD";
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
        mark_gpu_used(index);
        return buffer.release();
    }
#endif

    if (info->backend == Backend::Cuda) {
        auto& api = cuda();
        CudaApi::CUcontext context = nullptr;
        if (!api.current(info->backend_index, context, error)) return nullptr;
        CudaApi::CUdeviceptr pointer = 0;
        if (!cuda_take_device_block(info->backend_index,physical_bytes,pointer)) {
            if (api.mem_alloc(&pointer, physical_bytes) != 0 || pointer == 0) {
                cuda_release_free_device_blocks(api,info->backend_index);
                pointer=0;
                if (api.mem_alloc(&pointer, physical_bytes) != 0 || pointer == 0) {
                    error = "NVIDIA GPU memory allocation failed";
                    return nullptr;
                }
            }
        }
        buffer->cuda_pointer = pointer;
        buffer->cuda_context = context;
        mark_gpu_used(index);
        return buffer.release();
    }

    if (info->backend == Backend::Hip) {
        auto& api = hip();
        if (!api.ready || api.set_device(info->backend_index) != 0) {
            error = "failed to select AMD GPU";
            return nullptr;
        }
        api.mark_active(info->backend_index);
        void* pointer = nullptr;
        if (!hip_take_device_block(info->backend_index,physical_bytes,pointer)) {
            if (api.malloc_fn(&pointer, physical_bytes) != 0 || !pointer) {
                hip_release_free_device_blocks(api,info->backend_index);
                pointer=nullptr;
                if (api.malloc_fn(&pointer, physical_bytes) != 0 || !pointer) {
                    error = "AMD GPU memory allocation failed";
                    return nullptr;
                }
            }
        }
        buffer->pointer = pointer;
        mark_gpu_used(index);
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
            mark_gpu_used(index);
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
    if(buffer->validation_page!=no_validation_page){
        abandon_validation_view(*buffer);
        return;
    }
    if (buffer->backend == Backend::Cuda && buffer->cuda_pointer != 0) {
        cuda_return_device_block(buffer->backend_index,buffer->bytes,
                                 buffer->cuda_pointer);
    } else if (buffer->backend == Backend::Hip && buffer->pointer) {
        hip_return_device_block(buffer->backend_index,buffer->bytes,
                                buffer->pointer);
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
    if (raw->backend == Backend::Cuda) {
        auto& api = cuda();
        CudaApi::CUcontext context = nullptr;
        if (!api.current(raw->backend_index, context, error)) return false;
        return cuda_copy_from_host_async(
            api,raw->backend_index,raw->cuda_pointer+offset,source,bytes,error);
    }
    if (raw->backend == Backend::Hip) {
        auto& api = hip();
        if (!api.ready || api.set_device(raw->backend_index) != 0) {
            error = "failed to select AMD GPU";
            return false;
        }
        return hip_copy_from_host_async(
            api,raw->backend_index,
            static_cast<unsigned char*>(raw->pointer)+offset,
            source,bytes,error);
    }
#ifdef __APPLE__
    if (raw->backend == Backend::Metal)
        return metal_copy_from_host(raw, offset, source, bytes, error);
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
    if (raw->backend == Backend::Cuda) {
        if(!synchronize(raw->global_index,error)) return false;
        auto& api = cuda();
        if (api.copy_d2h(destination, raw->cuda_pointer + offset, bytes) != 0) {
            error = "NVIDIA GPU download failed";
            return false;
        }
        return true;
    }
    if (raw->backend == Backend::Hip) {
        if(!synchronize(raw->global_index,error)) return false;
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
    if (raw->backend == Backend::Metal)
        return metal_copy_to_host(raw, offset, destination, bytes, error);
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

    if (destination->backend == Backend::Cuda) {
        auto& api = cuda();
        CudaApi::CUcontext context = nullptr;
        if (!api.copy_d2d ||
            !api.current(destination->backend_index, context, error)) {
            if (error.empty()) error = "NVIDIA device-to-device copy is unavailable";
            return false;
        }
        if (!api.copy_d2d_async) {
            error = "NVIDIA asynchronous device-to-device copy is unavailable";
            return false;
        }
        if (api.copy_d2d_async(destination->cuda_pointer + destination_offset,
                               source->cuda_pointer + source_offset,
                               bytes,nullptr) != 0) {
            error = "NVIDIA asynchronous device-to-device copy failed";
            return false;
        }
        return true;
    }
    if (destination->backend == Backend::Hip) {
        auto& api = hip();
        if (!api.ready || api.set_device(destination->backend_index) != 0) {
            error = "failed to select AMD GPU";
            return false;
        }
        if (!api.memcpy_async) {
            error = "AMD asynchronous device-to-device copy is unavailable";
            return false;
        }
        if (api.memcpy_async(
                static_cast<unsigned char*>(destination->pointer)+destination_offset,
                static_cast<unsigned char*>(source->pointer)+source_offset,
                bytes,3,nullptr) != 0) {
            error = "AMD asynchronous device-to-device copy failed";
            return false;
        }
        return true;
    }
#ifdef __APPLE__
    if (destination->backend == Backend::Metal)
        return metal_copy_device_to_device(destination, destination_offset,
                                           source, source_offset, bytes, error);
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
    if (raw->backend == Backend::Cuda) {
        auto& api = cuda();
        CudaApi::CUcontext context = nullptr;
        if (!api.current(raw->backend_index, context, error)) return false;
        if (!api.memset_d8_async) {
            error = "NVIDIA asynchronous GPU memset is unavailable";
            return false;
        }
        if (api.memset_d8_async(raw->cuda_pointer+offset,0,bytes,nullptr)!=0) {
            error = "NVIDIA asynchronous GPU memset failed";
            return false;
        }
        return true;
    }
    if (raw->backend == Backend::Hip) {
        auto& api = hip();
        if (!api.ready || api.set_device(raw->backend_index) != 0) {
            error = "failed to select AMD GPU";
            return false;
        }
        if (!api.memset_async) {
            error = "AMD asynchronous GPU memset is unavailable";
            return false;
        }
        if (api.memset_async(
                static_cast<unsigned char*>(raw->pointer)+offset,0,bytes,nullptr)!=0) {
            error = "AMD asynchronous GPU memset failed";
            return false;
        }
        return true;
    }
#ifdef __APPLE__
    if (raw->backend == Backend::Metal)
        return metal_zero(raw, offset, bytes, error);
#endif
    error = "GPU backend memset is unavailable";
    return false;
}

int buffer_device(const Buffer* buffer) {
    return buffer ? buffer->global_index : -1;
}

Backend buffer_backend(const Buffer* buffer) {
    return buffer ? buffer->backend : Backend::Cuda;
}

std::uint64_t buffer_native_handle(const Buffer* buffer) {
    if (!buffer) return 0;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if (buffer->backend == Backend::Test) {
        return reinterpret_cast<std::uint64_t>(
            buffer->test_data.empty() ? nullptr :
            const_cast<unsigned char*>(buffer->test_data.data()));
    }
#endif
    if (buffer->backend == Backend::Cuda) return buffer->cuda_pointer;
    if (buffer->backend == Backend::Hip)
        return reinterpret_cast<std::uint64_t>(buffer->pointer);
#ifdef __APPLE__
    if (buffer->backend == Backend::Metal)
        return reinterpret_cast<std::uint64_t>((__bridge void*)buffer->metal_buffer);
#endif
    return 0;
}

bool activate(int index, std::string& error) {
    const auto* info = find(index);
    if (!info) {
        error = "gpu(" + std::to_string(index) + ") is not available";
        return false;
    }
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if (info->backend == Backend::Test) return true;
#endif
    if (info->backend == Backend::Cuda) {
        CudaApi::CUcontext context = nullptr;
        return cuda().current(info->backend_index, context, error);
    }
    if (info->backend == Backend::Hip) {
        auto& api = hip();
        if (!api.ready || !api.set_device ||
            api.set_device(info->backend_index) != 0) {
            error = "failed to select AMD GPU";
            return false;
        }
        return true;
    }
#ifdef __APPLE__
    if (info->backend == Backend::Metal) return true;
#endif
    error = "GPU backend activation is unavailable";
    return false;
}

Module* load_ptx(int index, const std::string& ptx, std::string& error) {
    const auto* info = find(index);
    if (!info) {
        error = "gpu(" + std::to_string(index) + ") is not available";
        return nullptr;
    }
    if (info->backend != Backend::Cuda) {
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
    result->backend = Backend::Cuda;
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
    if (info->backend != Backend::Hip) {
        error = "HIP source modules are only supported by the AMD backend";
        return nullptr;
    }
    auto& api = hip();
    if (!api.compute_ready || api.set_device(info->backend_index) != 0) {
        error = "AMD HIP runtime compilation/launch API is unavailable";
        return nullptr;
    }
    api.mark_active(info->backend_index);

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
    result->backend = Backend::Hip;
    result->global_index = index;
    result->backend_index = info->backend_index;
    result->hip_module = module;
    return result.release();
}

void release(Module* raw) {
    if (!raw) return;
    std::unique_ptr<Module> module(raw);
    if (module->backend == Backend::Cuda) {
        cuda_retire_module(
            module->backend_index,
            static_cast<CudaApi::CUmodule>(module->cuda_module));
        return;
    }
    if (module->backend == Backend::Hip && module->hip_module) {
        hip_retire_module(
            module->backend_index,
            static_cast<HipApi::Module>(module->hip_module));
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

    if (module->backend == Backend::Cuda) {
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
        // Default-stream ordering preserves GPU dependencies. Host-visible reads and
        // explicit transfers are the synchronization boundaries; blocking here would
        // serialize every queued tensor kernel in a compute chain.
        return true;
    }

    if (module->backend == Backend::Hip) {
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
        // Keep native HIP launches asynchronous for the same reason as CUDA:
        // same-device work is ordered by the default stream and host transfers wait.
        return true;
    }

    error = "kernel module backend is not launchable";
    return false;
}


#include "device_integer_compute.inc"
#include "device_compute.inc"
#include "device_autograd_compute.inc"

} // namespace quidra::device
