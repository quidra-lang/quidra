#!/usr/bin/env bash
# Golden equivalence gates of the compiler
# (reference: tests/golden/README.md; corpus: tests/golden/corpus.toml).
#
#   tests/ir_golden.sh MODE [options]
#
# Modes:
#   corpus     materialize corpus A and capture it with the head build
#   selfcheck  capture twice (memory perturbation, reverse order, permuted
#              views), then in one process per group (--batch forward and
#              reverse); every capture must equal the first
#   cli        `quidra ir` / `quidra llvm` must equal the tool's ir / llvm views
#   compare    --base-ref REF [--head-ref REF] [--harness] [--expect FILE]...
#              [--column-map FILE]...: capture both sides (cached by tree) and
#              compare every view and pinned status; with --expect, the
#              change's expected differences (tests/golden/expect.py) may
#              differ where their rules explain it; --column-map translates
#              the source columns of rewritten sources
#   walk       RANGE [--harness-auto]: compare every commit of RANGE with its
#              parent, oldest first (CI's per-commit walk)
#   census     aggregate the census of a capture per component; with
#              --require-all, fail unless every alternative occurs lowered and
#              optimized
#   perf       --base-ref REF [--head-ref REF] [--limit PCT]: Gperf. Compile
#              time per stage (lower, optimize, emit) of base and head tools
#              (tests/golden/compile_time.py): retired instructions on Linux,
#              gated at --limit percent (default 0.5); wall time on
#              macOS, reported only
#   objects    --base-ref REF [--head-ref REF]: Gobj. Build the runtime and
#              device objects of both sides in worktrees with
#              -ffile-prefix-map and compare every non-debug section
#              (scripts/refactor/check_objects.sh)
#   metal-sources --base-ref REF [--head-ref REF] [--attempts N]: Gobj for
#              kernels compiled at run time (macOS). Run
#              tests/real_gpu_integration.sh with each side's CLI under the
#              Metal source logger (tests/golden/metal_source_log) and compare
#              the logs as multisets. How often the runtime compiles a source
#              can vary between two runs of one build, so when the logs differ
#              only in such counts, both sides run again, up to N attempts in
#              all (default 3); the logs must be equal in one attempt. A source
#              that occurs on one side only fails at once
#   packages   [--package-root DIR]: Gp. Run every package suite
#              (<package>/tests/*.sh) and every <package>/tests/*.qui with this
#              build's CLI (fake GPU with build-test, Metal with a release
#              build on macOS), from the pinned store copies of the corpus
#              or from DIR. Running a program directly leaves a transient
#              .quidra-run-* artifact next to it until the run ends, so a
#              shared package checkout given as DIR sees short-lived files
#   sanitize   Gsan. Capture the corpus with an ASan+UBSan build of the head
#              tool ($GH/build-san); it must run clean and equal the release
#              capture
#   coverage   --base-ref REF [--accept FILE]: G1c. Capture the corpus with
#              a coverage build of the head tool ($GH/build-cov), then every
#              code region on a changed line of REF..HEAD under the compiler
#              directories must have executed (judged per region, not per
#              function), and branch coverage of every touched function must
#              not fall below its coverage at REF (a coverage build of REF in
#              a worktree captures the same corpus; its summary is cached by
#              REF's tree and the corpus). FILE lists `PATH:LINE reason` for
#              lines no input can reach
#   repl-check the REPL session simulator must make the same compile-time
#              decisions as `quidra repl` on every session of the corpus
#   observe    [--base-ref REF] [--suites LIST] [--packages]: corpus B. Run the
#              test suites with tests/golden/observe_shim.py in place of the
#              CLI (snapshots of every compiled program), then capture the
#              snapshots with the base tool (REF, default: the head build)
#              and the head tool and compare; the `deps` closure of every
#              snapshot is checked. --packages also runs the package suites
#              from the pinned store copies.
#
# Every capture of corpus A runs the [[run]] entries of corpus.toml too (the
# `run` view): each side's CLI builds them and they are run (capture.py
# --run), so the captures build the CLI and the runtime besides the golden
# tool.
#
# Options:
#   --build DIR      head build directory (default: build-release)
#   --capture DIR    capture to read (census) or to write (corpus)
#   --only PREFIX    restrict to entries whose name starts with PREFIX
#   -j N             parallel entries (default: CPU count)
#   --allow-missing  skip entries whose external sources are unavailable
#   --unpinned       do not check the corpus against corpus.toml (bootstrap)
#
# Environment:
#   QUIDRA_GOLDEN_HOME (required)  corpus root, captures, worktrees, archived
#                      indexes and `sources.conf` (package clones and external
#                      programs; see tests/golden/corpus.py). Captures are only
#                      comparable under the same QUIDRA_GOLDEN_HOME, because
#                      absolute corpus paths reach the compiler output.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GOLDEN="$REPO/tests/golden"
PYTHON="${PYTHON:-python3}"

