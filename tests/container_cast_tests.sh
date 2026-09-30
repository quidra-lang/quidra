#!/usr/bin/env bash
set -euo pipefail

QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/array-error.qui" <<'QUI'
int[] values = [1, 300]
auto | error converted = int8(values)
match converted
    int8[] narrowed
        print(narrowed[0])
        print(NL)
    error problem
        print(problem)
        print(NL)
QUI
array_error_output="$("$QUIDRA" "$TMP/array-error.qui")"
[[ "$array_error_output" == "numeric cast outside destination range" ]]

cat > "$TMP/nested-array-error.qui" <<'QUI'
int[][] values = [[1, 2], [3, 300]]
auto | error converted = int8(values)
match converted
    int8[][] narrowed
        print(narrowed[0][0])
        print(NL)
    error problem
        print(problem)
        print(NL)
QUI
nested_array_error_output="$("$QUIDRA" "$TMP/nested-array-error.qui")"
[[ "$nested_array_error_output" == "numeric cast outside destination range" ]]

cat > "$TMP/tensor-casts.qui" <<'QUI'
tensor<int><2> safe = tensor.ones<int>([2])
auto | error safe_result = int8(safe)
match safe_result
    tensor<int8><2> narrowed
        print(narrowed[0].item())
        print(NL)
    error problem
        print(problem)
        print(NL)

tensor<int><2> unsafe = tensor.ones<int>([2])
unsafe[1] = 300
auto | error unsafe_result = int8(unsafe)
match unsafe_result
    tensor<int8><2> narrowed
        print(narrowed[0].item())
        print(NL)
    error problem
        print(problem)
        print(NL)
QUI
tensor_output="$("$QUIDRA" "$TMP/tensor-casts.qui")"
[[ "$tensor_output" == "$(printf '1\nnumeric cast outside destination range')" ]]

cat > "$TMP/contextual-fail-fast.qui" <<'QUI'
int[] values = [1, 300]
int8[] converted = int8(values)
print(converted[0])
print(NL)
QUI
set +e
"$QUIDRA" "$TMP/contextual-fail-fast.qui" >"$TMP/contextual-fail-fast.out" 2>"$TMP/contextual-fail-fast.err"
rc=$?
set -e
[[ "$rc" -eq 101 ]]
grep -q 'UNHANDLED_ERROR' "$TMP/contextual-fail-fast.err"
grep -q 'numeric cast outside destination range' "$TMP/contextual-fail-fast.err"
