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

cat > "$TMP/bin-string.qui" <<'QUI'
bin allocated = bin.fill(5, 1)
print(len(allocated))
print(allocated)

bin parsed = bin.parse("01010000")
print(parsed[0])
print(parsed[1])

int8 signed = -1
bin packed = bin(signed)
print(packed)
int8 restored = int8(packed)
print(restored)

bool flag = bool(bin.parse("1"))
print(flag)

string repeated = string.repeat("a", 3)
print(repeated)
QUI
[[ "$("$QUIDRA" "$TMP/bin-string.qui")" == $'5\n11111\n0\n1\n11111111\n-1\ntrue\naaa' ]]

python3 - "$TMP/source.bin" <<'PY'
import sys
open(sys.argv[1], "wb").write(bytes([0, 255, 65, 10, 128]))
PY
cat > "$TMP/file-bin.qui" <<QUI
auto raw = file.read_bin("$TMP/source.bin")
match raw
    bin value
        print(len(value))
        uint8[] values = uint8[](value)
        print(values[0])
        print(values[1])
        values[2] = 66
        bin changed = bin(values)
        auto saved = file.write_bin("$TMP/copied.bin", changed)
        match saved
            void
                print("bin")
            error problem
                print(problem)
    error problem
        print(problem)
QUI
[[ "$("$QUIDRA" "$TMP/file-bin.qui")" == $'40\n0\n255\nbin' ]]
python3 - "$TMP/copied.bin" <<'PY'
import sys
data = open(sys.argv[1], "rb").read()
assert data == bytes([0, 255, 66, 10, 128]), data
PY

cat > "$TMP/file-bin-unaligned.qui" <<QUI
bin value = bin.fill(3, 1)
auto saved = file.write_bin("$TMP/unaligned.bin", value)
match saved
    void
        print("unexpected")
    error problem
        print(problem)
QUI
[[ "$("$QUIDRA" "$TMP/file-bin-unaligned.qui")" == "file operation failed" ]]
[[ ! -e "$TMP/unaligned.bin" ]]

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
test.equal(int(2) + 3, 5)
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
tags.add("compiler")
tags.add("ai")
tags.add("compiler")
print(tags.size())
print(tags.has("compiler"))
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
set_expected="$(printf '2\ntrue\nfalse\ncompiler\nai\nfalse\ntrue')"
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
        print(int(-1))
many.set("k73", 999)
auto replaced = many.get("k73")
match replaced
    int value
        print(value)
    none
        print(int(-1))
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
        print(int(-1))
large.set(4097, 123456)
auto replaced_large = large.get(4097)
match replaced_large
    int value
        print(value)
    none
        print(int(-1))
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
        print(int(-1))
auto collision_i = collisions.get("i")
match collision_i
    int value
        print(value)
    none
        print(int(-1))
auto collision_q = collisions.get("q")
match collision_q
    int value
        print(value)
    none
        print(int(-1))
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
tensor<float32> exact_cast = float32(exact_source)
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
"$QUIDRA" check "$TMP/tensor-rank-mismatch.qui" --json >"$TMP/tensor-rank-mismatch.json"
tensor_rank_rc=$?
set -e
[[ "$tensor_rank_rc" -eq 1 ]]
grep -q 'TYPE_MISMATCH' "$TMP/tensor-rank-mismatch.json"
grep -q 'identical rank' "$TMP/tensor-rank-mismatch.json"

cat > "$TMP/tensor-float-int-cast.qui" <<'QUI'
tensor<float> source = tensor.ones<float>([1]) * 1.5
tensor<int> converted = int(source)
print(converted[0].item())
QUI
set +e
"$QUIDRA" check "$TMP/tensor-float-int-cast.qui" --json >"$TMP/tensor-float-int-cast.json"
tensor_cast_rc=$?
set -e
[[ "$tensor_cast_rc" -eq 1 ]]
grep -qi 'floating-point to integer conversion requires' "$TMP/tensor-float-int-cast.json"

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
neural<float> plus64 = value64 + 1.0
neural<float> plus64_again = plus64 + 1.0
print(plus64_again.untrack()[0].item() == float(16777218))
QUI
[[ "$("$QUIDRA" "$TMP/neural-dtype-precision.qui")" == "$(printf 'true\ntrue')" ]]

