#!/usr/bin/env bash
set -euo pipefail
QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/cache"
cat > "$TMP/diamond.qui" <<'QUI'
int32 seed := 7
int32 left
int32 right
int32 result
parallel
    left = seed * 2
    right = seed + 3
    result = left + right
print(result)
print(NL)
QUI
cat > "$TMP/nested.qui" <<'QUI'
int32 compute(int32 value)
    int32 first
    int32 second
    parallel
        first = value * 2
        second = value + 3
    return first + second
int32 first
int32 second
parallel
    first = compute(7)
    second = compute(8)
print("{first} {second}{NL}")
QUI
cat > "$TMP/representative.qui" <<'QUI'
void first()
    time.sleep(time.seconds(0.01))
    int[] values := [1, 2, 3]
    int value := values[9]
void second()
    int[] values := [1, 2, 3]
    int value := values[7]
print("before" + NL)
parallel
    first()
    second()
QUI
cat > "$TMP/exit.qui" <<'QUI'
void first()
    time.sleep(time.seconds(0.01))
    process.exit(3)
void second()
    process.exit(7)
parallel
    first()
    second()
QUI
for name in diamond nested representative exit; do
    "$QUIDRA" build "$TMP/$name.qui" -o "$TMP/$name" >"$TMP/build.out" 2>"$TMP/build.err"
    case "$name" in representative) expected=101;; exit) expected=3;; *) expected=0;; esac
    for count in 1 2 3 8; do
        set +e
        QUIDRA_CPU_THREADS="$count" "$TMP/$name" >"$TMP/$name.$count.out" 2>"$TMP/$name.$count.err"
        status=$?
        set -e
        [[ "$status" -eq "$expected" ]]
        if [[ "$count" != 1 ]]; then
            cmp "$TMP/$name.1.out" "$TMP/$name.$count.out"
            cmp "$TMP/$name.1.err" "$TMP/$name.$count.err"
        fi
    done
    case "$name" in
        diamond) [[ "$(cat "$TMP/$name.1.out")" == 24 ]];;
        nested) [[ "$(cat "$TMP/$name.1.out")" == '24 27' ]];;
        representative) grep -q 'index 9 out of bounds' "$TMP/$name.1.err";;
    esac
done
echo 'parallel determinism tests passed'
