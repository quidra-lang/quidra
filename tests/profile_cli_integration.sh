#!/usr/bin/env bash
# Runs after main/cache/runtime integration. Every report and cache is isolated.
set -euo pipefail
QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/cache"
export QUIDRA_TEST_FAKE_GPU_COUNT=1
export QUIDRA_PROFILE="$TMP/compiler-profile.json"
export QUIDRA_TIMELINE="$TMP/compiler-timeline.json"
export QUIDRA_PROFILE_SPLIT=parent-marker
export QUIDRA_METAL_CAPTURE=step:0
export MTL_CAPTURE_ENABLED=0
printf 'compiler sentinel\n' > "$QUIDRA_PROFILE"
printf 'compiler sentinel\n' > "$QUIDRA_TIMELINE"

# Observe native compile/link invocations, while retaining the existing driver.
export QUIDRA_PROFILE_TEST_REAL_CLANG="${QUIDRA_CLANGXX:-$(command -v clang++)}"
export QUIDRA_PROFILE_TEST_BUILD_LOG="$TMP/builds.jsonl"
mkdir "$TMP/driver"
cat > "$TMP/driver/clang-observer" <<'PY'
#!/usr/bin/env python3
import json, os, sys
arguments = sys.argv[1:]
if (any(argument.endswith('.ll') for argument in arguments) or
        any(arguments[index:index + 3] == ['-x', 'ir', '-']
            for index in range(len(arguments)))):
    with open(os.environ['QUIDRA_PROFILE_TEST_BUILD_LOG'], 'a') as output:
        output.write(json.dumps({key: os.environ.get(key) for key in
            ('QUIDRA_PROFILE', 'QUIDRA_TIMELINE', 'QUIDRA_PROFILE_SPLIT',
             'QUIDRA_METAL_CAPTURE', 'MTL_CAPTURE_ENABLED')}) + '\n')
os.execv(os.environ['QUIDRA_PROFILE_TEST_REAL_CLANG'],
         [os.environ['QUIDRA_PROFILE_TEST_REAL_CLANG'], *sys.argv[1:]])
PY
chmod +x "$TMP/driver/clang-observer"
export QUIDRA_CLANGXX="$TMP/driver/clang-observer"
cat > "$TMP/program.qui" <<'QUI'
cli args
    string token = argument()
    int status = option(default = 0)
print("ARG=")
print(args.token)
print(NL)
print(environment.has("QUIDRA_PROFILE"))
print(NL)
print(environment.has("QUIDRA_TIMELINE"))
print(NL)
print("CAPTURE_FLAG=")
match environment.get("MTL_CAPTURE_ENABLED")
    string capture_flag
        print(capture_flag)
    none
        print("unset")
print(NL)
tensor<real32> weight = tensor.ones<real32>([4], gpu = 0).track()
for 0 <= nat step < 3
    tensor<real32> loss = (weight * weight).gather([0], [])
    .weight.clear_grad()
    loss.backward(&weight)
print(weight.grad.cpu()[0].item())
print(NL)
process.exit(args.status)
QUI
# Let file and driver-directory change times clear the cache's two-second
# recent-input guard. Reports stay outside the observed driver directory.
sleep 3
"$QUIDRA" profile "$TMP/program.qui" --top 2 --roofline -- 'two words' \
    > "$TMP/first.out" 2> "$TMP/first.err"
[[ "$(head -n 1 "$TMP/first.out")" == 'ARG=two words' ]]
if [[ "$(uname -s)" == Darwin ]]; then
    expected_capture_flag=1
else
    expected_capture_flag=0
fi
if ! grep -q "^CAPTURE_FLAG=$expected_capture_flag$" "$TMP/first.out"; then
    cat "$TMP/first.out" "$TMP/first.err"
    printf 'unexpected generated-child capture flag (expected %s)\n' "$expected_capture_flag" >&2
    exit 1
fi
grep -q 'Step medians:' "$TMP/first.out"
grep -q 'Top host' "$TMP/first.out"
grep -q 'Top GPU' "$TMP/first.out"
grep -q 'Roofline unavailable' "$TMP/first.out"
cp "$TMP/builds.jsonl" "$TMP/builds.first.jsonl"

