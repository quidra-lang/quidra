#!/usr/bin/env bash
# Extension lookup follows each declaring module's imports and public exports.
set -euo pipefail
QUIDRA="${1:?usage: extension_visibility_tests.sh /path/to/quidra}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/cache"
failures=0

accepts() {
    local name="$1"
    cat > "$TMP/$name.qui"
    if ! "$QUIDRA" check "$TMP/$name.qui" --json > "$TMP/$name.json" 2>&1; then
        echo "FAIL $name: rejected" >&2
        cat "$TMP/$name.json" >&2
        failures=$((failures + 1))
    fi
}

runs() {
    local name="$1" expected="$2" output
    cat > "$TMP/$name.qui"
    if ! output="$("$QUIDRA" "$TMP/$name.qui" 2> "$TMP/$name.err")"; then
        echo "FAIL $name: execution failed" >&2
        cat "$TMP/$name.err" >&2
        failures=$((failures + 1))
    elif [[ "$output" != "$expected" ]]; then
        echo "FAIL $name: expected '$expected', got '$output'" >&2
        failures=$((failures + 1))
    fi
}

fails() {
    local name="$1" code="$2" status=0
    cat > "$TMP/$name.qui"
    "$QUIDRA" "$TMP/$name.qui" > "$TMP/$name.out" 2> "$TMP/$name.err" || status=$?
    if [[ "$status" -ne 101 || -s "$TMP/$name.out" ]] ||
       ! grep -Fq "runtime error[$code]" "$TMP/$name.err"; then
        echo "FAIL $name: expected runtime error[$code], status $status" >&2
        cat "$TMP/$name.out" "$TMP/$name.err" >&2
        failures=$((failures + 1))
    fi
}

rejects() {
    local name="$1" code="$2"
    cat > "$TMP/$name.qui"
    if "$QUIDRA" check "$TMP/$name.qui" --json > "$TMP/$name.json" 2>&1; then
        echo "FAIL $name: accepted" >&2
        failures=$((failures + 1))
    elif ! python3 - "$TMP/$name.json" "$code" <<'PY'
import json, sys
report = json.load(open(sys.argv[1]))
if not any(d['code'] == sys.argv[2] for d in report['diagnostics']):
    print(json.dumps(report['diagnostics'], indent=2))
    sys.exit(1)
PY
    then
        echo "FAIL $name: missing $code" >&2
        failures=$((failures + 1))
    fi
}

runs contextual-literal-receivers $'1\n1\n2\n2' <<'QUI'
T first<T>(extend T[] value)
    return value[0]
int size(extend int[] value)
    return int(len(value))
print(first<int>([1, 2]))
print(NL)
print([1, 2].first<int>())
print(NL)
print(size([1, 2]))
print(NL)
print([1, 2].size())
print(NL)
QUI

runs array-expression-receivers $'2\n2\n3\n3\n2\n2\n3\n3\n3\n3' <<'QUI'
int size(extend int[] value)
    return int(len(value))
int count(extend bool[] value)
    return int(len(value))
int[] values = [1, 2, 3]
print(size(values[:1]))
print(NL)
print(values[:1].size())
print(NL)
print(size(values[values > 0] ^ 2))
print(NL)
print((values[values > 0] ^ 2).size())
print(NL)
print(size(values[[0, 2]]))
print(NL)
print(values[[0, 2]].size())
print(NL)
print(count(values > 0))
print(NL)
print((values > 0).count())
print(NL)
print(size(values + int(1)))
print(NL)
print((values + int(1)).size())
print(NL)
QUI


runs contextual-expression-receivers $'6\n6\n2\n2\n2\n2\n2\n2' <<'QUI'
int twice(extend int value)
    return value * 2
int count(extend bool[] value)
    return int(len(value))
int[] values = [1, 2]
int[2] fixed = [0, 0]
print("{twice(1 + 2)}{NL}")
print("{(1 + 2).twice()}{NL}")
print("{count(values > [0, 0])}{NL}")
print("{(values > [0, 0]).count()}{NL}")
print("{count(values > fixed)}{NL}")
print("{(values > fixed).count()}{NL}")
print("{count([1, 2] < [2, 3])}{NL}")
print("{([1, 2] < [2, 3]).count()}{NL}")
QUI