cat > "$TMP/image.qui" <<QUI
tensor<uint8> pixels = tensor<uint8>([3, 2, 2])
pixels[0, 0, 0] = 10
pixels[0, 0, 1] = 20
pixels[0, 1, 0] = 30
pixels[0, 1, 1] = 40
pixels[1, 0, 0] = 50
pixels[1, 0, 1] = 60
pixels[1, 1, 0] = 70
pixels[1, 1, 1] = 80
pixels[2, 0, 0] = 90
pixels[2, 0, 1] = 100
pixels[2, 1, 0] = 110
pixels[2, 1, 1] = 120

auto png_written = image.write("$TMP/image.png", pixels)
match png_written
    void
        print("png-write")
    error problem
        print(problem)

tensor<uint8> | error png_read = image.read("$TMP/image.png")
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

auto bmp_written = image.write("$TMP/image.bmp", pixels)
match bmp_written
    void
        print("bmp-write")
    error problem
        print(problem)

tensor<uint8> | error bmp_read = image.read("$TMP/image.bmp")
match bmp_read
    tensor<uint8> decoded
        print(decoded[1, 1, 0].item())
    error problem
        print(problem)

auto tiff_written = image.write("$TMP/image.tiff", pixels)
match tiff_written
    void
        print("tiff-write")
    error problem
        print(problem)

tensor<uint8> | error tiff_read = image.read("$TMP/image.tiff")
match tiff_read
    tensor<uint8> decoded
        int[] shape = decoded.shape()
        print(shape[0])
        print(decoded[2, 1, 0].item())
    error problem
        print(problem)

auto jpeg_written = image.write("$TMP/image.jpg", pixels, quality = 100)
match jpeg_written
    void
        print("jpeg-write")
    error problem
        print(problem)

tensor<uint8> | error jpeg_read = image.read("$TMP/image.jpg")
match jpeg_read
    tensor<uint8> decoded
        int[] shape = decoded.shape()
        print(shape[0])
        print(shape[1])
        print(shape[2])
    error problem
        print(problem)

auto webp_written = image.write("$TMP/image.webp", pixels, quality = 100)
match webp_written
    void
        print("webp-write")
    error problem
        print(problem)

tensor<uint8> | error webp_read = image.read("$TMP/image.webp")
match webp_read
    tensor<uint8> decoded
        int[] shape = decoded.shape()
        print(shape[0])
        print(shape[1])
        print(shape[2])
    error problem
        print(problem)

tensor<uint8> rgba = tensor.zeros<uint8>([4, 1, 1])
auto rgba_jpeg = image.write("$TMP/rgba.jpg", rgba)
match rgba_jpeg
    void
        print("unexpected-jpeg-alpha")
    error problem
        print("jpeg-alpha-error")
QUI
image_output="$("$QUIDRA" "$TMP/image.qui")"
image_expected="$(printf 'png-write\n3\n2\n2\n40\n100\nbmp-write\n70\ntiff-write\n3\n110\njpeg-write\n3\n2\n2\nwebp-write\n3\n2\n2\njpeg-alpha-error')"
[[ "$image_output" == "$image_expected" ]]

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

cat > "$TMP/exact-numerics.qui" <<'QUI'
bigint a = 1234567890123456789012345678901234567890
bigint b = 987654321098765432109876543210
print(a + b)
print(a - b)
print(a * b)
print(a / b)
print(a % b)

bigreal root2 = math.sqrt(2.0)
bigreal root8 = math.sqrt(8.0)
print(root2 * root8 == bigreal(4))
print(root2 * root2 == bigreal(2))
bigint exact_two = bigint(root2 * root2)
print(exact_two)
bigreal root4 = math.sqrt(4.0)
int exact_root4 = int(root4)
print(exact_root4)
string root2_100 = "{root2:sig=100}"
print(len(root2_100))

bigreal pi_value = math.pi
bigreal e_value = math.e
print(pi_value > bigreal(3))
print(math.log(e_value) == bigreal(1))
print(math.sin(pi_value) == bigreal(0))

int fixed_four = 4
bigreal exact_four = 4.0
print(bigreal(fixed_four) == exact_four)

bigreal decimal_tenth = 0.1
float ieee = 0.1
bigreal preserved = bigreal(ieee)
print(decimal_tenth == preserved)
float roundtrip = float(preserved)
print(roundtrip == ieee)

auto parsed_bigint = bigint.parse("1234567890123456789012345678901234567890")
match parsed_bigint
    bigint value
        print(value == a)
    error problem
        print(problem)

