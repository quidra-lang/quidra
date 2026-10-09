#!/usr/bin/env bash
# Unified-memory transfers: `.gpu(n)` / `.cpu()` served as copy-on-write views
# of one host-visible allocation must be indistinguishable from copies.
#
# Every program is built once and run with the views enabled and with
# QUIDRA_UNIFIED_MEMORY=0 (the copy path); both outputs must equal the expected
# text. The views run on the test-only fake GPU under
# QUIDRA_TEST_FAKE_GPU_UNIFIED=1 (builds with the fake backend) and on a real
# Metal GPU when one is present. QUIDRA_UNIFIED_MEMORY_STATS proves that the
# view paths were taken rather than silently falling back to copies.
#
#   QUIDRA_REQUIRE_REAL_GPU=1 QUIDRA_REQUIRE_GPU_BACKEND=Metal  fail without Metal
#   QUIDRA_REQUIRE_UNIFIED_MEMORY=1  fail when Metal serves no transfer as a
#       view (a Metal device without unified memory otherwise only checks that
#       the copies it keeps are correct)
#
# QUIDRA_UNIFIED_MEMORY=upload (only `.gpu(n)` views) is checked as well.
set -euo pipefail

QUIDRA="${1:-}"
if [[ -z "$QUIDRA" ]]; then
    echo "usage: $0 /path/to/quidra" >&2
    exit 2
fi
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/quidra-cache"  # a run cache of this suite run only

fail() {
    echo "unified memory: $*" >&2
    exit 1
}

# ------------------------------------------------------------------ programs

cat > "$TMP/semantics.qui" <<'QUI'
int dev = 0

void show(string label, real32 value)
    print("{label} {value}{NL}")

// 8 elements stay below the Metal wrap minimum; 32768 (128 KiB) are wrapped.
// Both must behave like independent values.
int[] sizes = [8, 32768]
for size in sizes
    print("size {size}{NL}")
    tensor<real32> host = tensor.ones<real32>([nat(size)]) * real32(2)
    tensor<real32> device = host.gpu(nat(dev))
    host[0] = real32(10)
    show("cpu write after gpu: device", device.cpu()[0].item())
    show("cpu write after gpu: host", host[0].item())

    tensor<real32> view = device.cpu()
    view[1] = real32(11)
    show("cpu view write: device", device.cpu()[1].item())
    show("cpu view write: view", view[1].item())

    tensor<real32> first = device.cpu()
    tensor<real32> second = device.cpu()
    device[2] = real32(12)
    show("gpu write with cpu views: first", first[2].item())
    show("gpu write with cpu views: second", second[2].item())
    show("gpu write with cpu views: device", device.cpu()[2].item())

    tensor<real32> round = device.cpu().gpu(nat(dev))
    round[3] = real32(13)
    show("round trip write: device", device.cpu()[3].item())
    show("round trip write: round", round.cpu()[3].item())
    second[3] = real32(14)
    show("view write after round trip: round", round.cpu()[3].item())
    show("view write after round trip: first", first[3].item())

    tensor<real32> alias = view
    alias[4] = real32(15)
    show("shared view write: view", view[4].item())
    show("shared view write: alias", alias[4].item())
    print("devices {device.device()} {view.device()} {round.device()}{NL}")
    print("contiguous {view.is_contiguous()} {round.is_contiguous()}{NL}")

// Device work queued before a host write must still read the old bytes, and
// work queued before .cpu() must be complete when the view is read. The device
// view is dropped first, so `base` is the only view of its allocation while
// the queued additions still read it.
tensor<real32> base = tensor.ones<real32>([65536])
tensor<real32> base_device = base.gpu(nat(dev))
tensor<real32> accumulated = base_device
for step in range(40)
    accumulated = accumulated + base_device
base_device = tensor.zeros<real32>([1], gpu = nat(dev))
base[0] = real32(1000)
base[1] = real32(2000)
tensor<real32> accumulated_host = accumulated.cpu()
show("queued reads before host writes", accumulated_host[0].item())
show("queued reads before host writes [1]", accumulated_host[1].item())
show("host writes", base[0].item() + base[1].item())

tensor<real32> written = tensor.zeros<real32>([65536], gpu = nat(dev))
for step in range(25)
    written = written + real32(1)
tensor<real32> written_host = written.cpu()
show("queued writes before cpu", written_host[65535].item())
written = written + real32(1)
show("later device work does not change the view", written_host[0].item())
show("later device work", written.cpu()[0].item())

