#!/usr/bin/env bash
set -euo pipefail
QUIDRA="$1"
TMP="$2"
mkdir -p "$TMP"
export QUIDRA_CACHE_DIR="$TMP/cache"

run_case() {
    local name="$1" expected="$2"
    cat > "$TMP/$name.qui"
    local actual
    actual="$("$QUIDRA" "$TMP/$name.qui")"
    [[ "$actual" == "$expected" ]] || {
        printf '%s: expected <%s>, got <%s>\n' "$name" "$expected" "$actual" >&2
        exit 1
    }
}

reject() {
    local name="$1" code="$2"
    cat > "$TMP/$name.qui"
    if "$QUIDRA" check "$TMP/$name.qui" > "$TMP/$name.out" 2> "$TMP/$name.err"; then
        printf 'Unexpected acceptance: %s\n' "$name" >&2
        exit 1
    fi
    grep -Fq "$code" "$TMP/$name.err" || { cat "$TMP/$name.err" >&2; exit 1; }
}

run_case direct $'true\n10\ntrue\n20\nfalse\n20\nfalse\n20\nfalse\n77' <<'QUI'
values := [10, 20]
it := Iterator(&values)
value := 99
println(.it(&value))
println(value)
println(.it(&value))
println(value)
println(.it(&value))
println(value)
println(.it(&value))
println(value)
int[] empty := []
empty_it := Iterator(&empty)
untouched := 77
println(.empty_it(&untouched))
println(untouched)
QUI

run_case outputs $'4\n9\nnone\n7' <<'QUI'
class Point(int x)
points := [Point(4), Point(9)]
point_it := Iterator(&points)
while .point_it(Point &point)
    println(point.x)
(int | none)[] optional := [none, 7]
optional_it := Iterator(&optional)
while .optional_it(int | none &item)
    match item
        int number
            println(number)
        none
            println("none")
QUI

run_case existing_output $'4\n9' <<'QUI'
class Point(int x)
values := [Point(4), Point(9)]
it := Iterator(&values)
Point point
while .it(&point)
    println(point.x)
QUI

run_case copies $'2\n2\n7\n1' <<'QUI'
values := [1, 2]
it := Iterator(&values)
value := 0
.it(&value)
copy := it
.it(&value)
println(value)
.copy(&value)
println(value)
class Numbers(int[] values)
    Iterator<int> iterator()
        return Iterator(&values)
numbers := Numbers([3, 4])
borrowed := numbers.iterator()
numbers.values[0] = 7
.borrowed(&value)
println(value)
int[][] rows := [[1, 2], [3]]
row_it := Iterator(&rows)
if .row_it(int[] &row)
    row[0] = 99
println(rows[0][0])
QUI

run_case borrowed $'9\n2\n1' <<'QUI'
Iterator<int> borrowed(int[] &source)
    return Iterator(&source)
values := [1, 2]
it := borrowed(&values)
values[0] = 9
value := 0
println(if .it(&value) then value else -1)
println(if .it(&value) then value else -1)
first := Iterator(&values)
second := Iterator(&values)
.first(&value)
.first(&value)
.second(&value)
println(if value == 9 then 1 else 0)
QUI

run_case factory $'10\n20\n30\n60' <<'QUI'
class Numbers(int[] values)
    Iterator<int> iterator()
        return Iterator(&values)
numbers := Numbers([10, 20, 30])
total := 0
for value := numbers[]
    println(value)
    total += value
println(total)
QUI

run_case control $'11\n13\n21\n23\n31\n33\n3\n88\n6' <<'QUI'
class Numbers(int[] values)
    Iterator<int> iterator()
        return Iterator(&values)
numbers := Numbers([1, 2, 3])
for left := numbers[]
    for right := numbers[]
        if right == 2
            continue
        println(left * 10 + right)
        if right == 3
            break
last := 88
for last = numbers[]
    last
println(last)
empty := Numbers([])
last = 88
for last = empty[]
    println("unexpected")
println(last)
total := 0
for int value := numbers[]
    total += value
println(total)
QUI

run_case patterns $'7\n11\n2' <<'QUI'
class Pairs((int, int)[] values)
    Iterator<(int, int)> iterator()
        return Iterator(&values)
pairs := Pairs([(3, 4), (5, 6)])
for (left, right) := pairs[]
    println(left + right)
count := 0
for _ = pairs[]
    count += 1
println(count)
QUI

run_case single_evaluation $'6\n1' <<'QUI'
class Numbers(int[] values)
    Iterator<int> iterator()
        return Iterator(&values)
Numbers make(int &calls)
    calls += 1
    return Numbers([1, 2, 3])
calls := 0
sum := 0
for value := make(&calls)[]
    sum += value
println(sum)
println(calls)
QUI

