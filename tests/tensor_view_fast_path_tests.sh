#!/usr/bin/env bash
set -euo pipefail

# Runtime fast paths for tensor views. Initialization checks, autograd
# snapshots and elementwise/gather/copy paths skip per-element bookkeeping
# when a view is proven dense or inside fully initialized storage. Every case
# here must behave exactly as the per-element paths do, and every use of an
# uninitialized element must still fail.

QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/quidra-cache"  # a run cache of this suite run only

cat > "$TMP/reduction_helpers.qui" <<'QUI'
tensor<real32> sum(tensor<real32> value)
    int count = 1
    for extent in value.shape()
        count = count * int(extent)
    tensor<real32> result = value.gather([0], [])
    for index in range(1, count)
        result = result + value.gather([index], [])
    return result
QUI

# Runs a program that prints one "true" per check.
expect_checks() {
    local name="$1"
    local checks="$2"
    local output
    output="$("$QUIDRA" "$TMP/$name.qui")"
    if [[ "$output" != "$(for ((i = 0; i < checks; ++i)); do echo true; done)" ]]; then
        echo "unexpected $name output:" >&2
        printf '%s\n' "$output" >&2
        exit 1
    fi
}

# Views over fully initialized storage are proven in O(rank); views over
# partially initialized storage are accepted only when they avoid every
# uninitialized element.
cat > "$TMP/initialization-views.qui" <<'QUI'
tensor<real32> grid = tensor.zeros<real32>([2, 3])
grid[0, 0] = real32(1)
grid[0, 1] = real32(2)
grid[0, 2] = real32(3)
grid[1, 0] = real32(4)
grid[1, 1] = real32(5)
grid[1, 2] = real32(6)
tensor<real32> flat = grid.reshape([6])

print((-grid)[1, 2].item() == real32(-6))
print(NL)
print((-grid[1:2, 0:3])[0, 0].item() == real32(-4))
print(NL)
print((-grid.transpose(0, 1))[2, 1].item() == real32(-6))
print(NL)
print((-grid.transpose(0, 1))[0, 1].item() == real32(-4))
print(NL)
print((-grid.reshape([1, 6, 1])[0:1, 5:6, 0:1])[0, 0, 0].item() == real32(-6))
print(NL)
print((-flat[3:6])[2].item() == real32(-6))
print(NL)
print((-flat[1:6:2])[2].item() == real32(-6))
print(NL)
print((-grid[1, 2]).item() == real32(-6))
print(NL)
print((-tensor.ones<real32>([])).item() == real32(-1))
print(NL)
print((-tensor.zeros<real32>([0, 3])).shape()[0] == 0)
print(NL)
print((-flat[6:6]).shape()[0] == 0)
print(NL)
print((grid.transpose(0, 1) == grid.transpose(0, 1)).all())
print(NL)
print(real64(flat[4:6])[1].item() == 6.0)
print(NL)

tensor<real32> partial = tensor<real32>([6])
partial[0] = real32(1)
partial[1] = real32(2)
partial[2] = real32(3)
partial[4] = real32(5)
print((-partial[0:3])[2].item() == real32(-3))
print(NL)
print((-partial[0:5:2])[2].item() == real32(-5))
print(NL)
print((-partial[4]).item() == real32(-5))
print(NL)
print((-partial[3:3]).shape()[0] == 0)
print(NL)
print((-partial.reshape([2, 3])[0:1, 0:3])[0, 2].item() == real32(-3))
print(NL)

partial[3] = real32(4)
partial[5] = real32(6)
print((-partial)[5].item() == real32(-6))
print(NL)
print((-partial.reshape([2, 3]).transpose(0, 1))[0, 1].item() == real32(-4))
print(NL)
QUI
expect_checks initialization-views 20

