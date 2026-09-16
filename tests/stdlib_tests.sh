#!/usr/bin/env bash
set -euo pipefail
set -x
QUIDRA="$1"
ROOT="$2"
TMP="$(mktemp -d)"
trap 'if [[ -n "${HTTP_PID:-}" ]]; then kill "$HTTP_PID" 2>/dev/null || true; fi; rm -rf "$TMP"' EXIT

cat > "$TMP/cli.qui" <<'QUI'
cli args
    string source = argument()
    int count = option(default = 1)
    bool verbose = flag()

print(args.source)
print(args.count)
print(args.verbose)
QUI

[[ "$("$QUIDRA" "$TMP/cli.qui" image.jpg --count 5 --verbose)" == $'image.jpg\n5\ntrue' ]]
[[ "$("$QUIDRA" "$TMP/cli.qui" image.jpg)" == $'image.jpg\n1\nfalse' ]]
[[ "$("$QUIDRA" run "$TMP/cli.qui" -- image.jpg --count 2)" == $'image.jpg\n2\nfalse' ]]

"$QUIDRA" inspect "$TMP/cli.qui" --kind cli > "$TMP/cli-inspect.json"
python3 - "$TMP/cli-inspect.json" <<'PY'
import json, sys
x = json.load(open(sys.argv[1]))
assert x["ok"] is True
assert len(x["nodes"]) == 1
assert x["nodes"][0]["kind"] == "cli"
PY

for mode in missing invalid unknown duplicate; do
    set +e
    case "$mode" in
        missing)
            "$QUIDRA" "$TMP/cli.qui" >"$TMP/$mode.out" 2>"$TMP/$mode.err"
            ;;
        invalid)
            "$QUIDRA" "$TMP/cli.qui" image.jpg --count nope >"$TMP/$mode.out" 2>"$TMP/$mode.err"
            ;;
        unknown)
            "$QUIDRA" "$TMP/cli.qui" image.jpg --wat >"$TMP/$mode.out" 2>"$TMP/$mode.err"
            ;;
        duplicate)
            "$QUIDRA" "$TMP/cli.qui" image.jpg --count 2 --count 3 >"$TMP/$mode.out" 2>"$TMP/$mode.err"
            ;;
    esac
    rc=$?
    set -e
    [[ "$rc" -eq 2 ]]
    grep -q 'Quidra CLI error:' "$TMP/$mode.err"
done

cat > "$TMP/file.qui" <<QUI
auto written = file.write("$TMP/source.txt", "hello")
match written
    void
        print("write")
    error e
        print(e)

auto read = file.read("$TMP/source.txt")
match read
    string value
        print(value)
    error e
        print(e)

auto present = file.exists("$TMP/source.txt")
match present
    bool value
        print(value)
    error e
        print(e)

auto copied = file.copy("$TMP/source.txt", "$TMP/copied.txt")
match copied
    void
        print("copy")
    error e
        print(e)

auto moved = file.move("$TMP/copied.txt", "$TMP/moved.txt")
match moved
    void
        print("move")
    error e
        print(e)

auto removed = file.remove("$TMP/moved.txt")
match removed
    void
        print("remove")
    error e
        print(e)

auto absent = file.exists("$TMP/moved.txt")
match absent
    bool value
        print(value)
    error e
        print(e)

auto directory = file.mkdir("$TMP/new-directory")
match directory
    void
        print("mkdir")
    error e
        print(e)
QUI

[[ "$("$QUIDRA" "$TMP/file.qui")" == $'write\nhello\ntrue\ncopy\nmove\nremove\nfalse\nmkdir' ]]
[[ "$(cat "$TMP/source.txt")" == "hello" ]]
[[ -d "$TMP/new-directory" ]]

python3 - "$TMP/source.bin" <<'PY'
import sys
open(sys.argv[1], "wb").write(bytes([0, 255, 65, 10, 128]))
PY
cat > "$TMP/file-bytes.qui" <<QUI
auto raw = file.read_bytes("$TMP/source.bin")
match raw
    bytes value
        print(len(value))
        print(value[0])
        print(value[1])
        value[2] = 66
        auto saved = file.write_bytes("$TMP/copied.bin", value)
        match saved
            void
                print("bytes")
            error problem
                print(problem)
    error problem
        print(problem)
QUI
[[ "$("$QUIDRA" "$TMP/file-bytes.qui")" == $'5\n0\n255\nbytes' ]]
python3 - "$TMP/copied.bin" <<'PY'
import sys
data = open(sys.argv[1], "rb").read()
assert data == bytes([0, 255, 66, 10, 128]), data
PY

printf 'b' > "$TMP/new-directory/b.txt"
printf 'a' > "$TMP/new-directory/a.txt"
cat > "$TMP/file-list.qui" <<QUI
auto listed = file.list("$TMP/new-directory")
match listed
    string[] entries
        print(len(entries))
        print(entries[0].ends_with("/a.txt"))
        print(entries[1].ends_with("/b.txt"))
    error problem
        print(problem)
QUI
[[ "$("$QUIDRA" "$TMP/file-list.qui")" == $'2\ntrue\ntrue' ]]

cat > "$TMP/file-list-missing.qui" <<QUI
auto listed = file.list("$TMP/no-such-directory")
match listed
    string[] entries
        print(len(entries))
    error problem
        print(problem)
QUI
[[ "$("$QUIDRA" "$TMP/file-list-missing.qui")" == "file operation failed" ]]

cat > "$TMP/missing-file.qui" <<QUI
auto read = file.read("$TMP/does-not-exist.txt")
match read
    string value
        print(value)
    error e
        print(e)
QUI
[[ "$("$QUIDRA" "$TMP/missing-file.qui")" == "file operation failed" ]]

python3 - "$TMP/invalid-utf8.txt" <<'PY'
import sys
open(sys.argv[1], "wb").write(b"\xc0\xaf")
PY
cat > "$TMP/invalid-utf8-file.qui" <<QUI
auto read = file.read("$TMP/invalid-utf8.txt")
match read
    string value
        print("unexpected")
    error problem
        print(problem)
QUI
[[ "$("$QUIDRA" "$TMP/invalid-utf8-file.qui")" == "file operation failed" ]]

python3 - "$TMP/nul-text.txt" <<'PY'
import sys
open(sys.argv[1], "wb").write(b"A\x00B")
PY
cat > "$TMP/nul-text-file.qui" <<QUI
auto read = file.read("$TMP/nul-text.txt")
match read
    string value
        print("unexpected")
    error problem
        print(problem)
QUI
[[ "$("$QUIDRA" "$TMP/nul-text-file.qui")" == "file operation failed" ]]

cat > "$TMP/local-file.qui" <<'QUI'
int answer()
    return 42
QUI
cat > "$TMP/local-import.qui" <<'QUI'
import local = "./local-file.qui"
print(local.answer())
QUI
(
  cd "$TMP"
  [[ "$("$QUIDRA" local-import.qui)" == "42" ]]
)

cat > "$TMP/not-standard.qui" <<'QUI'
int answer()
    return 99
QUI
cat > "$TMP/bare-local-import.qui" <<'QUI'
import not_standard
print("unreachable")
QUI
set +e
(
  cd "$TMP"
  "$QUIDRA" check bare-local-import.qui --json
) >"$TMP/bare-local.json"
rc=$?
set -e
[[ "$rc" -eq 1 ]]
grep -q 'PACKAGE_NOT_INSTALLED' "$TMP/bare-local.json"

cat > "$TMP/environment.qui" <<'QUI'
auto configured = environment.get("QUIDRA_TEST_ENV")
match configured
    string value
        print(value)
    none
        print("missing")

print(environment.has("QUIDRA_TEST_ENV"))

auto absent = environment.get("QUIDRA_TEST_ENV_DEFINITELY_MISSING")
match absent
    string value
        print(value)
    none
        print("none")

print(environment.has("QUIDRA_TEST_ENV_DEFINITELY_MISSING"))
QUI

environment_output="$(QUIDRA_TEST_ENV=hello "$QUIDRA" "$TMP/environment.qui")"
environment_expected="$(printf 'hello\ntrue\nnone\nfalse')"
[[ "$environment_output" == "$environment_expected" ]]

python3 - "$QUIDRA" "$TMP/environment.qui" <<'PY'
import os
import subprocess
import sys

env = os.environb.copy()
env[b"QUIDRA_TEST_ENV"] = b"\xff"
result = subprocess.run(
    [os.fsencode(sys.argv[1]), os.fsencode(sys.argv[2])],
    env=env,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
)
if result.returncode != 101:
    raise SystemExit(f"invalid UTF-8 environment status: {result.returncode}")
if b"environment value must be valid UTF-8 text without NUL" not in result.stderr:
    raise SystemExit(f"missing environment text diagnostic: {result.stderr!r}")
PY

cat > "$TMP/test-module.qui" <<'QUI'
test.check(true)
test.equal(2 + 3, 5)
test.equal("Quidra", "Quidra")
int[] actual = [1, 2, 3]
int[] expected = [1, 2, 3]
test.equal(actual, expected)
print("test-ok")
QUI
[[ "$("$QUIDRA" "$TMP/test-module.qui")" == "test-ok" ]]

cat > "$TMP/test-failure.qui" <<'QUI'
test.check(false)
print("unreachable")
QUI
set +e
"$QUIDRA" "$TMP/test-failure.qui" >"$TMP/test-failure.out" 2>"$TMP/test-failure.err"
test_failure_rc=$?
set -e
[[ "$test_failure_rc" -eq 1 ]]
grep -q 'Quidra test assertion failed' "$TMP/test-failure.err"
[[ ! -s "$TMP/test-failure.out" ]]

cat > "$TMP/time.qui" <<'QUI'
time.Instant start = time.now()
time.Duration pause = time.seconds(0.001)
time.sleep(pause)
time.Duration elapsed = time.since(start)
print(elapsed.seconds() >= 0.0)
QUI
[[ "$("$QUIDRA" "$TMP/time.qui")" == "true" ]]

cat > "$TMP/time-direct-construction.qui" <<'QUI'
time.Instant impossible = time.Instant()
QUI
set +e
"$QUIDRA" check "$TMP/time-direct-construction.qui" --json >"$TMP/time-direct-construction.json"
time_direct_rc=$?
set -e
[[ "$time_direct_rc" -eq 1 ]]
grep -q 'Standard library value types cannot be constructed directly' "$TMP/time-direct-construction.json"

cat > "$TMP/random.qui" <<'QUI'
random.Generator a = random.generator(seed = 42)
random.Generator b = random.generator(seed = 42)
print(a.int(1, 10) == b.int(1, 10))
print(a.float() == b.float())
print(a.bool() == b.bool())

random.Generator original = random.generator(seed = 7)
random.Generator copy = original
print(original.int(-1000, 1000) == copy.int(-1000, 1000))
QUI
random_output="$("$QUIDRA" "$TMP/random.qui")"
random_expected="$(printf 'true\ntrue\ntrue\ntrue')"
[[ "$random_output" == "$random_expected" ]]

cat > "$TMP/random-invalid.qui" <<'QUI'
random.Generator rng = random.generator(seed = 1)
print(rng.int(5, 5))
QUI
set +e
"$QUIDRA" "$TMP/random-invalid.qui" >"$TMP/random-invalid.out" 2>"$TMP/random-invalid.err"
random_invalid_rc=$?
set -e
[[ "$random_invalid_rc" -eq 101 ]]
grep -q 'invalid random range' "$TMP/random-invalid.out"

cat > "$TMP/process.qui" <<'QUI'
process.Result completed = process.run("/bin/sh", ["-c", "printf out; printf err >&2; exit 3"])
print(completed.started)
print(completed.status)
print(completed.output)
print(completed.error)

process.Result missing = process.run("/definitely/not/a/real/quidra-program", [])
print(missing.started)
print(missing.status)
print(missing.error != "")
QUI
process_output="$("$QUIDRA" "$TMP/process.qui")"
process_expected="$(printf 'true\n3\nout\nerr\nfalse\n-1\ntrue')"
[[ "$process_output" == "$process_expected" ]]

cat > "$TMP/process-invalid-text.qui" <<'QUI'
process.Result result = process.run("/bin/sh", ["-c", "printf '\377'"])
print(result.output)
QUI
set +e
"$QUIDRA" "$TMP/process-invalid-text.qui" >"$TMP/process-invalid-text.out" 2>"$TMP/process-invalid-text.err"
process_invalid_text_rc=$?
set -e
[[ "$process_invalid_text_rc" -eq 101 ]]
grep -q 'process stdout/stderr must be valid UTF-8 text without NUL' "$TMP/process-invalid-text.err"

cat > "$TMP/process-nul-text.qui" <<'QUI'
process.Result result = process.run("/bin/sh", ["-c", "printf 'A\000B'"])
print(result.output)
QUI
set +e
"$QUIDRA" "$TMP/process-nul-text.qui" >"$TMP/process-nul-text.out" 2>"$TMP/process-nul-text.err"
process_nul_text_rc=$?
set -e
[[ "$process_nul_text_rc" -eq 101 ]]
grep -q 'process stdout/stderr must be valid UTF-8 text without NUL' "$TMP/process-nul-text.err"

cat > "$TMP/process-exit.qui" <<'QUI'
process.exit(7)
QUI
set +e
"$QUIDRA" "$TMP/process-exit.qui" >"$TMP/process-exit.out" 2>"$TMP/process-exit.err"
process_exit_rc=$?
set -e
if [[ "$process_exit_rc" -ne 7 ]]; then
    echo "process.exit regression: expected status 7, got $process_exit_rc" >&2
    cat "$TMP/process-exit.err" >&2
    exit 1
fi
[[ ! -s "$TMP/process-exit.out" ]]
[[ ! -s "$TMP/process-exit.err" ]]

"$QUIDRA" build "$TMP/process-exit.qui" -o "$TMP/process-exit-bin"
set +e
"$TMP/process-exit-bin" >"$TMP/process-exit-bin.out" 2>"$TMP/process-exit-bin.err"
process_exit_bin_rc=$?
set -e
if [[ "$process_exit_bin_rc" -ne 7 ]]; then
    echo "native process.exit regression: expected status 7, got $process_exit_bin_rc" >&2
    cat "$TMP/process-exit-bin.err" >&2
    exit 1