runs selection-generic-shapes '3 2 2' <<'QUI'
T identity<T>(extend T value)
    return value
int[3] values = [1, 2, 3]
int[3] whole = values.identity()
int[] picked = values[[0, 1]].identity()
int[] sliced = values[:1].identity()
print("{len(whole)} {len(picked)} {len(sliced)}{NL}")
QUI

runs fixed-nested-generic-arrays true <<'QUI'
T[] copied<T>(extend T[] value)
    T[] result = []
    for T element = value[]
        result = result.append(element)
    return result
int[2][3] a = [[1, 2, 3], [4, 5, 6]]
int[2][4] b = [[1, 2, 3, 4], [5, 6, 7, 8]]
auto x = a.copied()
auto y = b.copied()
int[][3] expected_a = a
int[][4] expected_b = b
print(x == expected_a and y == expected_b)
QUI

runs fallible-concrete-and-explicit-receivers '6 6 3 3' <<'QUI'
int | error fall(int value)
    return value
int twice(extend int value)
    return value * 2
T identity<T>(extend T value)
    return value
print("{twice(fall(3))} {fall(3).twice()} {identity<int>(fall(3))} {fall(3).identity<int>()}{NL}")
QUI
for form in 'twice(fall())' 'fall().twice()' 'identity<int>(fall())' 'fall().identity<int>()'; do
    name="fallible-error-${form//[^a-z]/}"
    fails "$name" RECEIVER_ERROR <<QUI
int | error fall()
    return error("receiver failed", code = "RECEIVER_ERROR")
int twice(extend int value)
    return value * 2
T identity<T>(extend T value)
    return value
print($form)
QUI
done
runs inferred-family-preserves-union $'true\ntrue\ntrue\ntrue' <<'QUI'
T identity<T>(extend T value)
    return value
int identity(extend int value)
    return value + 100
int | error fall(bool fail)
    if fail
        return error("kept")
    return 3
match identity(fall(false))
    int value
        print("{value == 3}{NL}")
    error problem
        print("false{NL}")
match fall(false).identity()
    int value
        print("{value == 3}{NL}")
    error problem
        print("false{NL}")
match identity(fall(true))
    int value
        print("false{NL}")
    error problem
        print("{problem == error("kept")}{NL}")
match fall(true).identity()
    int value
        print("false{NL}")
    error problem
        print("{problem == error("kept")}{NL}")
QUI
cat > "$TMP/generic-string.qui" <<'QUI'
string string<T>(extend T value)
    return "generic"
string string(extend int value)
    return "integer"
QUI
runs inferred-union-avoids-unused-native-collision 'generic generic generic generic' <<'QUI'
import provider = "./generic-string.qui"
int | error value = 3
int | error fail()
    return error("kept")
print("{provider.string(value)} {value.string()} {provider.string(fail())} {fail().string()}{NL}")
QUI
rejects explicit-fallible-native-collision EXTENSION_COLLISION <<'QUI'
import provider = "./generic-string.qui"
int | error value = 3
print(value.string<int>())
QUI
rejects inferred-fallible-free-stays-union GENERIC_CONSTRAINT <<'QUI'
int | error fall()
    return 3
T twice<T: integer>(extend T value)
    return value * 2
print(twice(fall()))
QUI
rejects inferred-fallible-receiver-stays-union UNKNOWN_MEMBER <<'QUI'
int | error fall()
    return 3
T twice<T: integer>(extend T value)
    return value * 2
print(fall().twice())
QUI
rejects mutable-fallible-receiver-stays-union UNKNOWN_MEMBER <<'QUI'
void .increment(extend int &value)
    value += 1
int | error value = 3
.value.increment()
QUI

cat > "$TMP/provider.qui" <<'QUI'
int doubled(extend const int value)
    return value * 2
QUI
cat > "$TMP/consumer.qui" <<'QUI'
import provider = "./provider.qui"
int compute(int value)
    return value.doubled()
QUI
accepts nested-import <<'QUI'
import consumer = "./consumer.qui"
print(consumer.compute(3))
QUI
cat > "$TMP/generic-consumer.qui" <<'QUI'
import provider = "./provider.qui"
T compute<T: integer>(T value)
    return T(int(value).doubled())