// Views with an offset or strides, and partial initialization, keep copying.
tensor<real32> grid = (tensor.ones<real32>([4, 4]) * real32(3)).gpu(nat(dev))
tensor<real32> column = grid[:, 1].cpu()
show("strided view", column[2].item())
print("strided contiguous {column.is_contiguous()}{NL}")
tensor<real32> partial = tensor<real32>([4])
partial[0] = real32(5)
partial[2] = real32(6)
tensor<real32> partial_device = partial.gpu(nat(dev))
show("partial", partial_device.cpu()[2].item())
tensor<real32> complete = tensor<real32>([3])
complete[0] = real32(1)
complete[1] = real32(2)
complete[2] = real32(3)
tensor<real32> complete_device = complete.gpu(nat(dev))
complete[1] = real32(20)
show("element-initialized", complete_device.cpu()[1].item())

// Other dtypes.
tensor<int32> integers = tensor.ones<int32>([5]) * int32(7)
tensor<int32> integers_device = integers.gpu(nat(dev))
integers[0] = int32(9)
print("int32 {integers_device.cpu()[0].item()} {integers[0].item()}{NL}")
tensor<real64> doubles = tensor.ones<real64>([3]) * 1.5
tensor<real64> doubles_host = doubles.gpu(nat(dev)).cpu()
doubles_host[0] = 4.5
print("float64 {doubles[0].item()} {doubles_host[0].item()}{NL}")
tensor<nat8> bytes = tensor.ones<nat8>([70000])
tensor<nat8> bytes_device = bytes.gpu(nat(dev))
bytes[69999] = nat8(200)
print("uint8 {bytes_device.cpu()[69999].item()} {bytes[69999].item()}{NL}")
tensor<bool> flags = tensor.ones<real32>([4]) > real32(0)
print("bool {flags.gpu(nat(dev)).cpu().all()}{NL}")
QUI

cat > "$TMP/semantics.expected" <<'TEXT'
size 8
cpu write after gpu: device 2.0
cpu write after gpu: host 10.0
cpu view write: device 2.0
cpu view write: view 11.0
gpu write with cpu views: first 2.0
gpu write with cpu views: second 2.0
gpu write with cpu views: device 12.0
round trip write: device 2.0
round trip write: round 13.0
view write after round trip: round 13.0
view write after round trip: first 2.0
shared view write: view 2.0
shared view write: alias 15.0
devices 0 -1 0
contiguous true true
size 32768
cpu write after gpu: device 2.0
cpu write after gpu: host 10.0
cpu view write: device 2.0
cpu view write: view 11.0
gpu write with cpu views: first 2.0
gpu write with cpu views: second 2.0
gpu write with cpu views: device 12.0
round trip write: device 2.0
round trip write: round 13.0
view write after round trip: round 13.0
view write after round trip: first 2.0
shared view write: view 2.0
shared view write: alias 15.0
devices 0 -1 0
contiguous true true
queued reads before host writes 41.0
queued reads before host writes [1] 41.0
host writes 3000.0
queued writes before cpu 25.0
later device work does not change the view 25.0
later device work 26.0
strided view 3.0
strided contiguous true
partial 6.0
element-initialized 2.0
int32 7 9
float64 1.5 4.5
uint8 1 200
bool true
TEXT

# Autograd: transfers are untracked boundaries; gradients flow through the
# tracked values derived from them on either side, including second order.
# 3 elements are copied; 32768 (128 KiB) are viewed in both directions on
# Metal and the fake GPU, which the counters check.
cat > "$TMP/autograd.qui" <<'QUI'
int dev = 0

void show(string label, real32 value)
    print("{label} {value}{NL}")

tensor<real32> sum_first(tensor<real32> value, int count)
    tensor<real32> result = value.gather([0], [])
    for index in range(1, count)
        result = result + value.gather([index], [])
    return result

int[] sizes = [3, 32768]
for size in sizes
    print("size {size}{NL}")
    // First order on the device, through an upload.
    tensor<real32> weights_host = tensor.ones<real32>([nat(size)]) * real32(2)
    tensor<real32> weights = weights_host.gpu(nat(dev))
    tensor<real32> tracked = weights.track()
    sum_first(tracked * tracked * tracked, 3).backward(&weights)
    tensor<real32> gradient = weights.grad.cpu()
    show("gpu gradient", gradient[1].item())
    weights_host[1] = real32(100)
    show("gpu gradient after host write", weights.grad.cpu()[1].item())
    show("weights after host write", weights.cpu()[1].item())
    gradient[1] = real32(-1)
    show("gpu gradient after view write", weights.grad.cpu()[1].item())

    // Second order on the host, through a download.
    tensor<real32> on_host = (tensor.ones<real32>([nat(size)], gpu = nat(dev)) * real32(3)).cpu()
    tensor<real32> cube_input = on_host.track()
    tensor<real32> cube = cube_input * cube_input * cube_input
    sum_first(cube, 2).backward(&on_host, track = true)
    tensor<real32> first_derivative = on_host.grad
    show("first derivative", first_derivative.untrack()[1].item())
    on_host.clear_grad()
    sum_first(first_derivative, 2).backward(&on_host)
    show("second derivative", on_host.grad.untrack()[1].item())
    on_host[1] = real32(10)
    show("host write after second order", on_host[1].item())
    show("first derivative after host write", first_derivative.untrack()[1].item())

    // Gradient accumulation after a download of the gradient.
    tensor<real32> values = tensor.ones<real32>([nat(size)]).gpu(nat(dev))
    tensor<real32> values_tracked = values.track()
    sum_first(values_tracked * real32(3), 2).backward(&values)
    tensor<real32> earlier = values.grad.cpu()
    sum_first(values_tracked * real32(4), 2).backward(&values)
    show("accumulated", values.grad.cpu()[1].item())
    show("earlier gradient", earlier[1].item())
