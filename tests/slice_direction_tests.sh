#!/usr/bin/env bash
# Endpoint direction, its direct-loop lowering, and explicit-step safety.
set -euo pipefail
QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/cache"

cat > "$TMP/direction.qui" <<'QUI'
void show(int[] xs)
    for auto x = xs[]
        print(x)
    print(NL)
int[] a = [0, 1, 2, 3, 4]
show(a[4:0])
show(a[4:1])
show(a[1:4])
show(a[2:])
show(a[:4])
show(a[:])
show(a[::-1])
show(a[1:4:2])
show(a[4:1:-2])
int start = 4
int finish = 0
show(a[start:finish])
for auto x = a[start:finish][]
    print(x)
print(NL)
for _ = a[start:finish][]
    print("x")
print(NL)
start = 0
finish = 4
show(a[start:finish])
show(a[start:start])
int[] copied = a[4:0]
copied[0] = 9
show(a)
a[4:0] = [5, 6, 7, 8, 9]
show(a)
string word = "aé😀b"
print(word[3:0] + NL)
start = 3
finish = 0
print(word[start:finish] + NL)
auto bits = bin.parse("01101")
print(bits[4:0].string() + NL)
start = 4
print(bits[start:finish].string() + NL)
tensor<real32> t = tensor.zeros<real32>([5])
print(t[4:<1:1].shape()[0])
print(NL)
QUI
cat > "$TMP/expected" <<'TXT'
43210
4321
1234
234
01234
01234
43210
13
42
43210
43210
xxxxx
01234
0
01234
98765
b😀éa
b😀éa
10110
10110
0
TXT
"$QUIDRA" "$TMP/direction.qui" > "$TMP/actual"
diff -u "$TMP/expected" "$TMP/actual"

for slice in 'start:finish:1' 'start:<finish:1' 'finish:start:-1' 'finish:>start:-1'; do
    cat > "$TMP/fixed.qui" <<QUI
int[] a = [0, 1, 2, 3, 4]
int start = 4
int finish = 0
print("before" + NL)
int[] selected = a[$slice]
print(len(selected))
QUI
    status=0
    "$QUIDRA" "$TMP/fixed.qui" > "$TMP/out" 2> "$TMP/err" || status=$?
    [[ "$status" == 101 && "$(cat "$TMP/out")" == before ]]
    grep -Fq 'runtime error[INDEX_BOUNDS]' "$TMP/err"
done

for slice in '4:0:1' '4:<0:1' '0:4:-1' '0:>4:-1'; do
    cat > "$TMP/constant.qui" <<QUI
int[5] a = [0, 1, 2, 3, 4]
int[] selected = a[$slice]
QUI
    if "$QUIDRA" check "$TMP/constant.qui" > "$TMP/out" 2> "$TMP/err"; then
        printf 'invalid fixed direction accepted: %s\n' "$slice" >&2
        exit 1
    fi
done
printf 'slice direction passed\n'
