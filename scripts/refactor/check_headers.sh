#!/usr/bin/env bash
# Gate Ghdr (tests/golden/README.md, Gates and Layering).
#
#   scripts/refactor/check_headers.sh [BUILD_DIR]
#
# 1. Every header under include/quidra/{ir,abi} and src/{ir,llvm_text,
#    llvm_backend,lowering,optimizer,platform,toolchain,semantics}, and the public
#    headers of the three pipeline stages (quidra/lowering.hpp,
#    quidra/optimizer.hpp, quidra/llvm_backend.hpp), compiles on its own
#    (-fsyntax-only, C++20). BUILD_DIR supplies the generated headers
#    (default: build-release).
# 2. The include graph of those files respects the layering:
#      abi          std + quidra/native_extension.h
#      platform     std + OS headers (no Quidra header)
#      toolchain    platform, abi
#      llvm_text    std only (no quidra/ header, no other layer)
#      include/quidra/ir, src/ir   types.hpp (with numeric_types.hpp and
#                                  type_kind.hpp), compiler_extension.hpp,
#                                  abi, ir
#      semantics    the checker's analyses after type checking: never
#                   lowering, the optimizer or the backend
#      lowering     never the optimizer or the backend
#      optimizer    never lowering, the backend, the checker or semantics
#      llvm_backend never lowering, the optimizer, the checker or semantics
#    A stage's public header belongs to its stage. quidra/ir.hpp is the
#    umbrella of the IR and the stages (it includes lowering.hpp, which
#    includes the checker, and optimizer.hpp), so no layer includes it.
#    Reserved nodes (plan/, kernel_ir/, planner) are allowed where the
#    layering puts them, so their first commits need no gate edit.
# 3. The same holds through other headers: the compiler lists every file a
#    header or source of the ir, semantics, lowering, optimizer and
#    llvm_backend layers includes, directly or not, and none of them may be a file the layer
#    must not include.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build="$REPO/build-release"
for argument in "$@"; do
  build="$argument"
done
CXX="${CXX:-c++}"
cd "$REPO"

directories=(include/quidra/ir include/quidra/abi src/ir src/llvm_text src/llvm_backend
             src/lowering src/optimizer src/platform src/toolchain src/semantics)
stage_headers=(include/quidra/lowering.hpp include/quidra/optimizer.hpp include/quidra/llvm_backend.hpp)
headers=()
for directory in "${directories[@]}"; do
  [[ -d "$directory" ]] || continue
  while IFS= read -r header; do headers+=("$header"); done \
    < <(find "$directory" -name '*.hpp' -o -name '*.h' | sort)
done
headers+=("${stage_headers[@]}")

status=0
errors="$(mktemp "${TMPDIR:-/tmp}/check_headers.XXXXXX")"
trap 'rm -f "$errors"' EXIT
for header in "${headers[@]+"${headers[@]}"}"; do
  if ! "$CXX" -std=c++20 -fsyntax-only -Wall -Wextra -Wpedantic -Iinclude \
       -I"$build/generated" -Isrc -x c++-header "$header" 2> "$errors"; then
    echo "Ghdr: $header does not compile on its own:" >&2
    sed 's/^/    /' "$errors" >&2
    status=1
  fi
done

