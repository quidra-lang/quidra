#!/usr/bin/env bash
# The WebAssembly build of quidra_golden_dump (gate Gw), run under node with
# NODERAWFS so it reads the corpus from disk. tests/golden/capture.py runs it
# as --tool, with QUIDRA_WASM_GOLDEN=<path to quidra_golden_dump_wasm.js> in
# the entry environment (--env).
set -euo pipefail
exec node "${QUIDRA_WASM_GOLDEN:?set QUIDRA_WASM_GOLDEN to quidra_golden_dump_wasm.js}" "$@"
