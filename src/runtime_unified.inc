// region: unified-transfer
//
// Unified-memory transfers (11th optimization). Included by runtime.cpp inside
// its anonymous namespace, after tensor_transfer_storage().
//
// `.gpu(n)` and `.cpu()` remain explicit value boundaries: the result is a new
// TensorStorage with the target device, fresh descriptor, no graph, and
// complete initialization, exactly as the copy produces it. What changes is
// only the physical action. When the device is host-visible
// (quidra::device::host_visible), the new storage may *view* the source's
// bytes through a shared quidra::unified::Block instead of receiving a copy.
//
// The sharing is unobservable because of these invariants:
//
//   I1  Only full, dense, fully initialized storages are viewed. Every other
//       source (views with an offset or strides, partial initialization,
//       zero elements, cross-device, CUDA/HIP) takes the existing copy, and so
//       does a `.cpu()` of less than cpu_view_minimum_bytes.
//   I2  While a block has two or more views, nobody writes its bytes in place:
//       tensor_storage_writable_in_place() is false, so Core writers
//       (tensor_detach_for_write, in-place gradient accumulation) give the
//       writer private storage, exactly as for any shared storage.
//   I3  A block gains its second view only after all device work that can
//       write it has completed, and never over memory package code can still
//       reach. `.cpu()` refuses a storage whose device handle was ever lent to
//       package code (lent_to_package): a package may keep work that uses the
//       buffer in a command buffer it commits in a later call (NN's Metal Adam
//       batch encodes across calls), and no synchronization can wait for a
//       command buffer that is not committed yet. Otherwise only Core's own
//       work can write the buffer, and `.cpu()` runs the copy path's
//       synchronization (prepare_host_view) and views only when that proved
//       all work Core encoded before it complete (on Metal's command stream:
//       the buffer was idle, or the synchronization waited for the serial
//       that covers it). Otherwise `.cpu()` copies exactly as before. A wrapped host
//       allocation was never written by the device, and a device view writes
//       in place only while it is the only view (I2), which ends at such a
//       settled `.cpu()`.
//   I4  The host writes a CPU view in place only when it is the only view and
//       no device work can still be reading the bytes. device_reads_possible
//       is set when a device view leaves the block after it may have been used
//       since the block was settled (Core submitted device work, or a package
//       borrowed a device handle), and cleared by a host wait that leaves
//       async errors in place.
//   I5  Package code sees the same ownership contract as with a copy. The
//       static fact "a transfer result owns its storage" lets the compiler
//       hand the result to an in-place package target (NN relu_reuse), which
//       requires its const and mutable borrows to return the same pointer.
//       Therefore a native borrow of a view that is still shared first moves
//       *that view* to private memory (relocation), before any pointer is
//       handed out; other views of the block keep their bytes and pointers.
//       A borrow of an unshared view is served in place (after the I4 wait for
//       a mutable CPU borrow), so const and mutable pointers agree. Together
//       with I3 this means package code never holds a device handle of memory
//       that a CPU tensor views.
//   I6  Synchronization waits at least as much as after a copy. A viewed
//       upload commits no staging copy; note_uncopied_upload lets a backend
//       make up for the wait that copy would have caused. On Metal's command
//       stream an upload commits nothing either, and package work committed
//       on Core's queue is covered by the marker every synchronization
//       commits once a handle was lent, so nothing is owed there.
//
// Lifetimes: each view holds one Block reference; the last release frees the
// device buffer, which (Metal) stays alive until submitted command buffers
// that bind it complete, and frees a wrapped host allocation only then.

namespace unified = quidra::unified;

bool tensor_storage_writable_in_place(const TensorStorage& storage) {
    if (storage.owners != 1) return false;
    if (!storage.unified) return true;
    if (unified::views(*storage.unified) != 1) return false;
    return !tensor_on_cpu(storage) ||
           !storage.unified->device_reads_possible.load(std::memory_order_acquire);
}