layer_of() {
  case "$1" in
    include/quidra/abi/*) echo abi ;;
    include/quidra/ir/*|src/ir/analysis/*|src/ir/*) echo ir ;;
    src/platform/*) echo platform ;;
    src/toolchain/*) echo toolchain ;;
    src/llvm_text/*) echo llvm_text ;;
    src/semantics/*) echo semantics ;;
    src/lowering/*|include/quidra/lowering.hpp) echo lowering ;;
    src/optimizer/*|include/quidra/optimizer.hpp) echo optimizer ;;
    src/llvm_backend/*|include/quidra/llvm_backend.hpp) echo llvm_backend ;;
    *) echo other ;;
  esac
}

# Layer of an include target as written ("quidra/ir/x.hpp", "llvm_text/x.hpp", ...).
target_layer() {
  case "$1" in
    quidra/native_extension.h) echo native_extension ;;
    quidra/abi/*|abi/*) echo abi ;;
    quidra/ir.hpp) echo ir_umbrella ;;
    quidra/lowering.hpp) echo lowering ;;
    quidra/optimizer.hpp) echo optimizer ;;
    quidra/llvm_backend.hpp) echo llvm_backend ;;
    quidra/checker.hpp|quidra/ast.hpp|quidra/source_tools.hpp|quidra/source_patch.hpp) echo checker ;;
    quidra/ir/*|ir/*) echo ir ;;
    quidra/types.hpp|quidra/numeric_types.hpp|quidra/type_kind.hpp) echo ir_base ;;
    quidra/compiler_extension.hpp|quidra/language.hpp) echo ir_base ;;
    platform/*) echo platform ;;
    toolchain/*) echo toolchain ;;
    llvm_text/*) echo llvm_text ;;
    semantics/*) echo semantics ;;
    lowering/*) echo lowering ;;
    optimizer/*) echo optimizer ;;
    llvm_backend/*) echo llvm_backend ;;
    plan/*) echo plan ;;
    kernel_ir/*) echo kernel_ir ;;
    quidra/*) echo quidra_public ;;
    *) echo local ;;
  esac
}

allowed() {
  local from="$1" to="$2"
  [[ "$to" == local ]] && return 0
  case "$from" in
    abi) [[ "$to" == abi || "$to" == native_extension ]] ;;
    platform) [[ "$to" == platform ]] ;;
    toolchain) [[ "$to" == toolchain || "$to" == platform || "$to" == abi || "$to" == native_extension ]] ;;
    llvm_text) [[ "$to" == llvm_text ]] ;;
    ir) [[ "$to" == ir || "$to" == ir_base || "$to" == abi || "$to" == native_extension ]] ;;
    semantics) [[ "$to" != lowering && "$to" != optimizer && "$to" != llvm_backend && "$to" != plan &&
                  "$to" != kernel_ir && "$to" != ir_umbrella ]] ;;
    lowering) [[ "$to" != optimizer && "$to" != llvm_backend && "$to" != plan && "$to" != kernel_ir &&
                 "$to" != ir_umbrella ]] ;;
    optimizer) [[ "$to" != lowering && "$to" != llvm_backend && "$to" != kernel_ir &&
                  "$to" != ir_umbrella && "$to" != checker && "$to" != semantics ]] ;;
    llvm_backend) [[ "$to" != lowering && "$to" != optimizer && "$to" != kernel_ir &&
                     "$to" != ir_umbrella && "$to" != checker && "$to" != semantics ]] ;;
    *) return 0 ;;
  esac
}

layered_files() {
  local directory
  for directory in "${directories[@]}"; do
    [[ -d "$directory" ]] || continue
    find "$directory" \( -name '*.hpp' -o -name '*.h' -o -name '*.cpp' \) | sort
  done
  printf '%s\n' "${stage_headers[@]}"
}

while IFS= read -r file; do
  from="$(layer_of "$file")"
  while IFS= read -r target; do
    to="$(target_layer "$target")"
    if ! allowed "$from" "$to"; then
      echo "Ghdr: $file ($from) includes \"$target\" ($to), forbidden by the layering" >&2
      status=1
    fi
  done < <(sed -n 's/^[[:space:]]*#[[:space:]]*include[[:space:]]*"\([^"]*\)".*/\1/p' "$file")
done < <(layered_files)

# The include check above sees direct edges only. The compiler lists every
# file a header or source includes, directly or through other headers; a
# layer reaches none of the files it may not include.
reaches_forbidden() {
  local from="$1" reached="$2"
  case "$reached" in
    include/quidra/ir.hpp) [[ "$from" != other ]] ;;
    include/quidra/checker.hpp|include/quidra/ast.hpp|src/semantics/*)
      [[ "$from" == ir || "$from" == optimizer || "$from" == llvm_backend ]] ;;
    include/quidra/lowering.hpp|src/lowering/*)
      [[ "$from" == ir || "$from" == semantics || "$from" == optimizer || "$from" == llvm_backend ]] ;;
    include/quidra/optimizer.hpp|src/optimizer/*)
      [[ "$from" == ir || "$from" == semantics || "$from" == lowering || "$from" == llvm_backend ]] ;;
    include/quidra/llvm_backend.hpp|src/llvm_backend/*)
      [[ "$from" == ir || "$from" == semantics || "$from" == lowering || "$from" == optimizer ]] ;;
    *) return 1 ;;
  esac
}

while IFS= read -r file; do
  from="$(layer_of "$file")"
  case "$from" in ir|semantics|lowering|optimizer|llvm_backend) ;; *) continue ;; esac
  language=c++
  [[ "$file" == *.cpp ]] || language=c++-header
  if ! dependencies="$("$CXX" -std=c++20 -MM -Iinclude -I"$build/generated" -Isrc -x "$language" \
                       "$file" 2> "$errors")"; then
    echo "Ghdr: cannot list the includes of $file:" >&2
    sed 's/^/    /' "$errors" >&2
    status=1
    continue
  fi
  while IFS= read -r reached; do
    [[ -n "$reached" && "$reached" != "$file" ]] || continue
    if reaches_forbidden "$from" "$reached"; then
      echo "Ghdr: $file ($from) reaches $reached through its includes, forbidden by the layering" >&2
      status=1
    fi
  done < <(printf '%s\n' "$dependencies" | tr ' \\' '\n\n' | sed '/:$/d')
done < <(layered_files)

if [[ "$status" -eq 0 ]]; then
  echo "Ghdr: ${#headers[@]} headers compile on their own; layering respected"
fi
exit "$status"