cat > "$TMP/module.qui" <<'QUI'
class Sequence<T>(T[] values)
    Iterator<T> iterator()
        return Iterator(&values)
QUI
run_case imported $'a\nb' <<'QUI'
import sequences = "module.qui"
sequence := sequences.Sequence<string>(["a", "b"])
for value := sequence[]
    println(value)
QUI

reject missing_arguments GENERIC_ARGUMENTS_REQUIRED <<'QUI'
class Numbers(int[] values)
    Iterator iterator()
        return Iterator(&values)
QUI

run_case explicit_constructor '8' <<'QUI'
values := [8]
Iterator<int> it := Iterator<int>(&values)
while .it(int &value)
    println(value)
QUI

reject wrong_factory ITERATOR_TYPE <<'QUI'
class Wrong()
    int iterator()
        return 1
for value := Wrong()[]
    println(value)
QUI

reject mutation_marker MUTATION_MARKER <<'QUI'
values := [1]
it := Iterator(&values)
value := 0
it(&value)
QUI

reject output_type TYPE_MISMATCH <<'QUI'
values := [1]
it := Iterator(&values)
string value := ""
.it(&value)
QUI

reject unbound_output UNBOUND_REFERENCE <<'QUI'
class Point(int x)
values := [Point(1)]
it := Iterator(&values)
Point &destination
.it(&destination)
QUI

reject partial_class_output UNINITIALIZED <<'QUI'
class Partial()
    int value
bool produce(Partial &output)
    output = Partial()
    return true
if produce(Partial &point)
    println(point.value)
QUI

reject local_escape ITERATOR_LIFETIME <<'QUI'
Iterator<int> escaped()
    values := [1, 2]
    return Iterator(&values)
QUI

reject local_alias_escape ITERATOR_LIFETIME <<'QUI'
Iterator<int> escaped()
    values := [1, 2]
    it := Iterator(&values)
    return it
QUI

reject indexed_escape ITERATOR_LIFETIME <<'QUI'
Iterator<int> escaped()
    values := [1, 2]
    items := [Iterator(&values)]
    return items[0]
QUI

reject reference_alias_escape ITERATOR_LIFETIME <<'QUI'
Iterator<int> escaped()
    values := [1, 2]
    it := Iterator(&values)
    &alias := &it
    return alias
QUI

reject member_assignment_escape ITERATOR_LIFETIME <<'QUI'
class Holder(Iterator<int> it)
Iterator<int> escaped(int[] &caller)
    values := [1, 2]
    holder := Holder(Iterator(&caller))
    holder.it = Iterator(&values)
    return holder.it
QUI

reject pattern_escape ITERATOR_LIFETIME <<'QUI'
Iterator<int> escaped()
    values := [1, 2]
    pair := (Iterator(&values), 1)
    (it, count) := pair
    return it
QUI

reject try_escape ITERATOR_LIFETIME <<'QUI'
Iterator<int> | error borrowed(int[] &values)
    return Iterator(&values)
Iterator<int> | error escaped()
    values := [1, 2]
    return try borrowed(&values)
QUI

reject iterator_name SHADOWING <<'QUI'
void f()
    Iterator := 1
QUI

for change in 'values = [3, 4]' 'values = [1, 2, 3]'; do
    cat > "$TMP/invalidated.qui" <<QUI
values := [1, 2]
it := Iterator(&values)
$change
value := 0
.it(&value)
QUI
    set +e
    "$QUIDRA" "$TMP/invalidated.qui" > "$TMP/invalidated.out" 2> "$TMP/invalidated.err"
    status=$?
    set -e
    [[ "$status" == 101 ]] || { printf 'Invalidated iterator exited %s\n' "$status" >&2; exit 1; }
    grep -Fq 'FOR_ITERATION' "$TMP/invalidated.err"
done

# The iterator expansion has its own guarded frame. Deep user loops must
# diagnose nesting rather than exhausting the compiler's process stack.
{
    cat <<'QUI'
class Numbers(int[] values)
    Iterator<int> iterator()
        return Iterator(&values)
numbers := Numbers([1])
QUI
    for level in {0..299}; do
        printf '%*sfor value_%s := numbers[]\n' "$((level * 4))" '' "$level"
    done
    printf '%*svoid\n' 1200 ''
} > "$TMP/deep_input.qui"
reject deep_iterator NESTING_DEPTH < "$TMP/deep_input.qui"

# Formatting and inspection must preserve the public constructor/operator API.
"$QUIDRA" fmt "$TMP/factory.qui"
[[ "$("$QUIDRA" "$TMP/factory.qui")" == $'10\n20\n30\n60' ]]
"$QUIDRA" inspect "$TMP/factory.qui" --no-source --no-effects > "$TMP/inspect.json"
