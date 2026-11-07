#!/usr/bin/env bash
set -euo pipefail
QUIDRA=${1:?compiler path required}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/cache"
run_case() {
    local name=$1 expected=$2
    cat > "$TMP/$name.qui"
    "$QUIDRA" run "$TMP/$name.qui" > "$TMP/$name.out"
    printf '%s\n' "$expected" > "$TMP/$name.expected"
    diff -u "$TMP/$name.expected" "$TMP/$name.out"
}
reject_case() {
    local name=$1 code=$2
    cat > "$TMP/$name.qui"
    if "$QUIDRA" check "$TMP/$name.qui" > "$TMP/$name.out" 2> "$TMP/$name.err"; then
        printf 'expected rejection: %s\n' "$name" >&2
        exit 1
    fi
    grep -q "$code" "$TMP/$name.err"
}
fail_case() {
    local name=$1 code=${2:-RANGE_CONSTRAINT}
    cat > "$TMP/$name.qui"
    if "$QUIDRA" run "$TMP/$name.qui" > "$TMP/$name.out" 2> "$TMP/$name.err"; then
        printf 'expected range failure: %s\n' "$name" >&2
        exit 1
    fi
    grep -q "$code" "$TMP/$name.err"
    test ! -s "$TMP/$name.out"
}

run_case scalar $'3\n4\n5\ntrue' <<'QUI'
0 < x < 5 := 3
println(x)
y := 4
0 < x < 5 = y
println(x)
0.5 <= n <= 7.5 := 5
println(n)
println(0 < x < 5)
QUI

run_case parameters $'13\n7\n3\n0.5\n5' <<'QUI'
int bounded(3 < int value < 7, limit < 9 := 8)
    return value + limit
println(bounded(5))
println(bounded(value = 4, limit = 3))
class Config(nat attempts > 0 := 3, 0 <= real32 rate <= 1 := 0.5)
    nat count()
        return attempts
settings := Config()
println(settings.count())
println(settings.rate)
int identity(0 < int value < 6)
    return value
func<int(int)> callback := identity
println(callback(5))
QUI

run_case effects $'3\n11' <<'QUI'
int next(int &calls)
    calls += 1
    return 3
int upper(int &calls)
    calls += 10
    return 5
x := 1
calls := 0
0 < x < upper(&calls) = next(&calls)
println(x)
println(calls)
QUI

run_case converted_candidate true <<'QUI'
real64 upper := 2048.5
0 < real16 rounded < upper := 2049
println(rounded == 2048.0)
QUI

run_case validate_before_commit $'2\n3' <<'QUI'
int inspect(int &old_value)
    println(old_value)
    return 5
x := 2
0 < x < inspect(&x) = 3
println(x)
QUI

run_case descending_order 12 <<'QUI'
int upper(int &calls)
    calls = calls * 10 + 1
    return 5
int lower(int &calls)
    calls = calls * 10 + 2
    return 0
x := 1
calls := 0
upper(&calls) > x > lower(&calls) = 3
println(calls)
QUI

run_case owned_parameter_snapshot 1 <<'QUI'
int upper(int &value)
    value = 1
    return 100000000000000000000000000000001
void take(0 < int value < upper(&value))
    println(value)
take(100000000000000000000000000000000)
QUI

run_case reference_binding $'3\n9' <<'QUI'
x := 3
0 < int &r < 5 := &x
println(r)
r = 9
println(x)
QUI

run_case reference_parameter 9 <<'QUI'
void take(0 < int &value < 5)
    value = 9
x := 3
take(&x)
println(x)
QUI

run_case extern_parameter $'4\n3' <<'QUI'
int32 ceiling(int32 &value)
    value = 4
    return 10
extern int32 native(0 < int32 value < ceiling(&value)) = "abs"
int32 source := 3
println(native(source))
println(source)
QUI

run_case propagated_bound_error BOUND <<'QUI'
int | error cap()
    return error("bound failed", code = "BOUND")
int | error guarded(int value)
    0 < candidate < (try cap()) := value
    return candidate
int | error result := guarded(3)
match result
    int value
        println("unexpected")
    error problem
        println(problem.code)
QUI

run_case shapes $'3\n3\nother\n3' <<'QUI'
tensor<real32>{0 < nat N < 5} values := tensor.zeros([3])
println(N)
existing := 3
tensor<real32>{0 < existing < 5} fixed := tensor.ones([3])
println(fixed.shape()[0])
existing = 4
fixed = tensor.ones<real32>([3])
tensor<real32> | none wide := tensor.zeros<real32>([6])
match wide
    tensor<real32>{0 < nat K < 5} small
        println("small")
    tensor<real32> other
        println("other")
    none
        println("none")
nat dimension(tensor<real32>{0 < nat L < 5} input)
    return L
println(dimension(values))
QUI

run_case shaped_callback ok <<'QUI'
void bounded(tensor<real32>{0 < nat M < 5} value)
    println("ok")
func<void(tensor<real32>{0 < nat N < 5})> callback := bounded
callback(tensor.ones<real32>([3]))
QUI

run_case shaped_callback_bound_names ok <<'QUI'
void bounded(tensor<real32>{nat M, nat N < (if 0 < M < 5 then 10 else 20)} value)
    println("ok")
func<void(tensor<real32>{nat A, nat B < (if 0 < A < 5 then 10 else 20)})> callback := bounded
callback(tensor.ones<real32>([3, 6]))
QUI

run_case nested_callback ok <<'QUI'
void bounded(tensor<real32>{0 < nat M < 5} value)
    println("ok")