bool tensor_unified_candidate(const TensorValue& source) {
    const auto* storage = source.storage;
    return storage && storage->count != 0 &&
           storage->initialization.fully_initialized &&
           source.offset == 0 && tensor_is_contiguous_value(source) &&
           tensor_logical_count(source) == storage->count;
}

TensorStorage* tensor_unified_view(unified::Block& block,
                                   const TensorStorage& source, int device) {
    auto* view = new (std::nothrow) TensorStorage;
    if (!view) runtime_allocation_failure();
    view->dtype = source.dtype;
    view->count = source.count;
    view->device = device;
    view->initialization.count = source.count;
    view->initialization.unit_bytes = tensor_dtype_bytes(source.dtype);
    view->initialization.fully_initialized = true;
    view->initialization.initialized_count = source.count;
    if (device < 0) {
        view->data.view(block.host, block.bytes);
    } else {
        view->gpu_buffer = block.buffer;
    }
    view->unified = &block;
    unified::attach(block);
    return view;
}

// Turns a device storage into the first view of a block over its own buffer.
unified::Block* tensor_unified_block_of_device(TensorStorage& storage,
                                               std::size_t bytes) {
    if (storage.unified) return storage.unified;
    auto* host = quidra::device::host_address(storage.gpu_buffer);
    if (!host) return nullptr;
    auto* block = unified::create_block(
        storage.gpu_buffer, host, bytes, storage.device);
    unified::attach(*block);
    storage.unified = block;
    return block;
}

// Hands a CPU storage's own allocation to `device` without copying it. The
// storage becomes the first view of the new block; the device buffer now owns
// the allocation and frees it after its last use.
unified::Block* tensor_unified_wrap_host(TensorStorage& storage, int device,
                                         std::size_t bytes) {
    auto& data = storage.data;
    if (!data.owns() || data.size() != bytes) return nullptr;
    const auto granularity = quidra::device::host_wrap_granularity(device);
    if (granularity == 0 ||
        reinterpret_cast<std::uintptr_t>(data.data()) % granularity != 0 ||
        data.capacity() % granularity != 0)
        return nullptr;
    std::string error;
    auto* buffer = quidra::device::wrap_host_memory(
        device, data.data(), bytes, data.capacity(), data.deallocator(), error);
    if (!buffer) return nullptr; // the copy path handles it
    data.release_ownership();
    auto* block = unified::create_block(buffer, data.data(), bytes, device);
    // No device work references the new buffer yet.
    unified::settle(*block, quidra::device::submission_serial(device));
    unified::attach(*block);
    storage.unified = block;
    unified::count(unified::Counter::Wrap);
    return block;
}

// Package code is about to receive a native device handle of `storage` (I3).
void tensor_unified_note_device_lend(TensorStorage& storage) {
    storage.lent_to_package = true;
}