QUI

{
    for size in 3 32768; do
        cat <<TEXT
size $size
gpu gradient 12.0
gpu gradient after host write 12.0
weights after host write 2.0
gpu gradient after view write 12.0
first derivative 27.0
second derivative 18.0
host write after second order 10.0
first derivative after host write 27.0
accumulated 7.0
earlier gradient 3.0
TEXT
    done
} > "$TMP/autograd.expected"

# Package code borrows tensor memory through the native ABI. A transfer result
# is statically "owned", so the compiler may hand it to an in-place package
# target (NN relu_reuse) that requires its const and mutable borrows to agree.
mkdir -p "$TMP/packages/unified_probe/native"
cat > "$TMP/packages/unified_probe/main.qui" <<'QUI'
extern int32 reuse_native(
    tensor<real32> &value
) = "qtest_unified_reuse"
extern int32 alias_native(
    const tensor<real32> &source,
    tensor<real32> &target
) = "qtest_unified_alias"
extern int32 device_reuse_native(
    tensor<real32> &value
) = "qtest_unified_device_reuse"

int32 reuse(tensor<real32> &value)
    return reuse_native(&value)

int32 alias(tensor<real32> source, tensor<real32> &target)
    return alias_native(&source, &target)

int32 device_reuse(tensor<real32> &value)
    return device_reuse_native(&value)
QUI
cat > "$TMP/packages/unified_probe/quidra.package" <<'MANIFEST'
name = unified_probe
version = 0.1.0
native.source.bridge = native/bridge.cpp
MANIFEST
cat > "$TMP/packages/unified_probe/native/bridge.cpp" <<'CPP'
#include <quidra/native_extension.h>

#include <cstdint>

// relu_reuse pattern: const borrow, then mutable borrow of the same value.
extern "C" int32_t qtest_unified_reuse(void* value) {
    if (!value || qcore_tensor_backend(value) != QCORE_BACKEND_CPU ||
        qcore_tensor_element_count(value) == 0)
        return 1;
    const auto* before = static_cast<const float*>(qcore_tensor_cpu_data_const(value));
    if (!before) return 2;
    auto* data = static_cast<float*>(qcore_tensor_cpu_data(value));
    if (!data) return 3;
    if (data != before) return 5;
    data[0] += 1.0F;
    return 0;
}

// Two views of one allocation in one call: writing the target must not change
// what the source reads.
extern "C" int32_t qtest_unified_alias(const void* source, void* target) {
    if (!source || !target ||
        qcore_tensor_backend(source) != QCORE_BACKEND_CPU ||
        qcore_tensor_backend(target) != QCORE_BACKEND_CPU)
        return 1;
    const auto* input = static_cast<const float*>(qcore_tensor_cpu_data_const(source));
    if (!input) return 2;
    const float first = input[0];
    auto* output = static_cast<float*>(qcore_tensor_cpu_data(target));
    if (!output) return 3;
    if (output == input) return 4;
    output[0] = first + 100.0F;
    if (input[0] != first) return 6;
    return 0;
}

// Device handles: const then mutable borrow agree. Fake-GPU handles are host
// pointers, so the probe writes through them there.
extern "C" int32_t qtest_unified_device_reuse(void* value) {
    if (!value || qcore_tensor_device(value) < 0) return 1;
    const auto before = qcore_tensor_device_handle_const(value);
    if (before == 0) return 2;
    const auto handle = qcore_tensor_device_handle(value);
    if (handle == 0) return 3;
    if (handle != before) return 5;
    if (qcore_tensor_backend(value) == QCORE_BACKEND_TEST)
        reinterpret_cast<float*>(static_cast<std::uintptr_t>(handle))[0] += 1.0F;
    return 0;
}
CPP

cat > "$TMP/borrow.qui" <<'QUI'
import probe = unified_probe
int dev = 0

void show(string label, real32 value)
    print("{label} {value}{NL}")

// While the source is alive the result shares its allocation until the first
// native borrow moves it out; const and mutable pointers agree.
tensor<real32> device = (tensor.ones<real32>([32768]) * real32(2)).gpu(nat(dev))
tensor<real32> result = device.cpu()
print("reuse shared {probe.reuse(&result)}{NL}")
show("reuse shared: result", result[0].item())
show("reuse shared: device", device.cpu()[0].item())

