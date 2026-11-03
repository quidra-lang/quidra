#!/usr/bin/env bash
set -euo pipefail
QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/cache"
cat > "$TMP/overlap.qui" <<'QUI'
class Holder<T> this(local T &target)
    T &value

    &this.value = &target
    void .replace(T next)
        this.value = next
int .observe(int &value, Holder<int> holder)
    int first := value
    .holder.replace(17)
    return first * 100 + value
int .snapshot(int[] &values, Holder<int[]> holder)
    int total := 0
    for int value := values[nat index]
        if index == 0
            .holder.replace([1, 77, 3])
        total += value
    return total
int .read_copy(const int[] values, Holder<int[]> holder)
    .holder.replace([8, 9])
    return values[1]
int number := 4
Holder<int> scalar := Holder<int>(&number)
print("{.observe(&number, scalar)} {number}{NL}")
int[] values := [1, 2, 3]
Holder<int[]> array_holder := Holder<int[]>(&values)
print("{.snapshot(&values, array_holder)} {values[1]}{NL}")
print("{.read_copy(values, array_holder)} {values[1]}{NL}")
QUI
expected=$'417 17\n6 77\n77 9'
[[ "$("$QUIDRA" run "$TMP/overlap.qui")" == "$expected" ]]
"$QUIDRA" build "$TMP/overlap.qui" -o "$TMP/program"
[[ "$("$TMP/program")" == "$expected" ]]
cat > "$TMP/live.qui" <<'QUI'
class Holder this(local int[] &source)
    int[] &values

    &this.values = &source
    void .shorten()
        this.values = [7]
void .iterate(int[] &values, Holder holder)
    for int &value := &values[nat index]
        if index == 0
            .holder.shorten()
        print(value)
int[] values := [1, 2, 3]
Holder holder := Holder(&values)
.iterate(&values, holder)
QUI
if "$QUIDRA" run "$TMP/live.qui" > "$TMP/out" 2> "$TMP/err"; then
    echo 'Expected FOR_ITERATION after replacement through a stored reference' >&2
    exit 1
fi
grep -Fq FOR_ITERATION "$TMP/err"
cat > "$TMP/unbound-equality.qui" <<'QUI'
class Holder()
    int &value
Holder first := Holder()
Holder second := Holder()
print(first == second)
QUI
if "$QUIDRA" check "$TMP/unbound-equality.qui" > "$TMP/out" 2> "$TMP/err"; then
    echo 'Expected UNBOUND_REFERENCE for a never-bound field in equality' >&2
    exit 1
fi
grep -Fq UNBOUND_REFERENCE "$TMP/err"