# Autograd snapshots copy contiguous CPU views (offset views and views whose
# only non-unit axes are dense) with one bulk copy and gather every other view
# element by element. Each gradient below is the other operand's forward
# snapshot, so any copy error changes an exact value.
cat > "$TMP/autograd-snapshots.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"

tensor<real64> sum64(tensor<real64> value)
    tensor<real64> result = value.gather([0], [])
    for index in range(1, value.shape()[0])
        result = result + value.gather([index], [])
    return result

tensor<real32> values = tensor.zeros<real32>([2, 3])
values[0, 0] = real32(1)
values[0, 1] = real32(2)
values[0, 2] = real32(3)
values[1, 0] = real32(4)
values[1, 1] = real32(5)
values[1, 2] = real32(6)
tensor<real32> flat = values.reshape([6])

// Contiguous float32: the tracked leaf snapshot feeds x * x backward.
tensor<real32> whole = values.track()
tensor<real32> squared = whole * whole
print(squared.untrack()[1, 2].item() == real32(36))
print(NL)
reductions.sum(squared).backward(&whole)
tensor<real32> whole_grad = whole.grad.untrack()
print(whole_grad[0, 0].item() == real32(2) and whole_grad[1, 2].item() == real32(12))
print(NL)

// Offset view (contiguous run starting inside storage) against a
// non-contiguous stepped view (per-element fallback). The gradient of each
// side of left * right is the other side's forward snapshot.
tensor<real32> left = flat[3:6].track()
tensor<real32> right = flat[0:6:2].track()
tensor<real32> product = left * right
tensor<real32> product_values = product.untrack()
print(product_values[0].item() == real32(4) and product_values[1].item() == real32(15) and product_values[2].item() == real32(30))
print(NL)
reductions.sum(product).backward(&left, &right)
tensor<real32> left_grad = left.grad.untrack()
tensor<real32> right_grad = right.grad.untrack()
print(left_grad[0].item() == real32(1) and left_grad[1].item() == real32(3) and left_grad[2].item() == real32(5))
print(NL)
print(right_grad[0].item() == real32(4) and right_grad[1].item() == real32(5) and right_grad[2].item() == real32(6))
print(NL)

// Row slice with an offset, and transposed column slices whose only
// non-unit axis is dense in storage.
tensor<real32> first_row = values[0:1, 0:3].track()
tensor<real32> second_row = values[1:2, 0:3].track()
reductions.sum(first_row * second_row).backward(&first_row, &second_row)
print(first_row.grad.untrack()[0, 0].item() == real32(4) and first_row.grad.untrack()[0, 2].item() == real32(6))
print(NL)
print(second_row.grad.untrack()[0, 0].item() == real32(1) and second_row.grad.untrack()[0, 2].item() == real32(3))
print(NL)
tensor<real32> first_column = values.transpose(0, 1)[0:3, 0:1].track()
tensor<real32> second_column = values.transpose(0, 1)[0:3, 1:2].track()
tensor<real32> column_product = first_column * second_column
print(column_product.untrack()[2, 0].item() == real32(18))
print(NL)
reductions.sum(column_product).backward(&first_column, &second_column)
print(first_column.grad.untrack()[0, 0].item() == real32(4) and first_column.grad.untrack()[2, 0].item() == real32(6))
print(NL)
print(second_column.grad.untrack()[0, 0].item() == real32(1) and second_column.grad.untrack()[2, 0].item() == real32(3))
print(NL)

// Tracked views snapshot their own (non-contiguous) values.
tensor<real32> source = values.track()
tensor<real32> transposed = source.transpose(0, 1)
tensor<real32> weights = flat.reshape([3, 2])
reductions.sum(transposed * weights).backward(&source)
tensor<real32> source_grad = source.grad.untrack()
print(source_grad[0, 0].item() == real32(1) and source_grad[1, 0].item() == real32(2))
print(NL)
print(source_grad[0, 2].item() == real32(5) and source_grad[1, 2].item() == real32(6))
print(NL)

