#!/usr/bin/env bash
set -euo pipefail

QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/cache"
export QUIDRA_PACKAGE_PATH="$TMP/packages"
mkdir -p "$TMP/packages/math" "$TMP/packages/other" "$TMP/packages/reexport"
printf 'name = math\nversion = 0.1.0\n' > "$TMP/packages/math/quidra.package"
printf 'name = other\nversion = 0.1.0\n' > "$TMP/packages/other/quidra.package"
printf 'name = reexport\nversion = 0.1.0\n' > "$TMP/packages/reexport/quidra.package"
printf 'public import math\n' > "$TMP/packages/reexport/main.qui"
# A deliberately distinguishable package implementation proves the operator
# selects Math's ordinary overload rather than a Core numeric kernel.
cat > "$TMP/packages/math/main.qui" <<'QUI'
int matmul(extend int left, int right)
    return left * 10 + right
QUI
cat > "$TMP/packages/other/main.qui" <<'QUI'
import math
int matmul(extend int left, int right)
    return 999
int product()
    return 2 @ 3
QUI

expect() {
    local name="$1" expected="$2"
    cat > "$TMP/$name.qui"
    local actual
    actual="$("$QUIDRA" "$TMP/$name.qui")"
    if [[ "$actual" != "$expected" ]]; then
        printf '%s: expected %s, got %s\n' "$name" "$expected" "$actual" >&2
        exit 1
    fi
}
reject() {
    local name="$1" code="$2"
    cat > "$TMP/$name.qui"
    if "$QUIDRA" check "$TMP/$name.qui" --json > "$TMP/$name.out" 2>&1; then
        printf '%s unexpectedly accepted\n' "$name" >&2
        exit 1
    fi
    if ! grep -Fq "\"$code\"" "$TMP/$name.out"; then
        cat "$TMP/$name.out" >&2
        exit 1
    fi
}

expect alias 'true' <<'QUI'
import linear = math
println(2 @ 3 == linear.matmul(2, 3) and 2.matmul(3) == 23)
QUI
# Extension methods can be ambiguous with another package; the operator
# specifically calls Math. Keep its result independent of that ambiguity.
expect first-party 'true' <<'QUI'
import linear = math
import other
println(2 @ 3 == 23 and other.product() == 23)
QUI
expect public-reexport 'true' <<'QUI'
import reexport
println(2 @ 3 == 23)
QUI
expect precedence 'true' <<'QUI'
import math
println(2 @ 3 * 4 @ 5 == 925 and 2 @ (3 @ 4) == 54)
QUI
expect evaluation-order 'true' <<'QUI'
import math
class Counter(int value)
    int .next()
        value += 1
        return value
counter := Counter(0)
result := .counter.next() @ .counter.next()
println(result == 12 and counter.value == 2)
QUI
reject missing-import MATMUL_SCOPE <<'QUI'
int matmul(extend int left, int right)
    return left + right
print(2 @ 3)
QUI
reject private-transitive-import MATMUL_SCOPE <<'QUI'
import other
print(2 @ 3)
QUI
cat > "$TMP/fake.qui" <<'QUI'
int matmul(extend int left, int right)
    return left + right
QUI
reject local-alias MATMUL_SCOPE <<'QUI'
import math = "./fake.qui"
print(2 @ 3)
QUI
reject unsupported-operands TYPE_MISMATCH <<'QUI'
import math
print("a" @ "b")
QUI
printf 'matrix multiplication operator tests passed\n'
