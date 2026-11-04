#!/usr/bin/env bash
set -euo pipefail
QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/cache"
cat > "$TMP/values.qui" <<'QUI'
int identity(local int value)
    local int copy := value
    return copy
local int amount := 7
local int sum := 0
if true
    local int next := identity(amount)
    for: int index < 2
        local int value := next + index
        sum = sum + value
print(sum)
QUI
[[ "$("$QUIDRA" "$TMP/values.qui")" == '15' ]]
check_rejected() {
    local source="$1"
    printf '%s\n' "$source" > "$TMP/reject.qui"
    if "$QUIDRA" check "$TMP/reject.qui" --json > "$TMP/reject.json"; then exit 1; fi
    python3 - "$TMP/reject.json" <<'PYJSON'
import json,sys
assert any(d['code']=='UNKNOWN_NAME' for d in json.load(open(sys.argv[1]))['diagnostics'])
PYJSON
}
check_rejected $'print(value)\nlocal int value := 1'
check_rejected $'if true\n    local int value := 1\nprint(value)'
check_rejected $'if true\n    local int value := 1\nelse\n    print(value)'
check_rejected $'local int outer := 1\nint read()\n    return outer'
check_rejected $'local int outer := 1\nclass Box()\n    int read()\n        return outer'
check_rejected $'for: int index < 2\n    local int value := index\nprint(value)'
cat > "$TMP/stored.qui" <<'QUI'
class Box(local int initial)
    int amount := initial
    int read()
        return amount
local int amount := 7
Box value := Box(amount)
print(value.read())
QUI
[[ "$("$QUIDRA" "$TMP/stored.qui")" == '7' ]]
cat > "$TMP/constants.qui" <<'QUI'
local const int hidden := 7
const int exposed := 8
QUI
cat > "$TMP/import.qui" <<'QUI'
import values = "./constants.qui"
int value := values.hidden
QUI
if "$QUIDRA" check "$TMP/import.qui" --json > "$TMP/import.json"; then exit 1; fi
python3 - "$TMP/import.json" <<'PYJSON'
import json,sys
value=json.load(open(sys.argv[1]))
assert value['diagnostics'] and any('hidden' in d['message'] for d in value['diagnostics'])
assert any(d['code']=='UNKNOWN_MODULE_MEMBER' for d in value['diagnostics'])
PYJSON
cat > "$TMP/export.qui" <<'QUI'
import values = "./constants.qui"
int value := values.exposed
print(value)
QUI
[[ "$("$QUIDRA" "$TMP/export.qui")" == '8' ]]
cp "$TMP/values.qui" "$TMP/values-formatted.qui"
"$QUIDRA" fmt "$TMP/values-formatted.qui"
[[ "$("$QUIDRA" "$TMP/values-formatted.qui")" == '15' ]]
