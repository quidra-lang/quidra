#!/usr/bin/env bash
set -euo pipefail
QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/cache"

cat > "$TMP/shapes.qui" <<'QUI'
tensor<real32>{...} scalar = tensor.ones<real32>([])
tensor<real32>{2, ...} prefix = tensor.zeros<real32>([2])
tensor<real32>{_, 3, ...} second = tensor.zeros<real32>([2, 3, 4])
tensor<real32>{..., nat C} suffix = tensor.zeros<real32>([2, 3, 4])
tensor<real32>{nat N, ..., N} square = tensor.zeros<real32>([2, 3, 2])
tensor<real32>{2, ..., 4} endpoints = tensor.zeros<real32>([2, 4])
endpoints = tensor.zeros<real32>([2, 3, 4])
print("{scalar.item()} {C} {N} {endpoints.shape()[1]}{NL}")

nat last(tensor<real32>{..., nat L} x)
    return L

tensor<real32>{..., L} echo(tensor<real32>{..., nat L} x)
    return x

nat shared(tensor<real32>{..., nat W} a, tensor<real32>{W, ...} b)
    return W

print("{last(suffix)} {last(echo(suffix))} {shared(suffix, tensor.zeros<real32>([4, 1]))}{NL}")
func<nat>(tensor<real32>{..., nat Dimension}) tail = last
print(tail(suffix))
print(NL)

tensor<T>{2, ...} generic<T: floating>(tensor<T>{2, ...} value)
    return value
print(generic(suffix).shape()[2])
print(NL)

void describe(tensor<real32> | none value)
    match value
        tensor<real32>{2, ..., nat Tail} x
            print("tail {Tail}{NL}")
        tensor<real32> other
            print("other{NL}")
        none
            print("none{NL}")

describe(tensor.zeros<real32>([2, 5]))
describe(tensor.zeros<real32>([2]))
describe(tensor.zeros<real32>([3, 2, 5]))

void exhaustive(tensor<real32> | none value)
    match value
        tensor<real32>{...} x
            print("any{NL}")
        none
            print("none{NL}")
exhaustive(scalar)

tensor<real32> make(nat[] dimensions)
    return tensor.zeros<real32>(dimensions)
tensor<real32>{..., nat Last} dynamic = make([2, 3, 5])
dynamic = make([7, 5])
print("{Last} {dynamic.shape()[0]}{NL}")

void known_rank(tensor<real32>{_, _} | none value)
    match value
        tensor<real32>{2, ...} selected
            print("prefix{NL}")
        tensor<real32>{_, _} fallback
            print("fallback{NL}")
        none
            print("none{NL}")
known_rank(tensor.zeros<real32>([3, 4]))

parallel
    tensor<real32>{2, ..., 4} hoisted = make([2, 3, 4])
    print("{hoisted.shape()[1]}{NL}")
QUI
expected="$(printf '1.0 4 2 3\n4 4 4\n4\n4\ntail 5\nother\nother\nany\n5 7\nfallback\n3')"
actual="$("$QUIDRA" run "$TMP/shapes.qui")"
if [[ "$actual" != "$expected" ]]; then
    printf 'shape output differs:\n%s\nexpected:\n%s\n' "$actual" "$expected" >&2
    exit 1
fi

rejects() {
    local source="$1" diagnostic="$2"
    printf '%s\n' "$source" > "$TMP/rejected.qui"
    if "$QUIDRA" check "$TMP/rejected.qui" > "$TMP/out" 2> "$TMP/err"; then
        echo "unexpectedly accepted: $source" >&2
        exit 1
    fi
    if ! grep -q "$diagnostic" "$TMP/err"; then
        cat "$TMP/err" >&2
        exit 1
    fi
}
rejects 'tensor<real32>{2, ..., 3} x = tensor.zeros<real32>([2])' TYPE_MISMATCH
rejects 'tensor<real32>{2, ..., 3} x = tensor.zeros<real32>([2, 4])' TYPE_MISMATCH
rejects 'tensor<real32>{_, 2, ...} x = tensor.zeros<real32>([3])' TYPE_MISMATCH
rejects 'tensor<real32>{..., ...} x = tensor.zeros<real32>([])' PARSE
rejects 'void inconsistent(tensor<real32>{2, ...} value)
    tensor<real32>{3, ...} wrong = value[:]' TYPE_MISMATCH

fails_at_runtime() {
    local shape="$1" dimensions="$2" code="$3"
    cat > "$TMP/dynamic.qui" <<QUI
tensor<real32> make(nat[] dimensions)
    return tensor.zeros<real32>(dimensions)
tensor<real32>$shape x = make($dimensions)
print("unreachable")
QUI
    local status=0
    "$QUIDRA" run "$TMP/dynamic.qui" > "$TMP/out" 2> "$TMP/err" || status=$?
    [[ "$status" -eq 101 && ! -s "$TMP/out" ]]
    grep -q "$code" "$TMP/err"
}
fails_at_runtime '{2, ..., 3}' '[2]' TENSOR
fails_at_runtime '{2, ..., 3}' '[2, 4]' TENSOR
fails_at_runtime '{nat N, ..., N}' '[2, 4, 3]' SHAPE_MISMATCH

cat > "$TMP/captured.qui" <<'QUI'
tensor<real32> make(nat[] dimensions)
    return tensor.zeros<real32>(dimensions)
tensor<real32>{..., nat C} x = make([2, 3])
x = make([4, 5])
print("unreachable")
QUI
status=0
"$QUIDRA" run "$TMP/captured.qui" > "$TMP/out" 2> "$TMP/err" || status=$?
[[ "$status" -eq 101 && ! -s "$TMP/out" ]]
grep -q TENSOR "$TMP/err"

cat > "$TMP/reference.qui" <<'QUI'
tensor<real32> make(nat[] dimensions)
    return tensor.zeros<real32>(dimensions)
tensor<real32>{2, ...} x = make([2, 4])
tensor<real32>{2, ...} &r = &x
r = make([3, 4])
print("unreachable")
QUI
status=0
"$QUIDRA" run "$TMP/reference.qui" > "$TMP/out" 2> "$TMP/err" || status=$?
[[ "$status" -eq 101 && ! -s "$TMP/out" ]]
grep -q TENSOR "$TMP/err"

printf 'tensor<real32> {2,...,nat C} x = f()\n' > "$TMP/format.qui"
"$QUIDRA" fmt "$TMP/format.qui"
[[ "$(cat "$TMP/format.qui")" == 'tensor<real32>{2, ..., nat C} x = f()' ]]
"$QUIDRA" fmt "$TMP/format.qui" --check
echo 'shape ellipsis: ok'
