#!/usr/bin/env bash
# Gate Gobj (tests/golden/README.md): runtime and device objects are unchanged.
#
#   scripts/refactor/check_objects.sh BASE_BUILD HEAD_BUILD [OUT_DIR]
#
# Compares the objects of quidra_runtime, quidra_runtime_jit and the quidra
# executable's device_backend.cpp between two builds. Each object is stripped
# of debug information, then every remaining section is dumped (contents in
# hex) and the symbol table is listed sorted; the dumps must be identical.
# Builds should use the same compiler and flags, configured with
# -DCMAKE_CXX_FLAGS=-ffile-prefix-map=<source dir>=. so that source paths
# (e.g. in __FILE__) do not differ between worktrees.
#
# Metal kernels compiled from source at run time are covered separately: run
# the Metal suites with the source logger (tests/golden/metal_source_log) on
# both builds and compare the logs as multisets (see that directory).
set -euo pipefail

if [[ $# -lt 2 ]]; then
  sed -n '2,17p' "$0"
  exit 2
fi
base="$(cd "$1" && pwd)"
head="$(cd "$2" && pwd)"
out="${3:-$(mktemp -d "${TMPDIR:-/tmp}/check_objects.XXXXXX")}"
mkdir -p "$out"

find_tool() {
  local name="$1"
  for candidate in "${LLVM_TOOLS:-}/$name" "$(command -v "$name" 2>/dev/null || true)" \
                   /opt/homebrew/opt/llvm/bin/"$name" /usr/local/opt/llvm/bin/"$name" \
                   "$(xcrun --find "$name" 2>/dev/null || true)"; do
    [[ -n "$candidate" && -x "$candidate" ]] && { echo "$candidate"; return; }
  done
  for version in 20 19 18 17 16 15; do
    command -v "$name-$version" >/dev/null 2>&1 && { command -v "$name-$version"; return; }
  done
  echo "check_objects.sh: $name not found (set LLVM_TOOLS)" >&2
  exit 2
}
OBJCOPY="$(find_tool llvm-objcopy)"
OBJDUMP="$(find_tool llvm-objdump)"
NM="$(find_tool llvm-nm)"

objects() {
  local build="$1"
  (cd "$build" && find CMakeFiles/quidra_runtime.dir CMakeFiles/quidra_runtime_jit.dir \
      -name '*.o' -o -name '*.obj' 2>/dev/null; \
   find CMakeFiles/quidra.dir -name 'device_backend.cpp.o' -o -name 'device_backend.cpp.obj' \
      2>/dev/null) | sort
}

dump() {
  local object="$1" target="$2"
  local stripped="$target.stripped"
  "$OBJCOPY" --strip-debug "$object" "$stripped"
  {
    "$OBJDUMP" --section-headers "$stripped" | sed '1,/^Sections:/d'
    "$OBJDUMP" --full-contents "$stripped" | sed '1,2d'
    "$NM" --no-sort "$stripped" | LC_ALL=C sort
  } > "$target"
  rm -f "$stripped"
}

status=0
count=0
base_list="$(objects "$base")"
head_list="$(objects "$head")"
if [[ "$base_list" != "$head_list" ]]; then
  echo "Gobj: object lists differ:" >&2
  diff <(echo "$base_list") <(echo "$head_list") >&2 || true
  status=1
fi
while IFS= read -r object; do
  [[ -n "$object" && -f "$head/$object" ]] || continue
  key="$(echo "$object" | tr '/' '_')"
  dump "$base/$object" "$out/$key.base"
  dump "$head/$object" "$out/$key.head"
  count=$((count + 1))
  if ! cmp -s "$out/$key.base" "$out/$key.head"; then
    echo "Gobj: $object differs (non-debug sections or symbols):" >&2
    diff "$out/$key.base" "$out/$key.head" | head -40 >&2 || true
    status=1
  fi
done <<< "$base_list"

if [[ "$status" -eq 0 ]]; then
  echo "Gobj: $count objects identical after --strip-debug (dumps in $out)"
fi
exit "$status"
