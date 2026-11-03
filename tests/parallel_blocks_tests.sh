#!/usr/bin/env bash
set -euo pipefail
QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/cache"
cat > "$TMP/diamond.qui" <<'QUI'
int64 x := 0
int64 a := 0
int64 b := 0
int64 total := 0
parallel
    x = 40
    a = x + 1
    b = x + 1
    total = a + b
print(total)
print(NL)
QUI
cat > "$TMP/shapes.qui" <<'QUI'
int64 extent := 2
int64 result := 0
parallel
    extent = 3
    int64[nat(extent)] values := [10, 20, 30]
    result = values[0] + values[1] + values[2]
print(result)
print(NL)
parallel
    tensor<real32>{nat N, N + 1} a := tensor.zeros<real32>([2, 3])
    tensor<real32>{N, N + 1} b := tensor.ones<real32>()
    if b.shape()[0] == 2 and b.shape()[1] == 3
        print("shape captured" + NL)
    else
        print("bad shape" + NL)
QUI
cat > "$TMP/references.qui" <<'QUI'
int64 first := 1
int64 second := 2
int64 &current := &first
parallel
    &current = &second
    current = 42
print(first)
print(NL)
print(second)
print(NL)
QUI
cat > "$TMP/failure.qui" <<'QUI'
int64 zero := 0
int64 result := 0
parallel
    result = 1 / zero
    print("independent finished" + NL)
print("unexpected success" + NL)
QUI
for name in diamond shapes references failure; do
    "$QUIDRA" build "$TMP/$name.qui" -o "$TMP/$name" > "$TMP/$name.build" 2>&1 || {
        cat "$TMP/$name.build"; exit 1;
    }
    for threads in 1 2 3 8; do
        rc=0
        QUIDRA_CPU_THREADS="$threads" "$TMP/$name" > "$TMP/$name.$threads.out" 2> "$TMP/$name.$threads.err" || rc=$?
        expected=0
        if [[ "$name" == failure ]]; then expected=101; fi
        [[ "$rc" == "$expected" ]] || { cat "$TMP/$name.$threads.err"; exit 1; }
        if [[ "$threads" != 1 ]]; then
            cmp "$TMP/$name.1.out" "$TMP/$name.$threads.out"
            cmp "$TMP/$name.1.err" "$TMP/$name.$threads.err"
        fi
    done
done
printf '82\n' > "$TMP/diamond.expected"
printf '60\nshape captured\n' > "$TMP/shapes.expected"
printf '1\n42\n' > "$TMP/references.expected"
printf 'independent finished\n' > "$TMP/failure.expected"
cmp "$TMP/diamond.expected" "$TMP/diamond.1.out"
cmp "$TMP/shapes.expected" "$TMP/shapes.1.out"
cmp "$TMP/references.expected" "$TMP/references.1.out"
cmp "$TMP/failure.expected" "$TMP/failure.1.out"
[[ ! -s "$TMP/diamond.1.err" && ! -s "$TMP/shapes.1.err" && ! -s "$TMP/references.1.err" ]]
echo 'parallel joins, captures, shapes and deterministic failures: PASS'