if [[ -z "${QUIDRA_GOLDEN_HOME:-}" ]]; then
  echo "ir_golden.sh: set QUIDRA_GOLDEN_HOME to an absolute directory" >&2
  exit 2
fi
GH="$QUIDRA_GOLDEN_HOME"
mkdir -p "$GH"
GH="$(cd "$GH" && pwd -P)"
CORPUS="$GH/corpus"
HOME_DIR="$GH/home"
CAPTURES="$GH/captures"
KEEP_CAPTURES="${QUIDRA_GOLDEN_KEEP_CAPTURES:-8}"

mode="${1:-}"
[[ -n "$mode" ]] || { sed -n '2,/^set -euo/p' "$0" | sed '/^set -euo/,$d'; exit 2; }
shift

build="$REPO/build-release"
capture=""
base_ref=""
head_ref=""
harness=0
harness_auto=0
jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
only=()
materialize_flags=()
range=""
suites=""
packages=0
limit=0.5
package_root=""
accept=""
require_all=0
attempts=3
expect_args=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --expect|--column-map)
      expect_args+=("$1" "$(cd "$(dirname "$2")" && pwd -P)/$(basename "$2")"); shift 2 ;;
    --suites) suites="$2"; shift 2 ;;
    --accept) accept="$2"; shift 2 ;;
    --limit) limit="$2"; shift 2 ;;
    --package-root) package_root="$2"; shift 2 ;;
    --require-all) require_all=1; shift ;;
    --attempts) attempts="$2"; shift 2 ;;
    --packages) packages=1; shift ;;
    --build) build="$2"; shift 2 ;;
    --capture) capture="$2"; shift 2 ;;
    --base-ref) base_ref="$2"; shift 2 ;;
    --head-ref) head_ref="$2"; shift 2 ;;
    --harness) harness=1; shift ;;
    --harness-auto) harness_auto=1; shift ;;
    --only) only+=(--only "$2"); shift 2 ;;
    -j) jobs="$2"; shift 2 ;;
    --allow-missing) materialize_flags+=(--allow-missing); shift ;;
    --unpinned) materialize_flags+=(--unpinned); shift ;;
    -*) echo "ir_golden.sh: unknown option $1" >&2; exit 2 ;;
    *) range="$1"; shift ;;
  esac
done

platform="$(uname -s | tr '[:upper:]' '[:lower:]')-$(uname -m)"
toolchain="$( (${CXX:-c++} --version 2>/dev/null | head -1; echo Release) | shasum -a 256 | cut -c1-10)"

log() { echo "ir_golden.sh: $*" >&2; }

# Materialize corpus A from this checkout's harness and the pinned sources;
# the head build's tool lists the hand-built fixtures, and its compiler tests
# write their inline programs.
materialize() {
  local flags=("${materialize_flags[@]+"${materialize_flags[@]}"}")
  build_tools "$build" quidra_tests
  "$PYTHON" "$GOLDEN/corpus.py" materialize --root "$CORPUS" \
    --sources "$GH/sources.conf" --tool "$build/quidra_golden_dump" \
    --compiler-tests "$build/quidra_tests" "${flags[@]+"${flags[@]}"}" >&2
}

