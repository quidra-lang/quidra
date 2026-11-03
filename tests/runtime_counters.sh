#!/usr/bin/env bash
# Runtime counters (QUIDRA_COUNTERS): report shape, step boundaries and golden
# per-step counts on the fake test GPU. Counting only observes; the program
# output with counting on equals the output with counting off.
set -euo pipefail

QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/quidra-cache"  # a run cache of this suite run only

export QUIDRA_TEST_FAKE_GPU_COUNT=1

cat > "$TMP/steps.qui" <<'QUI'
tensor<real32> weight = tensor.ones<real32>([4], gpu = 0).track()
for 0 <= nat step < 3
    tensor<real32> scaled = weight * weight
    tensor<real32> loss = scaled.gather([0], []) + scaled.gather([3], [])
    .weight.clear_grad()
    loss.backward(&weight)
print(weight.grad.cpu()[0].item())
print(NL)
QUI

plain="$("$QUIDRA" run "$TMP/steps.qui")"
counted="$(QUIDRA_COUNTERS="$TMP/steps.jsonl" "$QUIDRA" run "$TMP/steps.qui")"
if [[ "$plain" != "$counted" ]]; then
    echo "counting changed program output: '$plain' vs '$counted'" >&2
    exit 1
fi
if [[ ! -s "$TMP/steps.jsonl" ]]; then
    echo "QUIDRA_COUNTERS wrote no report" >&2
    exit 1
fi

# Compiler-only commands must leave the program's completed report intact.
cp "$TMP/steps.jsonl" "$TMP/steps.before-cli.jsonl"
QUIDRA_COUNTERS="$TMP/steps.jsonl" "$QUIDRA" gpu >/dev/null
cmp "$TMP/steps.before-cli.jsonl" "$TMP/steps.jsonl"

python3 - "$TMP/steps.jsonl" <<'PY'
import json, sys
records = [json.loads(line) for line in open(sys.argv[1])]
steps = [r for r in records if r["record"] == "step"]
totals = [r for r in records if r["record"] == "total"]
assert len(totals) == 1, records
observation_keys = (
    "package_dispatches", "host_tensor_allocations", "host_tensor_allocation_bytes",
    "autograd_nodes", "autograd_custom_nodes", "autograd_saved_tensors",
    "autograd_formulas", "autograd_pruned", "runtime_shape_validations",
    "runtime_broadcast_validations", "runtime_device_checks", "cpu_parallel_regions",
    "cpu_parallel_serial_regions", "metal_pipeline_compiles_timed", "unified_views",
    "unified_copies",
)
assert all(key in totals[0] for key in observation_keys), totals[0]
# One record before each of the three backward() calls, one at exit.
events = [r["event"] for r in steps]
assert events == ["backward", "backward", "backward", "exit"], events
assert [r["step"] for r in steps] == [0, 1, 2, 3], steps
names = [k for k in totals[0] if k not in ("record", "wait_sites", "refusals")]
for name in names:
    total = sum(r[name] for r in steps)
    assert total == totals[0][name], (name, total, totals[0][name])
# Steady-state steps (1 and 2) repeat the same device work.
keys = ("core_dispatches", "allocations", "uploads", "readbacks", "fills",
        "device_copies", "validation_slots")
assert all(steps[1][k] == steps[2][k] for k in keys), (steps[1], steps[2])
# Golden steady-state step on the fake GPU: forward (mul, two gathers, add),
# backward (seed, add, two gather-backwards, mul) and their allocations. A
# change of these numbers must be the stated purpose of the change.
golden = {"core_dispatches": 9, "allocations": 8, "fills": 0, "uploads": 0,
          "readbacks": 0, "device_copies": 0, "validation_slots": 0}
for step in steps[1:3]:
    measured = {k: step[k] for k in golden}
    assert measured == golden, (measured, golden)
    assert step["autograd_nodes"] == 4, step
    assert step["autograd_saved_tensors"] == 4, step
    assert step["autograd_formulas"] == 4, step
    assert step["autograd_pruned"] == 0, step
    assert step["host_tensor_allocations"] == 0, step
# The only host read is the final .cpu() of the gradient.
assert sum(r["readbacks"] for r in steps[:3]) == 0, steps
assert steps[3]["readbacks"] == 1, steps[3]
print("counter report shape: ok")
PY