auto parsed_bigreal = bigreal.parse("0.1")
match parsed_bigreal
    bigreal value
        print(value == decimal_tenth)
    error problem
        print(problem)

bigint[] integers = [1, 123456789012345678901234567890]
bigreal[] reals = bigreal(integers)
print(reals[0] == bigreal(1))
print(reals[1] == bigreal(integers[1]))
QUI
exact_numeric_output="$("$QUIDRA" run "$TMP/exact-numerics.qui")"
exact_numeric_expected=$(printf '%s\n' \
'1234567891111111110111111111011111111100' \
'1234567889135802467913580246791358024680' \
'1219326311370217952261850327337448559633622923332237463801111263526900' \
'1249999988' \
'601851852060185185207253086410' \
'true' 'true' '2' '2' '101' \
'true' 'true' 'true' 'true' 'false' 'true' \
'true' 'true' 'true' 'true')
[[ "$exact_numeric_output" == "$exact_numeric_expected" ]]

cat > "$TMP/exact-noninteger-cast.qui" <<'QUI'
bigreal value = 4.5
int converted = int(value)
print(converted)
QUI
set +e
"$QUIDRA" run "$TMP/exact-noninteger-cast.qui" >"$TMP/exact-noninteger-cast.out" 2>"$TMP/exact-noninteger-cast.err"
exact_noninteger_rc=$?
set -e
[[ "$exact_noninteger_rc" -eq 101 ]]
grep -q 'bigreal is not provably an integer' "$TMP/exact-noninteger-cast.err"

