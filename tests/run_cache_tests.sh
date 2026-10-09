#!/usr/bin/env bash
# The run cache end to end: tests/run_cache_tests.sh QUIDRA ROOT [--full]
#
#   stale        every key component: run (a build), run again (a hit),
#                change it, run again (a build whose output reflects the
#                change); content changes keep the size and restore the
#                modification time, so a check by size and time would fail
#   build        the cached program equals `quidra build -o .../program`
#   transparent  example programs give the same stdout, stderr and status
#                from a cold cache, a warm cache and no cache
#   replay       a native source that warns: a hit prints what the build did
#   corrupt      every damaged form of an entry: the run is correct and the
#                entry is replaced
#   perm         a root writable by others is reset to 0700; a symbolic link
#                inside the root and an unusable root run uncached
#   concurrent   8 parallel cold runs: correct, one entry, no staging left;
#                runs while the entry source changes print one version or
#                the other
#   no-cache     both spellings build without the cache
#   clean        `quidra cache clean` while runs and builds are in flight:
#                every run is correct; dead staging goes, live staging stays;
#                a cache directory that cannot be used is refused
#
# A logging QUIDRA_CLANGXX wrapper tells a build from a hit: a hit runs no
# clang at all. The fixtures are made first and are then left alone for 3
# seconds, because a build never publishes an entry whose inputs changed in
# the 2 seconds before it started. --full also sets every variable of class
# `value` of the environment table (docs/development.md).
set -euo pipefail

QUIDRA="$(realpath "$1")"
ROOT="$(realpath "$2")"
FULL="${3:-}"
TMP="$(cd "$(mktemp -d)" && pwd -P)"
trap 'chmod -R u+rwx "$TMP" 2>/dev/null || true; rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/cache"
unset QUIDRA_CLANGXX QUIDRA_CLANG QUIDRA_PACKAGE_PATH QUIDRA_PKG_CONFIG
unset QUIDRA_RUNTIME_LIBRARY QUIDRA_NATIVE_INCLUDE_DIR

# A failed check is reported and counted; the suite goes on and fails at
# the end, so that one run shows every failure.
fail() {
    echo "FAIL: $*" >&2
    echo "$*" >> "$TMP/failures"
    return 0
}

REAL_CLANG=""
for candidate in clang++-20 clang++-19 clang++-18 clang++-17 clang++-16 clang++-15 clang++; do
    if command -v "$candidate" >/dev/null 2>&1; then
        REAL_CLANG="$(command -v "$candidate")"
        break
    fi
done
[[ -n "$REAL_CLANG" ]] || { echo "FAIL: no clang++" >&2; exit 1; }

LOG="$TMP/log/clang.log"
mkdir -p "$TMP/log"
: > "$LOG"
# make_wrapper DIR: a clang++ in DIR that logs every call and runs clang.
make_wrapper() {
    mkdir -p "$1"
    cat > "$1/clang++" <<SH
#!/bin/sh
echo "\$*" >> "$LOG"
exec "$REAL_CLANG" "\$@"
SH
    chmod +x "$1/clang++"
}
make_wrapper "$TMP/bin"
export QUIDRA_CLANGXX="$TMP/bin/clang++"

builds() { wc -l < "$LOG" | tr -d ' '; }

# expect NAME hit|build|any EXPECTED COMMAND...: runs COMMAND; its stdout
# must be EXPECTED, and the toolchain must have run (build) or not (hit).
expect() {
    local name="$1" kind="$2" expected="$3"
    shift 3
    local before after output status
    before="$(builds)"
    set +e
    output="$("$@" 2>"$TMP/last.err")"
    status=$?
    set -e
    after="$(builds)"
    [[ "$output" == "$expected" ]] ||
        fail "$name: printed '$output' (status $status), expected '$expected': $(cat "$TMP/last.err")"
    if [[ "$kind" == hit ]]; then
        [[ "$after" == "$before" ]] || fail "$name: expected a hit, but the toolchain ran"
    elif [[ "$kind" == build ]]; then
        [[ "$after" != "$before" ]] || fail "$name: expected a build, but nothing was built"
    fi
}

# edit FILE SED_SCRIPT: rewrites FILE in place with sed; the size must stay
# the same, and the modification time is restored.
edit() {
    local file="$1"
    touch -r "$file" "$TMP/mtime.ref"
    sed "$2" "$file" > "$TMP/edit.tmp"
    [[ "$(wc -c < "$TMP/edit.tmp" | tr -d ' ')" == "$(wc -c < "$file" | tr -d ' ')" ]] ||
        fail "edit of $file changes its size"
    cmp -s "$TMP/edit.tmp" "$file" && fail "edit of $file changes nothing"
    cat "$TMP/edit.tmp" > "$file"
    touch -r "$TMP/mtime.ref" "$file"
}

entries() {
    find "$QUIDRA_CACHE_DIR/run" -name metadata.json 2>/dev/null | wc -l | tr -d ' '
}

program() {
    mkdir -p "$(dirname "$1")"
    printf 'print("%s")\nprint(NL)\n' "$2" > "$1"
}

# ---- Fixtures ----------------------------------------------------------------

program "$TMP/e1/main.qui" a
program "$TMP/e1copy/main.qui" a
program "$TMP/cwd/main.qui" c

mkdir -p "$TMP/e2"
printf 'import util = "./util.qui"\nprint(util.word())\nprint(NL)\n' > "$TMP/e2/main.qui"
printf 'string word()\n    return "one"\n' > "$TMP/e2/util.qui"