QUI
accepts generic-body-keeps-import-scope <<'QUI'
import consumer = "./generic-consumer.qui"
print(consumer.compute<int>(3))
QUI

cat > "$TMP/reexport.qui" <<'QUI'
public import provider = "./provider.qui"
QUI
cat > "$TMP/reexport-again.qui" <<'QUI'
public import public_api = "./reexport.qui"
QUI
accepts public-import <<'QUI'
import api = "./reexport-again.qui"
print(3.doubled())
QUI
cat > "$TMP/cycle-a.qui" <<'QUI'
public import next = "./cycle-b.qui"
QUI
cat > "$TMP/cycle-b.qui" <<'QUI'
public import next = "./cycle-a.qui"
QUI
rejects public-import-cycle IMPORT_CYCLE <<'QUI'
import api = "./cycle-a.qui"
QUI
rejects private-import UNKNOWN_MEMBER <<'QUI'
import consumer = "./consumer.qui"
print(int(3).doubled())
QUI

cat > "$TMP/no-import.qui" <<'QUI'
int compute(int value)
    return value.doubled()
QUI
rejects sibling-import-does-not-leak UNKNOWN_MEMBER <<'QUI'
import provider = "./provider.qui"
import unrelated = "./no-import.qui"
print(unrelated.compute(3))
QUI
rejects root-extension-does-not-leak UNKNOWN_MEMBER <<'QUI'
import unrelated = "./no-import.qui"
int doubled(extend int value)
    return value * 2
print(unrelated.compute(3))
QUI

cat > "$TMP/private-provider.qui" <<'QUI'
private int doubled(extend int value)
    return value * 2
int compute(int value)
    return value.doubled()
QUI
accepts private-local-extension <<'QUI'
import provider = "./private-provider.qui"
print(provider.compute(3))
QUI
rejects private-extension-not-exported UNKNOWN_MEMBER <<'QUI'
import provider = "./private-provider.qui"
print(int(3).doubled())
QUI

cat > "$TMP/other.qui" <<'QUI'
int doubled(extend int value)
    return value + value
QUI
rejects reexport-ambiguity EXTENSION_AMBIGUOUS <<'QUI'
import api = "./reexport.qui"
import other = "./other.qui"
print(3.doubled())
QUI
accepts same-provider-two-aliases <<'QUI'
import first = "./provider.qui"
import second = "./provider.qui"
print(3.doubled())
QUI
accepts generic-class-extension <<'QUI'
class Box<T> box(T value)
T unwrap<T>(extend const Box<T> value)
    return value.value
Box<int> value = Box<int>(1)
print(value.unwrap())
QUI
cat > "$TMP/class-string.qui" <<'QUI'
class Box box(int value)
string string(extend Box value)
    return value.value.string()
QUI
accepts class-string-extension <<'QUI'
import provider = "./class-string.qui"
provider.Box value = provider.Box(1)
print(value.string())
QUI
cat > "$TMP/array-string.qui" <<'QUI'
string string(extend int[] value)
    return len(value).string()
QUI
accepts array-string-extension <<'QUI'
import provider = "./array-string.qui"
int[] value = [1, 2]
print(value.string())
QUI
rejects mixed-extension-family DUPLICATE_NAME <<'QUI'
T identity<T>(extend T value)
    return value
int identity(int value)
    return value
print(int(1).identity())
QUI
cat > "$TMP/integer-provider.qui" <<'QUI'
int kind<T: integer>(extend T value)
    return 1
QUI
cat > "$TMP/floating-provider.qui" <<'QUI'
int kind<T: floating>(extend T value)
    return 2
QUI
accepts disjoint-constraints <<'QUI'
import integers = "./integer-provider.qui"
import floating = "./floating-provider.qui"
print(int(1).kind())
print(real32(1).kind())
QUI
cat > "$TMP/tensor32-provider.qui" <<'QUI'
int kind(extend tensor<real32> value)
    return 1
QUI
cat > "$TMP/tensor64-provider.qui" <<'QUI'
int kind(extend tensor<real64> value)
    return 2
