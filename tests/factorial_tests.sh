#!/usr/bin/env bash
# Postfix factorial preserves integer types and fails at the operation boundary.
set -euo pipefail
QUIDRA="$1"
TMP="$2"
HELPER="$(cd "$(dirname "$0")" && pwd)/runtime_report.py"
mkdir -p "$TMP"
export QUIDRA_CACHE_DIR="$TMP/cache"

runs() {
    local name="$1" expected="$2"
    cat > "$TMP/$name.qui"
    "$QUIDRA" run "$TMP/$name.qui" > "$TMP/$name.out"
    diff -u <(printf '%s\n' "$expected") "$TMP/$name.out"
}

rejects() {
    local name="$1" code="${2:-}"
    cat > "$TMP/$name.qui"
    if "$QUIDRA" check "$TMP/$name.qui" > "$TMP/$name.check" 2>&1; then
        printf 'case %s: unexpectedly accepted\n' "$name" >&2
        return 1
    fi
    if [[ -n "$code" ]] && ! grep -q "$code" "$TMP/$name.check"; then
        cat "$TMP/$name.check" >&2
        printf 'case %s: expected compile-time %s\n' "$name" "$code" >&2
        return 1
    fi
}

fails() {
    local name="$1" code="$2" status=0
    cat > "$TMP/$name.qui"
    # Unknown values must type-check before the runtime check is exercised.
    "$QUIDRA" check "$TMP/$name.qui"
    "$QUIDRA" run "$TMP/$name.qui" > "$TMP/$name.out" 2> "$TMP/$name.err" || status=$?
    if [[ "$status" -ne 101 ]] || ! grep -q "Quidra runtime error\[$code\]" "$TMP/$name.err"; then
        cat "$TMP/$name.err" >&2
        printf 'case %s: expected runtime %s (101), got %s\n' "$name" "$code" "$status" >&2
        return 1
    fi
    diff -u <(printf 'before\n') "$TMP/$name.out"
}

# Calls, member access and indexing precede factorial; powers and unary minus
# follow it. The changing operand also exposes duplicated operand evaluation.
runs syntax $'120\n24\n6\n24\n1\ntrue\n64\n36\n-6\n720\n78\ntrue\ntrue\ntrue\n0! = 1\n1! = 1\n2! = 2\n3! = 6\n4! = 24\n5! = 120\n6! = 720' <<'QUI'
class Box(int value)
    int get()
        return value

int next(int &calls)
    calls += 1
    return calls + 3

int identity(int value)
    return value

box := Box(4)
values := [3, 4, 5]
println(identity(5)!)
println(box.value!)
println(values[0]!)
println(box.get()!)
calls := 0
evaluated := next(&calls)!
println(calls)
println(evaluated == 24)
println(2 ^ 3!)
println(3! ^ 2)
println(-3!)
println((3!)!)
println((5!).b16)
println(3 != 4)
println(3! != 5!)
println(0! == 1 and 1! == 1)
for: n <= 6
    println("{n}! = {n!}")
QUI

# Formatting must preserve parsing and behavior, including the parentheses
# required before a second factorial or a later member suffix.
"$QUIDRA" fmt "$TMP/syntax.qui"
"$QUIDRA" fmt "$TMP/syntax.qui" --check
cp "$TMP/syntax.qui" "$TMP/syntax.formatted.qui"
"$QUIDRA" fmt "$TMP/syntax.qui"
cmp "$TMP/syntax.formatted.qui" "$TMP/syntax.qui"
"$QUIDRA" run "$TMP/syntax.qui" > "$TMP/syntax.formatted.out"
diff -u "$TMP/syntax.out" "$TMP/syntax.formatted.out"

runs marked_call $'24\n1' <<'QUI'
class Counter(int calls)
    int .next()
        calls += 1
        return calls + 3

counter := Counter(0)
result := .counter.next()!
println(result)
println(counter.calls)
QUI

# Whitespace before factorial cannot turn the following binary minus into a
# second expression. Verify canonical spelling and execution after formatting.
runs spaced_factorial 119 <<'QUI'
answer:=5 ! - 1
println(answer)
QUI
"$QUIDRA" fmt "$TMP/spaced_factorial.qui"
cat > "$TMP/spaced_factorial.expected" <<'QUI'
answer := 5! - 1
println(answer)
QUI
cmp "$TMP/spaced_factorial.expected" "$TMP/spaced_factorial.qui"
"$QUIDRA" fmt "$TMP/spaced_factorial.qui" --check
"$QUIDRA" run "$TMP/spaced_factorial.qui" > "$TMP/spaced_factorial.formatted.out"
diff -u "$TMP/spaced_factorial.out" "$TMP/spaced_factorial.formatted.out"