fi
[[ ! -s "$TMP/process-exit-bin.out" ]]
[[ ! -s "$TMP/process-exit-bin.err" ]]

cat > "$TMP/process-direct-construction.qui" <<'QUI'
process.Result impossible = process.Result()
QUI
set +e
"$QUIDRA" check "$TMP/process-direct-construction.qui" --json >"$TMP/process-direct-construction.json"
process_direct_rc=$?
set -e
[[ "$process_direct_rc" -eq 1 ]]
grep -q 'Standard library value types cannot be constructed directly' "$TMP/process-direct-construction.json"

cat > "$TMP/map.qui" <<'QUI'
map.Map<string, int> counts = map.Map<string, int>()
print(counts.size())
print(counts.has("apple"))

auto missing = counts.get("apple")
match missing
    int value
        print(value)
    none
        print("none")

counts.set("apple", 2)
counts.set("banana", 1)
counts.set("apple", 3)
print(counts.size())

auto apple = counts.get("apple")
match apple
    int value
        print(value)
    none
        print("missing")

string[] keys = counts.keys()
int[] values = counts.values()
print(keys[0])
print(keys[1])
print(values[0])
print(values[1])

map.Map<string, int> copied = counts
copied.set("cherry", 4)
print(counts.has("cherry"))
print(copied.has("cherry"))
QUI
map_output="$("$QUIDRA" "$TMP/map.qui")"
map_expected="$(printf '0\nfalse\nnone\n2\n3\napple\nbanana\n3\n1\nfalse\ntrue')"
[[ "$map_output" == "$map_expected" ]]

cat > "$TMP/set.qui" <<'QUI'
set.Set<string> tags = set.Set<string>()
tags.add("vision")
tags.add("ai")
tags.add("vision")
print(tags.size())
print(tags.has("vision"))
print(tags.has("other"))
string[] values = tags.values()
print(values[0])
print(values[1])

set.Set<string> copied = tags
copied.add("new")
print(tags.has("new"))
print(copied.has("new"))
QUI
set_output="$("$QUIDRA" "$TMP/set.qui")"
set_expected="$(printf '2\ntrue\nfalse\nvision\nai\nfalse\ntrue')"
[[ "$set_output" == "$set_expected" ]]

cat > "$TMP/hash-collections.qui" <<'QUI'
map.Map<string, int> many = map.Map<string, int>()
for i in range(0, 100)
    many.set("k{i}", i * 3)
print(many.size())
auto found = many.get("k73")
match found
    int value
        print(value)
    none
        print(-1)
many.set("k73", 999)
auto replaced = many.get("k73")
match replaced
    int value
        print(value)
    none
        print(-1)
string[] ordered_keys = many.keys()
print(ordered_keys[0])
print(ordered_keys[99])
print(many.has("missing"))

map.Map<int, string> numbers = map.Map<int, string>()
numbers.set(-7, "negative")
numbers.set(42, "answer")
auto number_value = numbers.get(42)
match number_value
    string value
        print(value)
    none
        print("missing")

set.Set<string> unique = set.Set<string>()
for i in range(0, 100)
    unique.add("v{i}")
for i in range(0, 100)
    unique.add("v{i}")
print(unique.size())
print(unique.has("v73"))
print(unique.has("absent"))
string[] ordered_values = unique.values()
print(ordered_values[0])
print(ordered_values[99])
QUI
hash_collections_output="$("$QUIDRA" "$TMP/hash-collections.qui")"
hash_collections_expected="$(printf '100\n219\n999\nk0\nk99\nfalse\nanswer\n100\ntrue\nfalse\nv0\nv99')"
[[ "$hash_collections_output" == "$hash_collections_expected" ]]
"$QUIDRA" llvm "$TMP/hash-collections.qui" > "$TMP/hash-collections.ll"
grep -q 'append.field.move' "$TMP/hash-collections.ll"
grep -q '@quidra_array_grow_move' "$TMP/hash-collections.ll"

# Cross many rehash thresholds and verify lookup, replacement, insertion order,
# set uniqueness, and value-copy isolation at a scale large enough to catch
# accidental quadratic regressions in generated collection code.
cat > "$TMP/hash-collections-stress.qui" <<'QUI'
map.Map<int, int> large = map.Map<int, int>()
for i in range(0, 5000)
    large.set(i, i * 2)
print(large.size())
auto middle = large.get(4097)
match middle
    int value
        print(value)
    none
        print(-1)
large.set(4097, 123456)
auto replaced_large = large.get(4097)
match replaced_large
    int value
        print(value)
    none
        print(-1)
int[] large_keys = large.keys()
print(large_keys[0])
print(large_keys[4999])
print(large.has(6000))

map.Map<int, int> copied_large = large
copied_large.set(6000, 42)
print(large.has(6000))
print(copied_large.has(6000))

set.Set<int> large_set = set.Set<int>()
for i in range(0, 5000)
    large_set.add(i)
for i in range(0, 5000)
    large_set.add(i)
print(large_set.size())
print(large_set.has(4097))
print(large_set.has(6000))
int[] large_values = large_set.values()
print(large_values[0])
print(large_values[4999])
QUI
hash_stress_output="$("$QUIDRA" "$TMP/hash-collections-stress.qui")"
hash_stress_expected="$(printf '5000\n8194\n123456\n0\n4999\nfalse\nfalse\ntrue\n5000\ntrue\nfalse\n0\n4999')"
[[ "$hash_stress_output" == "$hash_stress_expected" ]]

# "a", "i", and "q" have the same initial bucket modulo the default capacity 8,
# so this directly exercises deterministic open-addressing collision handling.
cat > "$TMP/hash-collection-domains.qui" <<'QUI'
map.Map<string, int> collisions = map.Map<string, int>()
collisions.set("a", 1)
collisions.set("i", 2)
collisions.set("q", 3)
auto collision_a = collisions.get("a")
match collision_a
    int value
        print(value)
    none
        print(-1)
auto collision_i = collisions.get("i")
match collision_i
    int value
        print(value)
    none
        print(-1)
auto collision_q = collisions.get("q")
match collision_q
    int value
        print(value)
    none
        print(-1)
string[] collision_keys = collisions.keys()
print(collision_keys[0])
print(collision_keys[1])
print(collision_keys[2])

map.Map<bool, string> flags = map.Map<bool, string>()
flags.set(false, "off")
flags.set(true, "on")
auto flag_false = flags.get(false)
match flag_false
    string value
        print(value)
    none
        print("missing")
auto flag_true = flags.get(true)
match flag_true
    string value
        print(value)
    none
        print("missing")

set.Set<int> numbers_set = set.Set<int>()
numbers_set.add(-1)
numbers_set.add(42)
numbers_set.add(-1)
print(numbers_set.size())
print(numbers_set.has(42))

set.Set<bool> bool_set = set.Set<bool>()
bool_set.add(false)
bool_set.add(true)
bool_set.add(false)
print(bool_set.size())
bool[] bool_values = bool_set.values()
print(bool_values[0])
print(bool_values[1])
QUI
hash_domain_output="$("$QUIDRA" "$TMP/hash-collection-domains.qui")"
hash_domain_expected="$(printf '1\n2\n3\na\ni\nq\noff\non\n2\ntrue\n2\nfalse\ntrue')"
[[ "$hash_domain_output" == "$hash_domain_expected" ]]

cat > "$TMP/map-invalid-key.qui" <<'QUI'
map.Map<float, int> values = map.Map<float, int>()
QUI
set +e
"$QUIDRA" check "$TMP/map-invalid-key.qui" --json >"$TMP/map-invalid-key.json"
map_key_rc=$?
set -e
[[ "$map_key_rc" -eq 1 ]]
grep -q 'STANDARD_KEY_TYPE' "$TMP/map-invalid-key.json"

cat > "$TMP/map-hidden-field.qui" <<'QUI'
map.Map<string, int> values = map.Map<string, int>()
print(values.__keys)
QUI
set +e
"$QUIDRA" check "$TMP/map-hidden-field.qui" --json >"$TMP/map-hidden-field.json"
map_hidden_rc=$?
set -e
[[ "$map_hidden_rc" -eq 1 ]]
grep -q 'Standard collection internals are not source-visible' "$TMP/map-hidden-field.json"



cat > "$TMP/uninitialized-array.qui" <<'QUI'
int[] dynamic = array(3)
dynamic[1] = 7
print(dynamic[1])

int[2] fixed
fixed[0] = 9
print(fixed[0])
QUI
uninitialized_array_output="$("$QUIDRA" "$TMP/uninitialized-array.qui")"
[[ "$uninitialized_array_output" == "$(printf '7\n9')" ]]

cat > "$TMP/uninitialized-array-read.qui" <<'QUI'
int[] values = array(2)
values[0] = 1
print(values[1])
QUI
set +e
"$QUIDRA" "$TMP/uninitialized-array-read.qui" >"$TMP/uninitialized-array-read.out" 2>"$TMP/uninitialized-array-read.err"
uninitialized_array_rc=$?
set -e
[[ "$uninitialized_array_rc" -eq 101 ]]
grep -Eq 'Quidra runtime error\[UNINITIALIZED\] at [0-9]+:[0-9]+: value is uninitialized' "$TMP/uninitialized-array-read.err"

cat > "$TMP/tensor.qui" <<'QUI'
tensor<float32> zeros = tensor.zeros<float32>([2, 3])
tensor<float32> ones = tensor.ones<float32>([1, 3])
tensor<float32> combined = zeros + ones
int[] combined_shape = combined.shape()
print(combined_shape[0])
print(combined_shape[1])
print(combined[1, 2].item())

tensor<float32> view = combined[:, 1:3]
int[] view_shape = view.shape()
print(view_shape[0])
print(view_shape[1])
view[0, 0] = 9.0
print(view[0, 0].item())
print(combined[0, 1].item())

tensor<float32> reshaped = combined.reshape([3, 2])
int[] reshaped_shape = reshaped.shape()
print(reshaped_shape[0])
print(reshaped_shape[1])

tensor<float> exact_source = tensor.ones<float>([1])
tensor<float32> exact_cast = exact_source.cast<float32>()
print(exact_cast[0].item())
QUI
tensor_output="$("$QUIDRA" "$TMP/tensor.qui")"
tensor_expected="$(printf '2\n3\n1.0\n2\n2\n9.0\n1.0\n3\n2\n1.0')"
[[ "$tensor_output" == "$tensor_expected" ]]


cat > "$TMP/tensor-numeric-stdlib.qui" <<'QUI'
tensor<int> a = tensor<int>([2, 3])
a[0, 0] = 1
a[0, 1] = 2
a[0, 2] = 3
a[1, 0] = 4
a[1, 1] = 5
a[1, 2] = 6

tensor<int> b = tensor<int>([3, 2])
b[0, 0] = 7
b[0, 1] = 8
b[1, 0] = 9
b[1, 1] = 10
b[2, 0] = 11
b[2, 1] = 12

print(stats.mean(a))
tensor<int> left = tensor<int>([3])
left[0] = 1
left[1] = 2
left[2] = 3
tensor<int> right = tensor<int>([3])
right[0] = 4
right[1] = 5
right[2] = 6
print(linear.dot(left, right))
tensor<int> c = linear.matmul(a, b)
print(c[0, 0].item())
print(c[0, 1].item())
print(c[1, 0].item())
print(c[1, 1].item())
QUI
[[ "$("$QUIDRA" "$TMP/tensor-numeric-stdlib.qui")" == "$(printf '3.5\n32\n58\n64\n139\n154')" ]]

cat > "$TMP/tensor-float-linear.qui" <<'QUI'
tensor<float32> left = tensor<float32>([3])
left[0] = 1.0
left[1] = 2.0
left[2] = 3.0
tensor<float32> right = tensor<float32>([3])
right[0] = 4.0
right[1] = 5.0
right[2] = 6.0
print(linear.dot(left, right))

tensor<float32> a = tensor<float32>([2, 3])
a[0, 0] = 1.0
a[0, 1] = 2.0
a[0, 2] = 3.0
a[1, 0] = 4.0
a[1, 1] = 5.0
a[1, 2] = 6.0
tensor<float32> b = tensor<float32>([3, 2])
b[0, 0] = 7.0
b[0, 1] = 8.0
b[1, 0] = 9.0
b[1, 1] = 10.0
b[2, 0] = 11.0
b[2, 1] = 12.0
tensor<float32> c = linear.matmul(a, b)
print(c[0, 0].item())
print(c[0, 1].item())
print(c[1, 0].item())
print(c[1, 1].item())

tensor<float32> source = tensor.ones<float32>([6])
tensor<float32> strided = source[0:6:2]
tensor<float32> three = tensor.ones<float32>([3])
print(linear.dot(strided, three))
QUI
[[ "$("$QUIDRA" "$TMP/tensor-float-linear.qui")" == "$(printf '32.0\n58.0\n64.0\n139.0\n154.0\n3.0')" ]]

cat > "$TMP/tensor-dot-overflow.qui" <<'QUI'
tensor<int> left = tensor<int>([1])
left[0] = 9223372036854775807
tensor<int> right = tensor<int>([1])
right[0] = 2
print(linear.dot(left, right))
QUI
set +e
"$QUIDRA" "$TMP/tensor-dot-overflow.qui" >"$TMP/tensor-dot-overflow.out" 2>"$TMP/tensor-dot-overflow.err"
tensor_dot_overflow_rc=$?
set -e
[[ "$tensor_dot_overflow_rc" -eq 101 ]]
grep -q 'linear.dot integer arithmetic overflow' "$TMP/tensor-dot-overflow.err"

cat > "$TMP/tensor-mean-uninitialized.qui" <<'QUI'
tensor<int> values = tensor<int>([2])
values[0] = 1
print(stats.mean(values))
QUI
set +e
"$QUIDRA" "$TMP/tensor-mean-uninitialized.qui" >"$TMP/tensor-mean-uninitialized.out" 2>"$TMP/tensor-mean-uninitialized.err"
tensor_mean_uninitialized_rc=$?
set -e
[[ "$tensor_mean_uninitialized_rc" -eq 101 ]]
grep -Eq 'Quidra runtime error\[UNINITIALIZED\] at [0-9]+:[0-9]+: value is uninitialized' "$TMP/tensor-mean-uninitialized.err"