void accept(func<void(tensor<real32>{0 < nat N < 5})> callback)
    callback(tensor.ones<real32>([3]))
func<void(func<void(tensor<real32>{0 < nat K < 5})>)> outer := accept
outer(bounded)
QUI

run_case constant_shape_case other <<'QUI'
const int extent := 6
tensor<real32> | none wide := tensor.zeros<real32>([6])
match wide
    tensor<real32>{0 < extent < 5} small
        println("small")
    tensor<real32> other
        println("other")
    none
        println("none")
QUI

reject_case callback_contract FUNCTION_REFERENCE_SIGNATURE <<'QUI'
void unbounded(tensor<real32>{nat M} value)
    return
func<void(tensor<real32>{0 < nat N < 5})> callback := unbounded
QUI
reject_case callback_cast_contract FUNCTION_REFERENCE_SIGNATURE <<'QUI'
void bounded(tensor<real32>{nat M < nat(3.5)} value)
    return
func<void(tensor<real32>{nat N < 3.5})> callback := bounded
QUI
reject_case callback_iterator_contract FUNCTION_REFERENCE_SIGNATURE <<'QUI'
void bounded(tensor<real32>{nat M < ([for int a := [2][] then ([for int b := [3][] then a]).sum()]).sum()} value)
    return
func<void(tensor<real32>{nat N < ([for int c := [2][] then ([for int d := [3][] then d]).sum()]).sum()})> callback := bounded
QUI
reject_case nested_shape_contract RANGE_CONSTRAINT <<'QUI'
void take(tensor<real32>{0 < nat N < 5} | none value)
    return
QUI
reject_case constant RANGE_CONSTRAINT <<'QUI'
0 < x < 5 := 6
QUI
reject_case constant_shape RANGE_CONSTRAINT <<'QUI'
tensor<real32>{0 < nat N < 5} values := tensor.zeros<real32>([6])
QUI
reject_case boolean TYPE_MISMATCH <<'QUI'
0 < x < 5 := true
QUI
reject_case undeclared UNKNOWN_NAME <<'QUI'
value := 3
0 < missing < 5 = value
QUI
reject_case constant_argument RANGE_CONSTRAINT <<'QUI'
void take(0 < int value < 5)
    return
take(6)
QUI
reject_case named_argument RANGE_CONSTRAINT <<'QUI'
void take(0 < int value < 5)
    return
take(value = 6)
QUI
reject_case default_argument RANGE_CONSTRAINT <<'QUI'
void take(0 < int value < 5 := 6)
    return
take()
QUI
reject_case constructor_argument RANGE_CONSTRAINT <<'QUI'
class Positive(nat value > 0)
    nat read()
        return value
created := Positive(0)
QUI
reject_case reference_uninitialized UNINITIALIZED <<'QUI'
int x
0 < int &r < 5 := &x
QUI
reject_case constructor_receiver UNINITIALIZED <<'QUI'
class Unsafe(0 < int value < limit())
    int limit()
        return 5
created := Unsafe(3)
QUI
fail_case reference <<'QUI'
int next()
    return 6
x := next()
0 < int &r < 5 := &x
println("unexpected")
QUI
fail_case reference_maybe_uninitialized UNINITIALIZED <<'QUI'
bool choose()
    return false
int x
if choose()
    x = 3
0 < int &r < 5 := &x
println("unexpected")
QUI
fail_case binding <<'QUI'
int next()
    return 6
0 < x < 5 := next()
println("unexpected")
QUI
fail_case assignment <<'QUI'
int next()
    return 6
x := 2
0 < x < 5 = next()
println("unexpected")
QUI
fail_case parameter <<'QUI'
int next()
    return 6
void take(0 < int value < 5)
    println("unexpected")
take(next())
QUI
fail_case class <<'QUI'
int next()
    return 0
class Positive(nat value > 0)
    println("unexpected")
created := Positive(next())
QUI
fail_case shape <<'QUI'
int next()
    return 6
tensor<real32>{0 < nat N < 5} values := tensor.zeros<real32>([next()])
println("unexpected")
QUI
fail_case shape_parameter <<'QUI'
int next()
    return 6
void take(tensor<real32>{0 < nat N < 5} values)
    println("unexpected")
take(tensor.zeros<real32>([next()]))
QUI
fail_case descending_short_circuit <<'QUI'
int upper()
    return 0
int lower()
    println("unexpected")
    return -10
x := 1
upper() > x > lower() = 1
QUI

cat > "$TMP/range_module.qui" <<'QUI'
T identity<T: ordered>(0 < T value < 5)
    return value
QUI
run_case imported 3 <<'QUI'
import checked = "./range_module.qui"
println(checked.identity(3))
QUI

cat > "$TMP/constant_module.qui" <<'QUI'
int ceiling()
    println("checked")
    return 5
const 0 < int LIMIT < ceiling() := 3
QUI
run_case imported_const $'checked\n3\n3' <<'QUI'
import limits = "./constant_module.qui"
println(limits.LIMIT)
println(limits.LIMIT)
QUI

cp "$TMP/scalar.qui" "$TMP/formatted.qui"
"$QUIDRA" fmt "$TMP/formatted.qui"
"$QUIDRA" fmt "$TMP/formatted.qui" --check
"$QUIDRA" check "$TMP/formatted.qui"
"$QUIDRA" run "$TMP/formatted.qui" > "$TMP/formatted.out"
diff -u "$TMP/scalar.expected" "$TMP/formatted.out"
printf 'range constraint contracts passed\n'
