#include "device_backend.hpp"
#include "platform/environment.hpp"
#include "quidra/abi/dtype.hpp"
#include "quidra/abi/process_status.hpp"
#include "quidra/abi/tensor_codes.hpp"
#include "runtime_counters.hpp"
#include "quidra/project.hpp"
#include "unified_storage.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <deque>
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
#include <optional>
#include <sstream>
#include <type_traits>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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
// `covered` receives the serial whose completion the synchronization waited
// for (deferred checks queued up to it are readable).
bool synchronize_metal_backend(int backend_index, std::string& error,
                               std::uint64_t* covered = nullptr);
std::uint64_t metal_deferred_check_serial(const Buffer* status);
bool metal_copy_from_host(Buffer* raw, std::size_t offset, const void* source,
                          std::size_t bytes, std::string& error);
bool metal_copy_to_host(const Buffer* raw, std::size_t offset, void* destination,
                        std::size_t bytes, std::uint64_t& covered,
                        std::string& error);
bool metal_copy_device_to_device(Buffer* destination, std::size_t destination_offset,
                                 const Buffer* source, std::size_t source_offset,
                                 std::size_t bytes, std::string& error);
bool metal_zero(Buffer* raw, std::size_t offset, std::size_t bytes,
                std::string& error);
bool metal_clear_status_page(Buffer* page, std::size_t bytes, std::string& error);
struct MetalStream;
MetalStream* metal_stream_for(const Buffer* buffer);
void metal_lend_to_package(MetalStream& stream, const Buffer* buffer);
id<MTLBuffer> metal_acquire_buffer(int backend_index, std::size_t bytes,
                                   bool require_idle, std::size_t& class_bytes,
                                   std::uint64_t& busy_until, std::string& error);
void metal_release_buffer(int backend_index, id<MTLBuffer> buffer,
                          std::size_t class_bytes);
bool metal_copy_to_host_if_idle(const Buffer* raw, std::size_t offset,
                                void* destination, std::size_t bytes);