# A key for the corpus as materialized: its entry list and pins.
corpus_key() {
  cat "$CORPUS/entries.tsv" "$GOLDEN/corpus.toml" 2>/dev/null | shasum -a 256 | cut -c1-12
}

# Build the golden tool (and, with `cli`, the CLI) in a build directory.
build_tools() {
  local dir="$1"; shift
  # A target added since the last configure is unknown to the generated build
  # files until CMake regenerates them.
  if [[ "$REPO/CMakeLists.txt" -nt "$dir/CMakeCache.txt" ]]; then cmake "$dir" >&2; fi
  cmake --build "$dir" -j "$jobs" --target quidra_golden_dump "$@" >&2
}

# Check out REF in the detached worktree slot SLOT; prints the worktree.
worktree_at() {
  local slot="$1" ref
  # Resolve in this repository: a relative ref (HEAD~1) means something else
  # inside the worktree.
  ref="$(git -C "$REPO" rev-parse --verify "$2^{commit}")"
  local tree="$GH/worktrees/$slot"
  if [[ ! -d "$tree" ]]; then
    git -C "$REPO" worktree add --detach "$tree" "$ref" >&2
  else
    git -C "$tree" checkout --quiet --detach "$ref" >&2
  fi
  echo "$tree"
}

# Configure and build REF in a detached worktree slot (the golden tool and
# any further targets); prints the build dir.
build_ref() {
  local tree
  tree="$(worktree_at "$1" "$2")"
  shift 2
  if [[ ! -f "$tree/build/CMakeCache.txt" ]]; then
    cmake -S "$tree" -B "$tree/build" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON >&2
  fi
  build_tools "$tree/build" "$@"
  echo "$tree/build"
}

# Capture with a build's golden tool and CLI (the run view) into the cache
# slot of KEY (reused when present).
capture_into() {
  local tool_build="$1" key="$2"
  local dir="$CAPTURES/$key"
  if [[ -f "$dir/index.tsv" && -f "$dir/complete" ]]; then
    log "reusing capture $key"
  else
    "$PYTHON" "$GOLDEN/capture.py" --tool "$tool_build/quidra_golden_dump" --root "$CORPUS" \
      --home "$HOME_DIR" --out "$dir" -j "$jobs" --run "$tool_build/quidra" \
      "${only[@]+"${only[@]}"}" >&2
    touch "$dir/complete"
    if [[ "$key" != worktree-dirty ]]; then
      mkdir -p "$GH/index"
      cp "$dir/index.tsv" "$GH/index/$key.tsv"
    fi
  fi
  touch "$dir"
  prune_captures
  echo "$dir"
}

prune_captures() {
  [[ -d "$CAPTURES" ]] || return 0
  local old
  old="$(ls -1t "$CAPTURES" | tail -n +"$((KEEP_CAPTURES + 1))")"
  for item in $old; do rm -rf "${CAPTURES:?}/$item"; done
}

tree_of() { git -C "$REPO" rev-parse "$1^{tree}"; }