cat > "$TMP/exact-collections.qui" <<'QUI'
bigint key = 123456789012345678901234567890
map.Map<bigint, string> table = map.Map<bigint, string>()
table.set(key, "exact")
print(table.has(key))
set.Set<bigint> keys = set.Set<bigint>()
keys.add(key)
print(keys.has(key))
QUI
[[ "$("$QUIDRA" run "$TMP/exact-collections.qui")" == 
cat > "$TMP/json-exact-data.json" <<'JSON'
{"huge":12345678901234567890123456789012345678901234567890,"real":1.25e1000}
JSON
cat > "$TMP/json-exact-numerics.qui" <<QUI
auto loaded = file.read("$TMP/json-exact-data.json")
match loaded
    string source
        auto parsed = json.parse(source)
        match parsed
            json.Value root
                auto huge_value = root.get("huge")
                match huge_value
                    json.Value value
                        auto huge = value.bigint()
                        match huge
                            bigint integer
                                print(integer)
                            error problem
                                print(problem)
                    none
                        print("missing-huge")
                    error problem
                        print(problem)

                auto real_value = root.get("real")
                match real_value
                    json.Value value
                        auto exact = value.bigreal()
                        match exact
                            bigreal number
                                bigreal expected = 1.25e1000
                                print(number == expected)
                            error problem
                                print(problem)

                        auto narrow = value.number()
                        match narrow
                            float number
                                print(number)
                            error problem
                                print("narrow-error")
                    none
                        print("missing-real")
                    error problem
                        print(problem)

                print(root.encode())
            error problem
                print(problem)
    error problem
        print(problem)
QUI
json_exact_output="$("$QUIDRA" run "$TMP/json-exact-numerics.qui")"
json_exact_expected=$(printf '%s\n' \
'12345678901234567890123456789012345678901234567890' \
'true' \
'narrow-error' \
'{"huge":12345678901234567890123456789012345678901234567890,"real":1.25e1000}')
[[ "$json_exact_output" == "$json_exact_expected" ]]

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
http_expected="$(printf '200\n24\n0\n1\npresent\nnone\n404\n56\ntransport-error')"
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
print(math.floor(float(-1.25)))
print(math.ceil(float(-1.25)))
tensor<float> source = tensor.ones<float>([1]) * 1.25
tensor<float32> converted = float32(source)
print(converted[0].item())
QUI
practical_cast_output="$("$QUIDRA" "$TMP/practical-casts.qui")"
[[ "$practical_cast_output" == "$(printf '1.6777216e+07\n1.75\n1\n2\n-2\n-1\n1.25')" ]]

cat > "$TMP/captured-shapes.qui" <<'QUI'
int n = 3
int m = 2
tensor<float><n, 4> first = tensor.ones<float>([3, 4])
n = 5
first = tensor.ones<float>([3, 4])
tensor<float><n, 4> second = tensor.zeros()
tensor<float><n * m, 2> product = tensor.ones<float>([10, 2])
tensor<float><_, 4> explicit_shape = tensor.zeros([5, 4])
print(first.shape()[0])
print(second.shape()[0])
print(product.shape()[0])
print(explicit_shape.shape()[0])
QUI
captured_shapes_output="$("$QUIDRA" "$TMP/captured-shapes.qui")"
[[ "$captured_shapes_output" == "$(printf '3\n5\n10\n5')" ]]

cat > "$TMP/flow-shape-runtime.qui" <<'QUI'
tensor<float32> choose_shape(bool wider)
    tensor<float32> value = tensor.zeros<float32>([3, 4])
    if wider
        value = tensor.zeros<float32>([3, 5])
    return value

tensor<float32><3, 4> checked = choose_shape(false)
print(checked.shape()[1])
QUI
flow_shape_output="$("$QUIDRA" "$TMP/flow-shape-runtime.qui")"
[[ "$flow_shape_output" == "4" ]]

cat > "$TMP/flow-shape-runtime-fail.qui" <<'QUI'
tensor<float32> choose_shape(bool wider)
    tensor<float32> value = tensor.zeros<float32>([3, 4])
    if wider
        value = tensor.zeros<float32>([3, 5])
    return value

tensor<float32><3, 4> checked = choose_shape(true)
print(checked.shape()[1])
QUI
set +e
"$QUIDRA" "$TMP/flow-shape-runtime-fail.qui" >"$TMP/flow-shape-runtime-fail.out" 2>&1
flow_shape_rc=$?
set -e
[[ "$flow_shape_rc" -eq 101 ]]
grep -q 'captured shape constraint' "$TMP/flow-shape-runtime-fail.out"

cat > "$TMP/dependent-signature-shape.qui" <<'QUI'
tensor<float><n, 2> keep_shape(int n, tensor<float><n, 2> value)
    return value
tensor<float> source = tensor.ones<float>([3, 2])
tensor<float><3, 2> checked = keep_shape(3, source)
print(checked.shape()[0])
QUI
dependent_signature_output="$("$QUIDRA" "$TMP/dependent-signature-shape.qui")"
[[ "$dependent_signature_output" == "3" ]]

cat > "$TMP/dependent-signature-shape-fail.qui" <<'QUI'
tensor<float><n, 2> keep_shape(int n, tensor<float><n, 2> value)
    return value
tensor<float> source = tensor.ones<float>([4, 2])
auto checked = keep_shape(3, source)
print(checked.shape()[0])
QUI
set +e
"$QUIDRA" "$TMP/dependent-signature-shape-fail.qui" >"$TMP/dependent-signature-shape-fail.out" 2>"$TMP/dependent-signature-shape-fail.err"
dependent_signature_rc=$?
set -e
[[ "$dependent_signature_rc" -eq 101 ]]
grep -q 'captured shape constraint' "$TMP/dependent-signature-shape-fail.err"

cat > "$TMP/captured-shape-reassign-fail.qui" <<'QUI'
int n = 3
tensor<float><n, 4> value = tensor.ones<float>([3, 4])
n = 5
value = tensor.ones<float>([5, 4])
QUI
set +e
"$QUIDRA" "$TMP/captured-shape-reassign-fail.qui" >"$TMP/captured-shape-reassign-fail.out" 2>"$TMP/captured-shape-reassign-fail.err"
captured_shape_reassign_rc=$?
set -e
[[ "$captured_shape_reassign_rc" -eq 101 ]]
grep -q 'captured shape constraint' "$TMP/captured-shape-reassign-fail.err"

cat > "$TMP/captured-arrays.qui" <<'QUI'
int n = 2
int m = 2
int[n * m] values
values[3] = 7
print(len(values))
print(values[3])
int[][n] rows = [[1, 2], [3, 4]]
n = 3
rows = [[5, 6], [7, 8]]
print(rows[1][1])
QUI
captured_arrays_output="$("$QUIDRA" "$TMP/captured-arrays.qui")"
[[ "$captured_arrays_output" == "$(printf '4\n7\n8')" ]]

cat > "$TMP/captured-array-reassign-fail.qui" <<'QUI'
int n = 2
int[n] values = [1, 2]
n = 3
values = [1, 2, 3]
QUI
set +e
"$QUIDRA" "$TMP/captured-array-reassign-fail.qui" >"$TMP/captured-array-reassign-fail.out" 2>&1
captured_array_reassign_rc=$?
set -e
[[ "$captured_array_reassign_rc" -eq 101 ]]
grep -q 'Quidra runtime error' "$TMP/captured-array-reassign-fail.out"

cat > "$TMP/captured-nested-array-fail.qui" <<'QUI'
int n = 2
int[][n] rows = [[1, 2], [3, 4]]
n = 3
rows = [[1, 2, 3], [4, 5, 6]]
QUI
set +e
"$QUIDRA" "$TMP/captured-nested-array-fail.qui" >"$TMP/captured-nested-array-fail.out" 2>&1
captured_nested_array_rc=$?
set -e
[[ "$captured_nested_array_rc" -eq 101 ]]
grep -q 'Quidra runtime error' "$TMP/captured-nested-array-fail.out"

cat > "$TMP/contextual-wildcard-zero.qui" <<'QUI'
tensor<float><_, 4> value = tensor.zeros()
QUI
set +e
"$QUIDRA" check "$TMP/contextual-wildcard-zero.qui" --json >"$TMP/contextual-wildcard-zero.json"
contextual_wildcard_rc=$?
set -e
[[ "$contextual_wildcard_rc" -eq 1 ]]
grep -q 'Contextual tensor allocation cannot infer' "$TMP/contextual-wildcard-zero.json"

cat > "$TMP/container-casts.qui" <<'QUI'
int[][] dynamic = [[1, 2], [3, 4]]
float[][] dynamic_float = float(dynamic)
print(dynamic_float[1][0])

int[2][2] fixed = [[5, 6], [7, 8]]
float[2][2] fixed_float = float(fixed)
print(fixed_float[0][1])

tensor<int><2, 2> matrix = tensor.ones<int>([2, 2])
tensor<float><2, 2> matrix_float = float(matrix)
print(matrix_float[1, 1].item())
QUI
container_cast_output="$("$QUIDRA" "$TMP/container-casts.qui")"
[[ "$container_cast_output" == "$(printf '3.0\n6.0\n1.0')" ]]

cat > "$TMP/container-cast-range.qui" <<'QUI'
int[] values = [1, 300]
int8[] converted = int8(values)
print(converted[0])
QUI
set +e
"$QUIDRA" "$TMP/container-cast-range.qui" >"$TMP/container-cast-range.out" 2>"$TMP/container-cast-range.err"
container_range_rc=$?
set -e
[[ "$container_range_rc" -eq 101 ]]
grep -q 'NUMERIC_CAST_RANGE' "$TMP/container-cast-range.err"

cat > "$TMP/container-cast-uninitialized.qui" <<'QUI'
int[2] values
values[0] = 7
float[2] converted = float(values)
print(converted[0])
QUI
set +e
"$QUIDRA" "$TMP/container-cast-uninitialized.qui" >"$TMP/container-cast-uninitialized.out" 2>"$TMP/container-cast-uninitialized.err"
container_uninitialized_rc=$?
set -e
[[ "$container_uninitialized_rc" -eq 101 ]]
grep -q 'UNINITIALIZED' "$TMP/container-cast-uninitialized.err"

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


cat > "$TMP/bin-cast-length-fail.qui" <<'QUI'
bin value = bin.parse("101")
int8 decoded = int8(value)
print(decoded)
QUI
set +e
"$QUIDRA" "$TMP/bin-cast-length-fail.qui" >"$TMP/bin-cast-length-fail.out" 2>"$TMP/bin-cast-length-fail.err"
bin_cast_length_rc=$?
set -e
[[ "$bin_cast_length_rc" -eq 101 ]]
grep -q 'bin length does not match destination type width' "$TMP/bin-cast-length-fail.err"

cat > "$TMP/bin-bool-length-fail.qui" <<'QUI'
bin value = bin.parse("10")
bool decoded = bool(value)
print(decoded)
QUI
set +e
"$QUIDRA" "$TMP/bin-bool-length-fail.qui" >"$TMP/bin-bool-length-fail.out" 2>"$TMP/bin-bool-length-fail.err"
bin_bool_length_rc=$?
set -e
[[ "$bin_bool_length_rc" -eq 101 ]]
grep -q 'bin length does not match destination type width' "$TMP/bin-bool-length-fail.err"

cat > "$TMP/bin-array-length-fail.qui" <<'QUI'
bin value = bin.parse("101")
uint8[] decoded = uint8[](value)
print(len(decoded))
QUI
set +e
"$QUIDRA" "$TMP/bin-array-length-fail.qui" >"$TMP/bin-array-length-fail.out" 2>"$TMP/bin-array-length-fail.err"
bin_array_length_rc=$?
set -e
[[ "$bin_array_length_rc" -eq 101 ]]
grep -q 'bin length is not divisible by destination element width' "$TMP/bin-array-length-fail.err"
true\ntrue' ]]

cat > "$TMP/bigreal-map-key.qui" <<'QUI'
map.Map<bigreal, string> invalid = map.Map<bigreal, string>()
QUI
set +e
"$QUIDRA" check "$TMP/bigreal-map-key.qui" --json >"$TMP/bigreal-map-key.json"
bigreal_map_rc=$?
set -e
[[ "$bigreal_map_rc" -eq 1 ]]
grep -q 'STANDARD_KEY_TYPE' "$TMP/bigreal-map-key.json"

cat > "$TMP/bigreal-set-key.qui" <<'QUI'
set.Set<bigreal> invalid = set.Set<bigreal>()
QUI
set +e
"$QUIDRA" check "$TMP/bigreal-set-key.qui" --json >"$TMP/bigreal-set-key.json"
bigreal_set_rc=$?
set -e
[[ "$bigreal_set_rc" -eq 1 ]]
grep -q 'STANDARD_KEY_TYPE' "$TMP/bigreal-set-key.json"

cat > "$TMP/json-exact-data.json" <<'JSON'
{"huge":12345678901234567890123456789012345678901234567890,"real":1.25e1000}
JSON
cat > "$TMP/json-exact-numerics.qui" <<QUI
auto loaded = file.read("$TMP/json-exact-data.json")
match loaded
    string source
        auto parsed = json.parse(source)
        match parsed
            json.Value root
                auto huge_value = root.get("huge")
                match huge_value
                    json.Value value
                        auto huge = value.bigint()
                        match huge
                            bigint integer
                                print(integer)
                            error problem
                                print(problem)
                    none
                        print("missing-huge")
                    error problem
                        print(problem)

                auto real_value = root.get("real")
                match real_value
                    json.Value value
                        auto exact = value.bigreal()
                        match exact
                            bigreal number
                                bigreal expected = 1.25e1000
                                print(number == expected)
                            error problem
                                print(problem)

                        auto narrow = value.number()
                        match narrow
                            float number
                                print(number)
                            error problem
                                print("narrow-error")
                    none
                        print("missing-real")
                    error problem
                        print(problem)

                print(root.encode())
            error problem
                print(problem)
    error problem
        print(problem)
QUI
json_exact_output="$("$QUIDRA" run "$TMP/json-exact-numerics.qui")"
json_exact_expected=$(printf '%s\n' \
'12345678901234567890123456789012345678901234567890' \
'true' \
'narrow-error' \
'{"huge":12345678901234567890123456789012345678901234567890,"real":1.25e1000}')
[[ "$json_exact_output" == "$json_exact_expected" ]]

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
http_expected="$(printf '200\n24\n0\n1\npresent\nnone\n404\n56\ntransport-error')"
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
print(math.floor(float(-1.25)))
print(math.ceil(float(-1.25)))
tensor<float> source = tensor.ones<float>([1]) * 1.25
tensor<float32> converted = float32(source)
print(converted[0].item())
QUI
practical_cast_output="$("$QUIDRA" "$TMP/practical-casts.qui")"
[[ "$practical_cast_output" == "$(printf '1.6777216e+07\n1.75\n1\n2\n-2\n-1\n1.25')" ]]

cat > "$TMP/captured-shapes.qui" <<'QUI'
int n = 3
int m = 2
tensor<float><n, 4> first = tensor.ones<float>([3, 4])
n = 5
first = tensor.ones<float>([3, 4])
tensor<float><n, 4> second = tensor.zeros()
tensor<float><n * m, 2> product = tensor.ones<float>([10, 2])
tensor<float><_, 4> explicit_shape = tensor.zeros([5, 4])
print(first.shape()[0])
print(second.shape()[0])
print(product.shape()[0])
print(explicit_shape.shape()[0])
QUI
captured_shapes_output="$("$QUIDRA" "$TMP/captured-shapes.qui")"
[[ "$captured_shapes_output" == "$(printf '3\n5\n10\n5')" ]]

cat > "$TMP/flow-shape-runtime.qui" <<'QUI'
tensor<float32> choose_shape(bool wider)
    tensor<float32> value = tensor.zeros<float32>([3, 4])
    if wider
        value = tensor.zeros<float32>([3, 5])
    return value

tensor<float32><3, 4> checked = choose_shape(false)
print(checked.shape()[1])
QUI
flow_shape_output="$("$QUIDRA" "$TMP/flow-shape-runtime.qui")"
[[ "$flow_shape_output" == "4" ]]

cat > "$TMP/flow-shape-runtime-fail.qui" <<'QUI'
tensor<float32> choose_shape(bool wider)
    tensor<float32> value = tensor.zeros<float32>([3, 4])
    if wider
        value = tensor.zeros<float32>([3, 5])
    return value

tensor<float32><3, 4> checked = choose_shape(true)
print(checked.shape()[1])
QUI
set +e
"$QUIDRA" "$TMP/flow-shape-runtime-fail.qui" >"$TMP/flow-shape-runtime-fail.out" 2>&1
flow_shape_rc=$?
set -e
[[ "$flow_shape_rc" -eq 101 ]]
grep -q 'captured shape constraint' "$TMP/flow-shape-runtime-fail.out"

cat > "$TMP/dependent-signature-shape.qui" <<'QUI'
tensor<float><n, 2> keep_shape(int n, tensor<float><n, 2> value)
    return value
tensor<float> source = tensor.ones<float>([3, 2])
tensor<float><3, 2> checked = keep_shape(3, source)
print(checked.shape()[0])
QUI
dependent_signature_output="$("$QUIDRA" "$TMP/dependent-signature-shape.qui")"
[[ "$dependent_signature_output" == "3" ]]

cat > "$TMP/dependent-signature-shape-fail.qui" <<'QUI'
tensor<float><n, 2> keep_shape(int n, tensor<float><n, 2> value)
    return value
tensor<float> source = tensor.ones<float>([4, 2])
auto checked = keep_shape(3, source)
print(checked.shape()[0])
QUI
set +e
"$QUIDRA" "$TMP/dependent-signature-shape-fail.qui" >"$TMP/dependent-signature-shape-fail.out" 2>"$TMP/dependent-signature-shape-fail.err"
dependent_signature_rc=$?
set -e
[[ "$dependent_signature_rc" -eq 101 ]]
grep -q 'captured shape constraint' "$TMP/dependent-signature-shape-fail.err"

cat > "$TMP/captured-shape-reassign-fail.qui" <<'QUI'
int n = 3
tensor<float><n, 4> value = tensor.ones<float>([3, 4])
n = 5
value = tensor.ones<float>([5, 4])
QUI
set +e
"$QUIDRA" "$TMP/captured-shape-reassign-fail.qui" >"$TMP/captured-shape-reassign-fail.out" 2>"$TMP/captured-shape-reassign-fail.err"
captured_shape_reassign_rc=$?
set -e
[[ "$captured_shape_reassign_rc" -eq 101 ]]
grep -q 'captured shape constraint' "$TMP/captured-shape-reassign-fail.err"

cat > "$TMP/captured-arrays.qui" <<'QUI'
int n = 2
int m = 2
int[n * m] values
values[3] = 7
print(len(values))
print(values[3])
int[][n] rows = [[1, 2], [3, 4]]
n = 3
rows = [[5, 6], [7, 8]]
print(rows[1][1])
QUI
captured_arrays_output="$("$QUIDRA" "$TMP/captured-arrays.qui")"
[[ "$captured_arrays_output" == "$(printf '4\n7\n8')" ]]

cat > "$TMP/captured-array-reassign-fail.qui" <<'QUI'
int n = 2
int[n] values = [1, 2]
n = 3
values = [1, 2, 3]
QUI
set +e
"$QUIDRA" "$TMP/captured-array-reassign-fail.qui" >"$TMP/captured-array-reassign-fail.out" 2>&1
captured_array_reassign_rc=$?
set -e
[[ "$captured_array_reassign_rc" -eq 101 ]]
grep -q 'Quidra runtime error' "$TMP/captured-array-reassign-fail.out"

cat > "$TMP/captured-nested-array-fail.qui" <<'QUI'
int n = 2
int[][n] rows = [[1, 2], [3, 4]]
n = 3
rows = [[1, 2, 3], [4, 5, 6]]
QUI
set +e
"$QUIDRA" "$TMP/captured-nested-array-fail.qui" >"$TMP/captured-nested-array-fail.out" 2>&1
captured_nested_array_rc=$?
set -e
[[ "$captured_nested_array_rc" -eq 101 ]]
grep -q 'Quidra runtime error' "$TMP/captured-nested-array-fail.out"

cat > "$TMP/contextual-wildcard-zero.qui" <<'QUI'
tensor<float><_, 4> value = tensor.zeros()
QUI
set +e
"$QUIDRA" check "$TMP/contextual-wildcard-zero.qui" --json >"$TMP/contextual-wildcard-zero.json"
contextual_wildcard_rc=$?
set -e
[[ "$contextual_wildcard_rc" -eq 1 ]]
grep -q 'Contextual tensor allocation cannot infer' "$TMP/contextual-wildcard-zero.json"

cat > "$TMP/container-casts.qui" <<'QUI'
int[][] dynamic = [[1, 2], [3, 4]]
float[][] dynamic_float = float(dynamic)
print(dynamic_float[1][0])

int[2][2] fixed = [[5, 6], [7, 8]]
float[2][2] fixed_float = float(fixed)
print(fixed_float[0][1])

tensor<int><2, 2> matrix = tensor.ones<int>([2, 2])
tensor<float><2, 2> matrix_float = float(matrix)
print(matrix_float[1, 1].item())
QUI
container_cast_output="$("$QUIDRA" "$TMP/container-casts.qui")"
[[ "$container_cast_output" == "$(printf '3.0\n6.0\n1.0')" ]]

cat > "$TMP/container-cast-range.qui" <<'QUI'
int[] values = [1, 300]
int8[] converted = int8(values)
print(converted[0])
QUI
set +e
"$QUIDRA" "$TMP/container-cast-range.qui" >"$TMP/container-cast-range.out" 2>"$TMP/container-cast-range.err"
container_range_rc=$?
set -e
[[ "$container_range_rc" -eq 101 ]]
grep -q 'NUMERIC_CAST_RANGE' "$TMP/container-cast-range.err"

cat > "$TMP/container-cast-uninitialized.qui" <<'QUI'
int[2] values
values[0] = 7
float[2] converted = float(values)
print(converted[0])
QUI
set +e
"$QUIDRA" "$TMP/container-cast-uninitialized.qui" >"$TMP/container-cast-uninitialized.out" 2>"$TMP/container-cast-uninitialized.err"
container_uninitialized_rc=$?
set -e
[[ "$container_uninitialized_rc" -eq 101 ]]
grep -q 'UNINITIALIZED' "$TMP/container-cast-uninitialized.err"

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


cat > "$TMP/bin-cast-length-fail.qui" <<'QUI'
bin value = bin.parse("101")
int8 decoded = int8(value)
print(decoded)
QUI
set +e
"$QUIDRA" "$TMP/bin-cast-length-fail.qui" >"$TMP/bin-cast-length-fail.out" 2>"$TMP/bin-cast-length-fail.err"
bin_cast_length_rc=$?
set -e
[[ "$bin_cast_length_rc" -eq 101 ]]
grep -q 'bin length does not match destination type width' "$TMP/bin-cast-length-fail.err"

cat > "$TMP/bin-bool-length-fail.qui" <<'QUI'
bin value = bin.parse("10")
bool decoded = bool(value)
print(decoded)
QUI
set +e
"$QUIDRA" "$TMP/bin-bool-length-fail.qui" >"$TMP/bin-bool-length-fail.out" 2>"$TMP/bin-bool-length-fail.err"
bin_bool_length_rc=$?
set -e
[[ "$bin_bool_length_rc" -eq 101 ]]
grep -q 'bin length does not match destination type width' "$TMP/bin-bool-length-fail.err"

cat > "$TMP/bin-array-length-fail.qui" <<'QUI'
bin value = bin.parse("101")
uint8[] decoded = uint8[](value)
print(len(decoded))
QUI
set +e
"$QUIDRA" "$TMP/bin-array-length-fail.qui" >"$TMP/bin-array-length-fail.out" 2>"$TMP/bin-array-length-fail.err"
bin_array_length_rc=$?
set -e
[[ "$bin_array_length_rc" -eq 101 ]]
grep -q 'bin length is not divisible by destination element width' "$TMP/bin-array-length-fail.err"