bool metal_completions_pending(int backend_index);
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
    // Size of the backend allocation (Metal pool size class).
    std::size_t physical_bytes{};
    // Start of this buffer inside its backend allocation. Nonzero for Metal
    // views (paged status words); every binding adds it.
    std::size_t base_offset{};
    // Metal: the buffer owning a view's allocation (null otherwise).
    BufferImpl* owner{};
    // Metal: serial of the last command buffer Core bound this buffer in
    // (0: never), and whether package code received its native handle.
    std::uint64_t last_use{};
    bool exposed{};
    // Validation views borrow one uint32 slot from a page owned by the
    // deferred-validation arena. They never own the underlying device storage.
    std::size_t validation_page{no_validation_page};
    std::size_t validation_slot{};
    void* pointer{};
    std::uint64_t cuda_pointer{};
    void* cuda_context{};
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    // Host bytes of the fake device. They can adopt a wrapped host
    // allocation when the fake backend emulates unified memory.
    quidra::unified::HostBytes test_data;
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
    if (const auto configured = platform::environment_value("QUIDRA_TEST_FAKE_GPU_COUNT")) {
        const char* text = configured->c_str();
        char* end = nullptr;
        const long count = std::strtol(text, &end, 10);
        if (end == text || (end && *end != '\0') || count < 0 || count > 16) {
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

// Package warm-up functions (qcore_register_warmup). One Core thread
// runs every (function, device) pair once, in order: for each device when
// the program first allocates on it, and for a function registered later on
// every device already in use. So a program that never uses a GPU runs
// none, and the compilation they do overlaps the program's host work. The
// thread starts with the first pair; process exit stops it after the
// running call (std::atexit, registered when it starts, so objects created
// before that outlive the call).
using WarmupFunction = void (*)(long long);

struct WarmupState {
    std::mutex mutex;
    std::condition_variable wake;
    std::vector<WarmupFunction> functions;
    std::vector<int> devices;
    std::deque<std::pair<WarmupFunction, int>> queue;
    bool started{};
    bool stopping{};
    std::thread worker;
};

WarmupState& warmup_state() {
    static auto* state = new WarmupState; // never destroyed (exit races)
    return *state;
}

void run_warmups() {
    auto& state = warmup_state();
    std::unique_lock lock(state.mutex);
    for (;;) {
        state.wake.wait(lock, [&] { return state.stopping || !state.queue.empty(); });
        if (state.stopping) return;
        const auto [function, device] = state.queue.front();
        state.queue.pop_front();
        lock.unlock();
        // Package code outside any Quidra statement: a failure in it ends
        // the process at once (exit_failed_program).
        running_package_callback = true;
        try {
#ifdef __APPLE__
            @autoreleasepool {
                function(static_cast<long long>(device));
            }
#else
            function(static_cast<long long>(device));
#endif
        } catch (...) {
        }
        check_package_hold_returned("a package warm-up function returned");
        running_package_callback = false;
        counters::add(counters::Id::PackageWarmups); // qcount
        lock.lock();
    }
}

void stop_warmups() {
    // The warm-up thread may wait for a device stream that the exiting
    // thread still holds for a package encode (a runtime failure that exits
    // directly, in the statement whose extern call left the hold open), and
    // this handler runs before the stream teardown's check: end that hold
    // first, or the join below would wait for good.
    check_package_hold_returned("the program exited");
    auto& state = warmup_state();
    {
        std::lock_guard lock(state.mutex);
        state.stopping = true;
        state.queue.clear();
    }
    state.wake.notify_all();
    // Exit may start on the warm-up thread itself (package code there
    // called exit), which cannot join itself.
    if (state.worker.joinable() &&
        state.worker.get_id() != std::this_thread::get_id())
        state.worker.join();
}

// Caller holds state.mutex.
void queue_warmup_locked(WarmupState& state, WarmupFunction function, int device) {
    if (state.stopping) return;
    state.queue.emplace_back(function, device);
    if (!state.started) {
        state.started = true;
        state.worker = std::thread(run_warmups);
        std::atexit(stop_warmups);
    }
    state.wake.notify_one();
}

void warm_up_device(int index) {
    auto& state = warmup_state();
    std::lock_guard lock(state.mutex);
    if (std::find(state.devices.begin(), state.devices.end(), index) !=
        state.devices.end())
        return;
    state.devices.push_back(index);
    for (const auto function : state.functions)
        queue_warmup_locked(state, function, index);
}

void mark_gpu_used(int index) {
    {
        std::lock_guard lock(gpu_usage_mutex);
        if (std::find(used_gpu_indices.begin(), used_gpu_indices.end(), index) !=
            used_gpu_indices.end())
            return;
        used_gpu_indices.push_back(index);
    }
    warm_up_device(index);
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

bool synchronize_device(int index, bool consume_checks, std::string& error);

namespace {
struct ValidationPage {
    int global_index{-1};
    Buffer* storage{};
    std::size_t used{};
    std::size_t consumed{};
    std::size_t abandoned{};
    bool recycling{};  // being cleared; no slot may be handed out
};
struct DeferredValidation {
    int global_index{-1};
    Buffer* status{};
    std::size_t page{no_validation_page};
    std::size_t slot{};
    std::string message;
    // The statement that queued the checked work. A failure surfaces at the
    // next synchronization point, so the report names where it came from.
    counters::SourceSite site{};
    bool has_site{};
    // Metal: serial of the command buffer whose completion makes the status
    // word readable. A synchronization consumes only the checks its wait
    // covered; checks of work still queued stay pending for a later one.
    // 0 on the other backends, whose synchronization covers every check.
    std::uint64_t serial{};
};

// Covers every pending check (CUDA, HIP and the fake backend synchronize the
// whole device, and so does the exit guard after the last work).
constexpr std::uint64_t all_checks_covered=~std::uint64_t{0};

std::string deferred_validation_message(const DeferredValidation& validation) {
    if (!validation.has_site) return validation.message;
    return validation.message + " (deferred GPU check from " +
           counters::format_source_site(validation.site) + ")";
}
struct DeferredValidationState {
    std::mutex mutex;
    std::vector<DeferredValidation> pending;
    std::vector<ValidationPage> pages;
};
DeferredValidationState& deferred_validation_state() {
    static auto* state=new DeferredValidationState;
    return *state;
}

// Zeroes a whole status page (new, or recycled once every slot was
// consumed or abandoned). Never called with the validation-state lock held.
bool clear_validation_page(Buffer* storage,std::string& error) {
#ifdef __APPLE__
    if(storage->backend==Backend::Metal)
        return metal_clear_status_page(storage,validation_page_bytes,error);
#endif
    return zero(storage,0,validation_page_bytes,error);
}

Buffer* acquire_validation_status(int global_index,std::string& error) {
    const auto* info=find(global_index);
    if(!info){
        error="GPU validation requested an unavailable device";
        return nullptr;
    }
    counters::add(counters::Id::ValidationSlots); // qcount
    // Every backend sub-allocates status words from shared 4096-slot pages
    // (Metal binds a word with its byte offset), so a checked operation no
    // longer allocates and clears a status buffer of its own.
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if(info->backend==Backend::Test){
        auto* status=allocate(global_index,sizeof(std::uint32_t),error);
        if(!status) return nullptr;
        if(!zero(status,0,sizeof(std::uint32_t),error)){
            release(status);
            return nullptr;
        }
        return status;
    }
#endif

    // A new page is allocated and cleared outside the state lock: a Metal
    // clear may need the device stream, which a package encode hold on
    // another thread keeps while it asks for a status slot (lock order:
    // stream, then validation state).
    auto& state=deferred_validation_state();
    std::unique_lock lock(state.mutex);
    std::size_t page_index=no_validation_page;
    for(std::size_t i=0;i<state.pages.size();++i){
        const auto& page=state.pages[i];
        if(page.global_index==global_index&&!page.recycling&&
           page.used<validation_slots_per_page){
            page_index=i;
            break;
        }
    }
    if(page_index==no_validation_page){
        lock.unlock();
        auto* storage=allocate_buffer(global_index,validation_page_bytes,true,error);
        if(!storage) return nullptr;
        if(!clear_validation_page(storage,error)){
            release(storage);
            return nullptr;
        }
        lock.lock();
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
#ifdef __APPLE__
    else if(view->backend==Backend::Metal){
        // Borrowed: the page owns the Metal buffer.
        view->metal_buffer=page.storage->metal_buffer;
        view->base_offset=page.storage->base_offset+offset;
        view->owner=page.storage;
    }
#endif
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

bool raw_validation_synchronize(Buffer* status,std::uint64_t& covered,
                                std::string& error) {
    covered=all_checks_covered;
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
        return synchronize_metal_backend(status->backend_index,error,&covered);
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
        std::memcpy(destination,
                    static_cast<const unsigned char*>([status->metal_buffer contents])+
                        status->base_offset,
                    bytes);
        return true;
    }
#endif
    error="GPU validation status read is unavailable";
    return false;
}
// Reads and reports the deferred checks of the device whose work completed
// by serial `covered` (the synchronization just waited for it). Checks that
// other threads queued after that synchronization started stay pending: their
// status words may not be written yet.
bool consume_deferred_validations(int global_index,std::uint64_t covered,
                                  std::string& error) {
    std::vector<DeferredValidation> local;
    {
        auto& state=deferred_validation_state();
        std::lock_guard lock(state.mutex);
        auto out=state.pending.begin();
        for(auto it=state.pending.begin();it!=state.pending.end();++it){
            if(it->global_index==global_index&&it->serial<=covered){
                local.push_back(std::move(*it));
            }else{
                if(out!=it) *out=std::move(*it);
                ++out;
            }
        }
        state.pending.erase(out,state.pending.end());
        pending_deferred_validations.store(state.pending.size(),std::memory_order_relaxed);
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
            first_error=deferred_validation_message(validation);
        if(validation.page==no_validation_page){
            release(validation.status);
            validation.status=nullptr;
        }
    }

    // Mark consumed arena slots and recycle a whole page only when every
    // acquired slot is either consumed or was abandoned by a failed launch.
    // A recycling page hands out no slot until its clearing operation has
    // been enqueued. The clear runs outside the state lock, because a Metal
    // clear may need the device stream, which a package encode hold on
    // another thread can keep while it acquires a status slot.
    std::vector<std::size_t> recycle;
    {
        auto& state=deferred_validation_state();
        std::lock_guard lock(state.mutex);
        for(const auto& validation:local)
            if(validation.page!=no_validation_page&&
               validation.page<state.pages.size())
                ++state.pages[validation.page].consumed;
        for(std::size_t i=0;i<state.pages.size();++i){
            auto& page=state.pages[i];
            if(page.global_index!=global_index||page.used==0||page.recycling||
               page.used!=page.consumed+page.abandoned)
                continue;
            page.recycling=true;
            recycle.push_back(i);
        }
    }
    for(const auto index:recycle){
        Buffer* storage=nullptr;
        {
            auto& state=deferred_validation_state();
            std::lock_guard lock(state.mutex);
            storage=state.pages[index].storage;
        }
        std::string zero_error;
        const bool cleared=clear_validation_page(storage,zero_error);
        if(!cleared&&first_error.empty()) first_error=std::move(zero_error);
        auto& state=deferred_validation_state();
        std::lock_guard lock(state.mutex);
        auto& page=state.pages[index];
        if(cleared){
            page.used=0;
            page.consumed=0;
            page.abandoned=0;
        }
        page.recycling=false;
    }

    if(!first_error.empty()){
        error=std::move(first_error);
        return false;
    }
    return true;
}
// Teardown reports every deferred checked-GPU failure (language.md). Work the
// exit guard waits for can still queue checks: a package completion callback
// may encode a checked kernel and defer its status. Each pass therefore
// synchronizes every device with pending checks or pending completion
// callbacks (a Metal synchronization also waits for the callbacks of the
// work it covered) and consumes what it covered, until nothing is left. The
// pass limit stops callbacks that keep re-arming themselves.
constexpr int exit_guard_max_passes=64;

// Synchronizes and consumes until no deferred check is pending: the exit
// guard's passes, also run where an exported function returns to C
// (drain_deferred_validations). `first_error` receives the first failure.
void drain_pending_validations(std::string& first_error) {
    for(int pass=0;pass<exit_guard_max_passes&&first_error.empty();++pass){
        std::vector<int> indices;
        // Pending callbacks first, then pending checks: a callback defers
        // its check before its batch stops counting as pending (acquire
        // here pairs with that release), so a callback that finished
        // after this read left its check for the scan below. Scanning
        // checks first could miss a check deferred between the two reads.
#ifdef __APPLE__
        for(const auto& info:devices())
            if(info.backend==Backend::Metal&&
               metal_completions_pending(info.backend_index)&&
               std::find(indices.begin(),indices.end(),info.index)==indices.end())
                indices.push_back(info.index);
#endif
        {
            auto& state=deferred_validation_state();
            std::lock_guard lock(state.mutex);
            for(const auto& validation:state.pending)
                if(std::find(indices.begin(),indices.end(),validation.global_index)==indices.end())
                    indices.push_back(validation.global_index);
        }
        if(indices.empty()) break;
        for(int index:indices){
            std::string sync_error;
            std::uint64_t covered=all_checks_covered;
#ifdef __APPLE__
            const auto* info=find(index);
            if(info&&info->backend==Backend::Metal){
                if(!synchronize_metal_backend(info->backend_index,sync_error,&covered)){
                    if(first_error.empty()) first_error=std::move(sync_error);
                    continue;
                }
            }else
#endif
            {
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
                if(sample&&!raw_validation_synchronize(sample,covered,sync_error)){
                    if(first_error.empty()) first_error=std::move(sync_error);
                    continue;
                }
            }
            std::string validation_error;
            if(!consume_deferred_validations(index,covered,validation_error)&&
               first_error.empty())
                first_error=std::move(validation_error);
        }
    }
}

struct DeferredValidationExitGuard {
    ~DeferredValidationExitGuard() {
        // A package encode hold the exiting thread still has would refuse
        // the synchronizations below.
        check_package_hold_returned("the program exited");
        std::string first_error;
        drain_pending_validations(first_error);
        if(!first_error.empty()){
            // The guard runs among the exit handlers, before exit flushes
            // the C streams, and _Exit skips that flush: the program's
            // output is flushed here, ahead of the report.
            std::fflush(stdout);
            counters::report_unlocated_failure(abi::FailureReason::deferred_check_failed,
                                               first_error,true);
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
// Constructed before the first check is deferred, or before package code
// that may defer one later (a status slot or a completion callback) runs, so
// the guard exists before exit starts and is destroyed before the Metal
// streams' silent teardown.
void arm_deferred_validation_exit_guard() {
    static DeferredValidationExitGuard exit_guard;
    (void)exit_guard;
}
void defer_validation(Buffer* status,std::string message) {
    if(!status) return;
    arm_deferred_validation_exit_guard();
    DeferredValidation validation;
    validation.global_index=status->global_index;
    validation.message=std::move(message);
    validation.has_site=counters::current_user_source_site(validation.site);
#ifdef __APPLE__
    // Taken before the state lock (it takes the device stream).
    if(status->backend==Backend::Metal)
        validation.serial=metal_deferred_check_serial(status);
#endif
    auto& state=deferred_validation_state();
    if(status->validation_page!=no_validation_page){
        validation.page=status->validation_page;
        validation.slot=status->validation_slot;
        {
            std::lock_guard lock(state.mutex);
            state.pending.push_back(std::move(validation));
            pending_deferred_validations.store(state.pending.size(),std::memory_order_relaxed);
        }
        // The tiny Buffer is only a non-owning view into the page.
        delete status;
        return;
    }
    validation.status=status;
    std::lock_guard lock(state.mutex);
    state.pending.push_back(std::move(validation));
    pending_deferred_validations.store(state.pending.size(),std::memory_order_relaxed);
}
#ifdef __APPLE__
bool has_deferred_validations(int global_index) {
    auto& state=deferred_validation_state();
    std::lock_guard lock(state.mutex);
    for(const auto& validation:state.pending)
        if(validation.global_index==global_index) return true;
    return false;
}
#endif
} // namespace

const std::vector<Info>& devices() {
    static const std::vector<Info> value = enumerate_devices();
    return value;
}

bool drain_deferred_validations(std::string& error) {
    std::string first_error;
    drain_pending_validations(first_error);
    if(first_error.empty()) return true;
    error=std::move(first_error);
    return false;
}

const Info* find(int index) {
    const auto& all = devices();
    if (index < 0 || static_cast<std::size_t>(index) >= all.size()) return nullptr;
    return &all[static_cast<std::size_t>(index)];
}

bool synchronize(int index, std::string& error) {
    return synchronize_device(index, true, error);
}

// Waits for all work queued on the device. With `consume_checks` it also
// reports the deferred checks the wait covered (synchronize()); without, the
// checks stay pending for the next synchronization point (wait_idle(), which
// serves package waits and must not take another operation's failure).
bool synchronize_device(int index, bool consume_checks, std::string& error) {
    const auto* info = find(index);
    if (!info) {
        error = "gpu(" + std::to_string(index) + ") is not available";
        return false;
    }
    counters::add(counters::Id::Synchronizations); // qcount
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if (info->backend == Backend::Test) {
        if (platform::environment_has("QUIDRA_TEST_FAKE_GPU_SYNC_FAIL")) {
            error = "test-only fake GPU synchronization failure";
            return false;
        }
        if (const auto configured =
                platform::environment_value("QUIDRA_TEST_FAKE_GPU_SYNC_FAIL_INDEX")) {
            const char* text = configured->c_str();
            char* end = nullptr;
            const long fail_index = std::strtol(text, &end, 10);
            if (end != text && end && *end == '\0' && fail_index == index) {
                error = "test-only fake GPU synchronization failure";
                return false;
            }
        }
        return !consume_checks ||
               consume_deferred_validations(index, all_checks_covered, error);
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
        if(consume_checks&&
           !consume_deferred_validations(index,all_checks_covered,error))
            return false;
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
        if(consume_checks&&
           !consume_deferred_validations(index,all_checks_covered,error))
            return false;
        return true;
    }
#ifdef __APPLE__
    if (info->backend == Backend::Metal) {
        std::uint64_t covered=0;
        if(!synchronize_metal_backend(info->backend_index,error,&covered))
            return false;
        return !consume_checks||consume_deferred_validations(index,covered,error);
    }
#endif
    error = "GPU synchronization is unavailable";
    return false;
}

// Device status words for checked package kernels. A kernel sets its word
// to a nonzero value on failure; the package then either defers the check to
// the next synchronization point (reported with the statement that queued
// it) or waits for it now.
bool status_slot(int index, std::uint64_t& handle, std::uint64_t& offset_bytes,
                 std::uint64_t& slot, std::string& error) {
    auto* status = acquire_validation_status(index, error);
    if (!status) return false;
    // The package may defer this check from a completion callback that runs
    // during exit; the guard must exist before then (and after the device
    // state the acquisition created, so it is destroyed first).
    arm_deferred_validation_exit_guard();
    std::uint64_t native = 0;
#ifdef __APPLE__
    if (status->backend == Backend::Metal) {
        // Not a lend: nothing of Core's has to be committed first (a status
        // page is cleared on the host, or by a kernel committed at once), and
        // the host reads the word only after the synchronization that covers
        // the deferred check or the status wait.
        native = reinterpret_cast<std::uint64_t>(
            (__bridge void*)status->metal_buffer);
    } else
#endif
    {
        native = buffer_native_handle(status);
    }
    if (native == 0 && status->backend != Backend::Cuda) {
        release(status);
        error = "GPU status words have no native handle on this device";
        return false;
    }
    handle = native;
    offset_bytes = buffer_base_offset(status);
    slot = reinterpret_cast<std::uint64_t>(status);
    return true;
}

namespace {
Buffer* status_from_slot(int index, std::uint64_t slot, std::string& error) {
    auto* status = reinterpret_cast<Buffer*>(static_cast<std::uintptr_t>(slot));
    if (!status || status->global_index != index) {
        error = "invalid GPU status slot";
        return nullptr;
    }
    return status;
}
} // namespace

bool defer_status(int index, std::uint64_t slot, const char* message,
                  std::string& error) {
    auto* status = status_from_slot(index, slot, error);
    if (!status) return false;
    counters::add(counters::Id::PackageDeferredChecks); // qcount
    defer_validation(status, message && *message
        ? std::string(message) : std::string("package GPU check failed"));
    return true;
}

bool status_wait(int index, std::uint64_t slot, std::uint32_t& value,
                 std::string& error) {
    auto* status = status_from_slot(index, slot, error);
    if (!status) return false;
    value = 0;
    const bool ok = wait_idle(index, error) &&
        raw_validation_copy_bytes(status, &value, sizeof(value), error);
    release(status);
    return ok;
}

void status_release(int index, std::uint64_t slot) {
    std::string ignored;
    if (auto* status = status_from_slot(index, slot, ignored)) release(status);
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
    return allocate_buffer(index, bytes, false, error);
}

Buffer* allocate_buffer(int index, std::size_t bytes, bool require_idle,
                        std::string& error) {
    (void)require_idle; // Metal only
    const auto* info = find(index);
    if (!info) {
        error = "gpu(" + std::to_string(index) + ") is not available";
        return nullptr;
    }
    counters::add(counters::Id::Allocations); // qcount
    counters::add(counters::Id::AllocationBytes, bytes); // qcount
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
        std::uint64_t busy_until = 0;
        id<MTLBuffer> metal = metal_acquire_buffer(
            info->backend_index, physical_bytes, require_idle,
            buffer->physical_bytes, busy_until, error);
        if (!metal) return nullptr;
        buffer->metal_buffer = metal;
        // A pooled buffer that queued work may still use: host access waits
        // for that work (metal_buffer_idle_locked), device work is ordered.
        buffer->last_use = busy_until;
        mark_gpu_used(index);
        return buffer.release();
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
        metal_release_buffer(buffer->backend_index, buffer->metal_buffer,
                             buffer->physical_bytes);
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
    counters::add(counters::Id::Uploads); // qcount
    counters::add(counters::Id::UploadBytes, bytes); // qcount
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
    counters::add(counters::Id::Readbacks); // qcount
    counters::add(counters::Id::ReadbackBytes, bytes); // qcount
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    // A host read is a synchronization point on the fake device as on the
    // others: it reports the deferred checks of earlier work on the device,
    // all of which the fake device has finished.
    if (raw->backend == Backend::Test) {
        std::memcpy(destination, raw->test_data.data() + offset, bytes);
        return consume_deferred_validations(raw->global_index, all_checks_covered, error);
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
    // A host read is a synchronization point: like CUDA/HIP, it reports
    // the deferred checks of earlier GPU work on the device.
    if (raw->backend == Backend::Metal) {
        // Bytes that no queued GPU work can write are read without a wait,
        // unless a deferred check of the device is pending.
        if (!has_deferred_validations(raw->global_index) &&
            metal_copy_to_host_if_idle(raw, offset, destination, bytes))
            return true;
        std::uint64_t covered = 0;
        return metal_copy_to_host(raw, offset, destination, bytes, covered,
                                  error) &&
               consume_deferred_validations(raw->global_index, covered, error);
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
    counters::add(counters::Id::DeviceCopies); // qcount
    counters::add(counters::Id::DeviceCopyBytes, bytes); // qcount

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
    counters::add(counters::Id::Fills); // qcount
    counters::add(counters::Id::FillBytes, bytes); // qcount
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

void register_warmup(void (*function)(long long device)) {
    if (!function) return;
    auto& state = warmup_state();
    std::lock_guard lock(state.mutex);
    if (std::find(state.functions.begin(), state.functions.end(), function) !=
        state.functions.end())
        return;
    state.functions.push_back(function);
    for (const int device : state.devices)
        queue_warmup_locked(state, function, device);
}

bool written_outputs_skip_fill(int index) {
    static const bool always_fill =
        platform::environment_value("QUIDRA_GPU_ZERO_FILL").value_or("") == "always";
    if (always_fill) return false;
    const auto* info = find(index);
    if (!info) return false;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if (info->backend == Backend::Test) return true;
#endif
    return info->backend == Backend::Metal;
}

std::size_t buffer_base_offset(const Buffer* buffer) {
    return buffer ? buffer->base_offset : 0;
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
    if (buffer->backend == Backend::Metal) {
        // Native handles go only to package code, which may bind the buffer
        // in a command buffer of its own.
        if (auto* stream = metal_stream_for(buffer))
            metal_lend_to_package(*stream, buffer);
        return reinterpret_cast<std::uint64_t>((__bridge void*)buffer->metal_buffer);
    }
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
#include "device_unified.inc"

} // namespace quidra::device
