#!/usr/bin/env bash
set -euo pipefail
QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/cache"
expect_failure() {
    local source="$1" code="$2" message="${3:-}"
    if "$QUIDRA" check "$TMP/$source.qui" > "$TMP/out" 2> "$TMP/err"; then
        printf 'Expected %s in %s\n' "$code" "$source" >&2
        exit 1
    fi
    grep -Fq "$code" "$TMP/err"
    if [[ -n "$message" ]]; then grep -Fq "$message" "$TMP/err"; fi
}
cat > "$TMP/shared.qui" <<'QUI'
class Holder this(local int &source)
    private int &total

    &this.total = &source
    int read()
        return this.total
    void .add(int amount)
        this.total += amount
    void .bind(int &source)
        &this.total = &source
    bool same(int &source)
        &this.total == &source
        &this.total
        return &this.total == &source
int value := 4
int next := 11
Holder first := Holder(&value)
Holder second := first
.second.add(3)
print("{first.read()} {second.read()} {value}{NL}")
.second.bind(&next)
.second.add(2)
print("{first.read()} {second.read()} {value} {next}{NL}")
print("{first.same(&value)} {second.same(&next)}{NL}")
QUI
[[ "$("$QUIDRA" run "$TMP/shared.qui")" == $'7 7 7\n7 13 7 13\ntrue true' ]]
cat > "$TMP/value-parameter.qui" <<'QUI'
class Holder this(local int &source)
    int &value

    &this.value = &source
    void .replace(int next)
        this.value = next
int read(Holder holder)
    return holder.value
void .write(Holder holder)
    .holder.replace(17)
int source := 1
Holder holder := Holder(&source)
print("{read(holder)} ")
.write(holder)
print("{read(holder)} {source}{NL}")
QUI
[[ "$("$QUIDRA" run "$TMP/value-parameter.qui")" == '1 17 17' ]]
cat > "$TMP/unbound-parameter.qui" <<'QUI'
class Holder()
    int &value
int read(Holder holder)
    return holder.value
Holder holder := Holder()
print(read(holder))
QUI
"$QUIDRA" check "$TMP/unbound-parameter.qui"
set +e
"$QUIDRA" run "$TMP/unbound-parameter.qui" > "$TMP/out" 2> "$TMP/err"
status=$?
set -e
[[ "$status" == 101 ]]
grep -Fq 'UNBOUND_REFERENCE' "$TMP/err"
cat > "$TMP/escaped.qui" <<'QUI'
class Box<T> this(local T &source)
    T &item

    &this.item = &source
Box<int> make()
    int value := 5
    return Box<int>(&value)
Box<int> escaped := make()
Box<int>[] copies := [escaped, escaped]
copies[0].item = 9
print("{escaped.item} {copies[1].item}{NL}")
QUI
[[ "$("$QUIDRA" run "$TMP/escaped.qui")" == '9 9' ]]
cat > "$TMP/detached.qui" <<'QUI'
class Inner this(local int initial)
    int value

    this.value = initial
class Model this(local int initial)
    Inner inner

    this.inner = Inner(initial)
class Holder this(local int &source)
    int &total

    &this.total = &source
Model model := Model(4)
Holder field := Holder(&model.inner.value)
model.inner = Inner(8)
field.total = 6
print("{model.inner.value} {field.total}{NL}")
QUI
[[ "$("$QUIDRA" run "$TMP/detached.qui")" == '8 6' ]]
cat > "$TMP/default.qui" <<'QUI'
class Holder()
    int &total := &source
QUI
expect_failure default WRITE_CAPABILITY "A writable reference requires storage with write authority."
cat > "$TMP/const.qui" <<'QUI'
class Holder()
    const int &total
QUI
expect_failure const CONST_REFERENCE "Const references do not exist; write 'const T name' for a read-only value."
cat > "$TMP/cli.qui" <<'QUI'
cli options
    int &count = argument()
QUI
expect_failure cli REFERENCE_BINDING "A cli field holds a value from the command line; remove '&'."
cat > "$TMP/result.qui" <<'QUI'
int & f()
    return 1
QUI
expect_failure result PARSE_ERROR "A function cannot return a reference; return a value, or keep the reference in a field."
for declaration in 'Box<int&> value' 'int&[] values' '(int&|real64) value' 'func<int&(int)> operation' 'func<int(int&)> operation'; do
    printf '%s\n' "$declaration" > "$TMP/nested.qui"
    expect_failure nested PARSE_ERROR "'&' is not part of a type: references are local variables and class fields, 'T &name'."
done
printf '(int&, string) pair\n' > "$TMP/tuple.qui"
expect_failure tuple TUPLE 'Tuple types hold values, never references.'
cat > "$TMP/outside.qui" <<'QUI'
class Holder()
    int &total
int value := 1
Holder holder := Holder()
&holder.total = &value
QUI
expect_failure outside REFERENCE_BINDING
cat > "$TMP/unbound.qui" <<'QUI'
class Holder()
    int &total
Holder holder := Holder()
print(holder.total)
QUI
expect_failure unbound UNBOUND_REFERENCE
cat > "$TMP/moving.qui" <<'QUI'
class Holder this(local int &source)
    int &total

    &this.total = &source
int[] values := [1, 2]
Holder holder := Holder(&values[0])
QUI
expect_failure moving REFERENCE_TARGET
cat > "$TMP/self.qui" <<'QUI'
class Model this()
    int value := 1
    int &alias
    void .link()
        &this.alias = &this.value
QUI
expect_failure self REFERENCE_TARGET
cat > "$TMP/parallel-fresh.qui" <<'QUI'
class Holder this(local int &source)
    int &total

    &this.total = &source
    void .add(int amount)
        this.total += amount
int left := 1
int right := 2
Holder first := Holder(&left)
Holder second := Holder(&right)
parallel
    .first.add(3)
    .second.add(4)
print("{left} {right}{NL}")
QUI
[[ "$("$QUIDRA" run "$TMP/parallel-fresh.qui")" == '4 6' ]]
cat > "$TMP/parallel-unknown.qui" <<'QUI'
class Holder this(local int &source)
    int &total

    &this.total = &source
    void .add(int amount)
        this.total += amount
void .conflict(Holder holder, int &value)
    parallel
        .holder.add(1)
        value = 3
QUI
expect_failure parallel-unknown PARALLEL_CONFLICT
