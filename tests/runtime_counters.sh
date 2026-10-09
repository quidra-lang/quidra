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
for step in range(3)
    tensor<real32> scaled = weight * weight
    tensor<real32> loss = scaled.gather([0], []) + scaled.gather([3], [])
    weight.clear_grad()
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

python3 - "$TMP/steps.jsonl" <<'PY'
import json, sys
records = [json.loads(line) for line in open(sys.argv[1])]
steps = [r for r in records if r["record"] == "step"]
totals = [r for r in records if r["record"] == "total"]
assert len(totals) == 1, records
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
# The only host read is the final .cpu() of the gradient.
assert sum(r["readbacks"] for r in steps[:3]) == 0, steps
assert steps[3]["readbacks"] == 1, steps[3]
print("counter report shape: ok")
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
