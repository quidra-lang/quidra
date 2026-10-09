#!/usr/bin/env bash
# Differential runs of the internal lowering switches (quidra/lowering.hpp):
# each program is built with and without a switch that turns a narrowing
# predicate of the lowering off, and both builds must print the same output
# on stdout and stderr and exit with the same status.
#
#   lowering_switch_tests.sh BUILD_TOOL ROOT
#
# BUILD_TOOL is quidra_lowering_switch_build (tests/lowering_switch_build.cpp).
set -euo pipefail
TOOL="$(realpath "$1")"
ROOT="$(realpath "$2")"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# compare SWITCH SOURCE: the default build and the switched build of SOURCE
# behave the same.
compare() {
    local switch="$1" source="$2"
    local name
    name="$(basename "$source" .qui)"
    "$TOOL" "$source" "$TMP/$name.default"
    "$TOOL" --switch "$switch" "$source" "$TMP/$name.switched"
    local default_status=0 switched_status=0
    "$TMP/$name.default" >"$TMP/$name.default.out" 2>"$TMP/$name.default.err" || default_status=$?
    "$TMP/$name.switched" >"$TMP/$name.switched.out" 2>"$TMP/$name.switched.err" || switched_status=$?
    if [[ "$default_status" != "$switched_status" ]] ||
       ! cmp -s "$TMP/$name.default.out" "$TMP/$name.switched.out" ||
       ! cmp -s "$TMP/$name.default.err" "$TMP/$name.switched.err"; then
        echo "$source differs with --switch $switch (status $default_status, $switched_status)" >&2
        diff "$TMP/$name.default.out" "$TMP/$name.switched.out" >&2 || true
        diff "$TMP/$name.default.err" "$TMP/$name.switched.err" >&2 || true
        exit 1
    fi
}

for source in "$ROOT"/tests/golden/probes/assignment/*.qui; do
    compare reresolve-compound-stores "$source"
done
for source in "$ROOT"/tests/golden/probes/loops/*.qui \
              "$ROOT"/tests/benchmark/quidra/adversarial/ADV-15.qui; do
    compare copy-value-loops "$source"
done
for source in "$ROOT"/tests/golden/probes/loops/*.qui; do
    compare check-reference-loops "$source"
done
for source in "$ROOT"/tests/golden/probes/arguments/*.qui; do
    compare copy-borrowed-arguments "$source"
done
for source in "$ROOT"/tests/golden/probes/evaluation_order/*.qui \
              "$ROOT"/tests/golden/probes/assignment/*.qui; do
    compare reorder-assignments "$source"
done
