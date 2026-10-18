#!/usr/bin/env bash
set -euo pipefail
QUIDRA="$1"
SOURCE_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/cache"
export QUIDRA_PACKAGE_PATH="$TMP/packages"
mkdir -p "$TMP/packages/array_fixture/native"
cp "$SOURCE_DIR/native_tensor_array_fixture.cpp" "$TMP/packages/array_fixture/native/bridge.cpp"
printf '%s\n' 'name = array_fixture' \
    'version = 0.1.0' \
    'native.source.bridge = native/bridge.cpp' \
    > "$TMP/packages/array_fixture/quidra.package"
cat > "$TMP/packages/array_fixture/main.qui" <<'QUI'
extern int64 count(const tensor<real32>[] values) = "qtest_tensor_array_count"
extern int32 bump(tensor<real32>[] &values, int32 mode) = "qtest_tensor_array_bump"
extern int32 readonly(const tensor<real32>[] values) = "qtest_tensor_array_readonly"
extern int32 bounds(const tensor<real32>[] values) = "qtest_tensor_array_bounds"
extern int32 wrong_handle(const tensor<real32> value) = "qtest_tensor_array_wrong_handle"
QUI
cat > "$TMP/pass.qui" <<'QUI'
import probe = array_fixture
void | error assert(bool condition)
    if not condition
        return error("tensor-array fixture assertion failed", code = "TEST_ASSERT")
    return
tensor<real32> original = tensor.ones<real32>([4])
tensor<real32>[] values = [original, original[1:<3]]
tensor<real32>[] saved = values
assert(probe.count(values) == int64(2))
assert(probe.bump(&values, int32(0)) == int32(0))
assert(values[0][0].item() == real32(2.0))
assert(values[1][0].item() == real32(2.0))
assert(original[0].item() == real32(1.0))
assert(saved[0][0].item() == real32(1.0))
assert(saved[1][0].item() == real32(1.0))
tensor<real32>[] empty = []
assert(probe.count(empty) == int64(0))
assert(probe.bump(&empty, int32(0)) == int32(0))
assert(probe.count([original]) == int64(1))
print("pass" + NL)
QUI
"$QUIDRA" "$TMP/pass.qui" >"$TMP/pass.out" 2>"$TMP/pass.err"
printf 'pass\n' >"$TMP/pass.expected"
cmp "$TMP/pass.expected" "$TMP/pass.out"
[[ ! -s "$TMP/pass.err" ]]
expect_failure() {
    local name="$1" code="$2" detail="$3"
    if "$QUIDRA" "$TMP/$name.qui" >"$TMP/$name.out" 2>"$TMP/$name.err"; then
        echo "unexpected success: $name" >&2
        exit 1
    fi
    grep -Fq "$code" "$TMP/$name.err"
    grep -Fq "$detail" "$TMP/$name.err"
}
cat > "$TMP/readonly.qui" <<'QUI'
import probe = array_fixture
tensor<real32>[] values = [tensor.ones<real32>([1])]
probe.readonly(values)
QUI
cat > "$TMP/bounds.qui" <<'QUI'
import probe = array_fixture
tensor<real32>[] values = [tensor.ones<real32>([1])]
probe.bounds(values)
QUI
cat > "$TMP/nonfinite.qui" <<'QUI'
import probe = array_fixture
tensor<real32>[] values = [tensor.ones<real32>([1])]
probe.bump(&values, int32(1))
QUI
cat > "$TMP/tracked.qui" <<'QUI'
import probe = array_fixture
tensor<real32>[] values = [tensor.ones<real32>([1]).track()]
probe.bump(&values, int32(0))
QUI
cat > "$TMP/uninitialized.qui" <<'QUI'
import probe = array_fixture
tensor<real32>[2] values
probe.count(values)
QUI
cat > "$TMP/wrong_handle.qui" <<'QUI'
import probe = array_fixture
probe.wrong_handle(tensor.ones<real32>([1]))
QUI
expect_failure readonly TENSOR 'read-only'
expect_failure bounds TENSOR 'out of range'
expect_failure nonfinite NONFINITE_RESULT 'bump'
# A native tensor-array write of infinity is now valid. The copied owner
# remains independent and no NaN can reach the public array.
cat > "$TMP/infinity.qui" <<'QUI'
import probe = array_fixture
tensor<real32>[] values = [tensor.ones<real32>([1])]
tensor<real32>[] saved = values
print(probe.bump(&values, int32(2)) == int32(0))
print(NL)
print(values[0][0].item() == real32(inf) and saved[0][0].item() == real32(1))
print(NL)
QUI
[[ "$("$QUIDRA" "$TMP/infinity.qui")" ==  'untracked'
expect_failure uninitialized UNINITIALIZED 'uninitialized'
expect_failure wrong_handle TENSOR 'invalid or expired'
true\ntrue' ]]
expect_failure tracked TENSOR 'untracked'
expect_failure uninitialized UNINITIALIZED 'uninitialized'
expect_failure wrong_handle TENSOR 'invalid or expired'