cat > "$TMP/tensor-rank-mismatch.qui" <<'QUI'
tensor<float32> a = tensor.ones<float32>([2, 3])
tensor<float32> b = tensor.ones<float32>([3])
tensor<float32> c = a + b
print(c.shape()[0])
QUI
set +e
"$QUIDRA" "$TMP/tensor-rank-mismatch.qui" >"$TMP/tensor-rank-mismatch.out" 2>"$TMP/tensor-rank-mismatch.err"
tensor_rank_rc=$?
set -e
[[ "$tensor_rank_rc" -eq 101 ]]
grep -Eq 'Quidra runtime error\[TENSOR\] at [0-9]+:[0-9]+: .*identical ranks' "$TMP/tensor-rank-mismatch.err"

cat > "$TMP/tensor-float-int-cast.qui" <<'QUI'
tensor<float> source = tensor.ones<float>([1]) * 1.5
tensor<int> converted = source.cast<int>()
print(converted[0].item())
QUI
set +e
"$QUIDRA" check "$TMP/tensor-float-int-cast.qui" --json >"$TMP/tensor-float-int-cast.json"
tensor_cast_rc=$?
set -e
[[ "$tensor_cast_rc" -eq 1 ]]
grep -q 'floating-point to integer conversion requires' "$TMP/tensor-float-int-cast.json"

cat > "$TMP/tensor-uninitialized.qui" <<'QUI'
tensor<float32> values = tensor<float32>([2])
values[0] = 3.0
print(values[0].item())
print(values[1].item())
QUI
set +e
"$QUIDRA" "$TMP/tensor-uninitialized.qui" >"$TMP/tensor-uninitialized.out" 2>"$TMP/tensor-uninitialized.err"
tensor_uninitialized_rc=$?
set -e
[[ "$tensor_uninitialized_rc" -eq 101 ]]
[[ "$(cat "$TMP/tensor-uninitialized.out")" == "3.0" ]]
grep -Eq 'Quidra runtime error\[UNINITIALIZED\] at [0-9]+:[0-9]+: value is uninitialized' "$TMP/tensor-uninitialized.err"


cat > "$TMP/neural-dtype-precision.qui" <<'QUI'
tensor<float32> source32 = tensor<float32>([1])
source32[0] = 16777216.0
neural<float32> value32 = neural.track(source32)
neural<float32> plus32 = value32 + float32(1)
neural<float32> plus32_again = plus32 + float32(1)
print(plus32_again.untrack()[0].item() == float32(16777216))

tensor<float> source64 = tensor<float>([1])
source64[0] = 16777216.0
neural<float> value64 = neural.track(source64)
neural<float> plus64 = value64 + 1
neural<float> plus64_again = plus64 + 1
print(plus64_again.untrack()[0].item() == float(16777218))
QUI
[[ "$("$QUIDRA" "$TMP/neural-dtype-precision.qui")" == "$(printf 'true\ntrue')" ]]

cat > "$TMP/vision.qui" <<QUI
tensor<uint8> image = tensor<uint8>([3, 2, 2])
image[0, 0, 0] = 10
image[0, 0, 1] = 20
image[0, 1, 0] = 30
image[0, 1, 1] = 40
image[1, 0, 0] = 50
image[1, 0, 1] = 60
image[1, 1, 0] = 70
image[1, 1, 1] = 80
image[2, 0, 0] = 90
image[2, 0, 1] = 100
image[2, 1, 0] = 110
image[2, 1, 1] = 120

auto png_written = vision.write("$TMP/vision.png", image)
match png_written
    void
        print("png-write")
    error problem
        print(problem)

auto png_read = vision.read<uint8>("$TMP/vision.png")
match png_read
    tensor<uint8> decoded
        int[] shape = decoded.shape()
        print(shape[0])
        print(shape[1])
        print(shape[2])
        print(decoded[0, 1, 1].item())
        print(decoded[2, 0, 1].item())
    error problem
        print(problem)

auto bmp_written = vision.write("$TMP/vision.bmp", image)
match bmp_written
    void
        print("bmp-write")
    error problem
        print(problem)

auto bmp_read = vision.read<uint8>("$TMP/vision.bmp")
match bmp_read
    tensor<uint8> decoded
        print(decoded[1, 1, 0].item())
    error problem
        print(problem)

auto tiff_written = vision.write("$TMP/vision.tiff", image)
match tiff_written
    void
        print("tiff-write")
    error problem
        print(problem)

auto tiff_read = vision.read<uint8>("$TMP/vision.tiff")
match tiff_read
    tensor<uint8> decoded
        int[] shape = decoded.shape()
        print(shape[0])
        print(decoded[2, 1, 0].item())
    error problem
        print(problem)

auto jpeg_written = vision.write("$TMP/vision.jpg", image, quality = 100)
match jpeg_written
    void
        print("jpeg-write")
    error problem
        print(problem)

auto jpeg_read = vision.read<uint8>("$TMP/vision.jpg")
match jpeg_read
    tensor<uint8> decoded
        int[] shape = decoded.shape()
        print(shape[0])
        print(shape[1])
        print(shape[2])
    error problem
        print(problem)

auto webp_written = vision.write("$TMP/vision.webp", image, quality = 100)
match webp_written
    void
        print("webp-write")
    error problem
        print(problem)

auto webp_read = vision.read<uint8>("$TMP/vision.webp")
match webp_read
    tensor<uint8> decoded
        int[] shape = decoded.shape()
        print(shape[0])
        print(shape[1])
        print(shape[2])
    error problem
        print(problem)

tensor<uint8> rgba = tensor.zeros<uint8>([4, 1, 1])
auto rgba_jpeg = vision.write("$TMP/rgba.jpg", rgba)
match rgba_jpeg
    void
        print("unexpected-jpeg-alpha")
    error problem
        print("jpeg-alpha-error")
QUI
vision_output="$("$QUIDRA" "$TMP/vision.qui")"
vision_expected="$(printf 'png-write\n3\n2\n2\n40\n100\nbmp-write\n70\ntiff-write\n3\n110\njpeg-write\n3\n2\n2\nwebp-write\n3\n2\n2\njpeg-alpha-error')"
[[ "$vision_output" == "$vision_expected" ]]

cat > "$TMP/json-data.json" <<'JSON'
{"name":"Quidra","items":[1,2],"nothing":null,"ok":true,"pi":3.5}
JSON

cat > "$TMP/json.qui" <<QUI
auto loaded = file.read("$TMP/json-data.json")
match loaded
    string source
        auto parsed = json.parse(source)
        match parsed
            json.Value root
                print(root.kind())
                auto size = root.size()
                match size
                    int value
                        print(value)
                    error problem
                        print(problem)

                auto name = root.get("name")
                match name
                    json.Value value
                        auto text = value.text()
                        match text
                            string content
                                print(content)
                            error problem
                                print(problem)
                    none
                        print("missing-name")
                    error problem
                        print(problem)

                auto missing = root.get("missing")
                match missing
                    json.Value value
                        print(value.kind())
                    none
                        print("none")
                    error problem
                        print(problem)

                auto nothing = root.get("nothing")
                match nothing
                    json.Value value
                        print(value.kind())
                    none
                        print("missing-null")
                    error problem
                        print(problem)

                auto items = root.get("items")
                match items
                    json.Value items_value
                        auto second = items_value.at(1)
                        match second
                            json.Value value
                                auto integer = value.integer()
                                match integer
                                    int number
                                        print(number)
                                    error problem
                                        print(problem)
                            none
                                print("missing-index")
                            error problem
                                print(problem)
                    none
                        print("missing-items")
                    error problem
                        print(problem)

                auto ok = root.get("ok")
                match ok
                    json.Value value
                        auto boolean = value.boolean()
                        match boolean
                            bool bit
                                print(bit)
                            error problem
                                print(problem)
                    none
                        print("missing-ok")
                    error problem
                        print(problem)

                auto pi = root.get("pi")
                match pi
                    json.Value value
                        auto number = value.number()
                        match number
                            float scalar
                                print(scalar)
                            error problem
                                print(problem)
                    none
                        print("missing-pi")
                    error problem
                        print(problem)

                string encoded = root.encode()
                print(encoded)
                auto reparsed = json.parse(encoded)
                match reparsed
                    json.Value other
                        print(root.equal(other))
                    error problem
                        print(problem)
            error problem
                print(problem)
    error problem
        print(problem)
QUI

json_output="$("$QUIDRA" "$TMP/json.qui")"
json_source='{"name":"Quidra","items":[1,2],"nothing":null,"ok":true,"pi":3.5}'
json_expected="$(printf 'object\n5\nQuidra\nnone\nnull\n2\ntrue\n3.5\n%s\ntrue' "$json_source")"
[[ "$json_output" == "$json_expected" ]]

cat > "$TMP/json-kind-error.qui" <<'QUI'
auto parsed = json.parse("1")
match parsed
    json.Value value
        auto text = value.text()
        match text
            string content
                print(content)
            error problem
                print(problem)
    error problem
        print(problem)
QUI
[[ "$("$QUIDRA" "$TMP/json-kind-error.qui")" == "JSON value has incompatible kind" ]]

cat > "$TMP/json-invalid.qui" <<'QUI'
auto parsed = json.parse("[")
match parsed
    json.Value value
        print(value.kind())
    error problem
        print("error")
QUI
[[ "$("$QUIDRA" "$TMP/json-invalid.qui")" == "error" ]]

cat > "$TMP/json-duplicate.json" <<'JSON'
{"a":1,"a":2}
JSON
cat > "$TMP/json-duplicate.qui" <<QUI
auto loaded = file.read("$TMP/json-duplicate.json")
match loaded
    string source
        auto parsed = json.parse(source)
        match parsed
            json.Value value
                print(value.kind())
            error problem
                print("error")
    error problem
        print(problem)
QUI
[[ "$("$QUIDRA" "$TMP/json-duplicate.qui")" == "error" ]]

cat > "$TMP/json-direct-construction.qui" <<'QUI'
json.Value impossible = json.Value()
QUI
set +e
"$QUIDRA" check "$TMP/json-direct-construction.qui" --json >"$TMP/json-direct-construction.json"
json_direct_rc=$?
set -e
[[ "$json_direct_rc" -eq 1 ]]
grep -q 'Standard library value types cannot be constructed directly' "$TMP/json-direct-construction.json"

cat > "$TMP/json-equality.qui" <<'QUI'
auto left = json.parse("1")
auto right = json.parse("1")
match left
    json.Value a
        match right
            json.Value b
                print(a == b)
            error problem
                print(problem)
    error problem
        print(problem)
QUI
set +e
"$QUIDRA" check "$TMP/json-equality.qui" --json >"$TMP/json-equality.json"
json_equality_rc=$?
set -e
[[ "$json_equality_rc" -eq 1 ]]
grep -q 'Equality is not defined for this type' "$TMP/json-equality.json"

cat > "$TMP/http-server.py" <<'PY'
import http.server
import socketserver

class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/missing":
            status = 404
            body = b"missing"
            marker = "missing"
        else:
            status = 200
            body = b"A\x00B"
            marker = "present"
        self.send_response(status)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("X-Quidra", marker)
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format, *args):
        pass

class LocalHTTPServer(http.server.HTTPServer):
    def server_bind(self):
        socketserver.TCPServer.server_bind(self)
        self.server_name = "127.0.0.1"
        self.server_port = self.server_address[1]

server = LocalHTTPServer(("127.0.0.1", 0), Handler)
print(server.server_port, flush=True)
for _ in range(2):
    server.handle_request()
server.server_close()
PY

python3 "$TMP/http-server.py" >"$TMP/http-port" 2>"$TMP/http-server.err" &
HTTP_PID=$!
for _ in $(seq 1 100); do
    [[ -s "$TMP/http-port" ]] && break
    if ! kill -0 "$HTTP_PID" 2>/dev/null; then
        cat "$TMP/http-server.err" >&2
        exit 1
    fi
    sleep 0.05
done
if [[ ! -s "$TMP/http-port" ]]; then
    cat "$TMP/http-server.err" >&2
    kill "$HTTP_PID" 2>/dev/null || true
    wait "$HTTP_PID" 2>/dev/null || true
    HTTP_PID=""
    echo "local HTTP test server did not start" >&2
    exit 1
fi
HTTP_PORT="$(cat "$TMP/http-port")"

cat > "$TMP/http.qui" <<QUI
auto successful = http.get("http://127.0.0.1:$HTTP_PORT/ok")
match successful
    http.Response ok_response
        print(ok_response.status)
        print(len(ok_response.body))
        print(ok_response.body[0])
        print(ok_response.body[1])
        auto marker = ok_response.header("X-Quidra")
        match marker
            string value
                print(value)
            none
                print("none")
        auto absent = ok_response.header("missing-header")
        match absent
            string value
                print(value)
            none
                print("none")
    error problem
        print(problem)

auto missing = http.get("http://127.0.0.1:$HTTP_PORT/missing")
match missing
    http.Response missing_response
        print(missing_response.status)
        print(len(missing_response.body))
    error problem
        print(problem)

auto transport = http.get("file:///etc/passwd")
match transport
    http.Response unexpected_response
        print(unexpected_response.status)
    error problem
        print("transport-error")
QUI

http_output="$("$QUIDRA" "$TMP/http.qui")"
wait "$HTTP_PID"
HTTP_PID=""
http_expected="$(printf '200\n3\n65\n0\npresent\nnone\n404\n7\ntransport-error')"
[[ "$http_output" == "$http_expected" ]]

cat > "$TMP/http-direct-construction.qui" <<'QUI'
http.Response impossible = http.Response()
QUI
set +e
"$QUIDRA" check "$TMP/http-direct-construction.qui" --json >"$TMP/http-direct-construction.json"
http_direct_rc=$?
set -e
[[ "$http_direct_rc" -eq 1 ]]
grep -q 'Standard library value types cannot be constructed directly' "$TMP/http-direct-construction.json"

cat > "$TMP/http-equality.qui" <<QUI
auto first = http.get("http://127.0.0.1:1/")
match first
    http.Response a
        print(a == a)
    error problem
        print(problem)