mkdir -p "$TMP/e3" "$TMP/rootA" "$TMP/rootB"
printf 'import lib = "@/lib.qui"\nprint(lib.word())\nprint(NL)\n' > "$TMP/e3/main.qui"
printf 'string word()\n    return "A"\n' > "$TMP/rootA/lib.qui"
printf 'string word()\n    return "B"\n' > "$TMP/rootB/lib.qui"

# Installed packages: pk in the second of two package roots, and in two
# package stores.
mkdir -p "$TMP/roots/one" "$TMP/roots/two/pk" "$TMP/e4"
printf 'string word()\n    return "two"\n' > "$TMP/roots/two/pk/main.qui"
printf 'import pk = pk\nprint(pk.word())\nprint(NL)\n' > "$TMP/e4/main.qui"
for home in h1 h2; do
    mkdir -p "$TMP/$home"
done
STORE_RELATIVE="$(HOME=/x "$QUIDRA" package path | sed 's|^/x/||')"
for home in h1 h2; do
    mkdir -p "$TMP/$home/$STORE_RELATIVE/pk"
    printf 'string word()\n    return "%s"\n' "$home" > "$TMP/$home/$STORE_RELATIVE/pk/main.qui"
done

# A package with a native source, a package-local header and a prebuilt
# library.
make_native_package() {
    local root="$1"
    mkdir -p "$root/np/native" "$root/np/lib"
    cat > "$root/np/quidra.package" <<'MANIFEST'
name = np
version = 0.1.0
native.source.bridge = native/bridge.c
native.default = lib/libextra.a
MANIFEST
    cat > "$root/np/main.qui" <<'QUI'
extern int64 np_answer() = "np_answer"
extern int64 np_extra() = "np_extra"
int answer()
    return int(np_answer() + np_extra())
QUI
    cat > "$root/np/native/bridge.c" <<'C'
#include <quidra/native_extension.h>
#include "answer.h"
long long np_answer(void) { return NP_ANSWER + 0; }
C
    printf '#define NP_ANSWER 40\n' > "$root/np/native/answer.h"
    printf 'long long np_extra(void) { return 2; }\n' > "$root/np/lib/extra.c"
    "$REAL_CLANG" -x c -O2 -c "$root/np/lib/extra.c" -o "$root/np/lib/extra.o"
    ar rcs "$root/np/lib/libextra.a" "$root/np/lib/extra.o"
}
for copy in 1 2 3 4 5 6 7; do make_native_package "$TMP/natpk$copy"; done
mkdir -p "$TMP/e5"
printf 'import np = np\nprint(np.answer())\nprint(NL)\n' > "$TMP/e5/main.qui"

# A second include tree, a runtime library copy, compiler copies.
RUNTIME_LIBRARY="$(dirname "$QUIDRA")/$(ls "$(dirname "$QUIDRA")" | grep -E '^(lib)?quidra_runtime\.(a|lib)$' | head -1)"
[[ -f "$RUNTIME_LIBRARY" ]] || { echo "FAIL: no runtime library beside $QUIDRA" >&2; exit 1; }
mkdir -p "$TMP/include2" "$TMP/runtime"
cp -R "$ROOT/include/quidra" "$TMP/include2/"
cp -p "$RUNTIME_LIBRARY" "$TMP/runtime/"
RUNTIME_COPY="$TMP/runtime/$(basename "$RUNTIME_LIBRARY")"
mkdir -p "$TMP/q-same" "$TMP/q-changed"
cp -p "$QUIDRA" "$TMP/q-same/quidra"
cp -p "$QUIDRA" "$TMP/q-changed/quidra"
case "$(uname -s)" in
    Darwin)
        install_name_tool -add_rpath /nonexistent-run-cache-test "$TMP/q-changed/quidra" 2>/dev/null || true
        codesign -f -s - -i quidra.run-cache-test "$TMP/q-changed/quidra" 2>/dev/null
        ;;
    *)
        printf 'appended' >> "$TMP/q-changed/quidra"
        ;;
esac
cmp -s "$QUIDRA" "$TMP/q-changed/quidra" && fail "the changed compiler copy is unchanged"

# --link inputs.
mkdir -p "$TMP/e6"
printf 'extern int64 one() = "link_one"\nextern int64 two() = "link_two"\nprint(one() + two())\nprint(NL)\n' \
    > "$TMP/e6/main.qui"
printf 'long long link_one(void) { return 10; }\n' > "$TMP/e6/one.c"
printf 'long long link_two(void) { return 20; }\n' > "$TMP/e6/two.c"

# A lockfile project.
mkdir -p "$TMP/locked/pk" "$TMP/lockproj"
printf 'name = pk\nversion = 0.1.0\n' > "$TMP/locked/pk/quidra.package"
printf 'string word()\n    return "locked"\n' > "$TMP/locked/pk/main.qui"
printf 'unused\n' > "$TMP/locked/pk/notes.txt"
printf 'import pk = pk\nprint(pk.word())\nprint(NL)\n' > "$TMP/lockproj/main.qui"

# pkg-config: a fake one whose --cflags come from a data file.
mkdir -p "$TMP/pc" "$TMP/pcdata" "$TMP/pcpk/pcpk/native"
printf -- '-DPC_VALUE=1\n' > "$TMP/pcdata/cflags"
cat > "$TMP/pc/pkg-config" <<SH
#!/bin/sh
case "\$1" in
    --version) echo 0.29.2 ;;
    --cflags) cat "$TMP/pcdata/cflags" ;;
    --libs) echo ;;