// Once the device view is gone, queued device work may still read the bytes:
// the mutable borrow waits for it instead of moving the value, and the queued
// work still sees the old bytes.
tensor<real32> host = tensor.ones<real32>([65536])
tensor<real32> uploaded = host.gpu(nat(dev))
tensor<real32> accumulated = uploaded
for step in range(100)
    accumulated = accumulated + uploaded
uploaded = tensor.zeros<real32>([1], gpu = nat(dev))
print("reuse exclusive {probe.reuse(&host)}{NL}")
show("reuse exclusive: host", host[0].item())
show("reuse exclusive: queued", accumulated.cpu()[0].item())

// Two views of one allocation in one native call stay distinct values.
tensor<real32> shared = (tensor.ones<real32>([32768]) * real32(3)).gpu(nat(dev))
tensor<real32> left = shared.cpu()
tensor<real32> right = shared.cpu()
print("alias {probe.alias(left, &right)}{NL}")
show("alias: left", left[0].item())
show("alias: right", right[0].item())
show("alias: shared", shared.cpu()[0].item())

// Device handles of a device view that shares its allocation.
tensor<real32> source = tensor.ones<real32>([32768]) * real32(4)
tensor<real32> on_device = source.gpu(nat(dev))
print("device reuse {probe.device_reuse(&on_device)}{NL}")
show("device reuse: source", source[0].item())
QUI

cat > "$TMP/borrow.expected" <<'TEXT'
reuse shared 0
reuse shared: result 3.0
reuse shared: device 2.0
reuse exclusive 0
reuse exclusive: host 2.0
reuse exclusive: queued 101.0
alias 0
alias: left 3.0
alias: right 103.0
alias: shared 3.0
device reuse 0
device reuse: source 4.0
TEXT

# A `.cpu()` result whose device source is gone before any further device
# work needs no host wait for a mutable borrow (the relu_reuse pattern).
cat > "$TMP/nowait.qui" <<'QUI'
import probe = unified_probe
int dev = 0
tensor<real32> result = (tensor.ones<real32>([32768], gpu = nat(dev)) * real32(2)).cpu()
print("reuse after transfer {probe.reuse(&result)} {result[0].item()} {result[1].item()}{NL}")
QUI
cat > "$TMP/nowait.expected" <<'TEXT'
reuse after transfer 0 3.0 2.0
TEXT

# `.cpu()` after the device has finished all of Core's work (here after
# gpu.sync) is a view too, and stays a distinct value.
cat > "$TMP/idle.qui" <<'QUI'
int dev = 0
tensor<real32> device = tensor.ones<real32>([32768], gpu = nat(dev)) * real32(4)
gpu.sync(nat(dev))
tensor<real32> first = device.cpu()
tensor<real32> second = device.cpu()
first[0] = real32(1)
print("idle {first[0].item()} {first[1].item()} {second[0].item()} {device.cpu()[0].item()}{NL}")
QUI
cat > "$TMP/idle.expected" <<'TEXT'
idle 1.0 4.0 4.0 4.0
TEXT

# Metal only: package code commits command buffers on Core's queue that Core
# does not track (Math's unary kernels do), and may encode work into a command
# buffer that it commits only in a later call (NN's Metal Adam batch does).
# Both must see exactly what they see with copies:
# - gate: a gate opened by a timer holds a package write in flight while
#   `.cpu()` runs. With nothing tracked pending, the synchronization cannot
#   cover it: the result never changes afterwards and the source keeps the
#   package's write. With Core work pending, the synchronization's barrier
#   also orders the package write. A tensor lent to package code is never
#   viewed, so both `.cpu()` calls copy.
# - open: a later host write to a `.cpu()` result must not reach what an
#   uncommitted package command buffer reads.
# - upload: an upload served as a view commits no staging copy, yet the next
#   synchronization must still wait for package work committed after it.
mkdir -p "$TMP/gate_packages/unified_gate/native"
cat > "$TMP/gate_packages/unified_gate/main.qui" <<'QUI'
extern int32 delayed_fill_native(
    tensor<real32> &value,
    int32 milliseconds
) = "qtest_unified_delayed_fill"
extern int32 touch_native(const tensor<real32> &value) = "qtest_unified_touch"
extern int32 open_read_native(const tensor<real32> &value) = "qtest_unified_open_read"
extern int32 commit_open_native() = "qtest_unified_commit_open"
extern real32 snapshot_native(int64 index) = "qtest_unified_snapshot"

int32 delayed_fill(tensor<real32> &value, int32 milliseconds)
    return delayed_fill_native(&value, milliseconds)

int32 touch(tensor<real32> value)
    return touch_native(&value)