capture_key() {
  local tree="$1"
  local scope="all"
  if [[ ${#only[@]} -gt 0 ]]; then scope="$(printf '%s' "${only[*]}" | shasum -a 256 | cut -c1-8)"; fi
  echo "$tree-$platform-$toolchain-$(corpus_key)-$scope"
}

# The head side: the given ref, or this checkout (cached only when clean).
capture_head() {
  if [[ -n "$head_ref" ]]; then
    local dir
    dir="$(build_ref head "$head_ref" quidra)"
    capture_into "$dir" "$(capture_key "$(tree_of "$head_ref")")"
    return
  fi
  build_tools "$build" quidra
  if [[ -z "$(git -C "$REPO" status --porcelain --untracked-files=no)" ]]; then
    capture_into "$build" "$(capture_key "$(tree_of HEAD)")"
  else
    rm -rf "$CAPTURES/worktree-dirty"
    capture_into "$build" "worktree-dirty"
    rm -f "$CAPTURES/worktree-dirty/complete"
  fi
}

# A coverage build of the golden tool from the checkout SOURCE in DIR. The
# profile names the sources by absolute path, so a build configured from
# another checkout would give no evidence for this one: it is reconfigured.
coverage_build() {
  local source="$1" dir="$2" configured
  if [[ -f "$dir/CMakeCache.txt" ]]; then
    configured="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$dir/CMakeCache.txt")"
    if [[ "$(cd "$configured" 2>/dev/null && pwd -P)" != "$(cd "$source" && pwd -P)" ]]; then
      log "coverage: $dir was configured from $configured; reconfiguring for $source"
      rm -rf "$dir"
    fi
  fi
  if [[ ! -f "$dir/CMakeCache.txt" ]]; then
    cmake -S "$source" -B "$dir" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
      "-DCMAKE_CXX_FLAGS=-fprofile-instr-generate -fcoverage-mapping" >&2
  fi
  build_tools "$dir"
}

# A full build of REF (runtime, JIT runtime, CLI and, on macOS, the Metal
# source logger) in worktree SLOT, with source paths mapped to "." so object
# files do not depend on the worktree location; prints the build directory.
object_build() {
  local tree
  tree="$(worktree_at "$1" "$2")"
  if [[ ! -f "$tree/build/CMakeCache.txt" ]]; then
    cmake -S "$tree" -B "$tree/build" -DCMAKE_BUILD_TYPE=Release \
      "-DCMAKE_CXX_FLAGS=-ffile-prefix-map=$tree=." \
      "-DCMAKE_OBJCXX_FLAGS=-ffile-prefix-map=$tree=." >&2
  fi
  local targets=(quidra_runtime quidra_runtime_jit quidra)
  if [[ "$(uname -s)" == Darwin ]]; then targets+=(quidra_metal_source_log); fi
  cmake --build "$tree/build" -j "$jobs" --target "${targets[@]}" >&2
  echo "$tree/build"
}

# Run tests/real_gpu_integration.sh with the CLI of SIDE_BUILD under the Metal
# source logger, writing $GH/metal-SIDE.log (the sources) and
# $GH/metal-SIDE.out (the suite's output); nonzero when the suite fails.
metal_sources_run() {
  local side="$1" side_build="$2"
  # SIP-protected binaries (/bin/bash, /usr/bin/env) drop DYLD_* from
  # their environment, so a Python wrapper in a mirror of the build
  # directory sets DYLD_INSERT_LIBRARIES right before it execs the CLI;
  # the programs the CLI runs inherit it.
  local shim="$GH/metal-shim-$side" item
  rm -rf "$shim"
  mkdir -p "$shim"
  for item in "$side_build"/*; do ln -s "$item" "$shim/$(basename "$item")"; done
  rm -f "$shim/quidra"
  cat > "$shim/quidra" <<SHIM
#!/usr/bin/env python3
import os, sys
os.environ["DYLD_INSERT_LIBRARIES"] = "$side_build/libquidra_metal_source_log.dylib"
os.execv("$side_build/quidra", ["$side_build/quidra", *sys.argv[1:]])
SHIM
  chmod +x "$shim/quidra"
  local log="$GH/metal-$side.log"
  rm -f "$log"
  QUIDRA_METAL_SOURCE_LOG="$log" bash "$REPO/tests/real_gpu_integration.sh" "$shim/quidra" \
    > "$GH/metal-$side.out" 2>&1 || { log "real_gpu_integration failed on $side"; return 1; }
}

compare_flags() {
  if [[ "$harness" -eq 1 ]]; then echo "--common"; else echo "--pins"; fi
}

case "$mode" in
  corpus)
    materialize
    if [[ -n "$capture" ]]; then
      build_tools "$build" quidra
      "$PYTHON" "$GOLDEN/capture.py" --tool "$build/quidra_golden_dump" --root "$CORPUS" \
        --home "$HOME_DIR" --out "$capture" -j "$jobs" --run "$build/quidra" \
        "${only[@]+"${only[@]}"}"
    else
      capture_head
    fi
    ;;

  selfcheck)
    materialize
    build_tools "$build" quidra
    tool="$build/quidra_golden_dump"
    work="$GH/selfcheck"
    rm -rf "$work"
    run() { "$PYTHON" "$GOLDEN/capture.py" --tool "$tool" --root "$CORPUS" --home "$HOME_DIR" \
              -j "$jobs" --run "$build/quidra" "${only[@]+"${only[@]}"}" "$@"; }
    run --out "$work/first"
    run --out "$work/perturbed" --perturb --reverse --permute-views 1
    run --out "$work/batch-forward" --batch forward
    run --out "$work/batch-reverse" --batch reverse --perturb
    status=0
    for other in perturbed batch-forward batch-reverse; do
      log "selfcheck: first vs $other"
      "$PYTHON" "$GOLDEN/compare.py" "$work/first" "$work/$other" || status=1
    done
    exit "$status"
    ;;

  cli)
    materialize
    build_tools "$build" quidra
    work="$GH/cli"
    rm -rf "$work"
    "$PYTHON" "$GOLDEN/capture.py" --tool "$build/quidra_golden_dump" --root "$CORPUS" \
      --home "$HOME_DIR" --out "$work/tool" -j "$jobs" --views ir,llvm "${only[@]+"${only[@]}"}"
    "$PYTHON" "$GOLDEN/capture.py" --cli "$build/quidra" --tool "$build/quidra_golden_dump" \
      --root "$CORPUS" --home "$HOME_DIR" --out "$work/cli" -j "$jobs" --views ir,llvm \
      "${only[@]+"${only[@]}"}"
    "$PYTHON" "$GOLDEN/compare.py" "$work/tool" "$work/cli" --common --cli-status
    ;;

  compare)
    [[ -n "$base_ref" ]] || { echo "ir_golden.sh compare: --base-ref REF is required" >&2; exit 2; }
    materialize
    base_build="$(build_ref base "$base_ref" quidra)"
    base_capture="$(capture_into "$base_build" "$(capture_key "$(tree_of "$base_ref")")")"
    head_capture="$(capture_head)"
    log "compare: $base_ref vs ${head_ref:-working tree}"
    "$PYTHON" "$GOLDEN/compare.py" "$base_capture" "$head_capture" $(compare_flags) \
      "${expect_args[@]+"${expect_args[@]}"}"
    ;;

  walk)
    [[ -n "$range" ]] || { echo "ir_golden.sh walk: RANGE is required (A..B)" >&2; exit 2; }
    materialize
    status=0
    for commit in $(git -C "$REPO" rev-list --reverse "$range"); do
      parent="$(git -C "$REPO" rev-parse "$commit^")"
      flags="--pins"
      if [[ "$harness_auto" -eq 1 ]] && \
         [[ -z "$(git -C "$REPO" diff --name-only "$parent" "$commit" | grep -vE '^(tests/|scripts/refactor/|\.github/)' || true)" ]]; then
        flags="--common"
      fi
      log "walk: $(git -C "$REPO" log -1 --format='%h %s' "$commit") ($flags)"
      base_capture="$(capture_into "$(build_ref base "$parent" quidra)" "$(capture_key "$(tree_of "$parent")")")"
      head_capture="$(capture_into "$(build_ref head "$commit" quidra)" "$(capture_key "$(tree_of "$commit")")")"
      "$PYTHON" "$GOLDEN/compare.py" "$base_capture" "$head_capture" $flags || status=1
    done
    exit "$status"
    ;;

  repl-check)
    materialize
    build_tools "$build" quidra
    work="$GH/repl-check"
    "$PYTHON" "$GOLDEN/capture.py" --tool "$build/quidra_golden_dump" --root "$CORPUS" \
      --home "$HOME_DIR" --out "$work" -j "$jobs" --views repl.session \
      "${only[@]+"${only[@]}"}" >&2
    "$PYTHON" "$GOLDEN/repl_session_check.py" "$build/quidra" "$work" "$CORPUS" "$HOME_DIR"
    ;;

  observe)
    materialize
    build_tools "$build" quidra
    observe="$GH/observe"
    shim="$observe/shim"
    store="$observe/store"
    # The suites run in an empty directory: a REPL session's snapshot takes
    # the sources below its working directory, and $observe still holds the
    # previous run's materialized snapshots and captures.
    work="$observe/work"
    rm -rf "$shim" "$store" "$work" "$observe/cache" "$observe/root" "$observe/base" "$observe/head"
    mkdir -p "$shim" "$store" "$work"
    for item in "$build"/*; do ln -s "$item" "$shim/$(basename "$item")"; done
    rm -f "$shim/quidra"
    sed -e "s#@REAL@#$build/quidra#" -e "s#@STORE@#$store#" "$GOLDEN/observe_shim.py" > "$shim/quidra"
    chmod +x "$shim/quidra"
    default_suites="cli_tests stdlib_tests container_cast_tests tensor_autograd_foundation_tests"
    default_suites+=" tensor_view_fast_path_tests cpu_parallel_tests unified_memory_tests"
    default_suites+=" package_name_tests package_identity_tests package_native_tests"
    default_suites+=" package_release_tests package_metadata_sync_tests real_gpu_integration"
    if grep -q 'QUIDRA_ENABLE_TEST_GPU_BACKEND:BOOL=ON' "$build/CMakeCache.txt"; then
      default_suites+=" gpu_contract_tests gpu_autograd_contract_tests runtime_counters"
    fi
    default_suites+=" documentation_examples manifest_registry_tests"
    default_suites+=" generic_specialization_scaling native-smoke"
    : > "$observe/suites.tsv"
    for suite in ${suites:-$default_suites}; do
      start=$(date +%s)
      case "$suite" in
        native-smoke) command=(bash "$REPO/scripts/native-smoke.sh" "$shim/quidra" "$REPO") ;;
        generic_specialization_scaling)
          command=("$PYTHON" "$REPO/tests/$suite.py" "$shim/quidra" --sizes 2,4 --repeats 1) ;;
        documentation_examples|manifest_registry_tests)
          command=("$PYTHON" "$REPO/tests/$suite.py" "$shim/quidra" "$REPO") ;;
        *) command=(bash "$REPO/tests/$suite.sh" "$shim/quidra" "$REPO") ;;
      esac
      set +e
      (cd "$work" && QUIDRA_TIMING_BINARY="$build/quidra" "${command[@]}") \
        > "$observe/$suite.log" 2>&1
      rc=$?
      set -e
      printf '%s\t%s\t%ss\n' "$suite" "$rc" "$(( $(date +%s) - start ))" | tee -a "$observe/suites.tsv" >&2
    done
    if [[ "$packages" -eq 1 ]]; then
      for script in "$CORPUS"/store/*/tests/*.sh; do
        suite="package-$(basename "$(dirname "$(dirname "$script")")")-$(basename "$script" .sh)"
        start=$(date +%s)
        set +e
        (cd "$work" && QUIDRA_PACKAGE_PATH="$CORPUS/store" QUIDRA_CACHE_DIR="$observe/cache/$suite" \
          bash "$script" "$shim/quidra") > "$observe/$suite.log" 2>&1
        rc=$?
        set -e
        printf '%s\t%s\t%ss\n' "$suite" "$rc" "$(( $(date +%s) - start ))" | tee -a "$observe/suites.tsv" >&2
      done
    fi
    [[ ! -s "$store/shim-errors.log" ]] || { log "shim errors:"; cat "$store/shim-errors.log" >&2; }
    "$PYTHON" "$GOLDEN/observe_shim.py" --materialize "$store" "$observe/root" >&2
    base_tool_build="$build"
    if [[ -n "$base_ref" ]]; then base_tool_build="$(build_ref base "$base_ref")"; fi
    rm -rf "$observe/base" "$observe/head"
    "$PYTHON" "$GOLDEN/capture.py" --tool "$base_tool_build/quidra_golden_dump" --root "$observe/root" \
      --home "$HOME_DIR" --out "$observe/base" -j "$jobs" >&2
    "$PYTHON" "$GOLDEN/capture.py" --tool "$build/quidra_golden_dump" --root "$observe/root" \
      --home "$HOME_DIR" --out "$observe/head" -j "$jobs" >&2
    status=0
    "$PYTHON" "$GOLDEN/observe_shim.py" --check-deps "$observe/root" "$observe/head" || status=1
    "$PYTHON" "$GOLDEN/compare.py" "$observe/base" "$observe/head" || status=1
    exit "$status"
    ;;

  census)
    [[ -n "$capture" ]] || { echo "ir_golden.sh census: --capture DIR is required" >&2; exit 2; }
    flags=()
    [[ "$require_all" -eq 0 ]] || flags+=(--require-all)
    "$PYTHON" "$GOLDEN/corpus.py" census --captures "$capture/out" --root "$CORPUS" \
      "${flags[@]+"${flags[@]}"}"
    ;;

  perf)
    [[ -n "$base_ref" ]] || { echo "ir_golden.sh perf: --base-ref REF is required" >&2; exit 2; }
    materialize
    base_build="$(build_ref base "$base_ref")"
    if [[ -n "$head_ref" ]]; then
      head_build="$(build_ref head "$head_ref")"
    else
      head_build="$build"
    fi
    "$PYTHON" "$GOLDEN/compile_time.py" --base-tool "$base_build/quidra_golden_dump" \
      --head-tool "$head_build/quidra_golden_dump" --root "$CORPUS" --home "$HOME_DIR" \
      --limit "$limit"
    ;;

  objects)
    [[ -n "$base_ref" ]] || { echo "ir_golden.sh objects: --base-ref REF is required" >&2; exit 2; }
    base_objects="$(object_build objects-base "$base_ref")"
    head_objects="$(object_build objects-head "${head_ref:-HEAD}")"
    bash "$REPO/scripts/refactor/check_objects.sh" "$base_objects" "$head_objects" "$GH/objects"
    ;;

  metal-sources)
    [[ -n "$base_ref" ]] || { echo "ir_golden.sh metal-sources: --base-ref REF is required" >&2; exit 2; }
    [[ "$(uname -s)" == Darwin ]] || { echo "ir_golden.sh metal-sources: macOS only" >&2; exit 2; }
    [[ "$attempts" =~ ^[1-9][0-9]*$ ]] || { echo "ir_golden.sh metal-sources: --attempts takes a positive count" >&2; exit 2; }
    base_build="$(object_build objects-base "$base_ref")"
    head_build="$(object_build objects-head "${head_ref:-HEAD}")"
    attempt=1
    while :; do
      status=0
      metal_sources_run base "$base_build" || status=1
      metal_sources_run head "$head_build" || status=1
      compared=0
      "$PYTHON" "$GOLDEN/metal_source_log/compare_logs.py" "$GH/metal-base.log" "$GH/metal-head.log" \
        || compared=$?
      [[ "$status" -eq 0 && "$compared" -eq 3 && "$attempt" -lt "$attempts" ]] || break
      attempt=$((attempt + 1))
      log "metal-sources: only the compilation counts differ; attempt $attempt of $attempts"
    done
    [[ "$compared" -eq 0 ]] || status=1
    [[ -s "$GH/metal-base.log" ]] || { log "no Metal source was logged"; status=1; }
    exit "$status"
    ;;

  packages)
    if [[ -z "$package_root" ]]; then
      materialize
      package_root="$CORPUS/store"
    else
      build_tools "$build"
    fi
    build_tools "$build" quidra
    package_root="$(cd "$package_root" && pwd -P)"
    work="$GH/packages-$(basename "$build")"
    rm -rf "$work"
    mkdir -p "$work"
    status=0
    for package in "$package_root"/*/; do
      package="$(basename "$package")"
      [[ -f "$package_root/$package/quidra.package" ]] || continue
      for script in "$package_root/$package"/tests/*.sh "$package_root/$package"/tests/*.qui; do
        [[ -f "$script" ]] || continue
        name="$package-$(basename "$script")"
        start=$(date +%s)
        set +e
        # Each suite and each program gets a run cache of its own.
        if [[ "$script" == *.sh ]]; then
          (cd "$work" && QUIDRA_PACKAGE_PATH="$package_root" QUIDRA_CACHE_DIR="$work/cache/$name" \
            bash "$script" "$build/quidra") > "$work/$name.log" 2>&1
        else
          (cd "$work" && QUIDRA_PACKAGE_PATH="$package_root" QUIDRA_CACHE_DIR="$work/cache/$name" \
            "$build/quidra" "$script") > "$work/$name.log" 2>&1
        fi
        rc=$?
        set -e
        printf '%s\t%s\t%ss\n' "$name" "$rc" "$(( $(date +%s) - start ))" | tee -a "$work/suites.tsv"
        [[ "$rc" -eq 0 ]] || status=1
      done
    done
    exit "$status"
    ;;

  sanitize)
    materialize
    san="$GH/build-san"
    if [[ ! -f "$san/CMakeCache.txt" ]]; then
      cmake -S "$REPO" -B "$san" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON \
        "-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer" >&2
    fi
    build_tools "$san"
    rm -rf "$GH/sanitize"
    "$PYTHON" "$GOLDEN/capture.py" --tool "$san/quidra_golden_dump" --root "$CORPUS" \
      --home "$HOME_DIR" --out "$GH/sanitize" -j "$jobs" "${only[@]+"${only[@]}"}" \
      --env ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
      --env UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 >&2
    head_capture="$(capture_head)"
    # The sanitizer capture has no run view: the programs a sanitized CLI
    # builds are not what Gsan checks.
    "$PYTHON" "$GOLDEN/compare.py" "$head_capture" "$GH/sanitize" --skip-views run
    ;;

  coverage)
    [[ -n "$base_ref" ]] || { echo "ir_golden.sh coverage: --base-ref REF is required" >&2; exit 2; }
    materialize
    coverage_build "$REPO" "$GH/build-cov"
    "$PYTHON" "$GOLDEN/coverage.py" run --build "$GH/build-cov" --root "$CORPUS" \
      --home "$HOME_DIR" --out "$GH/coverage" -j "$jobs" >&2
    # The branch-coverage baseline: REF's tool, built for coverage in a
    # worktree, on the same corpus.
    baseline="$GH/coverage-baseline/$(capture_key "$(tree_of "$base_ref")").json"
    if [[ -f "$baseline" ]]; then
      log "reusing coverage baseline of $base_ref"
    else
      tree="$(worktree_at coverage-base "$base_ref")"
      coverage_build "$tree" "$tree/build-cov"
      "$PYTHON" "$GOLDEN/coverage.py" run --build "$tree/build-cov" --root "$CORPUS" \
        --home "$HOME_DIR" --out "$GH/coverage-base" -j "$jobs" >&2
      mkdir -p "$GH/coverage-baseline"
      "$PYTHON" "$GOLDEN/coverage.py" summary --profile "$GH/coverage-base" \
        --source-root "$tree" --write "$baseline.tmp" >&2
      mv "$baseline.tmp" "$baseline"
    fi
    flags=(--baseline "$baseline")
    [[ -z "$accept" ]] || flags+=(--accept "$accept")
    "$PYTHON" "$GOLDEN/coverage.py" check --profile "$GH/coverage" --base "$base_ref" \
      --head HEAD "${flags[@]}"
    ;;

  *)
    echo "ir_golden.sh: unknown mode $mode" >&2
    exit 2
    ;;
esac