// Contiguous float64 and an offset float64 view.
tensor<real64> doubles = tensor.zeros<real64>([4])
doubles[0] = 0.5
doubles[1] = 1.5
doubles[2] = 2.5
doubles[3] = 16777217.0
tensor<real64> tracked_doubles = doubles.track()
tensor<real64> tail = doubles[1:4].track()
tensor<real64> head = doubles[0:3].track()
sum64(tail * head).backward(&tail, &head)
print(tail.grad.untrack()[2].item() == 2.5 and head.grad.untrack()[2].item() == 16777217.0)
print(NL)
sum64(tracked_doubles * tracked_doubles).backward(&tracked_doubles)
print(tracked_doubles.grad.untrack()[3].item() == 33554434.0)
print(NL)
QUI
expect_checks autograd-snapshots 14

# Elementwise arithmetic, gather, copy-on-write and contiguous() read dense,
# fully initialized operands directly and mark outputs initialized in one
# step. Broadcast, strided and partially initialized operands keep the exact
# per-element paths.
cat > "$TMP/dense-operands.qui" <<'QUI'
tensor<real32> grid = tensor.zeros<real32>([2, 3])
grid[0, 0] = real32(1)
grid[0, 1] = real32(2)
grid[0, 2] = real32(3)
grid[1, 0] = real32(4)
grid[1, 1] = real32(5)
grid[1, 2] = real32(6)
tensor<real32> flat = grid.reshape([6])

// Elementwise arithmetic: dense same-shape operands (including offset views)
// and every broadcast or strided operand give the same element-by-element
// results.
tensor<real32> sums = flat[3:6] + flat[0:3]
print(sums[0].item() == real32(5) and sums[2].item() == real32(9))
print(NL)
tensor<real32> differences = grid - grid.transpose(0, 1).transpose(0, 1)
print(differences[1, 2].item() == real32(0))
print(NL)
tensor<real32> mixed = grid.transpose(0, 1) * grid.transpose(0, 1).contiguous()
print(mixed[2, 1].item() == real32(36) and mixed[0, 1].item() == real32(16))
print(NL)
tensor<real32> row = grid[1:2, 0:3]
tensor<real32> broadcast = grid / row
print(broadcast[0, 2].item() == real32(0.5) and broadcast[1, 1].item() == real32(1))
print(NL)
print((real32(12) / flat[3:6])[1].item() == real32(2.4))
print(NL)
print((flat[1:6:2] - real32(1))[2].item() == real32(5))
print(NL)
print((flat ^ real32(2))[5].item() == real32(36))
print(NL)

// Gathers from dense offset views, strided views and narrow element types.
tensor<real32> picked = flat[2:6].gather([3, 0, 3], [3])
print(picked[0].item() == real32(6) and picked[1].item() == real32(3))
print(NL)
tensor<real32> strided_pick = grid.transpose(0, 1).gather([1, 4], [2])
print(strided_pick[0].item() == real32(4) and strided_pick[1].item() == real32(3))
print(NL)
tensor<int8> small = tensor.ones<int8>([4]) * int8(3)
tensor<int8> small_pick = small[1:4].gather([2, 0], [2])
print(small_pick[0].item() == int8(3))
print(NL)
tensor<int16> medium = tensor.ones<int16>([3]) * int16(300)
print(medium[1:3].gather([1, 0], [2])[0].item() == int16(300))
print(NL)
tensor<bool> flags = tensor.ones<real32>([3]) == tensor.ones<real32>([3])
print(flags.gather([1, 2], [2]).all())
print(NL)

// Copy-on-write detaches a shared or offset view into fresh storage.
tensor<real32> copy = grid
copy[0, 0] = real32(9)
print(grid[0, 0].item() == real32(1) and copy[0, 0].item() == real32(9) and copy[1, 2].item() == real32(6))
print(NL)
tensor<real32> view = grid[1:2, 0:3]
view[0, 1] = real32(7)
print(view[0, 0].item() == real32(4) and view[0, 1].item() == real32(7) and grid[1, 1].item() == real32(5))
print(NL)