int32 open_read(tensor<real32> value)
    return open_read_native(&value)

int32 commit_open()
    return commit_open_native()

real32 snapshot(int64 index)
    return snapshot_native(index)
QUI
cat > "$TMP/gate_packages/unified_gate/quidra.package" <<'MANIFEST'
name = unified_gate
version = 0.1.0
native.source.cpu = native/bridge.cpp
native.source.macos-arm64.metal = native/gate_metal.mm
native.source.macos-x86_64.metal = native/gate_metal.mm
MANIFEST
cat > "$TMP/gate_packages/unified_gate/native/bridge.cpp" <<'CPP'
#include <quidra/native_extension.h>

#include <cstdint>

extern "C" int32_t qtest_unified_metal_delayed_fill(void* value, int32_t milliseconds);
extern "C" int32_t qtest_unified_metal_open_read(const void* value);
extern "C" int32_t qtest_unified_metal_commit_open();
extern "C" float qtest_unified_metal_snapshot(int64_t index);

extern "C" int32_t qtest_unified_delayed_fill(void* value, int32_t milliseconds) {
    return qtest_unified_metal_delayed_fill(value, milliseconds);
}

extern "C" int32_t qtest_unified_open_read(const void* value) {
    return qtest_unified_metal_open_read(value);
}

extern "C" int32_t qtest_unified_commit_open() {
    return qtest_unified_metal_commit_open();
}

extern "C" float qtest_unified_snapshot(int64_t index) {
    return qtest_unified_metal_snapshot(index);
}

// A plain const device borrow, as any package kernel input takes.
extern "C" int32_t qtest_unified_touch(const void* value) {
    return qcore_tensor_device_handle_const(value) != 0 ? 0 : 1;
}
CPP
cat > "$TMP/gate_packages/unified_gate/native/gate_metal.mm" <<'OBJC'
#import <Metal/Metal.h>
#include <dispatch/dispatch.h>
#include <quidra/native_extension.h>

#include <cstdint>

// Commits on Core's queue, without waiting and without Core tracking it, a
// fill of 0x3F bytes that runs only once a timer opens a gate.
extern "C" int32_t qtest_unified_metal_delayed_fill(void* value, int32_t milliseconds) {
    const auto device = qcore_tensor_device(value);
    if (device < 0) return 1;
    auto queue = (__bridge id<MTLCommandQueue>)reinterpret_cast<void*>(
        static_cast<std::uintptr_t>(qcore_device_queue_handle(device)));
    auto buffer = (__bridge id<MTLBuffer>)reinterpret_cast<void*>(
        static_cast<std::uintptr_t>(qcore_tensor_device_handle(value)));
    if (!queue || !buffer) return 2;
    id<MTLSharedEvent> gate = [[queue device] newSharedEvent];
    if (!gate) return 3;
    id<MTLCommandBuffer> command = [queue commandBuffer];
    [command encodeWaitForEvent:gate value:1];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    [blit fillBuffer:buffer
               range:NSMakeRange(0, qcore_tensor_element_count(value) * sizeof(float))
               value:0x3F];
    [blit endEncoding];
    [command commit];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                                 static_cast<int64_t>(milliseconds) * NSEC_PER_MSEC),
                   dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
                       gate.signaledValue = 1;
                       [gate release];
                   });
    return 0;
}

static id<MTLCommandBuffer> open_command = nil;
static id<MTLBuffer> snapshot_buffer = nil;

// Encodes a copy of the tensor into a command buffer that is committed only
// by a later call (qtest_unified_metal_commit_open).
extern "C" int32_t qtest_unified_metal_open_read(const void* value) {
    const auto device = qcore_tensor_device(value);
    if (device < 0 || open_command) return 1;
    auto queue = (__bridge id<MTLCommandQueue>)reinterpret_cast<void*>(
        static_cast<std::uintptr_t>(qcore_device_queue_handle(device)));
    auto buffer = (__bridge id<MTLBuffer>)reinterpret_cast<void*>(
        static_cast<std::uintptr_t>(qcore_tensor_device_handle_const(value)));
    if (!queue || !buffer) return 2;
    const auto bytes = qcore_tensor_element_count(value) * sizeof(float);
    if (snapshot_buffer) [snapshot_buffer release];
    snapshot_buffer = [[queue device] newBufferWithLength:bytes
                                                  options:MTLResourceStorageModeShared];
    if (!snapshot_buffer) return 3;
    open_command = [[queue commandBuffer] retain];
    id<MTLBlitCommandEncoder> blit = [open_command blitCommandEncoder];
    [blit copyFromBuffer:buffer sourceOffset:0
                toBuffer:snapshot_buffer destinationOffset:0
                    size:bytes];
    [blit endEncoding];
    return 0;
}