esac
SH
chmod +x "$TMP/pc/pkg-config"
cat > "$TMP/pcpk/pcpk/quidra.package" <<'MANIFEST'
name = pcpk
version = 0.1.0
native.source.bridge = native/bridge.c
native.pkg.fake = fakemodule
MANIFEST
printf 'extern int64 pc_value() = "pc_value"\nint64 value()\n    return pc_value()\n' > "$TMP/pcpk/pcpk/main.qui"
printf 'long long pc_value(void) { return PC_VALUE; }\n' > "$TMP/pcpk/pcpk/native/bridge.c"
mkdir -p "$TMP/e7"
printf 'import pcpk = pcpk\nprint(pcpk.value())\nprint(NL)\n' > "$TMP/e7/main.qui"

# Build-time macros, warnings, compile errors.
mkdir -p "$TMP/timepk/tp/native" "$TMP/e8"
printf 'name = tp\nversion = 0.1.0\nnative.source.bridge = native/bridge.c\n' > "$TMP/timepk/tp/quidra.package"
printf 'extern int64 tp_value() = "tp_value"\nint64 value()\n    return tp_value()\n' > "$TMP/timepk/tp/main.qui"
printf 'static const char *stamp = __TIME__;\nlong long tp_value(void) { return stamp[0] != 0; }\n' \
    > "$TMP/timepk/tp/native/bridge.c"
printf 'import tp = tp\nprint(tp.value())\nprint(NL)\n' > "$TMP/e8/main.qui"
mkdir -p "$TMP/warnpk/wp/native" "$TMP/e9"
printf 'name = wp\nversion = 0.1.0\nnative.source.bridge = native/bridge.c\n' > "$TMP/warnpk/wp/quidra.package"
printf 'extern int64 wp_value() = "wp_value"\nint64 value()\n    return wp_value()\n' > "$TMP/warnpk/wp/main.qui"
printf '#warning "replayed toolchain warning"\nlong long wp_value(void) { return 7; }\n' \
    > "$TMP/warnpk/wp/native/bridge.c"
printf 'import wp = wp\nprint(wp.value())\nprint(NL)\n' > "$TMP/e9/main.qui"
mkdir -p "$TMP/e10"
printf 'int x = "text"\nprint(x)\n' > "$TMP/e10/main.qui"

# Other drivers: other wrapper directories, and two found by discovery on
# PATH under the name discovery looks for.
make_wrapper "$TMP/bin2"
make_wrapper "$TMP/bin3"
for directory in pathA pathB; do
    make_wrapper "$TMP/$directory-wrapper"
    mkdir -p "$TMP/$directory"
    cp -p "$TMP/$directory-wrapper/clang++" "$TMP/$directory/$(basename "$REAL_CLANG")"
done
mkdir -p "$TMP/empty"

# Corruption, permissions, concurrency, transparency.
for name in c1 c2 c3 c4 c5 c6 c7 c8 c9 c10 c11; do program "$TMP/$name/main.qui" "$name"; done
program "$TMP/p0/main.qui" p0
program "$TMP/p1/main.qui" p1
program "$TMP/p2/main.qui" p2
program "$TMP/conc/main.qui" parallel
program "$TMP/edit/main.qui" v1
program "$TMP/b1/main.qui" built
for name in k1 k2 k3 k4; do program "$TMP/$name/main.qui" "$name"; done

sleep 3

# ---- stale: K15, K23 ------------------------------------------------------------

expect "K15 first run" build a "$QUIDRA" "$TMP/e1/main.qui"
expect "K15 second run" hit a "$QUIDRA" "$TMP/e1/main.qui"
expect "K15 run form" hit a "$QUIDRA" run "$TMP/e1/main.qui"
edit "$TMP/e1/main.qui" 's/"a"/"b"/'
expect "K15 content" build b "$QUIDRA" "$TMP/e1/main.qui"
expect "K15 same content elsewhere" build a "$QUIDRA" "$TMP/e1copy/main.qui"
expect "K15 same content elsewhere, again" hit a "$QUIDRA" "$TMP/e1copy/main.qui"
expect "K15 spelled otherwise" build a "$QUIDRA" "$TMP/e1copy/../e1copy/main.qui"
(cd "$TMP/cwd" && expect "K23 first" build c "$QUIDRA" main.qui)
(cd "$TMP/cwd" && expect "K23 again" hit c "$QUIDRA" main.qui)
(cd "$TMP" && expect "K23 other cwd" build c "$QUIDRA" cwd/main.qui)

# ---- stale: K16 ------------------------------------------------------------------

expect "K16 local import" build one "$QUIDRA" "$TMP/e2/main.qui"
expect "K16 local import, again" hit one "$QUIDRA" "$TMP/e2/main.qui"
edit "$TMP/e2/util.qui" 's/"one"/"two"/'
expect "K16 local import content" build two "$QUIDRA" "$TMP/e2/main.qui"

(cd "$TMP/rootA" && expect "K16 @/ import" build A "$QUIDRA" "$TMP/e3/main.qui")
(cd "$TMP/rootA" && expect "K16 @/ import, again" hit A "$QUIDRA" "$TMP/e3/main.qui")
(cd "$TMP/rootB" && expect "K16 @/ import, other cwd" build B "$QUIDRA" "$TMP/e3/main.qui")

export QUIDRA_PACKAGE_PATH="$TMP/roots/one:$TMP/roots/two"
expect "K16 package" build two "$QUIDRA" "$TMP/e4/main.qui"
expect "K16 package, again" hit two "$QUIDRA" "$TMP/e4/main.qui"
mkdir -p "$TMP/roots/one/pk"
printf 'string word()\n    return "one"\n' > "$TMP/roots/one/pk/main.qui"
expect "K16 package in an earlier root" build one "$QUIDRA" "$TMP/e4/main.qui"
unset QUIDRA_PACKAGE_PATH
expect "K16 package store" build h1 env HOME="$TMP/h1" "$QUIDRA" "$TMP/e4/main.qui"
expect "K16 package store, again" hit h1 env HOME="$TMP/h1" "$QUIDRA" "$TMP/e4/main.qui"
expect "K16 HOME at another store" build h2 env HOME="$TMP/h2" "$QUIDRA" "$TMP/e4/main.qui"
expect "K16 package moved to a path root" build one \
    env HOME="$TMP/h1" QUIDRA_PACKAGE_PATH="$TMP/roots/one" "$QUIDRA" "$TMP/e4/main.qui"

