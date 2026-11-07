#!/usr/bin/env bash
set -euo pipefail
QUIDRA="$1"
TMP="$2"
mkdir -p "$TMP"
export QUIDRA_CACHE_DIR="$TMP/cache"

cat > "$TMP/length.qui" <<'QUI'
class Group(string[] members)
    nat len()
        return #members

class Defaults()
    nat len(nat extra := 0)
        return 4 + extra

class OtherResult()
    string len()
        return "custom"

class Box<T>(T[] values)
    nat len()
        return #values

int[] make(int &calls)
    calls += 1
    return [1, 2, 3]

class Fallible()
    nat | error len()
        return 7

int[] | error make_fallible(int &calls)
    calls += 1
    return [4, 5]

int hidden_calls := 0
int[] .make_hidden()
    hidden_calls += 1
    return [6]

class Marked(int count)
    nat .len()
        count += 1
        return nat(count)

values := [10, 20, 30]
matrix := [[1, 2, 3], [4, 5, 6]]
int[5] uninitialized
println("{#values},{#matrix},{#matrix[0]},{#uninitialized}")
println("{#"é日"},{#""},{#bin.fill(9, 0)}")
println(#Group(["Aki", "Yui"]))
println(#Defaults())
println(#OtherResult())
println(#Box<int>(values))
int evaluations := 0
println(#make(&evaluations))
println(evaluations)
println(#values ^ 2)
println(#(0:<#values))
println(#(#values>:0))
println(#make_fallible(&evaluations))
println(evaluations)
println(.#make_hidden())
println(hidden_calls)
marked := Marked(0)
println(.#marked)
println(marked.count)
fallible := Fallible()
nat | error length := #fallible
match length
    nat amount
        println(amount)
    error failure
        println(failure)
QUI

expected=$'3,2,3,5\n2,0,9\n2\n4\ncustom\n3\n3\n1\n9\n3\n3\n2\n2\n1\n1\n1\n1\n7'
actual="$("$QUIDRA" "$TMP/length.qui")"
[[ "$actual" == "$expected" ]] || { printf 'Unexpected length output:\n%s\n' "$actual" >&2; exit 1; }
"$QUIDRA" fmt "$TMP/length.qui"
"$QUIDRA" fmt "$TMP/length.qui" --check
[[ "$("$QUIDRA" "$TMP/length.qui")" == "$expected" ]]

# A failed custom method retains the same typed failure and fail-fast boundary.
cat > "$TMP/failure.qui" <<'QUI'
class Fails()
    nat | error len()
        return error("length failed")
value := Fails()
println(#value)
QUI
if "$QUIDRA" "$TMP/failure.qui" > "$TMP/failure.out" 2> "$TMP/failure.err"; then
    echo "length shorthand swallowed a custom len error" >&2
    exit 1
fi
grep -q 'length failed' "$TMP/failure.err"

echo "length shorthand tests passed"
