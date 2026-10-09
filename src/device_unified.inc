// region: unified-memory
//
// Host-visible device buffers for the unified-memory transfer views
// (src/unified_storage.hpp, src/runtime_unified.inc). Included at the end of
// device_backend.cpp inside namespace quidra::device.
//
// Metal: every Core buffer is MTLResourceStorageModeShared. On a GPU with
// unified memory (Apple silicon) its `contents` is ordinary cached system
// memory that the CPU reads coherently once the command buffers writing it
// have completed, so a buffer can back a CPU tensor directly. Page-aligned
// host memory can likewise become a buffer with newBufferWithBytesNoCopy.
//
// Fake test backend: buffers are host memory already. QUIDRA_TEST_FAKE_GPU_UNIFIED=1
// lets the deterministic test GPU take the same unified paths, so the sharing
// rules are exercised on every platform without Apple hardware.
//
// CUDA and HIP are never host-visible here; their transfers keep copying.
//
// On Metal the views run on Core's command stream
// (device_metal_stream.inc):
// - Settled `.cpu()` (I3 in runtime_unified.inc): prepare_host_view runs the
//   synchronization copy_to_host runs. A buffer that no queued work can
//   write (metal_buffer_idle_locked: Core's last binding has completed, and
//   no native handle was lent outside a package hold) with no deferred check
//   pending and no failed command buffer is read without a wait, as the copy
//   does; it is settled. Otherwise the stream synchronization commits the
//   open command buffer (or a cover marker) and waits for its serial, which
//   covers everything Core encoded before the call, whatever other threads
//   wait for; that settles it too.
// - Lent buffers (I3): TensorStorage::lent_to_package plays the role of a
//   per-buffer "exposed to package code" mark; a buffer that has one is
//   never viewed by `.cpu()`, whatever the stream says.
// - Host writes (I4): submission_serial is the stream's work_epoch, which
//   changes when work is *encoded* (and when a handle is lent), not only when
//   a command buffer is committed. metal_wait_for_host_write skips the wait
//   for an idle buffer and otherwise commits the open command buffer (or a
//   cover marker) and waits for its serial, leaving failures and deferred
//   checks to the next synchronization point.
// - Uncopied uploads (I6): nothing to do on Metal. An upload on the stream
//   commits nothing either (an idle destination is written by the host), and
//   once a device has lent a handle every synchronization commits and waits
//   for a cover marker, which orders package work committed meanwhile.
// - Pool: a buffer from wrap_host_memory owns a host allocation that its
//   deallocator frees; its physical_bytes stay 0, so device::release frees it
//   and never recycles it into a size-class pool. Blocks release buffers
//   through device::release, so the pool's retire-after rule covers every
//   view. A relocated device view gets its bytes through copy_from_host,
//   because a pooled buffer may still be read by queued work (queue-order
//   reuse).