# ---- stale: K17, K14, K13, K3 ----------------------------------------------------------

# native_row COPY NAME CHANGE EXPECTED: a build and a hit of the native
# package's program with package copy COPY, CHANGE, then a build.
native_row() {
    local copy="$1" name="$2" change="$3" expected="$4" package="$TMP/natpk$1/np"
    expect "K17 $name, first" build 42 env QUIDRA_PACKAGE_PATH="$TMP/natpk$copy" "$QUIDRA" "$TMP/e5/main.qui"
    expect "K17 $name, again" hit 42 env QUIDRA_PACKAGE_PATH="$TMP/natpk$copy" "$QUIDRA" "$TMP/e5/main.qui"
    eval "$change"
    expect "K17 $name" build "$expected" env QUIDRA_PACKAGE_PATH="$TMP/natpk$copy" "$QUIDRA" "$TMP/e5/main.qui"
}
native_row 1 "package-local header" 'edit "$package/native/answer.h" "s/NP_ANSWER 40/NP_ANSWER 41/"' 43
native_row 2 "native source" \
    'edit "$package/native/bridge.c" "s/NP_ANSWER + 0/NP_ANSWER - 1/"' 41
native_row 3 "prebuilt library" \
    'printf "long long np_extra(void) { return 3; }\n" > "$package/lib/extra.c" &&
     "$REAL_CLANG" -x c -O2 -c "$package/lib/extra.c" -o "$package/lib/extra.o" &&
     rm "$package/lib/libextra.a" && ar rcs "$package/lib/libextra.a" "$package/lib/extra.o"' 43
native_row 4 "package source" \
    'edit "$package/main.qui" "s/np_answer() + np_extra()/np_answer() - np_extra()/"' 38
native_row 5 "manifest" \
    'edit "$package/quidra.package" "s/version = 0.1.0/version = 0.1.1/"' 42
native_row 6 "project.toml created" \
    'printf "[package]\nname = \"np\"\nimport = \"np\"\ndisplay_name = \"NP\"\nversion = \"0.1.0\"\nrepository = \"https://example.invalid/np\"\n" > "$package/project.toml"' 42
native_row 7 "shadowing header (K12)" \
    'mkdir -p "$package/native/quidra" && cp "$ROOT/include/quidra/native_extension.h" "$package/native/quidra/"' 42

expect "K14 include tree" build a env QUIDRA_NATIVE_INCLUDE_DIR="$ROOT/include" "$QUIDRA" "$TMP/e1copy/main.qui"
expect "K14 include tree, again" hit a env QUIDRA_NATIVE_INCLUDE_DIR="$ROOT/include" "$QUIDRA" "$TMP/e1copy/main.qui"
expect "K14 another include tree" build a env QUIDRA_NATIVE_INCLUDE_DIR="$TMP/include2" "$QUIDRA" "$TMP/e1copy/main.qui"

expect "K13 runtime copy" build a env QUIDRA_RUNTIME_LIBRARY="$RUNTIME_COPY" "$QUIDRA" "$TMP/e1copy/main.qui"
expect "K13 runtime copy, again" hit a env QUIDRA_RUNTIME_LIBRARY="$RUNTIME_COPY" "$QUIDRA" "$TMP/e1copy/main.qui"
printf 'int run_cache_test_member;\n' > "$TMP/runtime/member.c"
"$REAL_CLANG" -x c -c "$TMP/runtime/member.c" -o "$TMP/runtime/member.o"
ar rs "$RUNTIME_COPY" "$TMP/runtime/member.o" 2>/dev/null
expect "K13 runtime member added" build a env QUIDRA_RUNTIME_LIBRARY="$RUNTIME_COPY" "$QUIDRA" "$TMP/e1copy/main.qui"

compiler_env=(env QUIDRA_RUNTIME_LIBRARY="$RUNTIME_LIBRARY" QUIDRA_NATIVE_INCLUDE_DIR="$ROOT/include")
expect "K3 compiler" build a "${compiler_env[@]}" "$QUIDRA" "$TMP/e1copy/main.qui"
expect "K3 compiler, again" hit a "${compiler_env[@]}" "$QUIDRA" "$TMP/e1copy/main.qui"
expect "K3 identical copy" hit a "${compiler_env[@]}" "$TMP/q-same/quidra" "$TMP/e1copy/main.qui"
expect "K3 changed copy" build a "${compiler_env[@]}" "$TMP/q-changed/quidra" "$TMP/e1copy/main.qui"

# ---- stale: K19, K18, K11 ---------------------------------------------------------------

(
    cd "$TMP/e6"
    expect "K19 link" build 30 "$QUIDRA" run main.qui --link one.c --link two.c
    expect "K19 link, again" hit 30 "$QUIDRA" run main.qui --link one.c --link two.c
    expect "K19 link order" build 30 "$QUIDRA" run main.qui --link two.c --link one.c
    edit one.c 's/return 10/return 11/'
    expect "K19 link content" build 31 "$QUIDRA" run main.qui --link one.c --link two.c
)