// contiguous() copies a dense view whose size-1 axis carries a non-canonical
// stride (shape [3, 1], strides [1, 3], offset 3) in bulk into fresh storage.
tensor<real32> column = grid.transpose(0, 1)[0:3, 1:2].contiguous()
print(column.shape()[0] == 3 and column.shape()[1] == 1)
print(NL)
print((-column)[0, 0].item() == real32(-4) and column[1, 0].item() == real32(5) and column[2, 0].item() == real32(6))
print(NL)
column[0, 0] = real32(9)
print(column[0, 0].item() == real32(9) and grid[1, 0].item() == real32(4))
print(NL)
QUI
expect_checks dense-operands 17

cat > "$TMP/partial-operands.qui" <<'QUI'
tensor<real32> partial = tensor<real32>([6])
partial[0] = real32(1)
partial[1] = real32(2)
partial[2] = real32(3)
partial[4] = real32(5)

print((partial[0:3] + partial[0:3])[2].item() == real32(6))
print(NL)
print((real32(1) - partial[4]).item() == real32(-4))
print(NL)
tensor<real32> picked = partial.gather([0, 4, 2], [3])
print((-picked)[1].item() == real32(-5))
print(NL)
tensor<real32> holes = partial.gather([0, 3], [2])
print(holes[0].item() == real32(1))
print(NL)
tensor<real32> copy = partial
copy[3] = real32(4)
print(copy[3].item() == real32(4) and copy[4].item() == real32(5))
print(NL)
// contiguous() of a dense view over partially initialized storage keeps the
// holes (storage elements 3 and 5) uninitialized in the copy.
tensor<real32> column = partial.reshape([2, 3]).transpose(0, 1)[0:3, 1:2].contiguous()
print((-column[1:2, 0:1])[0, 0].item() == real32(-5))
print(NL)
QUI
expect_checks partial-operands 6

# CPU backward hands reshape and transpose gradients to their parents without
# copying them and builds custom-native gradient tensors directly from the
# gradient buffer. A rank-3 transpose followed by reshape checks every
# element's routing, and a second backward checks accumulation.
cat > "$TMP/backward-buffers.qui" <<'QUI'
import reductions = "./reduction_helpers.qui"
tensor<real32> base = tensor.zeros<real32>([2, 3])
base[0, 0] = real32(1)
base[0, 1] = real32(2)
base[0, 2] = real32(3)
base[1, 0] = real32(4)
base[1, 1] = real32(5)
base[1, 2] = real32(6)

tensor<real32> cube = base.reshape([2, 1, 3]).track()
tensor<real32> moved = cube.transpose(0, 2)
tensor<real32> weights = base.reshape([3, 1, 2])
tensor<real32> flat = (moved * weights).reshape([6])
reductions.sum(flat).backward(&cube)
tensor<real32> first = cube.grad.untrack()
print(first[0, 0, 0].item() == real32(1) and first[1, 0, 0].item() == real32(2))
print(NL)
print(first[1, 0, 1].item() == real32(4) and first[0, 0, 2].item() == real32(5) and first[1, 0, 2].item() == real32(6))
print(NL)
reductions.sum((cube.transpose(0, 2) * weights).reshape([6])).backward(&cube)
tensor<real32> accumulated = cube.grad.untrack()
print(accumulated[0, 0, 0].item() == real32(2) and accumulated[1, 0, 2].item() == real32(12))
print(NL)
QUI
expect_checks backward-buffers 3

# Each scenario reaches an uninitialized element, or an arithmetic failure,
# through a different path and must fail before printing anything. The
# program is built once and run per scenario.
cat > "$TMP/failures.qui" <<'QUI'
cli args
    int scenario = option(default = 0)