QUI
set +e
"$QUIDRA" check "$TMP/http-equality.qui" --json >"$TMP/http-equality.json"
http_equality_rc=$?
set -e
[[ "$http_equality_rc" -eq 1 ]]
grep -q 'Equality is not defined for this type' "$TMP/http-equality.json"

echo "stdlib integration: ok"


# Additive namespace extensions.
mkdir -p "$TMP/packages/math_extra"
cat > "$TMP/packages/math_extra/main.qui" <<'QUI'
int twice(int value)
    return value * 2
QUI
cat > "$TMP/packages/math_extra/quidra.package.json" <<'JSON'
{"extends":["math"]}
JSON
cat > "$TMP/namespace-extension.qui" <<'QUI'
import math += math_extra
print(math.twice(21))
QUI
[[ "$(QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" "$TMP/namespace-extension.qui")" == "42" ]]

mkdir -p "$TMP/packages/math_collision"
cat > "$TMP/packages/math_collision/main.qui" <<'QUI'
int twice(int value)
    return value + value
QUI
cat > "$TMP/packages/math_collision/quidra.package.json" <<'JSON'
{"extends":["math"]}
JSON
cat > "$TMP/namespace-extension-collision.qui" <<'QUI'
import math += math_extra
import math += math_collision
print(math.twice(4))
QUI
set +e
QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" check "$TMP/namespace-extension-collision.qui" --json >"$TMP/namespace-extension-collision.json" 2>&1
namespace_collision_rc=$?
set -e
[[ "$namespace_collision_rc" -eq 1 ]]
grep -q 'NAMESPACE_EXTENSION_COLLISION' "$TMP/namespace-extension-collision.json"

mkdir -p "$TMP/packages/not_math"
cat > "$TMP/packages/not_math/main.qui" <<'QUI'
int extra(int value)
    return value
QUI
cat > "$TMP/packages/not_math/quidra.package.json" <<'JSON'
{"extends":["vision"]}
JSON
cat > "$TMP/namespace-extension-undeclared.qui" <<'QUI'
import math += not_math
print(math.extra(1))
QUI
set +e
QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" check "$TMP/namespace-extension-undeclared.qui" --json >"$TMP/namespace-extension-undeclared.json" 2>&1
namespace_undeclared_rc=$?
set -e
[[ "$namespace_undeclared_rc" -eq 1 ]]
grep -q 'PACKAGE_EXTENSION_NOT_DECLARED' "$TMP/namespace-extension-undeclared.json"

# Named writable arguments.
cat > "$TMP/named-writable.qui" <<'QUI'
void set_value(int &value)
    value = 9

int x = 1
set_value(&value = &x)
print(x)
QUI
[[ "$("$QUIDRA" "$TMP/named-writable.qui")" == "9" ]]

cat > "$TMP/named-writable-old-shape.qui" <<'QUI'
void set_value(int &value)
    value = 9

int x = 1
set_value(value = &x)
QUI
set +e
"$QUIDRA" check "$TMP/named-writable-old-shape.qui" --json >"$TMP/named-writable-old-shape.json"
named_writable_old_rc=$?
set -e
[[ "$named_writable_old_rc" -eq 1 ]]

# Practical explicit casts and explicit floating-point rounding.
cat > "$TMP/practical-casts.qui" <<'QUI'
int large = 16777217
float32 rounded = float32(large)
print(rounded)
float value = 1.75
float32 narrowed = float32(value)
print(narrowed)
print(math.trunc(value))
print(math.round(value))
print(math.floor(-1.25))
print(math.ceil(-1.25))
tensor<float> source = tensor.ones<float>([1]) * 1.25
tensor<float32> converted = source.cast<float32>()
print(converted[0].item())
QUI
practical_cast_output="$("$QUIDRA" "$TMP/practical-casts.qui")"
[[ "$practical_cast_output" == "$(printf '1.6777216e+07\n1.75\n1\n2\n-2\n-1\n1.25')" ]]

cat > "$TMP/float-int-cast-rejected.qui" <<'QUI'
float value = 1.0
int converted = int(value)
print(converted)
QUI
set +e
"$QUIDRA" check "$TMP/float-int-cast-rejected.qui" --json >"$TMP/float-int-cast-rejected.json"
float_int_cast_rc=$?
set -e
[[ "$float_int_cast_rc" -eq 1 ]]
grep -q 'Floating-point to integer conversion requires' "$TMP/float-int-cast-rejected.json"

cat > "$TMP/neural-mode.qui" <<'QUI'
X select<X, M>(X value, M mode)
    return value

tensor<float32> raw = tensor.ones<float32>([1])
tensor<float32> inference_value = select(raw, neural.inference)
neural training_value = select(neural.track(raw), neural.training)
print(inference_value[0].item())
print(training_value.untrack()[0].item())
QUI
[[ "$("$QUIDRA" "$TMP/neural-mode.qui")" == "$(printf '1.0\n1.0')" ]]

cat > "$TMP/neural-parameter-state.qui" <<'QUI'
neural.Parameter weight = neural.Parameter(
    value = tensor.ones<float32>([1])
)
neural tracked = weight.track()
tensor<float32> raw = weight.raw()
neural.State<int> epoch = neural.State<int>(value = 3)
print(raw[0].item())
print(tracked.untrack()[0].item())
print(epoch.value)
QUI
[[ "$("$QUIDRA" "$TMP/neural-parameter-state.qui")" == "$(printf '1.0\n1.0\n3')" ]]

cat > "$TMP/neural-linear.qui" <<'QUI'
neural.Linear layer = neural.Linear(input = 2, output = 1, seed = 7)
tensor<float32> sample_values = tensor.ones<float32>([1, 2])
tensor<float32> inference = layer.forward(sample_values)
neural training = layer.forward(neural.track(sample_values))
tensor<float32> training_value = training.untrack()
print(inference[0, 0].item() == training_value[0, 0].item())
print(layer.weight.raw().shape()[0])
print(layer.weight.raw().shape()[1])

neural target_loss = neural.mse(training, tensor.zeros<float32>([1, 1]))
neural.Gradients gradients = neural.grad(target_loss)
print(training_value.shape()[1])
QUI
[[ "$("$QUIDRA" "$TMP/neural-linear.qui")" == "$(printf 'true\n1\n2\n1')" ]]

cat > "$TMP/neural-model-copy.qui" <<'QUI'
neural.Linear original_layer = neural.Linear(input = 2, output = 1, seed = 37)
neural.Linear copied_layer = original_layer
tensor<float32> copy_values = tensor.ones<float32>([1, 2])
tensor<float32> copy_target = tensor.zeros<float32>([1, 1])
float32 original_before = original_layer.weight.raw()[0, 0].item()

neural copy_prediction = copied_layer.forward(neural.track(copy_values))
neural copy_loss = neural.mse(copy_prediction, copy_target)
neural.Gradients copy_gradients = neural.grad(copy_loss)
neural.SGD copy_optimizer = neural.SGD(rate = 0.05)
neural.step(&copied_layer, &copy_optimizer, copy_gradients)

print(original_layer.weight.raw()[0, 0].item() == original_before)
print(copied_layer.weight.raw()[0, 0].item() != original_layer.weight.raw()[0, 0].item())
QUI
[[ "$("$QUIDRA" "$TMP/neural-model-copy.qui")" == "$(printf 'true\ntrue')" ]]

cat > "$TMP/neural-conv2d.qui" <<'QUI'
neural.Conv2D conv = neural.Conv2D(input = 1, output = 2, kernel = 3, stride = 1, padding = 1, seed = 5)
tensor<float32> image = tensor.ones<float32>([1, 1, 4, 4])
tensor<float32> inference = conv.forward(image)
print(inference.shape()[0])
print(inference.shape()[1])
print(inference.shape()[2])
print(inference.shape()[3])

tensor<float32> weight_before = conv.weight.raw()
neural training = conv.forward(neural.track(image))
neural loss = neural.mse(training, tensor.zeros<float32>([1, 2, 4, 4]))
neural.Gradients gradients = neural.grad(loss)
neural.SGD conv_optimizer = neural.SGD(rate = 0.01)
neural.step(&conv, &conv_optimizer, gradients)
print(training.untrack().shape()[1])
print(weight_before[0, 0, 0, 0].item() != conv.weight.raw()[0, 0, 0, 0].item())
QUI
[[ "$("$QUIDRA" "$TMP/neural-conv2d.qui")" == "$(printf '1\n2\n4\n4\n2\ntrue')" ]]

cat > "$TMP/neural-conv2d-backward-exact.qui" <<'QUI'
neural.Conv2D conv = neural.Conv2D(
    input = 1,
    output = 1,
    kernel = 1,
    stride = 1,
    padding = 0,
    seed = 23
)
tensor<float32> image = tensor.ones<float32>([1, 1, 1, 1])
float32 weight_before = conv.weight.raw()[0, 0, 0, 0].item()
float32 bias_before = conv.bias.raw()[0].item()
neural prediction = conv.forward(neural.track(image))
float32 prediction_before = prediction.untrack()[0, 0, 0, 0].item()
neural loss = neural.mse(prediction, tensor.zeros<float32>([1, 1, 1, 1]))
neural.Gradients gradients = neural.grad(loss)
neural.SGD optimizer = neural.SGD(rate = 0.125)
neural.step(&conv, &optimizer, gradients)

float actual = float(conv.weight.raw()[0, 0, 0, 0].item())
float expected = float(weight_before) - 0.25 * float(prediction_before)
print(math.abs(actual - expected) < 0.000001)
print(bias_before == 0.0)
QUI
[[ "$("$QUIDRA" "$TMP/neural-conv2d-backward-exact.qui")" == "$(printf 'true\ntrue')" ]]

cat > "$TMP/neural-conv2d-backward-all.qui" <<'QUI'
class ConvBackwardModel
    neural.Parameter input_parameter
    neural.Conv2D conv

ConvBackwardModel model = ConvBackwardModel(
    input_parameter = neural.Parameter(
        value = tensor.ones<float32>([1, 1, 1, 1])
    ),
    conv = neural.Conv2D(
        input = 1,
        output = 1,
        kernel = 1,
        stride = 1,
        padding = 0,
        seed = 29
    )
)

float x_before = float(model.input_parameter.raw()[0, 0, 0, 0].item())
float w_before = float(model.conv.weight.raw()[0, 0, 0, 0].item())
float b_before = float(model.conv.bias.raw()[0].item())

neural prediction = model.conv.forward(model.input_parameter.track())
float y_before = float(prediction.untrack()[0, 0, 0, 0].item())
neural loss = neural.mse(
    prediction,
    tensor.zeros<float32>([1, 1, 1, 1])
)
neural.Gradients gradients = neural.grad(loss)
neural.SGD optimizer = neural.SGD(rate = 0.125)
neural.step(&model, &optimizer, gradients)

float expected_weight = w_before - 0.25 * y_before * x_before
float expected_bias = b_before - 0.25 * y_before
float expected_input = x_before - 0.25 * y_before * w_before

float actual_weight = float(model.conv.weight.raw()[0, 0, 0, 0].item())
float actual_bias = float(model.conv.bias.raw()[0].item())
float actual_input = float(model.input_parameter.raw()[0, 0, 0, 0].item())

print(math.abs(actual_weight - expected_weight) < 0.000001)
print(math.abs(actual_bias - expected_bias) < 0.000001)
print(math.abs(actual_input - expected_input) < 0.000001)
QUI
[[ "$("$QUIDRA" "$TMP/neural-conv2d-backward-all.qui")" == "$(printf 'true\ntrue\ntrue')" ]]

cat > "$TMP/neural-lifetime-stress.qui" <<'QUI'
class LifetimeModel
    neural.Linear dense_layer
    neural.BatchNorm norm
    neural.Dropout dropout

void exercise()
    LifetimeModel stress_model = LifetimeModel(
        dense_layer = neural.Linear(input = 2, output = 2, seed = 1),
        norm = neural.BatchNorm(features = 2),
        dropout = neural.Dropout(rate = 0.25, seed = 3)
    )
    neural.Adam optimizer = neural.Adam(rate = 0.001)

    tensor<float32> values = tensor.ones<float32>([2, 2])
    neural projected = stress_model.dense_layer.forward(neural.track(values))
    projected = stress_model.norm.forward(projected, mode = neural.training)
    projected = stress_model.dropout.forward(projected, mode = neural.training)
    neural loss = neural.mse(projected, tensor.zeros<float32>([2, 2]))
    neural.Gradients gradients = neural.grad(loss)
    neural.step(&stress_model, &optimizer, gradients)

    neural.Conv2D conv = neural.Conv2D(input = 1, output = 1, kernel = 1, seed = 2)
    tensor<float32> image = tensor.ones<float32>([1, 1, 2, 2])
    neural convolved = conv.forward(neural.track(image))
    neural conv_loss = neural.mse(convolved, tensor.zeros<float32>([1, 1, 2, 2]))
    neural.Gradients conv_gradients = neural.grad(conv_loss)
    neural.SGD conv_optimizer = neural.SGD(rate = 0.01)
    neural.step(&conv, &conv_optimizer, conv_gradients)

for iteration in range(128)
    exercise()
print("ok")
QUI
"$QUIDRA" "$TMP/neural-lifetime-stress.qui" >"$TMP/neural-lifetime-stress.out"
[[ "$(cat "$TMP/neural-lifetime-stress.out")" == "ok" ]]

cat > "$TMP/neural-batchnorm.qui" <<'QUI'
neural.BatchNorm norm = neural.BatchNorm(features = 2)
tensor<float32> feature_map = tensor.ones<float32>([1, 2, 2, 2])
tensor<float32> inference = norm.forward(feature_map, mode = neural.inference)
print(inference.shape()[1])
neural normalized = norm.forward(neural.track(feature_map), mode = neural.training)
neural loss = neural.mse(normalized, tensor.ones<float32>([1, 2, 2, 2]))
neural.Gradients gradients = neural.grad(loss)
float32 bias_before = norm.bias.raw()[0].item()
neural.SGD optimizer = neural.SGD(rate = 0.1)
neural.step(&norm, &optimizer, gradients)
print(norm.running_mean.value[0].item())
print(norm.running_variance.value[0].item())
print(norm.bias.raw()[0].item() != bias_before)
QUI
[[ "$("$QUIDRA" "$TMP/neural-batchnorm.qui")" == "$(printf '2\n0.10000000149011612\n0.8999999761581421\ntrue')" ]]

cat > "$TMP/neural-dropout-state.qui" <<QUI
class DropoutModel
    neural.Dropout dropout

DropoutModel model = DropoutModel(
    dropout = neural.Dropout(rate = 0.5, seed = 17)
)
tensor<float32> values = tensor.ones<float32>([16])
neural first = model.dropout.forward(neural.track(values), mode = neural.training)
neural.save(model, path = "$TMP/dropout.quistate")
tensor<float32> expected = model.dropout.forward(
    neural.track(values), mode = neural.training
).untrack()
neural.load(&model = &model, path = "$TMP/dropout.quistate")
tensor<float32> replay = model.dropout.forward(
    neural.track(values), mode = neural.training
).untrack()
bool equal = true
for index in range(16)
    if expected[index].item() != replay[index].item()
        equal = false
print(equal)
QUI
[[ "$("$QUIDRA" "$TMP/neural-dropout-state.qui")" == "true" ]]

cat > "$TMP/neural-optimizers.qui" <<'QUI'
tensor<float32> sample_values = tensor.ones<float32>([1, 2])
tensor<float32> target = tensor.zeros<float32>([1, 1])

neural.Linear sgd_model = neural.Linear(input = 2, output = 1, seed = 3)
tensor<float32> sgd_before = sgd_model.weight.raw()
neural sgd_prediction = sgd_model.forward(neural.track(sample_values))
neural sgd_loss = neural.mse(sgd_prediction, target)
neural.Gradients sgd_gradients = neural.grad(sgd_loss)
neural.SGD sgd = neural.SGD(rate = 0.05)
neural.step(&sgd_model, &sgd, sgd_gradients)
tensor<float32> sgd_after = sgd_model.weight.raw()
print(sgd_before[0, 0].item() != sgd_after[0, 0].item())

neural.Linear adam_model = neural.Linear(input = 2, output = 1, seed = 4)
neural.Adam adam = neural.Adam(rate = 0.01)
for step_index in range(2)
    neural prediction = adam_model.forward(neural.track(sample_values))
    neural loss = neural.mse(prediction, target)
    neural.Gradients gradients = neural.grad(loss)
    neural.step(&adam_model, &adam, gradients)
print(adam_model.weight.raw()[0, 0].item() != 0.0)
QUI
[[ "$("$QUIDRA" "$TMP/neural-optimizers.qui")" == "$(printf 'true\ntrue')" ]]

cat > "$TMP/neural-bce-neural-target.qui" <<'QUI'
class BcePair
    neural.Parameter prediction_parameter
    neural.Parameter target_parameter

BcePair pair = BcePair(
    prediction_parameter = neural.Parameter(
        value = tensor.ones<float32>([1]) * 0.25
    ),
    target_parameter = neural.Parameter(
        value = tensor.ones<float32>([1])
    )
)
neural prediction_value = pair.prediction_parameter.track()
neural target_value = pair.target_parameter.track()
neural loss_value = neural.binary_cross_entropy(prediction_value, target_value)
neural.Gradients gradient_value = neural.grad(loss_value)
neural.SGD bce_optimizer = neural.SGD(rate = 0.1)
neural.step(&pair, &bce_optimizer, gradient_value)
print(pair.target_parameter.raw()[0].item() < 1.0)
QUI
[[ "$("$QUIDRA" "$TMP/neural-bce-neural-target.qui")" == "true" ]]

cat > "$TMP/neural-adam-dynamic-branch.qui" <<'QUI'
class BranchModel
    neural.Linear early_layer
    neural.Linear late_layer

BranchModel branch_model = BranchModel(
    early_layer = neural.Linear(input = 2, output = 1, seed = 31),
    late_layer = neural.Linear(input = 2, output = 1, seed = 47)
)
BranchModel reference_model = BranchModel(
    early_layer = neural.Linear(input = 2, output = 1, seed = 31),
    late_layer = neural.Linear(input = 2, output = 1, seed = 47)
)

tensor<float32> branch_values = tensor.ones<float32>([1, 2])
tensor<float32> branch_target = tensor.zeros<float32>([1, 1])

neural.Adam branch_optimizer = neural.Adam(rate = 0.01)
neural early_prediction = branch_model.early_layer.forward(neural.track(branch_values))
neural early_loss = neural.mse(early_prediction, branch_target)
neural.Gradients early_gradients = neural.grad(early_loss)
neural.step(&branch_model, &branch_optimizer, early_gradients)

neural late_prediction = branch_model.late_layer.forward(neural.track(branch_values))
neural late_loss = neural.mse(late_prediction, branch_target)
neural.Gradients late_gradients = neural.grad(late_loss)
neural.step(&branch_model, &branch_optimizer, late_gradients)

neural.Adam reference_optimizer = neural.Adam(rate = 0.01)
neural reference_prediction = reference_model.late_layer.forward(neural.track(branch_values))
neural reference_loss = neural.mse(reference_prediction, branch_target)
neural.Gradients reference_gradients = neural.grad(reference_loss)
neural.step(&reference_model, &reference_optimizer, reference_gradients)

print(
    branch_model.late_layer.weight.raw()[0, 0].item()
    == reference_model.late_layer.weight.raw()[0, 0].item()
)
QUI
[[ "$("$QUIDRA" "$TMP/neural-adam-dynamic-branch.qui")" == "true" ]]

cat > "$TMP/neural-adam-path-mismatch.qui" <<'QUI'
class LeftModel
    neural.Linear left_layer

class RightModel
    neural.Linear right_layer

LeftModel left_model = LeftModel(
    left_layer = neural.Linear(input = 2, output = 1, seed = 71)
)
RightModel right_model = RightModel(
    right_layer = neural.Linear(input = 2, output = 1, seed = 71)
)
tensor<float32> path_values = tensor.ones<float32>([1, 2])
tensor<float32> path_target = tensor.zeros<float32>([1, 1])
neural.Adam path_optimizer = neural.Adam(rate = 0.01)

neural left_prediction = left_model.left_layer.forward(neural.track(path_values))
neural left_loss = neural.mse(left_prediction, path_target)
neural.Gradients left_gradients = neural.grad(left_loss)
neural.step(&left_model, &path_optimizer, left_gradients)

neural right_prediction = right_model.right_layer.forward(neural.track(path_values))
neural right_loss = neural.mse(right_prediction, path_target)
neural.Gradients right_gradients = neural.grad(right_loss)
neural.step(&right_model, &path_optimizer, right_gradients)
QUI
set +e
"$QUIDRA" "$TMP/neural-adam-path-mismatch.qui" >"$TMP/neural-adam-path-mismatch.out" 2>"$TMP/neural-adam-path-mismatch.err"
adam_path_rc=$?
set -e
[[ "$adam_path_rc" -eq 101 ]]
grep -q 'structural path' "$TMP/neural-adam-path-mismatch.err"

cat > "$TMP/neural-step-mismatch.qui" <<'QUI'
tensor<float32> sample_values = tensor.ones<float32>([1, 2])
neural.Linear source = neural.Linear(input = 2, output = 1, seed = 1)
neural.Linear other = neural.Linear(input = 2, output = 1, seed = 2)
neural prediction = source.forward(neural.track(sample_values))
neural loss = neural.mse(prediction, tensor.zeros<float32>([1, 1]))
neural.Gradients gradients = neural.grad(loss)
neural.SGD optimizer = neural.SGD()
neural.step(&other, &optimizer, gradients)
QUI
set +e
"$QUIDRA" "$TMP/neural-step-mismatch.qui" >"$TMP/neural-step-mismatch.out" 2>"$TMP/neural-step-mismatch.err"
neural_step_mismatch_rc=$?
set -e
[[ "$neural_step_mismatch_rc" -eq 101 ]]
grep -q 'do not belong to the supplied model' "$TMP/neural-step-mismatch.err"

cat > "$TMP/neural-generic-forward.qui" <<'QUI'
class UnifiedModel
    neural.Linear layer
    neural.BatchNorm norm
    neural.Dropout dropout

    X forward<X, M>(X features, M execution)
        X projected = layer.forward(features)
        projected = norm.forward(projected, mode = execution)
        projected = neural.relu(projected)
        projected = dropout.forward(projected, mode = execution)
        return projected

UnifiedModel unified_model = UnifiedModel(
    layer = neural.Linear(input = 2, output = 2, seed = 13),
    norm = neural.BatchNorm(features = 2),
    dropout = neural.Dropout(rate = 0.25, seed = 19)
)
tensor<float32> unified_values = tensor.ones<float32>([2, 2])
tensor<float32> inferred_values = unified_model.forward(
    unified_values, execution = neural.inference
)
print(inferred_values.shape()[0])
print(inferred_values.shape()[1])

neural trained_values = unified_model.forward(
    neural.track(unified_values), execution = neural.training
)
print(trained_values.untrack().shape()[0])
print(trained_values.untrack().shape()[1])
QUI
[[ "$("$QUIDRA" "$TMP/neural-generic-forward.qui")" == "$(printf '2\n2\n2\n2')" ]]

cat > "$TMP/neural-load-partial.qui" <<'QUI'
class PartialLoadState
    int value

PartialLoadState partial_state = PartialLoadState()
neural.load(&model = &partial_state, path = "state.quistate")
QUI
set +e
"$QUIDRA" check "$TMP/neural-load-partial.qui" --json >"$TMP/neural-load-partial.json"
neural_load_partial_rc=$?
set -e
[[ "$neural_load_partial_rc" -eq 1 ]]
grep -q 'UNINITIALIZED_ARGUMENT' "$TMP/neural-load-partial.json"

cat > "$TMP/neural-state-roundtrip.qui" <<QUI
class StatefulModel
    neural.Linear layer
    neural.BatchNorm norm
    neural.Dropout dropout
    neural.State<int> epoch

StatefulModel model = StatefulModel(
    layer = neural.Linear(input = 2, output = 1, seed = 11),
    norm = neural.BatchNorm(features = 1),
    dropout = neural.Dropout(rate = 0.25, seed = 9),
    epoch = neural.State<int>(value = 3)
)
neural.Adam optimizer = neural.Adam(rate = 0.01)

tensor<float32> sample_values = tensor.ones<float32>([1, 2])
tensor<float32> target = tensor.zeros<float32>([1, 1])

neural first_prediction = model.layer.forward(neural.track(sample_values))
neural first_loss = neural.mse(first_prediction, target)
neural.Gradients first_gradients = neural.grad(first_loss)
neural.step(&model = &model, &optimizer = &optimizer, gradients = first_gradients)

model.norm.running_mean.value[0] = 4.0
model.epoch.value = 7
float32 saved_weight = model.layer.weight.raw()[0, 0].item()
neural.save(model, optimizer, path = "$TMP/training.quistate")

neural second_prediction = model.layer.forward(neural.track(sample_values))
neural second_loss = neural.mse(second_prediction, target)
neural.Gradients second_gradients = neural.grad(second_loss)
neural.step(&model = &model, &optimizer = &optimizer, gradients = second_gradients)
float32 expected_second_weight = model.layer.weight.raw()[0, 0].item()

model.norm.running_mean.value[0] = 99.0
model.epoch.value = 99
neural.load(&model = &model, &optimizer = &optimizer, path = "$TMP/training.quistate")
print(model.layer.weight.raw()[0, 0].item() == saved_weight)
print(model.norm.running_mean.value[0].item())
print(model.epoch.value)

neural replay_prediction = model.layer.forward(neural.track(sample_values))
neural replay_loss = neural.mse(replay_prediction, target)
neural.Gradients replay_gradients = neural.grad(replay_loss)
neural.step(&model = &model, &optimizer = &optimizer, gradients = replay_gradients)
print(model.layer.weight.raw()[0, 0].item() == expected_second_weight)
QUI
[[ "$("$QUIDRA" "$TMP/neural-state-roundtrip.qui")" == "$(printf 'true\n4.0\n7\ntrue')" ]]

cat > "$TMP/neural-state-schema-save.qui" <<QUI
class FirstModel
    neural.Linear layer
FirstModel model = FirstModel(layer = neural.Linear(input = 2, output = 1, seed = 1))
neural.save(model, path = "$TMP/schema.quistate")
QUI
"$QUIDRA" "$TMP/neural-state-schema-save.qui"

cat > "$TMP/neural-state-schema-load.qui" <<QUI
class DifferentModel
    neural.Linear layer
DifferentModel model = DifferentModel(layer = neural.Linear(input = 2, output = 1, seed = 1))
neural.load(&model = &model, path = "$TMP/schema.quistate")
QUI
set +e
"$QUIDRA" "$TMP/neural-state-schema-load.qui" >"$TMP/neural-state-schema.out" 2>"$TMP/neural-state-schema.err"
neural_state_schema_rc=$?
set -e
[[ "$neural_state_schema_rc" -eq 101 ]]
grep -q 'schema mismatch' "$TMP/neural-state-schema.err"

cp "$TMP/schema.quistate" "$TMP/corrupt.quistate"
python3 - "$TMP/corrupt.quistate" <<'PY'
import sys
path=sys.argv[1]
data=bytearray(open(path,"rb").read())
assert len(data)>24
data[len(data)//2] ^= 0x01
open(path,"wb").write(data)
PY
cat > "$TMP/neural-state-corrupt.qui" <<QUI
class FirstModel
    neural.Linear layer
FirstModel model = FirstModel(layer = neural.Linear(input = 2, output = 1, seed = 1))
neural.load(&model = &model, path = "$TMP/corrupt.quistate")
QUI
set +e
"$QUIDRA" "$TMP/neural-state-corrupt.qui" >"$TMP/neural-state-corrupt.out" 2>"$TMP/neural-state-corrupt.err"
neural_state_corrupt_rc=$?
set -e
[[ "$neural_state_corrupt_rc" -eq 101 ]]
grep -q 'checksum mismatch' "$TMP/neural-state-corrupt.err"

cat > "$TMP/neural-state-text-save.qui" <<QUI
class TextState
    int counter
    string label

TextState model = TextState(counter = 7, label = "valid-text")
neural.save(model, path = "$TMP/text-state.quistate")
QUI
"$QUIDRA" "$TMP/neural-state-text-save.qui"

cp "$TMP/text-state.quistate" "$TMP/text-state-invalid.quistate"
python3 - "$TMP/text-state-invalid.quistate" <<'PY'
import struct
import sys

path = sys.argv[1]
data = bytearray(open(path, "rb").read())
needle = b"valid-text"
position = data.find(needle)
assert position >= 0
data[position] = 0xff

hash_value = 1469598103934665603
for byte in data[:-8]:
    hash_value ^= byte
    hash_value = (hash_value * 1099511628211) & 0xffffffffffffffff
data[-8:] = struct.pack("<Q", hash_value)
open(path, "wb").write(data)
PY

cat > "$TMP/neural-state-text-load.qui" <<QUI
class TextState
    int counter
    string label

TextState model = TextState(counter = 99, label = "before")
neural.load(&model = &model, path = "$TMP/text-state-invalid.quistate")
QUI
set +e
"$QUIDRA" "$TMP/neural-state-text-load.qui" >"$TMP/neural-state-text-load.out" 2>"$TMP/neural-state-text-load.err"
neural_state_text_rc=$?
set -e
[[ "$neural_state_text_rc" -eq 101 ]]
grep -q '.quistate string is not valid UTF-8 text without NUL' "$TMP/neural-state-text-load.err"

cat > "$TMP/neural-state-extension.qui" <<QUI
class FirstModel
    neural.Linear layer
FirstModel model = FirstModel(layer = neural.Linear(input = 2, output = 1, seed = 1))
neural.save(model, path = "$TMP/not-state.bin")
QUI
set +e
"$QUIDRA" "$TMP/neural-state-extension.qui" >"$TMP/neural-state-extension.out" 2>"$TMP/neural-state-extension.err"
neural_state_extension_rc=$?
set -e
[[ "$neural_state_extension_rc" -eq 101 ]]
grep -q 'must end with .quistate' "$TMP/neural-state-extension.err"

cat > "$TMP/neural-parameter-direct-write.qui" <<'QUI'
neural.Parameter p = neural.Parameter(value = tensor.ones<float32>([1]))
p.value = tensor.zeros<float32>([1])
QUI
set +e
"$QUIDRA" check "$TMP/neural-parameter-direct-write.qui" --json >"$TMP/neural-parameter-direct-write.json"
parameter_direct_write_rc=$?
set -e
[[ "$parameter_direct_write_rc" -eq 1 ]]
grep -q 'persistent identity' "$TMP/neural-parameter-direct-write.json"

cat > "$TMP/neural-parameter-element-write.qui" <<'QUI'
neural.Parameter p = neural.Parameter(value = tensor.ones<float32>([1]))
p.value[0] = 2.0
QUI
set +e
"$QUIDRA" check "$TMP/neural-parameter-element-write.qui" --json >"$TMP/neural-parameter-element-write.json"
parameter_element_write_rc=$?
set -e
[[ "$parameter_element_write_rc" -eq 1 ]]
grep -q 'persistent identity' "$TMP/neural-parameter-element-write.json"

cat > "$TMP/neural-parameter-reference-write.qui" <<'QUI'
neural.Parameter p = neural.Parameter(value = tensor.ones<float32>([1]))
tensor<float32> &alias = &p.value
alias[0] = 2.0
QUI
set +e
"$QUIDRA" check "$TMP/neural-parameter-reference-write.qui" --json >"$TMP/neural-parameter-reference-write.json"
parameter_reference_write_rc=$?
set -e
[[ "$parameter_reference_write_rc" -eq 1 ]]
grep -q 'persistent identity' "$TMP/neural-parameter-reference-write.json"


cat > "$TMP/neural-track-snapshot.qui" <<'QUI'
tensor<float32> source = tensor.ones<float32>([1])
neural tracked = neural.track(source)
source[0] = 2.0
print(tracked.untrack()[0].item())
print(source[0].item())
QUI
[[ "$("$QUIDRA" "$TMP/neural-track-snapshot.qui")" == "$(printf '1.0\n2.0')" ]]

cat > "$TMP/neural-batchnorm-mode-state.qui" <<'QUI'
neural.BatchNorm norm = neural.BatchNorm(features = 2)
tensor<float32> values = tensor.ones<float32>([2, 2])
float32 mean0 = norm.running_mean.value[0].item()
float32 mean1 = norm.running_mean.value[1].item()
tensor<float32> inference = norm.forward(values, mode = neural.inference)
print(inference.shape()[1])
print(norm.running_mean.value[0].item() == mean0)
print(norm.running_mean.value[1].item() == mean1)
neural training = norm.forward(neural.track(values), mode = neural.training)
print(training.untrack().shape()[1])
print(norm.running_mean.value[0].item() != mean0)
print(norm.running_mean.value[1].item() != mean1)
QUI
[[ "$("$QUIDRA" "$TMP/neural-batchnorm-mode-state.qui")" == "$(printf '2\ntrue\ntrue\n2\ntrue\ntrue')" ]]

cat > "$TMP/neural-dropout-copy-mode.qui" <<'QUI'
class DropoutPair
    neural.Dropout dropout

DropoutPair first = DropoutPair(
    dropout = neural.Dropout(rate = 0.5, seed = 123)
)
DropoutPair second = first
tensor<float32> values = tensor.ones<float32>([64])

tensor<float32> identity = first.dropout.forward(values, mode = neural.inference)
tensor<float32> first_mask = first.dropout.forward(
    neural.track(values), mode = neural.training
).untrack()
tensor<float32> second_mask = second.dropout.forward(
    neural.track(values), mode = neural.training
).untrack()

bool identity_ok = true
bool first_equal = true
for index in range(64)
    if identity[index].item() != 1.0
        identity_ok = false
    if first_mask[index].item() != second_mask[index].item()
        first_equal = false

tensor<float32> first_next = first.dropout.forward(
    neural.track(values), mode = neural.training
).untrack()
tensor<float32> second_next = second.dropout.forward(
    neural.track(values), mode = neural.training
).untrack()

bool second_equal = true
for index in range(64)
    if first_next[index].item() != second_next[index].item()
        second_equal = false

print(identity_ok)
print(first_equal)
print(second_equal)
QUI
[[ "$("$QUIDRA" "$TMP/neural-dropout-copy-mode.qui")" == "$(printf 'true\ntrue\ntrue')" ]]

cat > "$TMP/neural-conv2d-boundaries.qui" <<'QUI'
neural.Conv2D conv = neural.Conv2D(
    input = 2,
    output = 3,
    kernel = 3,
    stride = 2,
    padding = 1,
    seed = 41
)
tensor<float32> image = tensor.ones<float32>([2, 2, 5, 7])
tensor<float32> inference = conv.forward(image)
print(inference.shape()[0])
print(inference.shape()[1])
print(inference.shape()[2])
print(inference.shape()[3])

float32 before = conv.weight.raw()[0, 0, 0, 0].item()
neural prediction = conv.forward(neural.track(image))
neural loss = neural.mse(prediction, tensor.zeros<float32>([2, 3, 3, 4]))
neural.Gradients gradients = neural.grad(loss)
neural.SGD optimizer = neural.SGD(rate = 0.01)
neural.step(&conv, &optimizer, gradients)
print(conv.weight.raw()[0, 0, 0, 0].item() != before)
QUI
[[ "$("$QUIDRA" "$TMP/neural-conv2d-boundaries.qui")" == "$(printf '2\n3\n3\n4\ntrue')" ]]

cat > "$TMP/neural-conv2d-kernel-too-large.qui" <<'QUI'
neural.Conv2D conv = neural.Conv2D(input = 1, output = 1, kernel = 5)
tensor<float32> image = tensor.ones<float32>([1, 1, 3, 3])
tensor<float32> output = conv.forward(image)
print(output.shape()[0])
QUI
set +e
"$QUIDRA" "$TMP/neural-conv2d-kernel-too-large.qui" >"$TMP/neural-conv2d-kernel-too-large.out" 2>"$TMP/neural-conv2d-kernel-too-large.err"
conv_kernel_rc=$?
set -e
[[ "$conv_kernel_rc" -eq 101 ]]
grep -q 'kernel is larger than padded input' "$TMP/neural-conv2d-kernel-too-large.err"

cat > "$TMP/neural-conv2d-dtype-mismatch.qui" <<'QUI'
neural.Conv2D conv = neural.Conv2D(input = 1, output = 1, kernel = 1)
tensor<float> image = tensor.ones<float>([1, 1, 1, 1])
tensor<float> output = conv.forward(image)
print(output.shape()[0])
QUI
set +e
"$QUIDRA" check "$TMP/neural-conv2d-dtype-mismatch.qui" --json >"$TMP/neural-conv2d-dtype-mismatch.json"
conv_dtype_rc=$?
set -e
[[ "$conv_dtype_rc" -eq 1 ]]
grep -q 'Conv2D input, weight, and bias element types must match' "$TMP/neural-conv2d-dtype-mismatch.json"

cat > "$TMP/neural-load-extension-invalid.qui" <<QUI
class ExtensionModel
    neural.Linear layer

ExtensionModel model = ExtensionModel(
    layer = neural.Linear(input = 1, output = 1, seed = 1)
)
neural.load(&model = &model, path = "$TMP/not-state.bin")
QUI
set +e
"$QUIDRA" "$TMP/neural-load-extension-invalid.qui" >"$TMP/neural-load-extension-invalid.out" 2>"$TMP/neural-load-extension-invalid.err"
load_extension_rc=$?
set -e
[[ "$load_extension_rc" -eq 101 ]]
grep -q 'must end with .quistate' "$TMP/neural-load-extension-invalid.err"


cat > "$TMP/neural-bce-invalid-probability.qui" <<'QUI'
neural prediction = neural.track(tensor.ones<float32>([1]) * 1.25)
tensor<float32> target = tensor.ones<float32>([1])
neural loss = neural.binary_cross_entropy(prediction, target)
print(loss.untrack().item())
QUI
set +e
"$QUIDRA" "$TMP/neural-bce-invalid-probability.qui" >"$TMP/neural-bce-invalid-probability.out" 2>"$TMP/neural-bce-invalid-probability.err"
bce_probability_rc=$?
set -e
[[ "$bce_probability_rc" -eq 101 ]]
grep -q 'requires finite prediction and target values in \[0,1\]' "$TMP/neural-bce-invalid-probability.err"

cat > "$TMP/neural-bce-invalid-target.qui" <<'QUI'
neural prediction = neural.track(tensor.ones<float32>([1]) * 0.5)
tensor<float32> target = tensor.zeros<float32>([1])
target[0] = float32(-0.25)
neural loss = neural.binary_cross_entropy(prediction, target)
print(loss.untrack().item())
QUI
set +e
"$QUIDRA" "$TMP/neural-bce-invalid-target.qui" >"$TMP/neural-bce-invalid-target.out" 2>"$TMP/neural-bce-invalid-target.err"
bce_target_rc=$?
set -e
[[ "$bce_target_rc" -eq 101 ]]
grep -q 'requires finite prediction and target values in \[0,1\]' "$TMP/neural-bce-invalid-target.err"


cat > "$TMP/neural-cross-entropy-target-shape.qui" <<'QUI'
neural logits = neural.track(tensor.ones<float32>([1, 2]))
tensor<int> target = tensor.zeros<int>([1, 1])
neural loss = neural.cross_entropy(logits, target)
print(loss.untrack().item())
QUI
set +e
"$QUIDRA" "$TMP/neural-cross-entropy-target-shape.qui" >"$TMP/neural-cross-entropy-target-shape.out" 2>"$TMP/neural-cross-entropy-target-shape.err"
ce_shape_rc=$?
set -e
[[ "$ce_shape_rc" -eq 101 ]]
grep -q 'target must have shape \[N\]' "$TMP/neural-cross-entropy-target-shape.err"

cat > "$TMP/neural-cross-entropy-zero-classes.qui" <<'QUI'
neural logits = neural.track(tensor.ones<float32>([1, 0]))
tensor<int> target = tensor.zeros<int>([1])
neural loss = neural.cross_entropy(logits, target)
print(loss.untrack().item())
QUI
set +e
"$QUIDRA" "$TMP/neural-cross-entropy-zero-classes.qui" >"$TMP/neural-cross-entropy-zero-classes.out" 2>"$TMP/neural-cross-entropy-zero-classes.err"
ce_classes_rc=$?
set -e
[[ "$ce_classes_rc" -eq 101 ]]
grep -q 'C > 0' "$TMP/neural-cross-entropy-zero-classes.err"


cat > "$TMP/neural-batchnorm-dtype-mismatch.qui" <<'QUI'
neural.BatchNorm<float> norm = neural.BatchNorm<float>(features = 2)
tensor<float32> values = tensor.ones<float32>([2, 2])
tensor<float32> output = norm.forward(values, mode = neural.inference)
print(output.shape()[0])
QUI
set +e
"$QUIDRA" check "$TMP/neural-batchnorm-dtype-mismatch.qui" --json >"$TMP/neural-batchnorm-dtype-mismatch.json"
batchnorm_dtype_rc=$?
set -e
[[ "$batchnorm_dtype_rc" -eq 1 ]]
grep -q 'BatchNorm input and state element types must match' "$TMP/neural-batchnorm-dtype-mismatch.json"

cat > "$TMP/neural-float64-layers.qui" <<'QUI'
neural.Linear<float> linear_layer = neural.Linear<float>(input = 2, output = 2, seed = 101)
tensor<float> linear_input = tensor.ones<float>([2, 2])
tensor<float> linear_output = linear_layer.forward(linear_input)
print(linear_output.shape()[0])
print(linear_output.shape()[1])

neural.Conv2D<float> conv = neural.Conv2D<float>(
    input = 1,
    output = 2,
    kernel = 3,
    stride = 1,
    padding = 1,
    seed = 103
)
tensor<float> image = tensor.ones<float>([1, 1, 3, 5])
tensor<float> conv_output = conv.forward(image)
print(conv_output.shape()[0])
print(conv_output.shape()[1])
print(conv_output.shape()[2])
print(conv_output.shape()[3])

neural.BatchNorm<float> norm = neural.BatchNorm<float>(features = 2)
tensor<float> inference = norm.forward(conv_output, mode = neural.inference)
print(inference.shape()[1])
auto training = norm.forward(neural.track(conv_output), mode = neural.training)
print(training.untrack().shape()[1])
QUI
[[ "$("$QUIDRA" "$TMP/neural-float64-layers.qui")" == "$(printf '2\n2\n1\n2\n3\n5\n2\n2')" ]]


# Neural namespace extension uses the same additive mechanism as every standard namespace.
mkdir -p "$TMP/packages/neural_extra"
cat > "$TMP/packages/neural_extra/main.qui" <<'QUI'
int twice(int value)
    return value * 2
QUI
cat > "$TMP/packages/neural_extra/quidra.package.json" <<'JSON'
{"extends":["neural"]}
JSON
cat > "$TMP/neural-namespace-extension.qui" <<'QUI'
import neural += neural_extra
print(neural.twice(21))
QUI
[[ "$(QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" "$TMP/neural-namespace-extension.qui")" == "42" ]]

mkdir -p "$TMP/packages/neural_collision"
cat > "$TMP/packages/neural_collision/main.qui" <<'QUI'
int relu(int value)
    return value
QUI
cat > "$TMP/packages/neural_collision/quidra.package.json" <<'JSON'
{"extends":["neural"]}
JSON
cat > "$TMP/neural-namespace-standard-collision.qui" <<'QUI'
import neural += neural_collision
print(1)
QUI
set +e
QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" check "$TMP/neural-namespace-standard-collision.qui" --json >"$TMP/neural-namespace-standard-collision.json" 2>&1
neural_namespace_collision_rc=$?
set -e
[[ "$neural_namespace_collision_rc" -eq 1 ]]
grep -q 'NAMESPACE_EXTENSION_COLLISION' "$TMP/neural-namespace-standard-collision.json"

cat > "$TMP/neural-extension-no-package-binding.qui" <<'QUI'
import neural += neural_extra
print(neural_extra.twice(1))
QUI
set +e
QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" check "$TMP/neural-extension-no-package-binding.qui" --json >"$TMP/neural-extension-no-package-binding.json" 2>&1
neural_extension_binding_rc=$?
set -e
[[ "$neural_extension_binding_rc" -eq 1 ]]
grep -q 'neural_extra' "$TMP/neural-extension-no-package-binding.json"

# Floating neural model classes reject unsupported generic element types before runtime.
for neural_type in Parameter Linear Conv2D BatchNorm; do
    cat > "$TMP/neural-invalid-generic-$neural_type.qui" <<QUI
neural.$neural_type<int> invalid_value
QUI
    set +e
    "$QUIDRA" check "$TMP/neural-invalid-generic-$neural_type.qui" --json >"$TMP/neural-invalid-generic-$neural_type.json" 2>&1
    neural_generic_rc=$?
    set -e
    [[ "$neural_generic_rc" -eq 1 ]]
    grep -q 'support only float32 or float element types' "$TMP/neural-invalid-generic-$neural_type.json"
done

# Shared DAG edges must accumulate into the same Parameter exactly once per path.
cat > "$TMP/neural-shared-dag.qui" <<'QUI'
class SharedDagModel
    neural.Parameter value

SharedDagModel model = SharedDagModel(
    value = neural.Parameter(value = tensor.ones<float32>([1]))
)
neural tracked = model.value.track()
neural doubled = tracked + tracked
neural loss = neural.mse(doubled, tensor.zeros<float32>([1]))
neural.Gradients gradients = neural.grad(loss)
neural.SGD optimizer = neural.SGD(rate = 0.1)
neural.step(&model, &optimizer, gradients)
float updated = float(model.value.raw()[0].item())
print(math.abs(updated - 0.2) < 0.000001)
QUI
[[ "$("$QUIDRA" "$TMP/neural-shared-dag.qui")" == "true" ]]

# A Gradients value outliving its original model must never match a later allocation.
cat > "$TMP/neural-stale-parameter-identity.qui" <<'QUI'
class IdentityModel
    neural.Parameter value

neural.Gradients make_stale_gradients()
    IdentityModel temporary = IdentityModel(
        value = neural.Parameter(value = tensor.ones<float32>([1]))
    )
    neural prediction = temporary.value.track()
    neural loss = neural.mse(prediction, tensor.zeros<float32>([1]))
    return neural.grad(loss)

neural.Gradients stale = make_stale_gradients()
IdentityModel replacement = IdentityModel(
    value = neural.Parameter(value = tensor.ones<float32>([1]))
)
neural.SGD optimizer = neural.SGD(rate = 0.1)
neural.step(&replacement, &optimizer, stale)
QUI
set +e
"$QUIDRA" "$TMP/neural-stale-parameter-identity.qui" >"$TMP/neural-stale-parameter-identity.out" 2>"$TMP/neural-stale-parameter-identity.err"
stale_identity_rc=$?
set -e
[[ "$stale_identity_rc" -eq 101 ]]
grep -q 'do not belong to the supplied model' "$TMP/neural-stale-parameter-identity.err"


# Deep dynamic graphs use explicit runtime traversal rather than the native C++ call stack.
cat > "$TMP/neural-deep-graph.qui" <<'QUI'
class DeepGraphModel
    neural.Parameter value

DeepGraphModel model = DeepGraphModel(
    value = neural.Parameter(value = tensor.ones<float32>([1]))
)
neural current = model.value.track()
for iteration in range(32768)
    current = neural.relu(current)
neural loss = neural.mse(current, tensor.zeros<float32>([1]))
neural.Gradients gradients = neural.grad(loss)
neural.SGD optimizer = neural.SGD(rate = 0.1)
neural.step(&model, &optimizer, gradients)
print(model.value.raw()[0].item() < 1.0)
QUI
"$QUIDRA" "$TMP/neural-deep-graph.qui" >"$TMP/neural-deep-graph.out"
[[ "$(cat "$TMP/neural-deep-graph.out")" == "true" ]]


# Undefined empty reductions/normalizations are rejected instead of silently producing 0.
cat > "$TMP/neural-softmax-empty-axis.qui" <<'QUI'
tensor<float32> values = tensor.ones<float32>([2, 0])
neural result = neural.softmax(neural.track(values))
print(result.untrack().shape()[0])
QUI
set +e
"$QUIDRA" "$TMP/neural-softmax-empty-axis.qui" >"$TMP/neural-softmax-empty-axis.out" 2>"$TMP/neural-softmax-empty-axis.err"
softmax_empty_rc=$?
set -e
[[ "$softmax_empty_rc" -eq 101 ]]
grep -q 'non-empty last axis' "$TMP/neural-softmax-empty-axis.err"

cat > "$TMP/neural-mse-empty.qui" <<'QUI'
neural prediction = neural.track(tensor.ones<float32>([0]))
tensor<float32> target = tensor.zeros<float32>([0])
neural loss = neural.mse(prediction, target)
print(loss.untrack().item())
QUI
set +e
"$QUIDRA" "$TMP/neural-mse-empty.qui" >"$TMP/neural-mse-empty.out" 2>"$TMP/neural-mse-empty.err"
mse_empty_rc=$?
set -e
[[ "$mse_empty_rc" -eq 101 ]]
grep -q 'require at least one element' "$TMP/neural-mse-empty.err"

cat > "$TMP/neural-bce-empty.qui" <<'QUI'
neural prediction = neural.track(tensor.ones<float32>([0]))
tensor<float32> target = tensor.zeros<float32>([0])
neural loss = neural.binary_cross_entropy(prediction, target)
print(loss.untrack().item())
QUI
set +e
"$QUIDRA" "$TMP/neural-bce-empty.qui" >"$TMP/neural-bce-empty.out" 2>"$TMP/neural-bce-empty.err"
bce_empty_rc=$?
set -e
[[ "$bce_empty_rc" -eq 101 ]]
grep -q 'require at least one element' "$TMP/neural-bce-empty.err"

cat > "$TMP/neural-cross-entropy-empty-batch.qui" <<'QUI'
neural logits = neural.track(tensor.ones<float32>([0, 2]))
tensor<int> target = tensor.zeros<int>([0])
neural loss = neural.cross_entropy(logits, target)
print(loss.untrack().item())
QUI
set +e
"$QUIDRA" "$TMP/neural-cross-entropy-empty-batch.qui" >"$TMP/neural-cross-entropy-empty-batch.out" 2>"$TMP/neural-cross-entropy-empty-batch.err"
ce_empty_rc=$?
set -e
[[ "$ce_empty_rc" -eq 101 ]]
grep -q 'N > 0 and C > 0' "$TMP/neural-cross-entropy-empty-batch.err"


# Numerical gradient checks exercise the public grad -> step contract without exposing Gradients internals.
cat > "$TMP/neural-activation-gradients.qui" <<'QUI'
class ActivationModel
    neural.Parameter<float> value

float epsilon = 0.000001
float rate = 0.0001

tensor<float> soft_base = tensor.zeros<float>([1, 3])
soft_base[0, 0] = 0.2
soft_base[0, 1] = -0.4
soft_base[0, 2] = 0.7
tensor<float> soft_target = tensor.zeros<float>([1, 3])
soft_target[0, 0] = 0.1
soft_target[0, 1] = 0.7
soft_target[0, 2] = 0.2

tensor<float> soft_plus = soft_base
tensor<float> soft_minus = soft_base
soft_plus[0, 1] = soft_plus[0, 1].item() + epsilon
soft_minus[0, 1] = soft_minus[0, 1].item() - epsilon
float soft_plus_loss = neural.mse(
    neural.softmax(neural.track(soft_plus)), soft_target
).untrack().item()
float soft_minus_loss = neural.mse(
    neural.softmax(neural.track(soft_minus)), soft_target
).untrack().item()
float soft_numerical = (soft_plus_loss - soft_minus_loss) / (2.0 * epsilon)

ActivationModel soft_model = ActivationModel(
    value = neural.Parameter<float>(value = soft_base)
)
float soft_before = soft_model.value.raw()[0, 1].item()
neural<float> soft_prediction = neural.softmax(soft_model.value.track())
neural<float> soft_loss = neural.mse(soft_prediction, soft_target)
neural.Gradients soft_gradients = neural.grad(soft_loss)
neural.SGD soft_optimizer = neural.SGD(rate = rate)
neural.step(&soft_model, &soft_optimizer, soft_gradients)
float soft_actual = (soft_before - soft_model.value.raw()[0, 1].item()) / rate
print(math.abs(soft_actual - soft_numerical) < 0.00001)

tensor<float> sigmoid_base = tensor.zeros<float>([1])
sigmoid_base[0] = 0.3
tensor<float> sigmoid_target = tensor.zeros<float>([1])
sigmoid_target[0] = 0.8
tensor<float> sigmoid_plus = sigmoid_base
tensor<float> sigmoid_minus = sigmoid_base
sigmoid_plus[0] = sigmoid_plus[0].item() + epsilon
sigmoid_minus[0] = sigmoid_minus[0].item() - epsilon
float sigmoid_plus_loss = neural.mse(
    neural.sigmoid(neural.track(sigmoid_plus)), sigmoid_target
).untrack().item()
float sigmoid_minus_loss = neural.mse(
    neural.sigmoid(neural.track(sigmoid_minus)), sigmoid_target
).untrack().item()
float sigmoid_numerical = (sigmoid_plus_loss - sigmoid_minus_loss) / (2.0 * epsilon)

ActivationModel sigmoid_model = ActivationModel(
    value = neural.Parameter<float>(value = sigmoid_base)
)
float sigmoid_before = sigmoid_model.value.raw()[0].item()
neural<float> sigmoid_prediction = neural.sigmoid(sigmoid_model.value.track())
neural<float> sigmoid_loss = neural.mse(sigmoid_prediction, sigmoid_target)
neural.Gradients sigmoid_gradients = neural.grad(sigmoid_loss)
neural.SGD sigmoid_optimizer = neural.SGD(rate = rate)
neural.step(&sigmoid_model, &sigmoid_optimizer, sigmoid_gradients)
float sigmoid_actual = (sigmoid_before - sigmoid_model.value.raw()[0].item()) / rate
print(math.abs(sigmoid_actual - sigmoid_numerical) < 0.00001)

tensor<float> tanh_base = tensor.zeros<float>([1])
tanh_base[0] = -0.4
tensor<float> tanh_target = tensor.zeros<float>([1])
tanh_target[0] = 0.2
tensor<float> tanh_plus = tanh_base
tensor<float> tanh_minus = tanh_base
tanh_plus[0] = tanh_plus[0].item() + epsilon
tanh_minus[0] = tanh_minus[0].item() - epsilon
float tanh_plus_loss = neural.mse(
    neural.tanh(neural.track(tanh_plus)), tanh_target
).untrack().item()
float tanh_minus_loss = neural.mse(
    neural.tanh(neural.track(tanh_minus)), tanh_target
).untrack().item()
float tanh_numerical = (tanh_plus_loss - tanh_minus_loss) / (2.0 * epsilon)

ActivationModel tanh_model = ActivationModel(
    value = neural.Parameter<float>(value = tanh_base)
)
float tanh_before = tanh_model.value.raw()[0].item()
neural<float> tanh_prediction = neural.tanh(tanh_model.value.track())
neural<float> tanh_loss = neural.mse(tanh_prediction, tanh_target)
neural.Gradients tanh_gradients = neural.grad(tanh_loss)
neural.SGD tanh_optimizer = neural.SGD(rate = rate)
neural.step(&tanh_model, &tanh_optimizer, tanh_gradients)
float tanh_actual = (tanh_before - tanh_model.value.raw()[0].item()) / rate
print(math.abs(tanh_actual - tanh_numerical) < 0.00001)

tensor<float> relu_base = tensor.zeros<float>([1])
tensor<float> relu_target = tensor.ones<float>([1])
ActivationModel relu_model = ActivationModel(
    value = neural.Parameter<float>(value = relu_base)
)
neural<float> relu_prediction = neural.relu(relu_model.value.track())
neural<float> relu_loss = neural.mse(relu_prediction, relu_target)
neural.Gradients relu_gradients = neural.grad(relu_loss)
neural.SGD relu_optimizer = neural.SGD(rate = 0.1)
neural.step(&relu_model, &relu_optimizer, relu_gradients)
print(relu_model.value.raw()[0].item() == 0.0)
QUI
[[ "$("$QUIDRA" "$TMP/neural-activation-gradients.qui")" == "$(printf 'true\ntrue\ntrue\ntrue')" ]]

cat > "$TMP/neural-linear-gradient-check.qui" <<'QUI'
class LinearGradientModel
    neural.Parameter<float> input_value
    neural.Linear<float> layer

tensor<float> base = tensor.zeros<float>([2, 2])
base[0, 0] = 0.5
base[0, 1] = -1.0
base[1, 0] = 1.5
base[1, 1] = 2.0
tensor<float> target = tensor.zeros<float>([2, 2])

LinearGradientModel model = LinearGradientModel(
    input_value = neural.Parameter<float>(value = base),
    layer = neural.Linear<float>(input = 2, output = 2, seed = 43)
)
tensor<float> prediction_before = model.layer.forward(base)
float weight_before = model.layer.weight.raw()[0, 0].item()
float bias_before = model.layer.bias.raw()[0].item()
float input_before = model.input_value.raw()[0, 0].item()

float expected_weight = 0.0
float expected_bias = 0.0
for batch in range(2)
    float upstream = 0.5 * prediction_before[batch, 0].item()
    expected_weight += upstream * base[batch, 0].item()
    expected_bias += upstream

float epsilon = 0.000001
tensor<float> plus = base
tensor<float> minus = base
plus[0, 0] = plus[0, 0].item() + epsilon
minus[0, 0] = minus[0, 0].item() - epsilon
float plus_loss = neural.mse(
    model.layer.forward(neural.track(plus)), target
).untrack().item()
float minus_loss = neural.mse(
    model.layer.forward(neural.track(minus)), target
).untrack().item()
float numerical_input = (plus_loss - minus_loss) / (2.0 * epsilon)

neural<float> prediction = model.layer.forward(model.input_value.track())
neural<float> loss = neural.mse(prediction, target)
neural.Gradients gradients = neural.grad(loss)
float rate = 0.0001
neural.SGD optimizer = neural.SGD(rate = rate)
neural.step(&model, &optimizer, gradients)

float actual_weight = (weight_before - model.layer.weight.raw()[0, 0].item()) / rate
float actual_bias = (bias_before - model.layer.bias.raw()[0].item()) / rate
float actual_input = (input_before - model.input_value.raw()[0, 0].item()) / rate
print(math.abs(actual_weight - expected_weight) < 0.000001)
print(math.abs(actual_bias - expected_bias) < 0.000001)
print(math.abs(actual_input - numerical_input) < 0.00001)
QUI
[[ "$("$QUIDRA" "$TMP/neural-linear-gradient-check.qui")" == "$(printf 'true\ntrue\ntrue')" ]]

cat > "$TMP/neural-conv2d-gradient-check.qui" <<'QUI'
class ConvGradientModel
    neural.Parameter<float> input_value
    neural.Conv2D<float> conv

tensor<float> base = tensor.ones<float>([2, 2, 4, 5])
tensor<float> target = tensor.zeros<float>([2, 2, 4, 5])
ConvGradientModel model = ConvGradientModel(
    input_value = neural.Parameter<float>(value = base),
    conv = neural.Conv2D<float>(
        input = 2,
        output = 2,
        kernel = 3,
        stride = 1,
        padding = 1,
        seed = 53
    )
)

tensor<float> prediction_before = model.conv.forward(base)
float weight_before = model.conv.weight.raw()[0, 0, 1, 1].item()
float bias_before = model.conv.bias.raw()[0].item()
float input_before = model.input_value.raw()[0, 0, 1, 2].item()

float expected_weight = 0.0
float expected_bias = 0.0
for batch in range(2)
    for y in range(4)
        for x in range(5)
            float upstream = prediction_before[batch, 0, y, x].item() / 40.0
            expected_weight += upstream
            expected_bias += upstream

float epsilon = 0.000001
tensor<float> plus = base
tensor<float> minus = base
plus[0, 0, 1, 2] = plus[0, 0, 1, 2].item() + epsilon
minus[0, 0, 1, 2] = minus[0, 0, 1, 2].item() - epsilon
float plus_loss = neural.mse(
    model.conv.forward(neural.track(plus)), target
).untrack().item()
float minus_loss = neural.mse(
    model.conv.forward(neural.track(minus)), target
).untrack().item()
float numerical_input = (plus_loss - minus_loss) / (2.0 * epsilon)

neural<float> prediction = model.conv.forward(model.input_value.track())
neural<float> loss = neural.mse(prediction, target)
neural.Gradients gradients = neural.grad(loss)
float rate = 0.0001
neural.SGD optimizer = neural.SGD(rate = rate)
neural.step(&model, &optimizer, gradients)

float actual_weight = (weight_before - model.conv.weight.raw()[0, 0, 1, 1].item()) / rate
float actual_bias = (bias_before - model.conv.bias.raw()[0].item()) / rate
float actual_input = (input_before - model.input_value.raw()[0, 0, 1, 2].item()) / rate
print(math.abs(actual_weight - expected_weight) < 0.000001)
print(math.abs(actual_bias - expected_bias) < 0.000001)
print(math.abs(actual_input - numerical_input) < 0.00001)
QUI
[[ "$("$QUIDRA" "$TMP/neural-conv2d-gradient-check.qui")" == "$(printf 'true\ntrue\ntrue')" ]]

cat > "$TMP/neural-batchnorm-gradient-check.qui" <<'QUI'
class BatchNormGradientModel
    neural.Parameter<float> input_value
    neural.BatchNorm<float> norm

tensor<float> base = tensor.zeros<float>([2, 2])
base[0, 0] = 1.0
base[0, 1] = 2.0
base[1, 0] = 3.0
base[1, 1] = 5.0
tensor<float> target = tensor.ones<float>([2, 2])

BatchNormGradientModel model = BatchNormGradientModel(
    input_value = neural.Parameter<float>(value = base),
    norm = neural.BatchNorm<float>(features = 2)
)

float epsilon = 0.000001
tensor<float> plus = base
tensor<float> minus = base
plus[0, 0] = plus[0, 0].item() + epsilon
minus[0, 0] = minus[0, 0].item() - epsilon
neural.BatchNorm<float> plus_norm = model.norm
neural.BatchNorm<float> minus_norm = model.norm
float plus_loss = neural.mse(
    plus_norm.forward(neural.track(plus), mode = neural.training), target
).untrack().item()
float minus_loss = neural.mse(
    minus_norm.forward(neural.track(minus), mode = neural.training), target
).untrack().item()
float numerical_input = (plus_loss - minus_loss) / (2.0 * epsilon)

float input_before = model.input_value.raw()[0, 0].item()
float scale_before = model.norm.scale.raw()[0].item()
float bias_before = model.norm.bias.raw()[0].item()
neural<float> prediction = model.norm.forward(
    model.input_value.track(), mode = neural.training
)
tensor<float> prediction_before = prediction.untrack()

float expected_scale = 0.0
float expected_bias = 0.0
for batch in range(2)
    float y = prediction_before[batch, 0].item()
    float upstream = 0.5 * (y - 1.0)
    expected_scale += upstream * y
    expected_bias += upstream

neural<float> loss = neural.mse(prediction, target)
neural.Gradients gradients = neural.grad(loss)
float rate = 0.0001
neural.SGD optimizer = neural.SGD(rate = rate)
neural.step(&model, &optimizer, gradients)

float actual_input = (input_before - model.input_value.raw()[0, 0].item()) / rate
float actual_scale = (scale_before - model.norm.scale.raw()[0].item()) / rate
float actual_bias = (bias_before - model.norm.bias.raw()[0].item()) / rate
print(math.abs(actual_input - numerical_input) < 0.00002)
print(math.abs(actual_scale - expected_scale) < 0.000001)
print(math.abs(actual_bias - expected_bias) < 0.000001)
QUI
[[ "$("$QUIDRA" "$TMP/neural-batchnorm-gradient-check.qui")" == "$(printf 'true\ntrue\ntrue')" ]]

cat > "$TMP/neural-batchnorm-copy-state.qui" <<'QUI'
neural.BatchNorm first = neural.BatchNorm(features = 2)
neural.BatchNorm second = first
tensor<float32> values = tensor.ones<float32>([2, 2])
neural ignored = first.forward(neural.track(values), mode = neural.training)
print(first.running_mean.value[0].item() != second.running_mean.value[0].item())
print(second.running_mean.value[0].item() == 0.0)
print(second.running_variance.value[0].item() == 1.0)
QUI
[[ "$("$QUIDRA" "$TMP/neural-batchnorm-copy-state.qui")" == "$(printf 'true\ntrue\ntrue')" ]]

cat > "$TMP/neural-adam-copy-state.qui" <<'QUI'
tensor<float32> first_input = tensor.ones<float32>([1, 2])
tensor<float32> second_input = tensor.ones<float32>([1, 2]) * 2.0
tensor<float32> high_target = tensor.ones<float32>([1, 1]) * 100.0
tensor<float32> low_target = tensor.zeros<float32>([1, 1])
low_target[0, 0] = float32(-100.0)

neural.Linear model_a = neural.Linear(input = 2, output = 1, seed = 67)
neural.Linear model_b = neural.Linear(input = 2, output = 1, seed = 67)
neural.Linear reference = neural.Linear(input = 2, output = 1, seed = 67)

neural.Adam base_optimizer = neural.Adam(rate = 0.01)
neural.Adam optimizer_a = base_optimizer
neural.Adam optimizer_b = base_optimizer
neural.Adam reference_optimizer = neural.Adam(rate = 0.01)

neural prediction_a = model_a.forward(neural.track(first_input))
neural loss_a = neural.mse(prediction_a, high_target)
neural.Gradients gradients_a = neural.grad(loss_a)
neural.step(&model_a, &optimizer_a, gradients_a)

neural prediction_b = model_b.forward(neural.track(second_input))
neural loss_b = neural.mse(prediction_b, low_target)
neural.Gradients gradients_b = neural.grad(loss_b)
neural.step(&model_b, &optimizer_b, gradients_b)

neural reference_prediction = reference.forward(neural.track(second_input))
neural reference_loss = neural.mse(reference_prediction, low_target)
neural.Gradients reference_gradients = neural.grad(reference_loss)
neural.step(&reference, &reference_optimizer, reference_gradients)

print(model_b.weight.raw()[0, 0].item() == reference.weight.raw()[0, 0].item())
print(model_b.bias.raw()[0].item() == reference.bias.raw()[0].item())
QUI
[[ "$("$QUIDRA" "$TMP/neural-adam-copy-state.qui")" == "$(printf 'true\ntrue')" ]]

cat > "$TMP/neural-zero-parameter-step.qui" <<'QUI'
class EmptyModel
    int marker

EmptyModel model = EmptyModel(marker = 7)
neural prediction = neural.track(tensor.ones<float32>([1]))
neural loss = neural.mse(prediction, tensor.zeros<float32>([1]))
neural.Gradients gradients = neural.grad(loss)
neural.SGD optimizer = neural.SGD(rate = 0.1)
neural.step(&model, &optimizer, gradients)
print(model.marker)
QUI
[[ "$("$QUIDRA" "$TMP/neural-zero-parameter-step.qui")" == "7" ]]

cat > "$TMP/neural-dropout-boundaries.qui" <<'QUI'
neural.Dropout no_drop = neural.Dropout(rate = 0.0, seed = 5)
tensor<float32> values = tensor.ones<float32>([16])
tensor<float32> first = no_drop.forward(
    neural.track(values), mode = neural.training
).untrack()
tensor<float32> second = no_drop.forward(
    neural.track(values), mode = neural.training
).untrack()
bool equal = true
for index in range(16)
    if first[index].item() != 1.0 or second[index].item() != 1.0
        equal = false
print(equal)
QUI
[[ "$("$QUIDRA" "$TMP/neural-dropout-boundaries.qui")" == "true" ]]

for bad_rate in -0.1 1.0; do
    cat > "$TMP/neural-dropout-invalid-$bad_rate.qui" <<QUI
neural.Dropout invalid = neural.Dropout(rate = $bad_rate, seed = 1)
print("unreachable")
QUI
    set +e
    "$QUIDRA" "$TMP/neural-dropout-invalid-$bad_rate.qui" >"$TMP/neural-dropout-invalid-$bad_rate.out" 2>"$TMP/neural-dropout-invalid-$bad_rate.err"
    dropout_rate_rc=$?
    set -e
    [[ "$dropout_rate_rc" -eq 101 ]]
    grep -q 'Dropout rate must be in \[0,1)' "$TMP/neural-dropout-invalid-$bad_rate.err"
done

cat > "$TMP/neural-save-load-stress.qui" <<QUI
class SaveStressModel
    neural.Linear layer
    neural.BatchNorm norm
    neural.Dropout dropout
    neural.State<int> epoch

SaveStressModel model = SaveStressModel(
    layer = neural.Linear(input = 2, output = 2, seed = 79),
    norm = neural.BatchNorm(features = 2),
    dropout = neural.Dropout(rate = 0.25, seed = 83),
    epoch = neural.State<int>(value = 0)
)
neural.Adam optimizer = neural.Adam(rate = 0.001)
tensor<float32> values = tensor.ones<float32>([2, 2])
tensor<float32> target = tensor.zeros<float32>([2, 2])

for iteration in range(128)
    neural projected = model.layer.forward(neural.track(values))
    projected = model.norm.forward(projected, mode = neural.training)
    projected = model.dropout.forward(projected, mode = neural.training)
    neural loss = neural.mse(projected, target)
    neural.Gradients gradients = neural.grad(loss)
    neural.step(&model, &optimizer, gradients)
    model.epoch.value = iteration
    neural.save(model, optimizer, path = "$TMP/save-stress.quistate")
    neural.load(
        &model = &model,
        &optimizer = &optimizer,
        path = "$TMP/save-stress.quistate"
    )
print(model.epoch.value)
QUI
"$QUIDRA" "$TMP/neural-save-load-stress.qui" >"$TMP/neural-save-load-stress.out"
[[ "$(cat "$TMP/neural-save-load-stress.out")" == "127" ]]