extern "C" int32_t qtest_unified_metal_commit_open() {
    if (!open_command) return 1;
    [open_command commit];
    [open_command waitUntilCompleted];
    const bool ok = [open_command status] == MTLCommandBufferStatusCompleted;
    [open_command release];
    open_command = nil;
    return ok ? 0 : 2;
}

extern "C" float qtest_unified_metal_snapshot(int64_t index) {
    if (!snapshot_buffer) return -1.0F;
    return static_cast<const float*>([snapshot_buffer contents])[index];
}
OBJC

cat > "$TMP/gate.qui" <<'QUI'
import probe = unified_gate
int dev = 0

// Nothing tracked pending. Whether `.cpu()` sees the fill depends on timing
// in both modes, but the result must not change later and the source must
// keep the fill after a native borrow.
tensor<real32> idle = tensor.zeros<real32>([65536], gpu = nat(dev))
gpu.sync(nat(dev))
int32 idle_status = probe.delayed_fill(&idle, 400)
tensor<real32> idle_host = idle.cpu()
real32 idle_before = idle_host[65535].item()
int32 idle_touch = probe.touch(idle)
time.sleep(time.seconds(0.8))
real32 idle_after = idle_host[65535].item()
print("idle status {idle_status} {idle_touch}{NL}")
print("idle result unchanged {idle_before == idle_after}{NL}")
print("idle source {idle.cpu()[65535].item()}{NL}")

// Core work pending: the synchronization's barrier waits for the fill.
tensor<real32> busy = tensor.zeros<real32>([65536], gpu = nat(dev))
gpu.sync(nat(dev))
int32 busy_status = probe.delayed_fill(&busy, 200)
tensor<real32> other = tensor.ones<real32>([4], gpu = nat(dev)) + real32(1)
tensor<real32> busy_host = busy.cpu()
real32 busy_before = busy_host[65535].item()
int32 busy_touch = probe.touch(busy)
time.sleep(time.seconds(0.4))
print("busy status {busy_status} {busy_touch}{NL}")
print("busy result {busy_before} {busy_host[65535].item()}{NL}")
print("busy source {busy.cpu()[65535].item()}{NL}")
QUI
cat > "$TMP/gate.expected" <<'TEXT'
idle status 0 0
idle result unchanged true
idle source 0.7470588088035583
busy status 0 0
busy result 0.7470588088035583 0.7470588088035583
busy source 0.7470588088035583
TEXT

cat > "$TMP/open.qui" <<'QUI'
import probe = unified_gate
int dev = 0

// The package encodes a read of `g` in one call and commits it in a later
// one; `g` is gone by then, and `c` is the only value left.
tensor<real32> download(int device)
    tensor<real32> g = tensor.ones<real32>([65536], gpu = nat(device)) * real32(2)
    int32 status = probe.open_read(g)
    print("open status {status}{NL}")
    return g.cpu()

tensor<real32> c = download(dev)
c[0] = real32(99)
print("commit {probe.commit_open()}{NL}")
print("package read {probe.snapshot(0)} {probe.snapshot(65535)}{NL}")
print("host value {c[0].item()} {c[65535].item()}{NL}")
QUI
cat > "$TMP/open.expected" <<'TEXT'
open status 0
commit 0
package read 2.0 2.0
host value 99.0 2.0
TEXT

cat > "$TMP/upload.qui" <<'QUI'
import probe = unified_gate
int dev = 0

tensor<real32> upload(int device)
    tensor<real32> source = tensor.ones<real32>([65536]) * real32(5)
    return source.gpu(nat(device))

// The host value stays alive, so the package's mutable borrow moves the
// device value to memory of its own first.
tensor<real32> host = tensor.ones<real32>([65536]) * real32(4)
tensor<real32> shared = host.gpu(nat(dev))
int32 shared_status = probe.delayed_fill(&shared, 100)
gpu.sync(nat(dev))
print("shared {shared_status} host {host[100].item()} device {shared.cpu()[100].item()}{NL}")

// The host value is gone: the package fills the uploaded memory itself.
tensor<real32> sole = upload(dev)
int32 sole_status = probe.delayed_fill(&sole, 100)
gpu.sync(nat(dev))
print("sole {sole_status} device {sole.cpu()[100].item()}{NL}")
QUI
cat > "$TMP/upload.expected" <<'TEXT'
shared 0 host 4.0 device 0.7470588088035583
sole 0 device 0.7470588088035583
TEXT