namespace {

#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
bool fake_gpu_unified() {
    static const bool value =
        platform::environment_value("QUIDRA_TEST_FAKE_GPU_UNIFIED").value_or("") == "1";
    return value;
}
#endif

#ifdef __APPLE__
struct MetalUnifiedDevices {
    std::vector<id<MTLDevice>> devices; // retained for the process lifetime
    std::vector<bool> unified;
};

const MetalUnifiedDevices& metal_unified_devices() {
    static const MetalUnifiedDevices value = [] {
        MetalUnifiedDevices result;
        @autoreleasepool {
            NSArray<id<MTLDevice>>* all = MTLCopyAllDevices();
            for (NSUInteger i = 0; i < [all count]; ++i) {
                id<MTLDevice> device = [all objectAtIndex:i];
                [device retain];
                result.devices.push_back(device);
                result.unified.push_back([device hasUnifiedMemory] == YES);
            }
            [all release];
        }
        return result;
    }();
    return value;
}

id<MTLDevice> metal_unified_device(int backend_index) {
    const auto& all = metal_unified_devices();
    if (backend_index < 0 ||
        static_cast<std::size_t>(backend_index) >= all.devices.size() ||
        !all.unified[static_cast<std::size_t>(backend_index)])
        return nil;
    return all.devices[static_cast<std::size_t>(backend_index)];
}

// Waits until no queued GPU work can still read `raw`, so the host may write
// its bytes in place. An idle buffer needs no wait. Otherwise the open command
// buffer (or a marker covering package command buffers) is committed and
// waited for. Failed command buffers and deferred checks stay pending for the
// next synchronization point, where they surface without this wait.
bool metal_wait_for_host_write(const Buffer* raw, std::string& error) {
    auto* stream = metal_stream_for(raw);
    if (!stream) {
        error = "Metal GPU index is unavailable";
        return false;
    }
    std::uint64_t target = 0;
    {
        std::lock_guard lock(stream->mutex);
        // In the blit transfer mode Core's blits record no per-buffer use.
        if (!metal_transfer_config().blit && metal_buffer_idle_locked(*stream, *raw))
            return true;
        if (metal_hold_refuses_commit_locked(*stream, "a Metal host write wait", error))
            return false;
        if (!metal_commit_cover_locked(*stream, target, error)) return false;
    }
    if (stream->completed_serial.load(std::memory_order_acquire) < target) {
        counters::note_wait(counters::WaitReason::Read); // qcount
        metal_wait_serial(*stream, target);
    }
    return true;
}

// copy_to_host's synchronization without the copy (see prepare_host_view).
bool metal_prepare_host_view(const Buffer* raw, std::string& error,
                             bool& settled) {
    auto* stream = metal_stream_for(raw);
    if (!stream) {
        error = "Metal GPU index is unavailable";
        return false;
    }
    if (!has_deferred_validations(raw->global_index) &&
        !metal_transfer_config().blit &&
        !stream->async_failed.load(std::memory_order_acquire)) {
        std::lock_guard lock(stream->mutex);
        if (metal_buffer_idle_locked(*stream, *raw)) {
            settled = true;
            return true;
        }
    }
    std::uint64_t covered = 0;
    if (!metal_stream_synchronize(*stream, counters::WaitReason::Read, error,
                                  &covered) ||
        !consume_deferred_validations(raw->global_index, covered, error))
        return false;
    settled = true;
    return true;
}
#endif

} // namespace

bool host_visible(int index) {
    const auto* info = find(index);
    if (!info) return false;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if (info->backend == Backend::Test) return fake_gpu_unified();
#endif
#ifdef __APPLE__
    if (info->backend == Backend::Metal)
        return metal_unified_device(info->backend_index) != nil;
#endif
    return false;
}

unsigned char* host_address(Buffer* buffer) {
    if (!buffer || buffer->validation_page != no_validation_page) return nullptr;
    if (!host_visible(buffer->global_index)) return nullptr;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if (buffer->backend == Backend::Test) return buffer->test_data.data();
#endif
#ifdef __APPLE__
    if (buffer->backend == Backend::Metal && buffer->metal_buffer)
        return static_cast<unsigned char*>([buffer->metal_buffer contents]) +
               buffer->base_offset;
#endif
    return nullptr;
}

bool prepare_host_view(const Buffer* buffer, std::size_t bytes,
                       std::string& error, bool& settled) {
    settled = false;
    if (!buffer || !range_ok(*buffer, 0, bytes)) {
        error = "invalid GPU download range";
        return false;
    }
    if (bytes == 0) return true;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    // The fake backend executes every operation before it returns. The
    // view is a host read, a synchronization point: it reports the deferred
    // checks of the device, as copy_to_host does.
    if (buffer->backend == Backend::Test) {
        if (!consume_deferred_validations(buffer->global_index, all_checks_covered, error))
            return false;
        settled = true;
        return true;
    }
#endif
#ifdef __APPLE__
    // The same synchronization as copy_to_host: an idle buffer is read
    // without a wait, anything else waits for the serial that covers all
    // work Core encoded before the call. Either way no work Core encoded so
    // far can still write the buffer, so the view is settled. Command buffers
    // that Math, NN or Vision commit on Core's queue are covered only once
    // the device lent a handle, which is why the runtime never views memory
    // lent to package code.
    if (buffer->backend == Backend::Metal)
        return metal_prepare_host_view(buffer, error, settled);
#endif
    error = "GPU backend host view is unavailable";
    return false;
}

