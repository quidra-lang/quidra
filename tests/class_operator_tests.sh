#!/usr/bin/env bash
set -euo pipefail
QUIDRA="$1"
TMP="$2"
mkdir -p "$TMP"
export QUIDRA_CACHE_DIR="$TMP/cache"

cat > "$TMP/operators.qui" <<'QUI'
class Values(int[] data)
    int operator()()
        return data.sum()
    int operator[](int index)
        return data[index]
    int operator__()
        return 999

class Lookup()
    string operator[](string key)
        return "key=" + key

class Matrix(int[][] rows)
    int operator[](int row, int col := 1)
        return rows[row][col]

class Box<T>(T[] data)
    T operator[](int index)
        return data[index]

class Addition(int offset)
    int operator()(int value)
        return offset + value

class Counter(int value)
    int .operator()(int extra := 1)
        value += extra
        return value

class Tracked(int count)
    int .operator[](int index)
        count += 1
        return index + count

class Fallible()
    int | error operator[](int index)
        if index < 0
            return error("negative key")
        return index + 10

class Named()
    int operator(int value)
        return value

class Plain(int value)
    int operator()
        return value

class Generic()
    T operator()<T>(T key)
        return key
    T operator[]<T>(T key)
        return key

class Copies(int[][] data)
    int[] operator[](int index)
        return data[index]

Values make_values(int &evaluations)
    evaluations += 1
    return Values([4, 5, 6])

int note(int &order, int digit)
    order = order * 10 + digit
    return 0

Matrix make_matrix(int &order)
    order = order * 10 + 1
    return Matrix([[7, 8], [9, 10]])

values := Values([10, 20, 30])
println(values())
println(values[-1])
println(values.operator__())
println(Lookup()["name"])
matrix := Matrix([[1, 2], [3, 4]])
println(matrix[1, 0])
println(matrix[0])
println(Box<string>(["a", "b"])[1])
add := Addition(7)
println(add(5))
counter := Counter(0)
println(.counter())
println(.counter(4))
tracked := Tracked(0)
println(.tracked[5])
println(tracked.count)
int evaluations := 0
println(make_values(&evaluations)[1])
println(evaluations)
int order := 0
println(make_matrix(&order)[note(&order, 2), note(&order, 3)])
println(order)
fallible := Fallible()
int | error result := fallible[2]
match result
    int number
        println(number)
    error failure
        println(failure)
int | error failed := fallible[-1]
match failed
    int unexpected
        println(unexpected)
    error problem
        println("error")
copies := Copies([[1, 2]])
copy := copies[0]
copy[0] = 99
println(copies[0][0])
println([values][0][1])
named := Named()
println(named.operator(8))
println(Generic()["generic"])
generic := Generic()
println(generic(11))
println(generic[12])
println(#Box<string>(["abc"])[0])
println(values.operator()())
println(values.operator[](1))
println(generic.operator[]<int>(13))
plain := Plain(3)
println(plain.operator() < 4)
QUI

expected=$'60\n30\n999\nkey=name\n3\n2\nb\n12\n1\n5\n6\n1\n5\n1\n7\n123\n12\nerror\n1\n20\n8\ngeneric\n11\n12\n3\n60\n20\n13\ntrue'
actual="$("$QUIDRA" "$TMP/operators.qui")"
[[ "$actual" == "$expected" ]] || { printf 'Unexpected operator output:\n%s\n' "$actual" >&2; exit 1; }

# Formatting must preserve the distinction between the operator name and args.
"$QUIDRA" fmt "$TMP/operators.qui"
[[ "$("$QUIDRA" "$TMP/operators.qui")" == "$expected" ]]
"$QUIDRA" inspect "$TMP/operators.qui" --no-source --no-effects --kind call > "$TMP/inspect.json"

cat > "$TMP/module.qui" <<'QUI'
class Table<T>(T[] data)
    T operator[](int index)
        return data[index]
QUI
cat > "$TMP/imported.qui" <<'QUI'
import tables = "module.qui"
table := tables.Table<int>([5, 8])
println(table[1])
QUI
[[ "$("$QUIDRA" "$TMP/imported.qui")" == '8' ]]

reject() {
    local name="$1"
    cat > "$TMP/$name.qui"
    if "$QUIDRA" check "$TMP/$name.qui" > "$TMP/$name.out" 2> "$TMP/$name.err"; then
        printf 'Unexpected acceptance: %s\n' "$name" >&2
        exit 1
    fi
}

reject old-callable <<'QUI'
class Old()
    int operator()
        return 1
old := Old()
println(old())
QUI

reject assignment <<'QUI'
class Values(int[] data)
    int operator[](int index)
        return data[index]
values := Values([1])
values[0] = 2
QUI

reject compound-assignment <<'QUI'
class Values(int[] data)
    int operator[](int index)
        return data[index]
values := Values([1])
values[0] += 2
QUI

reject reference <<'QUI'
class Values(int[] data)
    int operator[](int index)
        return data[index]
values := Values([1])
int &item := &values[0]
QUI

reject returned-member-assignment <<'QUI'
class Point(int x)
class Points(Point[] data)
    Point operator[](int index)
        return data[index]
points := Points([Point(1)])
points[0].x = 2
QUI

reject marked-builtin-index <<'QUI'
values := [1]
println(.values[0])
QUI

reject slice <<'QUI'
class Values(int[] data)
    int operator[](int index)
        return data[index]
values := Values([1, 2])
copy := values[0:1]
QUI

reject missing-mutation-marker <<'QUI'
class Values(int count)
    int .operator[](int index)
        count += 1
        return index
values := Values(0)
println(values[0])
QUI

reject private-index <<'QUI'
class Values(int[] data)
    private int operator[](int index)
        return data[index]
values := Values([1])
println(values[0])
QUI

reject duplicate-index <<'QUI'
class Values()
    int operator[](int index)
        return index
    string operator[](string key)
        return key
QUI

reject argument-type <<'QUI'
class Values()
    int operator[](int index)
        return index
values := Values()
println(values["bad"])
QUI

reject empty-index-operator <<'QUI'
class Values()
    int operator[]()
        return 0
QUI

reject free-call-operator <<'QUI'
int operator()()
    return 1
QUI

reject free-index-operator <<'QUI'
int operator[](int index)
    return index
QUI

printf 'class operator tests passed\n'