# Diagnostics: identical message, location and exit status in both modes.
cat > "$TMP/error-index.qui" <<'QUI'
tensor<real32> host = tensor.ones<real32>([32768])
tensor<real32> device = host.gpu(7)
print(device.device())
QUI
cat > "$TMP/error-tracked-cpu.qui" <<'QUI'
tensor<real32> device = (tensor.ones<real32>([32768]) * real32(2)).gpu(0).track()
tensor<real32> host = device.cpu()
print(host.device())
QUI
cat > "$TMP/error-tracked-gpu.qui" <<'QUI'
tensor<real32> host = tensor.ones<real32>([32768]).track()
tensor<real32> device = host.gpu(0)
print(device.device())
QUI
cat > "$TMP/error-uninitialized.qui" <<'QUI'
tensor<real32> partial = tensor<real32>([4])
partial[0] = real32(1)
tensor<real32> device = partial.gpu(0)
tensor<real32> back = device.cpu()
print(back[0].item())
print(NL)
print(back[1].item())
QUI
cat > "$TMP/error-deferred.qui" <<'QUI'
tensor<int32> value = tensor.ones<int32>([32768], gpu = 0)
tensor<int32> invalid = value / int32(0)
tensor<int32> host = invalid.cpu()
print("after cpu")
print(NL)
gpu.sync(0)
print("after sync")
print(NL)
QUI
error_programs=(error-index error-tracked-cpu error-tracked-gpu error-uninitialized error-deferred)

build() {
    local name="$1" packages="${2:-$TMP/packages}"
    if ! QUIDRA_PACKAGE_PATH="$packages" "$QUIDRA" build "$TMP/$name.qui" \
            -o "$TMP/$name" >"$TMP/$name.build.log" 2>&1; then
        cat "$TMP/$name.build.log" >&2
        fail "building $name failed"
    fi
}

# run NAME LABEL [ENV...]: runs the built program with the given environment,
# leaving stdout, stderr and the status in $TMP/NAME.LABEL.{out,err,status}.
run() {
    local name="$1" label="$2"
    shift 2
    set +e
    env "$@" "$TMP/$name" >"$TMP/$name.$label.out" 2>"$TMP/$name.$label.err"
    echo $? >"$TMP/$name.$label.status"
    set -e
}

expect_output() {
    local name="$1" label="$2"
    if [[ "$(cat "$TMP/$name.$label.status")" != "0" ]]; then
        cat "$TMP/$name.$label.err" >&2
        fail "$name ($label) failed"
    fi
    if ! diff -u "$TMP/$name.expected" "$TMP/$name.$label.out" >&2; then
        fail "$name ($label) output differs from the expected values"
    fi
}

# The copy path is the reference: status, stdout and the diagnostic (stderr
# without the stats line) must match exactly.
expect_same_as_copy() {
    local name="$1" label="$2" reference="$3"
    for part in status out; do
        if ! diff -u "$TMP/$name.$reference.$part" "$TMP/$name.$label.$part" >&2; then
            fail "$name ($label) $part differs from the copy path"
        fi
    done
    if ! diff -u <(grep -v '^quidra unified memory:' "$TMP/$name.$reference.err") \
                 <(grep -v '^quidra unified memory:' "$TMP/$name.$label.err") >&2; then
        fail "$name ($label) diagnostics differ from the copy path"
    fi
}

counter() {
    local name="$1" label="$2" key="$3"
    sed -n "s/.* $key=\([0-9]*\).*/\1/p" "$TMP/$name.$label.err" | tail -1
}

expect_counter_zero() {
    local name="$1" label="$2" key="$3"
    local value
    value="$(counter "$name" "$label" "$key")"
    if [[ "$value" != "0" ]]; then
        cat "$TMP/$name.$label.err" >&2
        fail "$name ($label) expected $key=0, got ${value:-missing}"
    fi
}

expect_counter_positive() {
    local name="$1" label="$2" key="$3"
    local value
    value="$(counter "$name" "$label" "$key")"
    if [[ -z "$value" || "$value" -eq 0 ]]; then
        cat "$TMP/$name.$label.err" >&2
        fail "$name ($label) did not take the unified path ($key=${value:-missing})"
    fi
}

build semantics
build autograd
build borrow
build nowait
build idle
for program in "${error_programs[@]}"; do build "$program"; done

ran_any=0