tensor<real32> partial = tensor<real32>([6])
partial[0] = real32(1)
partial[1] = real32(2)
partial[2] = real32(3)
partial[4] = real32(5)
if args.scenario == 1
    tensor<real32> ignored = -partial
if args.scenario == 2
    tensor<real32> ignored = -partial[1:4]
if args.scenario == 3
    tensor<real32> ignored = -partial[3:6:2]
if args.scenario == 4
    tensor<real32> ignored = -partial[3]
if args.scenario == 5
    tensor<real32> ignored = -partial.reshape([2, 3]).transpose(0, 1)
if args.scenario == 6
    tensor<real32> ignored = -partial.reshape([2, 3])[1:2, 0:1]
if args.scenario == 7
    tensor<real32> ignored = partial[2:4].track()
if args.scenario == 8
    bool ignored = (partial[0:4] == partial[0:4]).all()
if args.scenario == 9
    tensor<real32> ignored = partial + partial
if args.scenario == 10
    tensor<real32> ignored = partial[1:4] * real32(2)
if args.scenario == 11
    tensor<real32> ignored = partial.reshape([2, 3]) + tensor.ones<real32>([1, 3])
if args.scenario == 12
    real32 ignored = partial.gather([0, 3], [2])[1].item()
if args.scenario == 13
    tensor<real32> ignored = -partial.gather([0, 3], [2])
if args.scenario == 14
    tensor<real32> copy = partial
    copy[3] = real32(4)
    real32 ignored = copy[5].item()
if args.scenario == 15
    tensor<real32> copy = partial
    copy[3] = real32(4)
    real32 ignored = partial[3].item()
if args.scenario == 16
    tensor<int8> ignored = tensor.ones<int8>([3]) * int8(127) + int8(1)
if args.scenario == 17
    tensor<int64> ignored = tensor.ones<int64>([2]) / tensor.zeros<int64>([2])
if args.scenario == 18
    tensor<int64> ignored = tensor.ones<int64>([4])[1:3] % tensor.zeros<int64>([2])
if args.scenario == 19
    real32 ignored = partial.reshape([2, 3]).transpose(0, 1)[0:3, 1:2].contiguous()[0, 0].item()
if args.scenario == 20
    real32 ignored = partial.reshape([2, 3]).transpose(0, 1)[0:3, 1:2].contiguous()[2, 0].item()
print("unreachable")
QUI
"$QUIDRA" build "$TMP/failures.qui" -o "$TMP/failures" >"$TMP/failures-build.log" 2>&1 || {
    cat "$TMP/failures-build.log" >&2
    exit 1
}
while IFS='|' read -r scenario expected; do
    set +e
    "$TMP/failures" --scenario "$scenario" >"$TMP/failure.out" 2>"$TMP/failure.err"
    status=$?
    set -e
    if [[ "$status" -ne 101 ]] || ! grep -Fq "$expected" "$TMP/failure.err" ||
       [[ -s "$TMP/failure.out" ]]; then
        echo "scenario $scenario did not fail with '$expected' (status $status)" >&2
        cat "$TMP/failure.out" "$TMP/failure.err" >&2
        exit 1
    fi
done <<'CASES'
1|UNINITIALIZED
2|UNINITIALIZED
3|UNINITIALIZED
4|UNINITIALIZED
5|UNINITIALIZED
6|UNINITIALIZED
7|UNINITIALIZED
8|UNINITIALIZED
9|UNINITIALIZED
10|UNINITIALIZED
11|UNINITIALIZED
12|UNINITIALIZED
13|UNINITIALIZED
14|UNINITIALIZED
15|UNINITIALIZED
16|tensor integer arithmetic overflow
17|invalid tensor division/remainder or integer overflow
18|invalid tensor division/remainder or integer overflow
19|UNINITIALIZED
20|UNINITIALIZED
CASES

echo "tensor view fast paths: ok"
