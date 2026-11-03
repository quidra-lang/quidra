#!/usr/bin/env bash
set -euo pipefail
QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/cache"

cat > "$TMP/families.qui" <<'QUI'
T sum<T: floating>(const tensor<T> values)
    return values.item()

T sum<T: floating>(const T[] values)
    return values[0]

real32 sum(const real32[] values)
    return values[0] * 2.0

tensor<real32> scalar = tensor.ones<real32>([])
real32[] single = [1.0]
real64[] other = [3.0]
print("{sum(scalar)} {sum(single)} {sum(other)}{NL}")
print("{sum<real32>(scalar)} {sum<real32>(single)} {sum<real64>(other)}{NL}")
print("{sum<real32>([1.0])} {sum<real32>(tensor.zeros([]))}{NL}")

T sum_array<T: floating>(const T[] values)
    return sum<T>(values)

print("{sum_array<real32>(single)}{NL}")

T identity<T: numeric>(T value)
    return value
T identity<T: floating>(T value)
    return value

print("{identity<real32>(2.0)}{NL}")

nat count_items<T: floating>(const tensor<T> values)
    return len(values.shape())
nat count_items<T: floating>(const T[] values)
    return len(values)

nat[] none_values = []
print("{count_items<real32>([])} {count_items<real32>([for nat x = none_values[] then 1.0])}{NL}")

print("{sum<real32>(if true then [1.0] else [2.0])} {sum<real32>(if false then tensor.ones([]) else tensor.zeros([]))}{NL}")

class Reducer()
    T sum<T: floating>(const tensor<T> values)
        return values.item()
    T sum<T: floating>(const T[] values)
        return values[0]
    T sum_array<T: floating>(const T[] values)
        return sum<T>(values)

Reducer reducer = Reducer()
print("{reducer.sum<real32>(scalar)} {reducer.sum<real32>(single)} {reducer.sum_array<real32>(single)}{NL}")
print("{reducer.sum<real32>(if true then [1.0] else [2.0])}{NL}")

// A floating constraint names scalar values, disjoint from tensor values.
string pick<T: floating>(tensor<T> values)
    return "tensor"
string pick<T: floating>(T values)
    return "scalar"
print("{pick(scalar)} {pick(real32(2.0))}{NL}")
print("{pick<real32>(scalar)} {pick<real32>(2.0)}{NL}")
QUI
"$QUIDRA" check "$TMP/families.qui"
expected=$'1.0 2.0 3.0\n1.0 1.0 3.0\n1.0 0.0\n1.0\n2.0\n0 0\n1.0 0.0\n1.0 1.0 1.0\n1.0\ntensor scalar\ntensor scalar'
if actual="$("$QUIDRA" run "$TMP/families.qui")"; then
    if [[ "$actual" != "$expected" ]]; then
        printf 'unexpected specialization results\nexpected:\n%s\nactual:\n%s\n' "$expected" "$actual" >&2
        exit 1
    fi
else
    echo 'specialization fixture failed to run' >&2
    exit 1
fi

rejects() {
    local diagnostic="$1"
    cat > "$TMP/rejected.qui"
    if "$QUIDRA" check "$TMP/rejected.qui" > "$TMP/out" 2> "$TMP/err"; then
        echo 'unexpectedly accepted invalid specialization family' >&2
        exit 1
    fi
    grep -q "$diagnostic" "$TMP/err"
}

rejects AMBIGUOUS_SPECIALIZATION <<'QUI'
string pick<T: ordered>(const T[] values)
    return "ordered"
string pick<T: equatable>(const T[] values)
    return "equatable"
QUI

rejects DUPLICATE_NAME <<'QUI'
string pick<T: floating>(const tensor<T> values)
    return "tensor"
string pick<T: floating>(const T[] items)
    return "array"
QUI

rejects DUPLICATE_NAME <<'QUI'
string pick<T: floating>(const tensor<T> values, int mode = 0)
    return "tensor"
string pick<T: floating>(const T[] values, int mode)
    return "array"
QUI

rejects GENERIC_CONSTRAINT <<'QUI'
T sum<T: floating>(const tensor<T> values)
    return values.item()
T sum<T: floating>(const T[] values)
    return values[0]
int[] values = [1]
print(sum<int>(values))
QUI

rejects AMBIGUOUS_SPECIALIZATION <<'QUI'
T sum<T: floating>(const tensor<T> values)
    return values.item()
T sum<T: floating>(const T[] values)
    return values[0]
print(sum<real32>(if true then [1.0] else tensor.ones([])))
QUI

echo 'generic tensor/array specialization families: ok'
