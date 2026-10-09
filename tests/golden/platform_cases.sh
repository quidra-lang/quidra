#!/usr/bin/env bash
# Gate Gplat (tests/golden/README.md): toolchain discovery and platform behavior.
#
#   tests/golden/platform_cases.sh QUIDRA OUT_DIR
#   diff -r OUT_DIR_base OUT_DIR_head      # must be empty, per OS
#
# Failure paths: every toolchain override set to a bogus value, multi-entry and
# empty-entry QUIDRA_PACKAGE_PATH, HOME="" - the exit status and stderr of
# `build`, `run` and `repl` are recorded.
# Success paths: a PATH shim of logging wrappers (clang, clang++, lli,
# pkg-config, nvcc) records which tools run with which arguments, and the
# loader log (DYLD_PRINT_LIBRARIES on macOS, LD_DEBUG=files on Linux) records
# the libraries the CLI and the JIT load (filtered to Quidra and LLVM).
# Temporary paths and the build directory are normalized away.
set -uo pipefail

if [[ $# -ne 2 ]]; then
  sed -n '2,15p' "$0"
  exit 2
fi
QUIDRA="$(cd "$(dirname "$1")" && pwd -P)/$(basename "$1")"
OUT="$2"
BUILD="$(dirname "$QUIDRA")"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
rm -rf "$OUT"
mkdir -p "$OUT"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/quidra-gplat.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
WORK="$(cd "$WORK" && pwd -P)"

cat > "$WORK/hello.qui" <<'QUI'
print("hello{NL}")
QUI
mkdir -p "$WORK/packages/greeting"
cat > "$WORK/packages/greeting/main.qui" <<'QUI'
string greeting()
    return "hi"
QUI
cat > "$WORK/use-package.qui" <<'QUI'
import greeting
print(greeting.greeting())
QUI
printf '1 + 2\n' > "$WORK/repl.input"

# Paths of this run (scratch directory, build, repository) and generated
# temporary names are replaced, so base and head logs compare across
# worktrees.
normalize() {
  sed -e "s#/private$WORK#<WORK>#g" -e "s#$WORK#<WORK>#g" \
      -e "s#/private$BUILD#<BUILD>#g" -e "s#$BUILD#<BUILD>#g" \
      -e "s#/private$REPO#<REPO>#g" -e "s#$REPO#<REPO>#g" \
      -e 's#\.quidra-run-[0-9a-f-]*#.quidra-run-<RAND>#g' \
      -e 's#/private/var/folders/[^ :]*#<TMPDIR>#g' \
      -e 's#quidra-[A-Za-z0-9]\{6,\}#quidra-<RAND>#g'
}

# case NAME COMMAND...: run in WORK with the current environment additions.
run_case() {
  local name="$1"; shift
  (cd "$WORK" && "$@") > "$WORK/stdout" 2> "$WORK/stderr" < "$WORK/repl.input"
  local status=$?
  {
    echo "exit $status"
    echo "--- stdout"; normalize < "$WORK/stdout"
    echo "--- stderr"; normalize < "$WORK/stderr"
  } > "$OUT/$name.log"
}

overrides=(QUIDRA_CLANGXX QUIDRA_CLANG QUIDRA_LLI QUIDRA_RUNTIME_LIBRARY
           QUIDRA_JIT_RUNTIME_LIBRARY QUIDRA_NATIVE_INCLUDE_DIR QUIDRA_DEBUGGER QUIDRA_NVCC
           QUIDRA_CUDA_HOME QUIDRA_PKG_CONFIG QUIDRA_LLVM_LIBRARY)
if [[ "$(uname -s)" == Darwin ]]; then
  overrides+=(QUIDRA_ORC_RUNTIME QUIDRA_COMPILER_RT_BUILTINS)
fi
for variable in "${overrides[@]}"; do
  for command in build run repl; do
    case "$command" in
      build) arguments=(build hello.qui -o hello.out) ;;
      run) arguments=(run hello.qui) ;;
      repl) arguments=(repl) ;;
    esac
    run_case "fail-$variable-$command" env "$variable=/nonexistent/quidra-bogus" \
      "$QUIDRA" "${arguments[@]}"
  done
done

run_case "package-path-multi" env "QUIDRA_PACKAGE_PATH=/nonexistent:$WORK/packages" \
  "$QUIDRA" check use-package.qui
run_case "package-path-empty-entries" env "QUIDRA_PACKAGE_PATH=::$WORK/packages:" \
  "$QUIDRA" check use-package.qui
run_case "package-path-empty" env "QUIDRA_PACKAGE_PATH=" "$QUIDRA" check use-package.qui
run_case "home-empty" env -u QUIDRA_PACKAGE_PATH HOME= "$QUIDRA" check use-package.qui

# Success paths through a logging PATH shim.
SHIM="$WORK/shim"
mkdir -p "$SHIM"
for tool in clang clang++ lli pkg-config nvcc; do
  real="$(command -v "$tool" 2>/dev/null || true)"
  cat > "$SHIM/$tool" <<SH
#!/usr/bin/env bash
printf '%s' "$tool" >> "$WORK/toolchain.log"
for argument in "\$@"; do printf ' %q' "\$argument" >> "$WORK/toolchain.log"; done
printf '\n' >> "$WORK/toolchain.log"
if [[ -z "$real" ]]; then echo "$tool: not installed" >&2; exit 127; fi
exec "$real" "\$@"
SH
  chmod +x "$SHIM/$tool"
done
loader=()
if [[ "$(uname -s)" == Darwin ]]; then
  loader=(DYLD_PRINT_LIBRARIES=1)
else
  loader=(LD_DEBUG=files)
fi
for command in build run repl; do
  case "$command" in
    build) arguments=(build hello.qui -o hello.out) ;;
    run) arguments=(run hello.qui) ;;
    repl) arguments=(repl) ;;
  esac
  : > "$WORK/toolchain.log"
  run_case "success-$command" env "PATH=$SHIM:$PATH" "${loader[@]}" "$QUIDRA" "${arguments[@]}"
  {
    echo "--- toolchain"; normalize < "$WORK/toolchain.log"
    echo "--- libraries"
    grep -Eo '/[^ ]*(quidra|llvm|LLVM|clang|compiler_rt|orc_rt)[^ ]*' "$OUT/success-$command.log" \
      | grep -v '^<WORK>' | sort -u
  } >> "$OUT/success-$command.log"
  # The loader log itself mostly lists system libraries; keep only the summary.
  sed -i.bak '/^--- stderr$/,/^--- toolchain$/{/^dyld\[/d;/file=/d;}' "$OUT/success-$command.log"
  rm -f "$OUT/success-$command.log.bak"
done

echo "platform_cases.sh: $(ls "$OUT" | wc -l | tr -d ' ') cases in $OUT"