cat > "$TMP/host.qui" <<'QUI'
tensor<real32> values = tensor.ones<real32>([4])
tensor<real32> scaled = values * 2.0
tensor<real32> empty = tensor.zeros<real32>([0])
print("{scaled[0].item()} {empty.shape()[0]}{NL}")
QUI
QUIDRA_COUNTERS="$TMP/host.jsonl" "$QUIDRA" run "$TMP/host.qui" >/dev/null
python3 - "$TMP/host.jsonl" <<'PY'
import json, sys
total = [json.loads(line) for line in open(sys.argv[1]) if '"record":"total"' in line][0]
assert total["host_tensor_allocations"] == 2, total
assert total["host_tensor_allocation_bytes"] == 32, total
assert total["autograd_nodes"] == 0, total
PY

mkdir -p "$TMP/packages/counter_probe/native"
cat > "$TMP/packages/counter_probe/quidra.package" <<'MANIFEST'
name = counter_probe
version = 0.1.0
native.source.bridge = native/bridge.cpp
MANIFEST
cat > "$TMP/packages/counter_probe/main.qui" <<'QUI'
extern int32 borrow(const tensor<real32> value) = "qtest_counter_borrow"
QUI
cat > "$TMP/packages/counter_probe/native/bridge.cpp" <<'CPP'
#include <quidra/native_extension.h>
extern "C" int32_t qtest_counter_borrow(const void* value) {
    const auto allocations = qcore_counter_value("host_tensor_allocations");
    const auto bytes = qcore_counter_value("host_tensor_allocation_bytes");
    if (!qcore_tensor_cpu_data_const(value)) return 1;
    if (qcore_counter_value("host_tensor_allocations") - allocations != 1) return 2;
    if (qcore_counter_value("host_tensor_allocation_bytes") - bytes != 65536) return 3;
    return 0;
}
CPP
cat > "$TMP/borrow-counters.qui" <<'QUI'
import counter_probe
tensor<real32> device = tensor.ones<real32>([16384], gpu = 0)
tensor<real32> host = device.cpu()
print("{counter_probe.borrow(host)} {device.shape()[0]}{NL}")
QUI
borrowed="$(QUIDRA_COUNTERS="$TMP/borrow.jsonl" QUIDRA_TEST_FAKE_GPU_UNIFIED=1 \
    QUIDRA_UNIFIED_MEMORY=1 QUIDRA_PACKAGE_PATH="$TMP/packages" \
    "$QUIDRA" run "$TMP/borrow-counters.qui")"
if [[ "$borrowed" != "0 16384" ]]; then
    echo "native CPU view relocation allocation was not observed: $borrowed" >&2
    exit 1
fi

cat > "$TMP/transfers.qui" <<'QUI'
tensor<real32> empty = tensor.zeros<real32>([0])
tensor<real32> device_empty = empty.gpu(0)
tensor<real32> host_empty = device_empty.cpu()
tensor<real32> uninitialized = tensor<real32>([4])
tensor<real32> device_uninitialized = uninitialized.gpu(0)
tensor<real32> host_uninitialized = device_uninitialized.cpu()
tensor<real32> values = tensor.ones<real32>([4])
tensor<real32> device_values = values.gpu(0)
tensor<real32> host_values = device_values.cpu()
print("{host_empty.shape()[0]} {host_uninitialized.shape()[0]} {host_values[0].item()}{NL}")
QUI
QUIDRA_COUNTERS="$TMP/transfers.jsonl" QUIDRA_UNIFIED_MEMORY=0 \
    "$QUIDRA" run "$TMP/transfers.qui" >/dev/null
python3 - "$TMP/transfers.jsonl" <<'PY'
import json, sys
total = [json.loads(line) for line in open(sys.argv[1]) if '"record":"total"' in line][0]
assert total["unified_copies"] == 2, total
PY

QUIDRA_COUNTERS="$TMP/none.jsonl" QUIDRA_COUNTERS_STEP=none \
    "$QUIDRA" run "$TMP/steps.qui" >/dev/null
python3 - "$TMP/none.jsonl" <<'PY'
import json, sys
records = [json.loads(line) for line in open(sys.argv[1])]
assert [r["record"] for r in records] == ["step", "total"], records
assert records[0]["event"] == "exit", records
PY

echo "runtime counters: ok"