QUI
accepts disjoint-tensor-elements <<'QUI'
import single = "./tensor32-provider.qui"
import double = "./tensor64-provider.qui"
print(tensor.ones<real32>([1]).kind())
print(tensor.ones<real64>([1]).kind())
QUI
cat > "$TMP/numeric-argument.qui" <<'QUI'
int plus(extend int value, int other)
    return value + other
int generic_plus<T>(extend int value, T other)
    return value
QUI
cat > "$TMP/string-argument.qui" <<'QUI'
int plus(extend int value, string other)
    return value + int(len(other))
int generic_plus<T>(extend int value, T[] other)
    return value
QUI
accepts disjoint-argument-types <<'QUI'
import numeric = "./numeric-argument.qui"
import text = "./string-argument.qui"
print(int(3).plus(2))
print(int(3).plus("two"))
print(int(3).generic_plus<int>(2))
QUI
accepts recursive-array-family <<'QUI'
T doubled<T: floating>(extend T value)
    return value + value
T[] doubled<T>(extend T[] value)
    T[] result = []
    for T element = value[]
        result = result.append(doubled(element))
    return result
real32[][] value = [[real32(1)], [real32(2), real32(3)]]
real32[][] result = value.doubled()
print(result[1][1])
real32[] vector = [real32(2)]
real32[] explicit = vector.doubled<real32>()
real32 scalar = real32(2).doubled<real32>()
print(explicit[0] == scalar)
QUI
runs receiver-and-argument-order true <<'QUI'
int event(int &sequence, int digit)
    sequence = sequence * 10 + digit
    return digit
int combine(extend const int value, int left, int right)
    return value * 100 + left * 10 + right
int sequence = 0
int result = event(&sequence, 1).combine(right = event(&sequence, 2), left = event(&sequence, 3))
print(sequence == 123 and result == 132)
QUI
runs writable-receiver-evaluated-once true <<'QUI'
nat next_index(nat &calls)
    calls += 1
    return 0
void increment(extend int &value, int amount)
    value += amount
int[] values = [1, 2]
nat calls = 0
.values[next_index(&calls)].increment(3)
print(calls == 1 and values[0] == 4 and values[1] == 2)
QUI
rejects mutable-temporary WRITE_CAPABILITY <<'QUI'
int[] make()
    return [1]
void reset(extend int[] &value)
    value = []
.make().reset()
QUI
rejects mutable-const-receiver WRITE_CAPABILITY <<'QUI'
void reset(extend int[] &value)
    value = []
const int[] value = [1]
.value.reset()
QUI

rejects string-intrinsic-collision EXTENSION_COLLISION <<'QUI'
string trim(extend string value)
    return value
print(" text ".trim())
QUI
cat > "$TMP/scalar-string.qui" <<'QUI'
string string(extend int value)
    return "extension"
QUI
rejects literal-string-intrinsic-collision EXTENSION_COLLISION <<'QUI'
import provider = "./scalar-string.qui"
print(3.string())
QUI
rejects expression-string-intrinsic-collision EXTENSION_COLLISION <<'QUI'
import provider = "./scalar-string.qui"
print((1 + 2).string())
QUI
rejects array-literal-intrinsic-collision EXTENSION_COLLISION <<'QUI'
nat[] indices(extend int[] value)
    return [0]
nat[] index_values = [1, 2].indices()
QUI
for method in transpose contiguous gather scatter device is_contiguous is_tracked has_grad clear_grad; do
    rejects "tensor-$method-collision" EXTENSION_COLLISION <<QUI
int $method(extend tensor<real32> value)
    return 0
tensor<real32> value = tensor.ones<real32>([2])
print(value.$method())
QUI
done
rejects tensor-boolean-collision EXTENSION_COLLISION <<'QUI'
bool all(extend tensor<bool> value)
    return true
tensor<bool> value = tensor.ones<bool>([2])
print(value.all())
QUI
rejects class-intrinsic-collision EXTENSION_COLLISION <<'QUI'
class Box<T> box(T value)
    T read()
        return box.value
int read<T>(extend Box<T> value)
    return 0
Box<int> value = Box<int>(1)
print(value.read())
QUI

if [[ "$failures" -ne 0 ]]; then
    echo "$failures extension visibility tests failed" >&2
    exit 1
fi
echo "Extension visibility tests passed"
