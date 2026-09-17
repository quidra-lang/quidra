#!/usr/bin/env bash
set -euo pipefail

QUIDRA="$(realpath "$1")"
ROOT="$(realpath "$2")"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
"$QUIDRA" llvm "$ROOT/examples/hello.qui" > "$TMP/hello.ll"
if grep -Eq 'call .*@quidra_(image|http)_' "$TMP/hello.ll"; then
    echo "unused optional runtime call leaked into Hello World" >&2
    exit 1
fi

test "$("$QUIDRA" run "$ROOT/examples/hello.qui")" = "Hello from Quidra"
test "$("$QUIDRA" run "$ROOT/examples/functions.qui")" = "120"
test "$("$QUIDRA" run "$ROOT/examples/logic.qui")" = "positive even integer"
test "$("$QUIDRA" run "$ROOT/examples/named_arguments.qui")" = "true"
test "$("$QUIDRA" run "$ROOT/examples/arrays.qui")" = $'2\n4\n6'
test "$("$QUIDRA" run "$ROOT/examples/results.qui")" = "42"
test "$("$QUIDRA" run "$ROOT/examples/options.qui")" = "none"
test "$("$QUIDRA" run "$ROOT/examples/fixed_arrays.qui")" = $'6\n0'
test "$("$QUIDRA" run "$ROOT/examples/defaults.qui")" = $'7\n20'
test "$("$QUIDRA" run "$ROOT/examples/classes.qui")" = $'1\n5\n9\n12\n7\n17\n1\n99\n2'
test "$("$QUIDRA" run "$ROOT/examples/value_objects.qui")" = $'true\ntrue\n4'
test "$("$QUIDRA" run "$ROOT/examples/strings.qui")" = $'Hello, Quidra\nn = 42; ready = true\nfirst line\nsecond line\nfirst\nsecond\nA\tB\n"quoted"\nC:\\Users\\data\\image.png\nnested value\ntrue'

test "$("$QUIDRA" run "$ROOT/examples/while.qui")" = "6"
test "$("$QUIDRA" run "$ROOT/examples/generics.qui")" = "7"
test "$("$QUIDRA" run "$ROOT/examples/bytes.qui")" = $'3\n66'
test "$("$QUIDRA" run "$ROOT/examples/modules/main.qui")" = "42"
test "$("$QUIDRA" "$ROOT/examples/cli.qui" image.jpg --count 2 --verbose)" = $'image.jpg\n2\ntrue'