# ------------------------------------------------------------ fake GPU views
fake_info="$(QUIDRA_TEST_FAKE_GPU_COUNT=1 "$QUIDRA" gpu 2>&1 || true)"
if grep -Fq "backend: TEST" <<<"$fake_info"; then
    fake=(QUIDRA_TEST_FAKE_GPU_COUNT=1)
    views=(QUIDRA_TEST_FAKE_GPU_UNIFIED=1 QUIDRA_UNIFIED_MEMORY_STATS=1)
    for name in semantics autograd borrow nowait idle; do
        run "$name" fake-copy "${fake[@]}" QUIDRA_TEST_FAKE_GPU_UNIFIED=1 QUIDRA_UNIFIED_MEMORY=0
        run "$name" fake-plain "${fake[@]}"
        run "$name" fake-views "${fake[@]}" "${views[@]}"
        expect_output "$name" fake-copy
        expect_output "$name" fake-plain
        expect_output "$name" fake-views
    done
    for key in cpu_views device_views wraps; do
        expect_counter_positive semantics fake-views "$key"
        expect_counter_positive autograd fake-views "$key"
    done
    # QUIDRA_UNIFIED_MEMORY=upload keeps only the `.gpu(n)` views.
    run semantics fake-upload "${fake[@]}" "${views[@]}" QUIDRA_UNIFIED_MEMORY=upload
    expect_output semantics fake-upload
    expect_counter_zero semantics fake-upload cpu_views
    expect_counter_positive semantics fake-upload device_views
    for key in relocations host_waits; do
        expect_counter_positive borrow fake-views "$key"
    done
    for program in "${error_programs[@]}"; do
        run "$program" fake-copy "${fake[@]}" QUIDRA_TEST_FAKE_GPU_UNIFIED=1 QUIDRA_UNIFIED_MEMORY=0
        run "$program" fake-views "${fake[@]}" "${views[@]}"
        expect_same_as_copy "$program" fake-views fake-copy
    done
    ran_any=1
    echo "unified memory (fake GPU): ok"
fi

# ---------------------------------------------------------- real Metal views
require_metal=0
if [[ "${QUIDRA_REQUIRE_REAL_GPU:-0}" == "1" &&
      "${QUIDRA_REQUIRE_GPU_BACKEND:-}" == "Metal" ]]; then
    require_metal=1
fi
real_info="$(env -u QUIDRA_TEST_FAKE_GPU_COUNT "$QUIDRA" gpu 2>&1 || true)"
gpu0_block="$(awk '
    $0 == "GPU 0" { found = 1; print; next }
    found && /^GPU [0-9]+$/ { exit }
    found { print }
' <<<"$real_info")"
if grep -Fq "backend: Metal" <<<"$gpu0_block"; then
    real=(-u QUIDRA_TEST_FAKE_GPU_COUNT -u QUIDRA_TEST_FAKE_GPU_UNIFIED)
    for name in gate open upload; do build "$name" "$TMP/gate_packages"; done
    for name in semantics autograd borrow nowait idle gate open upload; do
        run "$name" metal-copy "${real[@]}" QUIDRA_UNIFIED_MEMORY=0
        run "$name" metal-views "${real[@]}" QUIDRA_UNIFIED_MEMORY_STATS=1
        expect_output "$name" metal-views
        # The copy path of `upload` waits for the package only while its
        # staging copy is still pending at gpu.sync (it nearly always is, but
        # that is timing), so only its status is checked.
        if [[ "$name" == "upload" ]]; then
            if [[ "$(cat "$TMP/$name.metal-copy.status")" != "0" ]]; then
                cat "$TMP/$name.metal-copy.err" >&2
                fail "$name (metal-copy) failed"
            fi
        else
            expect_output "$name" metal-copy
        fi
    done
    metal_views="$(counter semantics metal-views cpu_views)"
    if [[ "${metal_views:-0}" -gt 0 || "${QUIDRA_REQUIRE_UNIFIED_MEMORY:-0}" == "1" ]]; then
        for key in cpu_views device_views wraps; do
            expect_counter_positive semantics metal-views "$key"
            expect_counter_positive autograd metal-views "$key"
        done
        run semantics metal-upload "${real[@]}" QUIDRA_UNIFIED_MEMORY_STATS=1 QUIDRA_UNIFIED_MEMORY=upload
        expect_output semantics metal-upload
        expect_counter_zero semantics metal-upload cpu_views
        expect_counter_positive semantics metal-upload device_views
        for key in relocations host_waits; do
            expect_counter_positive borrow metal-views "$key"
        done
        # Tensors lent to package code are never viewed by `.cpu()`.
        expect_counter_zero gate metal-views cpu_views
        expect_counter_zero open metal-views cpu_views
        expect_counter_positive upload metal-views wraps
        expect_counter_positive upload metal-views relocations
        expect_counter_positive nowait metal-views cpu_views
        expect_counter_positive idle metal-views cpu_views
        expect_counter_zero nowait metal-views host_waits
    else
        echo "unified memory: Metal gpu(0) has no unified memory; only the copies were checked"
    fi
    for program in "${error_programs[@]}"; do
        run "$program" metal-copy "${real[@]}" QUIDRA_UNIFIED_MEMORY=0
        run "$program" metal-views "${real[@]}" QUIDRA_UNIFIED_MEMORY_STATS=1
        expect_same_as_copy "$program" metal-views metal-copy
    done
    ran_any=1
    echo "unified memory (Metal): ok"
elif [[ "$require_metal" == "1" ]]; then
    printf '%s\n' "$real_info" >&2
    fail "a real Metal gpu(0) is required but unavailable"
fi

if [[ "$ran_any" == "0" ]]; then
    echo "unified memory: skipped (no fake GPU backend and no Metal GPU)"
fi