(
    cd "$TMP/lockproj"
    export QUIDRA_PACKAGE_PATH="$TMP/locked"
    expect "K18 no lock" build locked "$QUIDRA" main.qui
    expect "K18 no lock, again" hit locked "$QUIDRA" main.qui
    "$QUIDRA" lock main.qui >/dev/null
    expect "K18 lock created" build locked "$QUIDRA" main.qui
    printf 'changed\n' > "$TMP/locked/pk/notes.txt"
    set +e
    "$QUIDRA" main.qui > "$TMP/lock.out" 2> "$TMP/lock.err"
    status=$?
    set -e
    [[ "$status" == 1 ]] && grep -q PACKAGE_LOCK_MISMATCH "$TMP/lock.err" ||
        fail "K18 a file changed inside a locked package fails as without the cache"
    "$QUIDRA" lock main.qui >/dev/null
    expect "K18 lock changed" build locked "$QUIDRA" main.qui
    rm quidra.lock
    # The state of the first run again: its entry still holds.
    expect "K18 lock removed" hit locked "$QUIDRA" main.qui
)

(
    export QUIDRA_PACKAGE_PATH="$TMP/pcpk" QUIDRA_PKG_CONFIG="$TMP/pc/pkg-config"
    expect "K11 pkg-config" build 1 "$QUIDRA" "$TMP/e7/main.qui"
    expect "K11 pkg-config, again" hit 1 "$QUIDRA" "$TMP/e7/main.qui"
    printf -- '-DPC_VALUE=2\n' > "$TMP/pcdata/cflags"
    expect "K11 pkg-config output" build 2 "$QUIDRA" "$TMP/e7/main.qui"
)

# ---- stale: K8, K9, K21, K22, K7 -------------------------------------------------------

expect "K8 other driver path" build a env QUIDRA_CLANGXX="$TMP/bin2/clang++" "$QUIDRA" "$TMP/e1copy/main.qui"
expect "K8 other driver path, again" hit a env QUIDRA_CLANGXX="$TMP/bin2/clang++" "$QUIDRA" "$TMP/e1copy/main.qui"
printf '# changed\n' >> "$TMP/bin2/clang++"
expect "K8 driver changed" build a env QUIDRA_CLANGXX="$TMP/bin2/clang++" "$QUIDRA" "$TMP/e1copy/main.qui"
expect "K8 driver 3" build a env QUIDRA_CLANGXX="$TMP/bin3/clang++" "$QUIDRA" "$TMP/e1copy/main.qui"
expect "K8 driver 3, again" hit a env QUIDRA_CLANGXX="$TMP/bin3/clang++" "$QUIDRA" "$TMP/e1copy/main.qui"
touch "$TMP/bin3/clang++.cfg"
expect "K8 configuration file beside the driver" build a \
    env QUIDRA_CLANGXX="$TMP/bin3/clang++" "$QUIDRA" "$TMP/e1copy/main.qui"

expect "K21 discovered driver" build a \
    env -u QUIDRA_CLANGXX PATH="$TMP/pathA:$PATH" "$QUIDRA" "$TMP/e1copy/main.qui"
expect "K21 discovered driver, again" hit a \
    env -u QUIDRA_CLANGXX PATH="$TMP/pathA:$PATH" "$QUIDRA" "$TMP/e1copy/main.qui"
expect "K21 PATH finds another clang first" build a \
    env -u QUIDRA_CLANGXX PATH="$TMP/pathB:$TMP/pathA:$PATH" "$QUIDRA" "$TMP/e1copy/main.qui"
expect "K21 value variable" build a env CPATH="$TMP/empty" "$QUIDRA" "$TMP/e1copy/main.qui"
expect "K21 value variable, again" hit a env CPATH="$TMP/empty" "$QUIDRA" "$TMP/e1copy/main.qui"
expect "K21 value variable empty" build a env CPATH= "$QUIDRA" "$TMP/e1copy/main.qui"
if [[ "$(uname -s)" == Darwin ]]; then
    target="$(sw_vers -productVersion | cut -d. -f1).0"
    expect "K22 deployment target" build a \
        env MACOSX_DEPLOYMENT_TARGET="$target" "$QUIDRA" "$TMP/e1copy/main.qui"
    sdk="$(xcrun --show-sdk-path)"
    other_sdk=""
    for candidate in "$(dirname "$sdk")"/MacOSX*.sdk; do
        [[ "$(realpath "$candidate")" != "$(realpath "$sdk")" ]] && other_sdk="$candidate" && break
    done
    # Another installed SDK, where an uncached build can use it.
    if [[ -n "$other_sdk" ]] &&
        SDKROOT="$other_sdk" "$QUIDRA" --no-cache "$TMP/e1copy/main.qui" >/dev/null 2>&1; then
        expect "K7 another SDK" build a env SDKROOT="$other_sdk" "$QUIDRA" "$TMP/e1copy/main.qui"
    fi
fi
if [[ "$FULL" == --full ]]; then
    # Every `value` variable of the environment table, set.
    while read -r variable; do
        value="$TMP/empty"
        case "$variable" in
            QUIDRA_CLANGXX|QUIDRA_CLANG) value="$TMP/bin/clang++" ;;
            QUIDRA_RUNTIME_LIBRARY) value="$RUNTIME_LIBRARY" ;;
            QUIDRA_NATIVE_INCLUDE_DIR) value="$ROOT/include" ;;
            CCC_OVERRIDE_OPTIONS) value="#" ;;
            MACOSX_DEPLOYMENT_TARGET) value="$(sw_vers -productVersion 2>/dev/null | cut -d. -f1).0" ;;
            SDKROOT) value="$(xcrun --show-sdk-path 2>/dev/null || echo /)" ;;
            DEVELOPER_DIR) value="$(xcode-select -p 2>/dev/null || echo /)" ;;
            ZERO_AR_DATE) value=1 ;;
        esac
        expect "K21/K22 $variable" build a env "$variable=$value" "$QUIDRA" "$TMP/e1copy/main.qui"
    done < <(python3 - "$ROOT/docs/development.md" <<'PY'
import re, sys
for line in open(sys.argv[1], encoding="utf-8"):
    cells = [cell.strip() for cell in line.split("|")]
    if len(cells) > 4 and cells[-2] == "`value`":
        for name in re.findall(r"`([A-Za-z_][A-Za-z0-9_]*)`", cells[1]):
            if name.isupper() or name.startswith("Windows") or name.startswith("VC") \
                    or name.startswith("Universal") or name.startswith("UCRT"):
                print(name)
PY
)
fi

