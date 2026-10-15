#!/usr/bin/env bash
set -euo pipefail
QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/cache"

cat > "$TMP/ranges.qui" <<'QUI'
void show(int[] values)
    for int value = values[]
        print("{value},")
    print(NL)
show(:3)
show(:<3)
show(3:0)
show(3:>0)
show(3>:0:-2)
show(0<:5:2)
show(0<:<5:2)
show(-3:0)
show(0:-3)
show(-3:-1)
show(3:3)
show(3:3:-1)
show(3:<3)
show(3:>3)
show(3>:3:-1)
show(3<:3:1)
show(3<:0)
show(0:>3)
show((:3 ^ 2))
show((-3:0 ^ 2))
show((0:-3 ^ 2))
show((3:0:-1 ^ 2))
int start = 3
int stop = 0
show(start:stop)
show(stop:start)
show(start:>start)
show(start>:start:-1)
show(start:start:-1)
show(start<:stop)
show(stop:>start)
show(0:1:18446744073709551616)
show(1:0:-18446744073709551616)
int huge = 18446744073709551616
show(huge:(huge + 2))
show((huge + 2):huge)
QUI
cat > "$TMP/ranges.expected" <<'OUT'
0,1,2,3,
0,1,2,
3,2,1,0,
3,2,1,
1,
2,4,
2,4,
-3,-2,-1,0,
0,-1,-2,-3,
-3,-2,-1,
3,
3,






0,1,4,9,
9,4,1,0,
0,1,4,9,
9,4,1,0,
3,2,1,0,
0,1,2,3,


3,


0,
1,
18446744073709551616,18446744073709551617,18446744073709551618,
18446744073709551618,18446744073709551617,18446744073709551616,
OUT
"$QUIDRA" "$TMP/ranges.qui" > "$TMP/ranges.actual"
diff -u "$TMP/ranges.expected" "$TMP/ranges.actual"

# A typed endpoint fixes the array element type; it is never widened.
for kind in int nat int8 int16 int32 int64 nat8 nat16 nat32 nat64; do
    cat > "$TMP/family.qui" <<QUI
void consume($kind[] values)
    for $kind value = values[]
        print("{value},")
    print(NL)
$kind start = 1
$kind stop = 3
auto values = start:stop
consume(values)
consume(stop:start)
consume(:stop)
QUI
    printf '1,2,3,\n3,2,1,\n0,1,2,3,\n' > "$TMP/family.expected"
    "$QUIDRA" "$TMP/family.qui" > "$TMP/family.actual"
    diff -u "$TMP/family.expected" "$TMP/family.actual"
done

for range in '0:3:0' '0:3:-1' '3:0:1' '3<:0:-1' '0>:3:1' \
             '3:>3:1' '3:<3:-1' '3<:0:1' '0:>3:-1' \
             '18446744073709551616<:0:1'; do
    printf 'int[] values = %s\n' "$range" > "$TMP/invalid.qui"
    if "$QUIDRA" check "$TMP/invalid.qui" > "$TMP/out" 2> "$TMP/err"; then
        printf 'invalid constant range accepted: %s\n' "$range" >&2
        exit 1
    fi
    grep -Fq 'SLICE_STEP' "$TMP/err"
done

for range in 'start<:stop:-1' 'stop>:start:1' 'start:<stop:-1' \
             'stop:>start:1' 'start:stop:1' 'stop:start:-1' \
             'start:>start:step' 'start:<start:negative_step' 'start:stop:zero'; do
    cat > "$TMP/dynamic.qui" <<QUI
int start = 3
int stop = 0
int step = 1
int negative_step = -1
int zero = 0
print("before" + NL)
auto values = $range
print("after" + NL)
QUI
    status=0
    "$QUIDRA" "$TMP/dynamic.qui" > "$TMP/out" 2> "$TMP/err" || status=$?
    if [[ "$status" != 101 || "$(cat "$TMP/out")" != before ]]; then
        printf 'invalid dynamic range did not fail before use: %s\n' "$range" >&2
        cat "$TMP/err" >&2
        exit 1
    fi
    grep -Eq 'runtime error\[(SLICE_STEP|FOR_STEP)\]' "$TMP/err"
done

for source in 'int8 start = 1\nint16 stop = 3\nauto values = start:stop' \
              'real start = 1.0\nreal stop = 3.0\nauto values = start:stop' \
              'int8 start = 1\nint8 stop = 3\nint[] values = start:stop'; do
    printf '%b\n' "$source" > "$TMP/types.qui"
    if "$QUIDRA" check "$TMP/types.qui" > "$TMP/out" 2> "$TMP/err"; then
        printf 'invalid range operand types accepted\n' >&2
        exit 1
    fi
    grep -Fq 'TYPE_MISMATCH' "$TMP/err"
done
printf 'bounded range contracts passed\n'
