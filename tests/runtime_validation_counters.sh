#!/usr/bin/env bash
# Observe actual tensor safety checks, including the check that fails, without
# conflating them with pure shape/device metadata queries.
set -euo pipefail
QUIDRA="${1:?usage: runtime_validation_counters.sh /path/to/quidra}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/cache"
export QUIDRA_TEST_FAKE_GPU_COUNT=1

cat > "$TMP/reshape.qui" <<'QUI'
extern void mark(const string label) = "qcore_counters_mark"
tensor<real32> values = tensor.ones<real32>([4])
nat[] shape = [5]
mark("created")
tensor<real32> invalid = values.reshape(shape)
print("{invalid.shape()[0]}{NL}")
QUI
set +e
QUIDRA_COUNTERS="$TMP/reshape.jsonl" "$QUIDRA" run "$TMP/reshape.qui" \
    >"$TMP/reshape.out" 2>"$TMP/reshape.err"
status=$?
set -e
if [[ "$status" != 101 ]]; then
    cat "$TMP/reshape.err" >&2
    echo "reshape returned $status instead of runtime-failure status 101" >&2
    exit 1
fi
grep -Fq 'reshape cannot change the number of elements' "$TMP/reshape.err"

cat > "$TMP/track.qui" <<'QUI'
extern void mark(const string label) = "qcore_counters_mark"
tensor<real32> values = tensor.ones<real32>([4])
mark("created")
tensor<real32> tracked = values.track()
mark("tracked")
print("{tracked.untrack()[0].item()}{NL}")
QUI
QUIDRA_COUNTERS="$TMP/track.jsonl" "$QUIDRA" run "$TMP/track.qui" >"$TMP/track.out"
[[ "$(cat "$TMP/track.out")" == 1 ]]

cat > "$TMP/materialize.qui" <<'QUI'
extern void mark(const string label) = "qcore_counters_mark"
tensor<real32> values = tensor.ones<real32>([2, 2], gpu = 0)
mark("created")
tensor<real32> view = values.transpose(0, 1)
tensor<real32> dense = view.contiguous()
mark("materialized")
print("{dense.cpu()[0, 0].item()}{NL}")
QUI
QUIDRA_COUNTERS="$TMP/materialize.jsonl" "$QUIDRA" run "$TMP/materialize.qui" >"$TMP/materialize.out"
[[ "$(cat "$TMP/materialize.out")" == 1 ]]

python3 - "$TMP" <<'PY'
import json, pathlib, sys
root = pathlib.Path(sys.argv[1])
def steps(name):
    return [r for line in (root / (name + '.jsonl')).read_text().splitlines()
            if (r := json.loads(line))['record'] == 'step']
reshape = steps('reshape')
assert [r['event'] for r in reshape] == ['mark', 'exit'], reshape
assert reshape[1]['runtime_shape_validations'] > 0, reshape
track = steps('track')
assert track[1]['label'] == 'tracked' and track[1]['autograd_nodes'] == 1, track
assert track[1]['runtime_device_checks'] > 0, track
dense = steps('materialize')
assert dense[1]['label'] == 'materialized', dense
assert dense[1]['runtime_shape_validations'] > 0, dense
assert dense[1]['runtime_device_checks'] > 0, dense
PY
echo 'runtime validation counters: ok'