# ---- stale: build-time macros, compile errors, a missing toolchain -----------------------

expect "__TIME__ first" build 1 env QUIDRA_PACKAGE_PATH="$TMP/timepk" "$QUIDRA" "$TMP/e8/main.qui"
expect "__TIME__ never cached" build 1 env QUIDRA_PACKAGE_PATH="$TMP/timepk" "$QUIDRA" "$TMP/e8/main.qui"

count="$(entries)"
set +e
"$QUIDRA" "$TMP/e10/main.qui" > "$TMP/error.out" 2> "$TMP/error.err"
status=$?
"$QUIDRA" --no-cache "$TMP/e10/main.qui" > "$TMP/error-uncached.out" 2> "$TMP/error-uncached.err"
uncached_status=$?
set -e
[[ "$status" == 1 && "$uncached_status" == 1 ]] || fail "a compile error exits 1"
cmp -s "$TMP/error.err" "$TMP/error-uncached.err" || fail "a compile error prints what it prints uncached"
[[ "$(entries)" == "$count" ]] || fail "a compile error stores nothing"
printf 'int x = 7\nprint(x)\n' > "$TMP/e10/main.qui"
expect "compile error fixed" build 7 "$QUIDRA" "$TMP/e10/main.qui"

set +e
QUIDRA_CLANGXX=/definitely/not-a-clang "$QUIDRA" "$TMP/e1copy/main.qui" > "$TMP/gone.out" 2> "$TMP/gone.err"
status=$?
QUIDRA_CLANGXX=/definitely/not-a-clang "$QUIDRA" --no-cache "$TMP/e1copy/main.qui" > "$TMP/gone-uncached.out" 2> "$TMP/gone-uncached.err"
uncached_status=$?
set -e
[[ "$status" == 1 && "$uncached_status" == 1 ]] || fail "a missing toolchain exits 1"
cmp -s "$TMP/gone.err" "$TMP/gone-uncached.err" || fail "a missing toolchain prints what it prints uncached"

# ---- build: the cached program is what `quidra build` writes ------------------------------

expect "build first" build built "$QUIDRA" "$TMP/b1/main.qui"
cached_program="$(dirname "$(grep -l "\"$TMP/b1/main.qui\"" $(find "$QUIDRA_CACHE_DIR/run" -name metadata.json))")/program"
mkdir -p "$TMP/b1/out"
"$QUIDRA" build "$TMP/b1/main.qui" -o "$TMP/b1/out/program"
cmp "$cached_program" "$TMP/b1/out/program" || fail "the cached program differs from quidra build's"

# ---- transparent ------------------------------------------------------------------------

for example in hello functions logic modules/main; do
    source="$ROOT/examples/$example.qui"
    [[ -f "$source" ]] || continue
    for pass in cold warm uncached; do
        cache="$TMP/transparent-cache"
        [[ "$pass" == uncached ]] && cache=/dev/null/x
        set +e
        (cd "$TMP" && QUIDRA_CACHE_DIR="$cache" "$QUIDRA" "$source" > "$TMP/t-$pass.out" 2> "$TMP/t-$pass.err")
        echo $? > "$TMP/t-$pass.status"
        set -e
    done
    for pass in warm uncached; do
        for stream in out err status; do
            cmp -s "$TMP/t-cold.$stream" "$TMP/t-$pass.$stream" ||
                fail "transparent: $example $stream differs between cold and $pass"
        done
    done
done

# ---- replay: toolchain warnings ------------------------------------------------------------

export QUIDRA_PACKAGE_PATH="$TMP/warnpk"
"$QUIDRA" "$TMP/e9/main.qui" > "$TMP/warn-miss.out" 2> "$TMP/warn-miss.err"
before="$(builds)"
"$QUIDRA" "$TMP/e9/main.qui" > "$TMP/warn-hit.out" 2> "$TMP/warn-hit.err"
[[ "$(builds)" == "$before" ]] || fail "replay: the second run is not a hit"
"$QUIDRA" --no-cache "$TMP/e9/main.qui" > "$TMP/warn-uncached.out" 2> "$TMP/warn-uncached.err"
grep -q "replayed toolchain warning" "$TMP/warn-miss.err" || fail "replay: no warning on the build"
for run in hit uncached; do
    cmp -s "$TMP/warn-miss.out" "$TMP/warn-$run.out" || fail "replay: stdout of the $run differs"
    cmp -s "$TMP/warn-miss.err" "$TMP/warn-$run.err" || fail "replay: stderr of the $run differs"
done
unset QUIDRA_PACKAGE_PATH

# ---- corrupt -------------------------------------------------------------------------------

entry_of() {
    dirname "$(grep -l "\"$1\"" $(find "$QUIDRA_CACHE_DIR/run" -name metadata.json))"
}