// Serves an explicit transfer as a view when I1 holds; nullptr means "copy".
TensorStorage* tensor_unified_transfer(
    const TensorValue& source, int target_device,
    unsigned long long line, unsigned long long column) {
    if (!unified::enabled() || !tensor_unified_candidate(source)) return nullptr;
    auto& storage = *source.storage;
    const auto bytes = storage.count * tensor_dtype_bytes(storage.dtype);

    if (!tensor_on_cpu(storage) && target_device < 0) {
        if (!unified::cpu_views_enabled() ||
            bytes < unified::cpu_view_minimum_bytes || !storage.gpu_buffer ||
            storage.lent_to_package ||
            !quidra::device::host_visible(storage.device) ||
            (!storage.unified && !quidra::device::host_address(storage.gpu_buffer)))
            return nullptr;
        // Read before the synchronization: device work Core submits later
        // changes the serial (I4).
        const auto serial = quidra::device::submission_serial(storage.device);
        // The copy path's copy_to_host: same validation, same synchronization,
        // same error, before any byte is read on the host.
        std::string error;
        bool settled = false;
        if (!quidra::device::prepare_host_view(storage.gpu_buffer, bytes, error, settled))
            tensor_fail(error.c_str(), line, column);
        // Unsettled: another thread is still waiting for Core's work, which may
        // be writing the bytes (I3). The copy repeats the synchronization
        // (nothing is pending, so it returns at once, as before) and reads the
        // bytes as they are now.
        if (!settled) return nullptr;
        auto* block = tensor_unified_block_of_device(storage, bytes);
        if (!block) return nullptr;
        // This thread owns the only view, and the barrier above has waited for
        // every use of it so far.
        if (unified::views(*block) == 1) unified::settle(*block, serial);
        unified::count(unified::Counter::CpuView);
        return tensor_unified_view(*block, storage, -1);
    }

    if (tensor_on_cpu(storage) && target_device >= 0) {
        // An unavailable or non-host-visible index takes the copy path, which
        // reports exactly the error it always did.
        if (!quidra::device::host_visible(target_device)) return nullptr;
        auto* block = storage.unified;
        if (block) {
            if (block->device != target_device || block->bytes != bytes) return nullptr;
        } else {
            block = tensor_unified_wrap_host(storage, target_device, bytes);
            if (!block) return nullptr;
        }
        quidra::device::note_uncopied_upload(target_device); // I6
        unified::count(unified::Counter::DeviceView);
        return tensor_unified_view(*block, storage, target_device);
    }
    return nullptr;
}

// Moves one view out of its block into private memory with the same bytes.
// The block content is stable here (I2/I3), so a host copy is exact.
bool tensor_unified_relocate(TensorStorage& storage) {
    auto* block = storage.unified;
    if (!block) return true;
    const auto bytes = block->bytes;
    const bool device_view = !tensor_on_cpu(storage);
    if (!device_view) {
        unified::HostBytes fresh;
        try {
            fresh.resize(bytes);
        } catch (...) {
            return false;
        }
        std::memcpy(fresh.data(), block->host, bytes);
        storage.data = std::move(fresh);
    } else {
        std::string error;
        auto* buffer = quidra::device::allocate(storage.device, bytes, error);
        if (!buffer) return false;
        // A pooled Metal buffer may still be read by queued work (queue-order
        // reuse), so the bytes go through copy_from_host, which writes an
        // idle buffer on the host and orders the copy after that work
        // otherwise.
        if (!quidra::device::copy_from_host(buffer, 0, block->host, bytes, error)) {
            quidra::device::release(buffer);
            return false;
        }
        storage.gpu_buffer = buffer;
    }
    storage.unified = nullptr;
    unified::detach(block, device_view);
    unified::count(unified::Counter::Relocate);
    return true;
}

// Called by the native ABI before it hands package code a pointer or handle to
// tensor memory (I5). Returns false when the borrow must fail (allocation
// failure while separating a shared view, or a failed host wait), which the
// ABI reports as a null pointer or handle, like any other refused borrow.
bool tensor_unified_prepare_native_borrow(TensorStorage* storage, bool writable) {
    if (!storage || !storage->unified) return true;
    auto& block = *storage->unified;
    if (unified::views(block) > 1) {
        // A writable borrow of a storage with several owners is detached by
        // tensor_detach_for_write anyway; the shared view itself stays put.
        if (writable && storage->owners != 1) return true;
        return tensor_unified_relocate(*storage);
    }
    if (!tensor_on_cpu(*storage)) {
        // The package may submit device work that uses the buffer after the
        // call returns (I4).
        block.device_borrowed.store(true, std::memory_order_relaxed);
        return true;
    }
    if (writable && block.device_reads_possible.load(std::memory_order_acquire)) {
        const auto serial = quidra::device::submission_serial(block.device);
        std::string error;
        // A const pointer to these bytes may already have been handed out in
        // this call, so moving the view now could leave it dangling (I5).
        // Refuse the borrow instead; the ABI reports it like any refused one.
        if (!quidra::device::wait_for_host_write(block.buffer, error)) return false;
        unified::settle(block, serial);
        unified::count(unified::Counter::HostWait);
    }
    return true;
}

// endregion: unified-transfer