# New output paths, split mode and program args must still use the same T1 entry.
set +e
QUIDRA_METAL_CAPTURE=step:1 "$QUIDRA" profile "$TMP/program.qui" --top 1 --split \
    --trace "$TMP/second-trace.json" --timeline "$TMP/second-timeline.json" \
    -- 'cached arguments' --status 7 > "$TMP/second.out" 2> "$TMP/second.err"
status=$?
set -e
[[ "$status" -eq 7 ]]
[[ "$(head -n 1 "$TMP/second.out")" == 'ARG=cached arguments' ]]
cmp "$TMP/builds.first.jsonl" "$TMP/builds.jsonl"

# An invalid runtime request keeps the parent's flag and still reuses T1.
QUIDRA_METAL_CAPTURE=-1 "$QUIDRA" profile "$TMP/program.qui" \
    --trace "$TMP/invalid-trace.json" --timeline "$TMP/invalid-timeline.json" \
    -- 'invalid capture' > "$TMP/invalid.out" 2> "$TMP/invalid.err"
grep -q '^CAPTURE_FLAG=0$' "$TMP/invalid.out"
cmp "$TMP/builds.first.jsonl" "$TMP/builds.jsonl"
[[ "$MTL_CAPTURE_ENABLED" == 0 && "$QUIDRA_METAL_CAPTURE" == step:0 ]]

python3 - "$TMP" <<'PY'
import json, pathlib, sys
root = pathlib.Path(sys.argv[1])
rows = list(map(json.loads, (root/'builds.jsonl').read_text().splitlines()))
assert rows, 'no native compilation observed'
for row in rows:
    assert row == {'QUIDRA_PROFILE': str(root/'compiler-profile.json'),
                   'QUIDRA_TIMELINE': str(root/'compiler-timeline.json'),
                   'QUIDRA_PROFILE_SPLIT': 'parent-marker',
                   'QUIDRA_METAL_CAPTURE': 'step:0', 'MTL_CAPTURE_ENABLED': '0'}, row
for name in ('compiler-profile.json', 'compiler-timeline.json'):
    assert (root/name).read_text() == 'compiler sentinel\n'
for name, schema in (('second-trace.json', 2), ('second-timeline.json', 1)):
    report = json.loads((root/name).read_text())
    assert report['schema'] == schema and report['clock_units'] == 'ns'
trace = json.loads((root/'second-trace.json').read_text())
assert trace['configuration']['split_requested'] is True
assert trace['configuration']['capture_step'] == 1
assert trace['capture']['requested_step'] == 1
invalid = json.loads((root/'invalid-trace.json').read_text())
assert invalid['configuration']['capture_step'] is None
assert invalid.get('capture') is None
assert trace['aggregate'], trace
first = (root/'first.out').read_text().splitlines()
for prefix, schema in (('Trace: ', 2), ('Timeline: ', 1)):
    path = pathlib.Path(next(line.removeprefix(prefix) for line in first if line.startswith(prefix)))
    assert path.is_relative_to((root/'cache'/'profiles').resolve()), path
    assert json.loads(path.read_text())['schema'] == schema
captures = [json.loads(path.read_text()) for path in (root/'cache'/'profiles').rglob('capture.json')]
assert sorted(report['capture']['requested_step'] for report in captures) == [0, 1], captures
PY

# Immediate runtime failure must still leave a trace for extraction.
cat > "$TMP/failure.qui" <<'QUI'
tensor<real32> values = tensor.ones<real32>([4])
nat[] shape = [5]
tensor<real32> invalid = values.reshape(shape)
print(invalid.shape()[0])
QUI
set +e
"$QUIDRA" profile "$TMP/failure.qui" --trace "$TMP/failure-trace.json" \
    --timeline "$TMP/failure-timeline.json" > "$TMP/failure.out" 2> "$TMP/failure.err"
status=$?
set -e
[[ "$status" -eq 101 ]]
grep -Fq 'reshape cannot change the number of elements' "$TMP/failure.err"
grep -q 'Top host' "$TMP/failure.out"
python3 - "$TMP/failure-trace.json" "$TMP/failure-timeline.json" <<'PY'
import json, sys
for path, schema in zip(sys.argv[1:], (2, 1)):
    report = json.load(open(path))
    assert report['schema'] == schema
PY
printf 'profile CLI cache, child environment and failure reports: ok\n'