# corrupt NAME DAMAGE: a cached run of $TMP/NAME, the damage, then a build
# with the right output and a hit.
corrupt() {
    local name="$1" damage="$2" entry
    expect "corrupt $name first" build "$name" "$QUIDRA" "$TMP/$name/main.qui"
    entry="$(entry_of "$TMP/$name/main.qui")"
    eval "$damage"
    expect "corrupt $name: $damage" build "$name" "$QUIDRA" "$TMP/$name/main.qui"
    expect "corrupt $name: replaced" hit "$name" "$QUIDRA" "$TMP/$name/main.qui"
}
corrupt c1 'rm "$entry/metadata.json"'
corrupt c2 'printf "{not json" > "$entry/metadata.json"'
corrupt c3 'python3 - "$entry/metadata.json" <<'"'"'PY'"'"'
import json, sys
path = sys.argv[1]
data = json.load(open(path))
data["host"]["os"] += " (edited)"
open(path, "w").write(json.dumps(data))
PY'
corrupt c4 'rm "$entry/program"'
corrupt c5 'printf x >> "$entry/program"'
corrupt c6 'python3 - "$entry/program" <<'"'"'PY'"'"'
import sys
path = sys.argv[1]
data = bytearray(open(path, "rb").read())
data[len(data) // 2] ^= 0xff
open(path, "wb").write(bytes(data))
PY'
corrupt c7 'mv "$entry/program" "$entry/program.real" && ln -s program.real "$entry/program"'
corrupt c8 'rm -f "$entry/build.log"'
# A program that cannot be executed, recorded as intact: the run discards
# it and builds once more.
corrupt c9 'printf "not a program" > "$entry/program" && chmod 600 "$entry/program" && python3 - "$entry/metadata.json" "$entry/program" <<'"'"'PY'"'"'
import hashlib, json, sys
path, program = sys.argv[1], sys.argv[2]
data = json.load(open(path))
content = open(program, "rb").read()
data["program"]["size"] = len(content)
data["program"]["sha256"] = hashlib.sha256(content).hexdigest()
open(path, "w").write(json.dumps(data, separators=(",", ":")))
PY'
# The key does not match the directory: the entry moved under another name
# and the index names it.
corrupt c10 'other="$(dirname "$entry")/$(printf "%064d" 0)"; mv "$entry" "$other"; for index in "$(dirname "$entry")"/index/*.json; do sed -i.bak "s/$(basename "$entry")/$(basename "$other")/" "$index"; rm -f "$index.bak"; done'
# A missing or invalid index is a miss and is written again.
expect "index first" build c11 "$QUIDRA" "$TMP/c11/main.qui"
c11_entry="$(entry_of "$TMP/c11/main.qui")"
c11_index="$(grep -l "$(basename "$c11_entry")" "$(dirname "$c11_entry")"/index/*.json)"
rm "$c11_index"
expect "index missing" build c11 "$QUIDRA" "$TMP/c11/main.qui"
expect "index rewritten" hit c11 "$QUIDRA" "$TMP/c11/main.qui"
printf garbage > "$c11_index"
expect "index invalid" build c11 "$QUIDRA" "$TMP/c11/main.qui"
expect "index invalid, rewritten" hit c11 "$QUIDRA" "$TMP/c11/main.qui"

# ---- perm -----------------------------------------------------------------------------------

expect "perm before" build p0 "$QUIDRA" "$TMP/p0/main.qui"
chmod 0777 "$QUIDRA_CACHE_DIR"
expect "perm 0777 root" hit p0 "$QUIDRA" "$TMP/p0/main.qui"
# GNU stat -f can print filesystem information before failing on the
# BSD "%Lp" operand, polluting command-substitution output. Read the
# POSIX permission bits directly instead of mixing the two stat dialects.
python3 -c 'import os, stat, sys; assert stat.S_IMODE(os.stat(sys.argv[1]).st_mode) == 0o700' \
    "$QUIDRA_CACHE_DIR" || fail "perm: a 0777 root is reset to 0700"
(
    export QUIDRA_CACHE_DIR="$TMP/linked-cache"
    expect "perm symlink first" build p1 "$QUIDRA" "$TMP/p1/main.qui"
    mv "$QUIDRA_CACHE_DIR/run" "$TMP/run-elsewhere"
    ln -s "$TMP/run-elsewhere" "$QUIDRA_CACHE_DIR/run"
    expect "perm symlink inside the root" build p1 "$QUIDRA" "$TMP/p1/main.qui"
    expect "perm symlink inside the root, uncached" build p1 "$QUIDRA" "$TMP/p1/main.qui"
)
for root in /dev/null/x "$TMP/p1/main.qui"; do
    expect "perm unusable root $root" build p2 env QUIDRA_CACHE_DIR="$root" "$QUIDRA" "$TMP/p2/main.qui"
    expect "perm unusable root $root, uncached" build p2 env QUIDRA_CACHE_DIR="$root" "$QUIDRA" "$TMP/p2/main.qui"
done

# ---- concurrent -----------------------------------------------------------------------------

pids=()
for index in 1 2 3 4 5 6 7 8; do
    "$QUIDRA" "$TMP/conc/main.qui" > "$TMP/conc/out-$index" 2> "$TMP/conc/err-$index" &
    pids+=("$!")
done
for pid in "${pids[@]}"; do wait "$pid" || fail "concurrent: a cold run failed"; done
for index in 1 2 3 4 5 6 7 8; do
    [[ "$(cat "$TMP/conc/out-$index")" == parallel ]] || fail "concurrent: run $index printed the wrong output"
done
[[ "$(grep -l "\"$TMP/conc/main.qui\"" $(find "$QUIDRA_CACHE_DIR/run" -name metadata.json) | wc -l | tr -d ' ')" == 1 ]] ||
    fail "concurrent: 8 cold runs leave exactly one entry"
[[ -z "$(ls -A "$QUIDRA_CACHE_DIR/staging")" ]] || fail "concurrent: staging left behind"
expect "concurrent: then a hit" hit parallel "$QUIDRA" "$TMP/conc/main.qui"

expect "edit in flight, first" build v1 "$QUIDRA" "$TMP/edit/main.qui"
pids=()
for index in 1 2 3 4; do
    "$QUIDRA" "$TMP/edit/main.qui" > "$TMP/edit/out-$index" 2>/dev/null &
    pids+=("$!")
    if [[ "$index" == 2 ]]; then
        printf 'print("v2")\nprint(NL)\n' > "$TMP/edit/next.qui"
        mv "$TMP/edit/next.qui" "$TMP/edit/main.qui"
    fi
done
for pid in "${pids[@]}"; do wait "$pid" || fail "edit in flight: a run failed"; done
for index in 1 2 3 4; do
    output="$(cat "$TMP/edit/out-$index")"
    [[ "$output" == v1 || "$output" == v2 ]] || fail "edit in flight: run $index printed '$output'"
done
expect "edit in flight, after" any v2 "$QUIDRA" "$TMP/edit/main.qui"

# ---- no-cache -------------------------------------------------------------------------------

count="$(entries)"
expect "--no-cache before the file" build a "$QUIDRA" --no-cache "$TMP/e1copy/main.qui"
expect "--no-cache after run's file" build a "$QUIDRA" run "$TMP/e1copy/main.qui" --no-cache
[[ "$(entries)" == "$count" ]] || fail "--no-cache stores nothing"
if find "$TMP/e1copy" -maxdepth 1 -name '.quidra-run-*' -print -quit | grep -q .; then
    fail "a run left a temporary artifact beside the source"
fi

# ---- clean ------------------------------------------------------------------------------------

expect "clean: a cached program" build k1 "$QUIDRA" "$TMP/k1/main.qui"
expect "clean: cached" hit k1 "$QUIDRA" "$TMP/k1/main.qui"
mkdir -p "$QUIDRA_CACHE_DIR/staging/dead"
: > "$QUIDRA_CACHE_DIR/staging/dead.lease"
count="$(entries)"
output="$("$QUIDRA" cache clean)"
[[ "$output" =~ ^removed\ $count\ entr(y|ies)\ \([0-9]+\.[0-9]\ MiB\)$ ]] || fail "clean printed '$output'"
[[ "$(entries)" == 0 ]] || fail "clean leaves no entry"
[[ ! -e "$QUIDRA_CACHE_DIR/staging/dead" && ! -e "$QUIDRA_CACHE_DIR/staging/dead.lease" ]] ||
    fail "clean removes dead staging"
[[ ! -e "$QUIDRA_CACHE_DIR/compiler-id" ]] || fail "clean removes the build id memos"
[[ "$("$QUIDRA" cache clean)" == "removed 0 entries (0.0 MiB)" ]] || fail "a second clean removes nothing"
expect "clean: after a clean" build k1 "$QUIDRA" "$TMP/k1/main.qui"
expect "clean: cached again" hit k1 "$QUIDRA" "$TMP/k1/main.qui"

# Runs (hits) and builds in flight while the cache is cleaned.
pids=()
for index in 1 2 3 4 5 6; do
    case "$index" in
        1|2|3) source="$TMP/k1/main.qui"; expected=k1 ;;
        4) source="$TMP/k2/main.qui"; expected=k2 ;;
        5) source="$TMP/k3/main.qui"; expected=k3 ;;
        6) source="$TMP/k4/main.qui"; expected=k4 ;;
    esac
    ( output="$("$QUIDRA" "$source" 2>"$TMP/flight-$index.err")" && [[ "$output" == "$expected" ]] ) &
    pids+=("$!")
    if [[ "$index" == 3 || "$index" == 5 ]]; then
        "$QUIDRA" cache clean > "$TMP/flight-clean-$index.out" || fail "clean in flight failed"
    fi