# Extents supply an integer context to the factorial operand.
runs extents $'6\n6\n1' <<'QUI'
int[3!] values := [0, 1, 2, 3, 4, 5]
println(#values)
tensor<int32>{3!} shaped := tensor.ones<int32>([6])
println(shaped.shape()[0])
println(shaped[5].item())
QUI

# Writable references require the exact storage type, so these calls prove
# that an inferred result retained its operand type without numeric conversion.
cat > "$TMP/kinds.qui" <<'QUI'
T factorial<T>(T value)
    return value!
QUI
: > "$TMP/kinds.expected"
while read -r kind input answer; do
    cat >> "$TMP/kinds.qui" <<QUI
void require_$kind($kind &value)
    return
$kind input_$kind := $input
result_$kind := input_$kind!
require_$kind(&result_$kind)
println(result_$kind)
$kind zero_$kind := 0
$kind one_$kind := 1
println(zero_$kind! == 1 and one_$kind! == 1)
generic_$kind := factorial(input_$kind)
require_$kind(&generic_$kind)
println(generic_$kind == result_$kind)
$kind literal_$kind := $input!
println(literal_$kind == result_$kind)
QUI
    printf '%s\ntrue\ntrue\ntrue\n' "$answer" >> "$TMP/kinds.expected"
done <<'CASES'
int8 5 120
int16 7 5040
int32 12 479001600
int64 20 2432902008176640000
int 30 265252859812191058636308480000000
nat8 5 120
nat16 8 40320
nat32 12 479001600
nat64 20 2432902008176640000
nat 50 30414093201713378043612608166064768844377641568960512000000000000
CASES
cat >> "$TMP/kinds.qui" <<'QUI'
defaulted := 5!
require_int(&defaulted)
println(defaulted)
int arbitrary_signed := 50
nat arbitrary_natural := 30
println(arbitrary_signed!)
println(arbitrary_natural!)
int8 parameter_context(int8 value)
    return value
int16 return_context()
    return 7!
println(parameter_context(5!))
println(return_context())
int8[] array_context := [0!, 5!]
println("{array_context[0]},{array_context[1]}")
real32 converted := 5!
println(converted)
QUI
cat >> "$TMP/kinds.expected" <<'EXPECTED'
120
30414093201713378043612608166064768844377641568960512000000000000
265252859812191058636308480000000
120
5040
1,120
120.0
EXPECTED
"$QUIDRA" run "$TMP/kinds.qui" > "$TMP/kinds.out"
diff -u "$TMP/kinds.expected" "$TMP/kinds.out"

# Check the first overflowing factorial in each fixed-width type, both in a
# contextual literal and at runtime. Runtime input is a function parameter.
while read -r kind input; do
    rejects "literal_overflow_$kind" INTEGER_OVERFLOW <<QUI
$kind value := $input!
QUI
    fails "runtime_overflow_$kind" INTEGER_OVERFLOW <<QUI
$kind compute($kind value)
    println("before")
    return value!
println(compute($input))
println("after")
QUI
done <<'CASES'
int8 6
int16 8
int32 13
int64 21
nat8 6
nat16 9
nat32 13
nat64 21
CASES

# Large unsigned operands must be range-checked without narrowing to signed
# storage first, including when a constant cast supplies the operand type.
rejects maximum_nat64_literal INTEGER_OVERFLOW <<'QUI'
nat64 value := 18446744073709551615!
QUI
rejects maximum_nat64_cast INTEGER_OVERFLOW <<'QUI'
nat64 value := nat64(18446744073709551615)!
QUI

fails wider_destination INTEGER_OVERFLOW <<'QUI'
int compute(int8 value)
    println("before")
    int result := value!
    return result
println(compute(6))
println("after")
QUI

rejects literal_negative FACTORIAL_DOMAIN <<'QUI'
value := (-3)!
QUI
rejects literal_expression_negative FACTORIAL_DOMAIN <<'QUI'
value := (2 - 5)!
QUI
rejects contextual_expression_overflow INTEGER_OVERFLOW <<'QUI'
int8 value := (3 + 3)!
QUI
rejects parameter_context_overflow INTEGER_OVERFLOW <<'QUI'
void consume(int8 value)
    return
consume(6!)
QUI
rejects return_context_overflow INTEGER_OVERFLOW <<'QUI'
int8 compute()
    return 6!
QUI

for kind in int8 int16 int32 int64 int; do
    fails "runtime_negative_$kind" FACTORIAL_DOMAIN <<QUI
$kind compute($kind value)
    println("before")
    return value!
println(compute(-3))
println("after")
QUI
done

# Generic specialization and nested grouping retain the factorial token's
# source site even when the grouping starts and ends on different lines.
fails runtime_negative_wrapped_generic FACTORIAL_DOMAIN <<'QUI'
T wrapped_factorial<T>(T value)
    println("before")
    return ((
        value!
    ))
println(wrapped_factorial(-3))
println("after")
QUI
python3 "$HELPER" "$TMP/runtime_negative_wrapped_generic.err" \
    code=FACTORIAL_DOMAIN line=4 column=14

# Factorial terminates an unparenthesized postfix chain.
for suffix in '!!' '!.b16' '!()' '![0]'; do
    rejects invalid_postfix <<QUI
int value := 3
println(value$suffix)
QUI
done

# No implicit numeric conversion or container lifting makes an operand valid.
for declaration in \
    'bool value := true' \
    'real16 value := 3.0' \
    'real16b value := 3.0' \
    'real32 value := 3.0' \
    'real64 value := 3.0' \
    'real value := 3.0' \
    'com16 value := 3.0 + 0.0i' \
    'com16b value := 3.0 + 0.0i' \
    'com32 value := 3.0 + 0.0i' \
    'com64 value := 3.0 + 0.0i' \
    'com value := 3.0 + 0.0i' \
    'bin value := bin("01")' \
    'string value := "3"' \
    'int[] value := [1, 2, 3]' \
    'tensor<int32> value := tensor.ones<int32>([2])'; do
    rejects invalid_operand TYPE_MISMATCH <<QUI
$declaration
println(value!)
QUI
done

echo "factorial tests passed"
