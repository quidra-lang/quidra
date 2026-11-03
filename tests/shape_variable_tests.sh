#!/usr/bin/env bash
# Shapes in braces and shape variables (L17): the values shape variables
# take, the run-time checks of axes they fix (SHAPE_MISMATCH naming the
# variable and the binding), the evaluation order of axis expressions, match
# cases selected by their axes, and the formatter's brace spacing.
set -euo pipefail
set -x
QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/quidra-cache"

# Run a program that must stop: status 101, the code and the message on
# stderr.
fails_with() {
    local file="$1" code="$2" message="$3"
    set +e
    "$QUIDRA" run "$file" >"$file.out" 2>"$file.err"
    local rc=$?
    set -e
    [[ "$rc" -eq 101 ]]
    grep -q "$code" "$file.err"
    grep -qF "$message" "$file.err"
}

# Values: a `nat N` axis is the extent, `N := EXPR` its expression, and a
# bare name an existing value; shape variables are nat.
cat > "$TMP/values.qui" <<'QUI'
tensor<real32> make(nat a, nat b)
    return tensor.zeros<real32>([a, b])

tensor<real32>{nat R, nat W} d := make(2, 3)
print("{R} x {W}{NL}")
nat rows := 4
nat cols := 5
tensor<real32>{H := rows * cols, nat C} img := make(20, 3)
print("{H} {C}{NL}")
tensor<real32>{M := 2 * N, nat N} pair := make(4, 2)
print("{M} {N}{NL}")
tensor<real32>{nat K, K} square := make(3, 3)
print(int(K) - 4)
print(NL)
tensor<real32>{R, W} same := make(2, 3)
print(same.shape()[1])
print(NL)
QUI
[[ "$("$QUIDRA" run "$TMP/values.qui")" == "$(printf '2 x 3\n20 3\n4 2\n-1\n3')" ]]

# Runtime extents remain unknown to the checker; a repeat that does not
# hold names the variable and binding. Literal mismatches reject statically.
cat > "$TMP/square.qui" <<'QUI'
tensor<real32> make(nat a, nat b)
    return tensor.zeros<real32>([a, b])

nat width := nat(tensor.ones<real32>([]).item())
tensor<real32>{nat K, K} sq := make(width * 2, 3)
print("unreachable")
QUI
fails_with "$TMP/square.qui" SHAPE_MISMATCH "shape variable 'K' is 2, but axis 1 of 'sq' is 3"

cat > "$TMP/named.qui" <<'QUI'
tensor<real32> make(nat a, nat b)
    return tensor.zeros<real32>([a, b])

nat rows := 4
nat cols := 5
nat unit := nat(tensor.ones<real32>([]).item())
tensor<real32>{H := rows * cols, nat W} img := make(unit * 21, 3)
QUI
fails_with "$TMP/named.qui" SHAPE_MISMATCH "shape variable 'H' is 20, but axis 0 of 'img' is 21"

# An existing shape variable named bare is checked the same way; a later
# assignment conforms to the declared extents.
cat > "$TMP/reuse.qui" <<'QUI'
tensor<real32> make(nat a, nat b)
    return tensor.zeros<real32>([a, b])

tensor<real32>{nat B, nat C} x := make(2, 3)
nat unit := nat(tensor.ones<real32>([]).item())
tensor<real32>{B, C} y := make(2, unit * 4)
QUI
fails_with "$TMP/reuse.qui" SHAPE_MISMATCH "shape variable 'C' is 3, but axis 1 of 'y' is 4"

cat > "$TMP/reassign.qui" <<'QUI'
tensor<real32> make(nat a, nat b)
    return tensor.zeros<real32>([a, b])

tensor<real32>{nat B, nat C} x := make(2, 3)
x = make(2, 3)
print("same")
print(NL)
nat unit := nat(tensor.ones<real32>([]).item())
x = make(2, unit * 5)
QUI
fails_with "$TMP/reassign.qui" TENSOR "tensor extent does not satisfy captured shape constraint"
grep -q '^same$' "$TMP/reassign.qui.out"

# An any-rank value is checked for its rank before its extents are read.
cat > "$TMP/rank.qui" <<'QUI'
tensor<real32> line(nat n)
    if tensor.ones<real32>([]).item() == real32(1)
        return tensor.zeros<real32>([n])
    return tensor.zeros<real32>([n, n, n])

tensor<real32>{nat C, nat H, nat W} img := line(3)
QUI
fails_with "$TMP/rank.qui" TENSOR "tensor rank does not satisfy captured shape constraint"

# Order (Q58): an axis expression over no variable of the declaration reads
# its operands before the initializer runs, and nat(n) of a negative n stops
# like a negative captured extent.
cat > "$TMP/order.qui" <<'QUI'
tensor<real32> grow(int &n)
    n = n + 1
    return tensor.zeros<real32>([nat(n)])

int unit := int(tensor.ones<real32>([]).item())
int n := unit * 2
tensor<real32>{nat(n)} x := grow(&n)
QUI
fails_with "$TMP/order.qui" TENSOR "tensor extent does not satisfy captured shape constraint"

cat > "$TMP/negative.qui" <<'QUI'
int n := -1
tensor<real32>{nat(n)} x := tensor.zeros<real32>([2])
QUI
fails_with "$TMP/negative.qui" TENSOR "captured tensor extent cannot be negative"

# Shape variables of a loop body are bound anew each iteration.
cat > "$TMP/loop.qui" <<'QUI'
tensor<real32> line(nat n)
    return tensor.zeros<real32>([n])

nat i := 1
while i <= 3
    tensor<real32>{nat L} v := line(i)
    print(L)
    i = i + 1
print(NL)
QUI
[[ "$("$QUIDRA" run "$TMP/loop.qui")" == "123" ]]

# A match case is taken when its axes hold (Q60); a case with an equality
# axis covers nothing, so a later case of the same type takes the rest.
cat > "$TMP/match.qui" <<'QUI'
tensor<real32> make(nat a, nat b)
    return tensor.zeros<real32>([a, b])

void describe(tensor<real32> | none value)
    match value
        tensor<real32>{nat K, K} square
            print("square {K}{NL}")
        tensor<real32>{3, nat W} rgb
            print("rgb {W}{NL}")
        tensor<real32> other
            print("other{NL}")
        none
            print("none{NL}")

describe(make(2, 2))
describe(make(3, 5))
describe(make(2, 3))
describe(tensor.zeros<real32>([4]))
describe(none)

void rank_only(tensor<real32> | none value)
    match value
        tensor<real32>{nat Rows, nat Cols} shaped
            print("matrix{NL}")
        tensor<real32> other
            print("other{NL}")
        none
            print("none{NL}")

rank_only(make(2, 3))
rank_only(tensor.zeros<real32>([4]))
QUI
[[ "$("$QUIDRA" run "$TMP/match.qui")" == "$(printf 'square 2\nrgb 5\nother\nother\nnone\nmatrix\nother')" ]]

# The formatter writes braces next to the element type and spaces axis
# expressions as expressions.
printf 'tensor<real32> { nat N ,L:=N+1 , _ }  x:=f()\n' > "$TMP/format.qui"
"$QUIDRA" fmt "$TMP/format.qui"
[[ "$(cat "$TMP/format.qui")" == 'tensor<real32>{nat N, L := N + 1, _} x := f()' ]]
"$QUIDRA" fmt "$TMP/format.qui" --check