done
for pid in "${pids[@]}"; do wait "$pid" || fail "a run in flight of a clean failed"; done
for index in 1 2 3 4 5 6; do
    [[ ! -s "$TMP/flight-$index.err" ]] || fail "a run in flight of a clean printed: $(cat "$TMP/flight-$index.err")"
done
[[ -z "$(ls -A "$QUIDRA_CACHE_DIR/staging")" ]] || fail "clean in flight: staging left behind"
expect "clean in flight, after" any k2 "$QUIDRA" "$TMP/k2/main.qui"

set +e
QUIDRA_CACHE_DIR="$TMP/k1/main.qui" "$QUIDRA" cache clean > "$TMP/refused.out" 2> "$TMP/refused.err"
status=$?
QUIDRA_CACHE_DIR="$TMP/no-such-cache" "$QUIDRA" cache clean > "$TMP/absent.out" 2>&1
absent_status=$?
"$QUIDRA" cache > /dev/null 2>&1
usage_status=$?
set -e
[[ "$status" == 1 && ! -s "$TMP/refused.out" ]] || fail "clean of an unusable cache directory exits 1"
grep -q "cannot clean the run cache" "$TMP/refused.err" || fail "clean says why it refuses"
[[ "$absent_status" == 0 && "$(cat "$TMP/absent.out")" == "removed 0 entries (0.0 MiB)" &&
   ! -e "$TMP/no-such-cache" ]] || fail "clean of a missing cache directory creates nothing"
[[ "$usage_status" == 2 ]] || fail "quidra cache without clean is a usage error"

if [[ -s "$TMP/failures" ]]; then
    echo "$(wc -l < "$TMP/failures" | tr -d ' ') run cache check(s) failed" >&2
    exit 1
fi
echo "run cache tests passed"