void note_uncopied_upload(int index) {
    // Metal: an upload on Core's stream leaves nothing a later
    // synchronization must drain either (an idle destination is written by
    // the host), and once a device has lent a handle every synchronization
    // commits and waits for a cover marker, which orders package work
    // committed on Core's queue (I6 holds without a drain). The fake backend
    // finishes every operation before it returns.
    (void)index;
}

std::uint64_t submission_serial(int index) {
#ifdef __APPLE__
    const auto* info = find(index);
    if (info && info->backend == Backend::Metal) {
        auto* stream = metal_stream(info->backend_index);
        if (stream) return stream->work_epoch.load(std::memory_order_acquire);
    }
#else
    (void)index;
#endif
    // The fake backend finishes every operation before it returns, but it
    // reports "unknown" so the tests exercise the conservative host waits.
    return unknown_submission_serial;
}

bool wait_for_host_write(const Buffer* buffer, std::string& error) {
    if (!buffer) {
        error = "missing GPU buffer";
        return false;
    }
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    // The fake backend executes every operation before it returns.
    if (buffer->backend == Backend::Test) return true;
#endif
#ifdef __APPLE__
    if (buffer->backend == Backend::Metal && buffer->metal_buffer)
        return metal_wait_for_host_write(buffer, error);
#endif
    error = "GPU backend host write wait is unavailable";
    return false;
}

std::size_t host_wrap_granularity(int index) {
    const auto* info = find(index);
    if (!info || !host_visible(index)) return 0;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if (info->backend == Backend::Test) return 1;
#endif
#ifdef __APPLE__
    if (info->backend == Backend::Metal) return quidra::unified::host_page_bytes();
#endif
    return 0;
}

Buffer* wrap_host_memory(int index, unsigned char* data, std::size_t bytes,
                         std::size_t capacity,
                         void (*deallocate)(void* pointer, std::size_t capacity),
                         std::string& error) {
    const auto* info = find(index);
    const auto granularity = host_wrap_granularity(index);
    if (!info || granularity == 0 || !data || !deallocate || bytes == 0 ||
        capacity < bytes ||
        reinterpret_cast<std::uintptr_t>(data) % granularity != 0 ||
        capacity % granularity != 0) {
        error = "host memory cannot be wrapped for gpu(" + std::to_string(index) + ")";
        return nullptr;
    }
    auto buffer = std::make_unique<Buffer>();
    buffer->backend = info->backend;
    buffer->global_index = index;
    buffer->backend_index = info->backend_index;
    buffer->bytes = bytes;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if (info->backend == Backend::Test) {
        buffer->test_data.adopt(data, bytes, capacity, deallocate);
        mark_gpu_used(index);
        return buffer.release();
    }
#endif
#ifdef __APPLE__
    if (info->backend == Backend::Metal) {
        // Core's stream device: the wrapped buffer is bound in Core's
        // command buffers like any other.
        auto* stream = metal_unified_device(info->backend_index)
                           ? metal_stream(info->backend_index) : nullptr;
        id<MTLDevice> device = stream ? stream->device : nil;
        id<MTLBuffer> wrapped = nil;
        if (device) {
            wrapped = [device newBufferWithBytesNoCopy:data
                                                length:capacity
                                               options:MTLResourceStorageModeShared
                                           deallocator:^(void* pointer, NSUInteger length) {
                                               deallocate(pointer, static_cast<std::size_t>(length));
                                           }];
        }
        if (!wrapped) {
            error = "Metal could not wrap host memory";
            return nullptr;
        }
        buffer->metal_buffer = wrapped;
        // physical_bytes stays 0: release() frees the wrapper (and with it
        // the host allocation, once no command buffer binds it) instead of
        // recycling it into the pool.
        mark_gpu_used(index);
        return buffer.release();
    }
#endif
    error = "GPU backend cannot wrap host memory";
    return nullptr;
}

// endregion: unified-memory
