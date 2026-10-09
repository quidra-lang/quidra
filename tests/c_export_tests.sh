#!/usr/bin/env bash
# Exported functions (export "C", docs/spec/language.md, "C ABI export")
# called from C: the values of every exported scalar type cross the boundary
# unchanged in both directions; C can call back into Quidra while Quidra code
# runs (export -> extern C -> export); host threads call an export
# concurrently; a runtime failure inside an exported function is reported at
# its Quidra source and ends the process with status 101 before control
# returns to C; and an exported function that leaves a failing deferred GPU
# check behind reports it before it returns. The C side is compiled with
# ${CC:-cc} and linked into the program with `quidra build --link`, or, for a
# library build (`quidra build --lib`), is a program with its own main that
# links the archive and the flags --print-link-flags prints.
set -euo pipefail

QUIDRA="$1"
CC="${CC:-cc}"
HELPER="$(cd "$(dirname "$0")" && pwd)/runtime_report.py"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/quidra-cache"  # a run cache of this suite run only

failures=0
fail() { echo "FAIL $*"; failures=$((failures + 1)); }

# compile NAME: $TMP/NAME.c into $TMP/NAME.o.
compile() {
    "$CC" -std=c11 -O1 -c "$TMP/$1.c" -o "$TMP/$1.o" 2> "$TMP/$1.cc.err" ||
        { cat "$TMP/$1.cc.err"; return 1; }
}

# build NAME [quidra args...]: builds $TMP/NAME.qui into $TMP/NAME.
build() {
    local name=$1
    shift
    "$QUIDRA" build "$TMP/$name.qui" -o "$TMP/$name" "$@" > "$TMP/$name.build" 2>&1 ||
        { cat "$TMP/$name.build"; return 1; }
}

# run NAME: runs $TMP/NAME from $TMP; sets rc.
run() {
    rc=0
    (cd "$TMP" && perl -e 'alarm shift; exec @ARGV' 120 "./$1" > "$TMP/$1.out" 2> "$TMP/$1.err") || rc=$?
}

cat > "$TMP/exports.qui" <<'QUI'
export "C" int8 echo_int8(int8 value)
    return value
export "C" int16 echo_int16(int16 value)
    return value
export "C" int32 echo_int32(int32 value)
    return value
export "C" int64 echo_int64(int64 value)
    return value
export "C" nat8 echo_nat8(nat8 value)
    return value
export "C" nat16 echo_nat16(nat16 value)
    return value
export "C" nat32 echo_nat32(nat32 value)
    return value
export "C" nat64 echo_nat64(nat64 value)
    return value
export "C" real32 echo_real32(real32 value)
    return value
export "C" real64 echo_real64(real64 value)
    return value
export "C" int8 low_int8(int16 value)
    return int8(value + int16(100))
export "C" nat16 high_nat16(nat8 value)
    return nat16(value) * nat16(257)
export "C" real64 add_real64(real64 a, real64 b)
    return a + b
export "C" int32 add2(int32 a, int32 b)
    return a + b
export "C" int64 factorial(int64 n)
    if n <= 1
        return 1
    return n * factorial(n - 1)
export "C" void say(int32 value)
    print(value)
    print(NL)

extern int32 c_twice(int32 value) = "c_twice"
export "C" int32 through_c(int32 value)
    return c_twice(value) + 1

extern int32 run_driver() = "run_driver"
int32 failed = run_driver()
print("failed checks: {failed}")
print(NL)
QUI
cat > "$TMP/driver.c" <<'C'
#include <math.h>
#include <pthread.h>
#include <stdint.h>

int8_t echo_int8(int8_t);
int16_t echo_int16(int16_t);
int32_t echo_int32(int32_t);
int64_t echo_int64(int64_t);
uint8_t echo_nat8(uint8_t);
uint16_t echo_nat16(uint16_t);
uint32_t echo_nat32(uint32_t);
uint64_t echo_nat64(uint64_t);
float echo_real32(float);
double echo_real64(double);
int8_t low_int8(int16_t);
uint16_t high_nat16(uint8_t);
double add_real64(double, double);
int32_t add2(int32_t, int32_t);
int64_t factorial(int64_t);
void say(int32_t);
int32_t through_c(int32_t);

/* Called by Quidra code (extern); calls back into an export. */
int32_t c_twice(int32_t value) { return add2(value, value); }

static void* hammer(void* argument) {
    int64_t* sum = argument;
    for (int32_t i = 0; i < 100000; ++i) *sum += add2(i, 1);
    return 0;
}

int32_t run_driver(void) {
    int32_t failed = 0;
    failed += echo_int8(-128) != -128;
    failed += echo_int8(127) != 127;
    failed += echo_int16(-32768) != -32768;
    failed += echo_int16(32767) != 32767;
    failed += echo_int32(INT32_MIN) != INT32_MIN;
    failed += echo_int32(INT32_MAX) != INT32_MAX;
    failed += echo_int64(INT64_MIN) != INT64_MIN;
    failed += echo_int64(INT64_MAX) != INT64_MAX;
    failed += echo_nat8(255) != 255;
    failed += echo_nat16(65535) != 65535;
    failed += echo_nat32(4294967295u) != 4294967295u;
    failed += echo_nat64(UINT64_MAX) != UINT64_MAX;
    failed += echo_real32(1.5f) != 1.5f;
    float negative_zero = echo_real32(-0.0f);
    failed += negative_zero != 0.0f || !signbit(negative_zero);
    failed += echo_real32(1e-45f) != 1e-45f;
    failed += echo_real64(0.1) != 0.1;
    failed += echo_real64(5e-324) != 5e-324;
    /* Narrow results arrive extended in a full register. */
    volatile int16_t wide = -199;
    int32_t low = low_int8(wide);
    failed += low != -99;
    volatile uint8_t byte = 255;
    uint32_t high = high_nat16(byte);
    failed += high != 65535u;
    failed += add_real64(0.1, 0.2) != 0.1 + 0.2;
    failed += factorial(20) != 2432902008176640000LL;
    say(42);
    /* Reentrancy: export -> extern C -> export. */
    failed += through_c(20) != 41;
    /* Two host threads call an export concurrently. */
    pthread_t first, second;
    int64_t first_sum = 0, second_sum = 0;
    pthread_create(&first, 0, hammer, &first_sum);
    pthread_create(&second, 0, hammer, &second_sum);
    pthread_join(first, 0);
    pthread_join(second, 0);
    const int64_t expected = 100000LL * 100001LL / 2;
    failed += first_sum != expected || second_sum != expected;
    return failed;
}
C
if compile driver && build exports --link "$TMP/driver.o"; then
    run exports
    if [[ "$rc" -ne 0 ]]; then
        fail "exports: exit status $rc"; cat "$TMP/exports.err"
    elif [[ "$(cat "$TMP/exports.out")" != $'42\nfailed checks: 0' ]]; then
        fail "exports: unexpected output"; cat "$TMP/exports.out"
    fi
else
    fail "exports: build failed"
fi

# A failure inside an exported function ends the process at its Quidra
# source; the C code after the call never runs.
cat > "$TMP/overflow.qui" <<'QUI'
export "C" int32 add2(int32 a, int32 b)
    return a + b

extern void run_driver() = "run_driver"
run_driver()
QUI
cat > "$TMP/overflow_driver.c" <<'C'
#include <stdint.h>
#include <stdio.h>
int32_t add2(int32_t, int32_t);
void run_driver(void) {
    int32_t value = add2(INT32_MAX, 1);
    printf("returned %d\n", value);
}
C
if compile overflow_driver && build overflow --link "$TMP/overflow_driver.o"; then
    run overflow
    if [[ "$rc" -ne 101 ]]; then
        fail "overflow: exit status $rc, expected 101"; cat "$TMP/overflow.err"
    elif grep -q returned "$TMP/overflow.out"; then
        fail "overflow: control returned to C"
    elif ! python3 "$HELPER" "$TMP/overflow.err" kind=runtime_error code=INTEGER_OVERFLOW \
            'file=/overflow\.qui$/' line=2 snippet=yes; then
        fail "overflow: report"; cat "$TMP/overflow.err"
    fi
else
    fail "overflow: build failed"
fi

# An exported function that leaves a failing deferred GPU check behind
# reports it before it returns to C (Metal; skipped without a real GPU).
if "$QUIDRA" gpu 2>/dev/null | grep -Fq "backend: Metal"; then
    cat > "$TMP/deferred.qui" <<'QUI'
export "C" void overflow_on_gpu()
    tensor<int8> value = tensor.ones<int8>([1], gpu = 0) * int8(127)
    tensor<int8> invalid = value + int8(1)

extern void run_driver() = "run_driver"
run_driver()
QUI
    cat > "$TMP/deferred_driver.c" <<'C'
#include <stdio.h>
void overflow_on_gpu(void);
void run_driver(void) {
    overflow_on_gpu();
    printf("returned\n");
}
C
    if compile deferred_driver && build deferred --link "$TMP/deferred_driver.o"; then
        run deferred
        if [[ "$rc" -ne 101 ]]; then
            fail "deferred: exit status $rc, expected 101"; cat "$TMP/deferred.err"
        elif grep -q returned "$TMP/deferred.out"; then
            fail "deferred: control returned to C before the deferred check"
        elif ! grep -Fq "tensor integer arithmetic overflow (deferred GPU check from" "$TMP/deferred.err"; then
            fail "deferred: report"; cat "$TMP/deferred.err"
        fi
    else
        fail "deferred: build failed"
    fi
else
    echo "c_export_tests: deferred GPU check skipped (no Metal device)"
fi

# A library build: a C program with its own main links the archive that
# `quidra build --lib` writes and the flags it prints, and nothing else.
cat > "$TMP/add.qui" <<'QUI'
const int64 offset = 1

export "C" int32 add2(int32 a, int32 b)
    return a + b
export "C" int8 echo_int8(int8 value)
    return value
export "C" nat64 echo_nat64(nat64 value)
    return value
export "C" real32 echo_real32(real32 value)
    return value
export "C" real64 add_real64(real64 a, real64 b)
    return a + b
export "C" int64 next(int64 value)
    return value + 1
export "C" void say(int32 value)
    print(value)
    print(NL)

if main
    print("the guard ran")
    print(NL)
QUI
cat > "$TMP/host.c" <<'C'
#include <math.h>
#include <stdint.h>
#include <stdio.h>
int32_t add2(int32_t, int32_t);
int8_t echo_int8(int8_t);
uint64_t echo_nat64(uint64_t);
float echo_real32(float);
double add_real64(double, double);
int64_t next(int64_t);
void say(int32_t);
int main(void) {
    int failed = 0;
    failed += add2(2, 3) != 5;
    failed += echo_int8(-128) != -128;
    failed += echo_nat64(UINT64_MAX) != UINT64_MAX;
    float negative_zero = echo_real32(-0.0f);
    failed += negative_zero != 0.0f || !signbit(negative_zero);
    failed += echo_real32(1e-45f) != 1e-45f;
    failed += add_real64(0.1, 0.2) != 0.1 + 0.2;
    failed += next(INT64_MAX - 1) != INT64_MAX;
    say(42);
    fflush(stdout);
    printf("failed checks: %d\n", failed);
    return failed != 0;
}
C
if "$QUIDRA" build "$TMP/add.qui" --lib -o "$TMP/libadd.a" --print-link-flags \
        > "$TMP/flags" 2> "$TMP/lib.build"; then
    read -r -a link_flags < "$TMP/flags"
    if "$CC" -std=c11 "$TMP/host.c" "$TMP/libadd.a" "${link_flags[@]}" -o "$TMP/host" \
            2> "$TMP/host.link"; then
        run host
        if [[ "$rc" -ne 0 ]]; then
            fail "host: exit status $rc"; cat "$TMP/host.out" "$TMP/host.err"
        elif [[ "$(cat "$TMP/host.out")" != $'42\nfailed checks: 0' ]]; then
            fail "host: unexpected output (an if main guard must not run)"; cat "$TMP/host.out"
        fi
    else
        fail "host: link failed with: ${link_flags[*]}"; cat "$TMP/host.link"
    fi
    symbols=$(nm -g "$TMP/libadd.a" 2>/dev/null || true)
    if ! grep -Eq ' T _?add2$' <<< "$symbols"; then
        fail "libadd.a: no defined text symbol add2"
    fi
    if grep -Eq ' T _?main$' <<< "$symbols"; then
        fail "libadd.a: defines main"
    fi
else
    fail "library build failed"; cat "$TMP/lib.build"
fi

# A failure inside an exported function of the library ends the host at the
# Quidra source; the C code after the call never runs.
cat > "$TMP/failing_host.c" <<'C'
#include <stdint.h>
#include <stdio.h>
int32_t add2(int32_t, int32_t);
int main(void) {
    int32_t value = add2(INT32_MAX, 1);
    puts("returned");
    return value == 0;
}
C
if [[ -f "$TMP/libadd.a" ]] &&
   "$CC" -std=c11 "$TMP/failing_host.c" "$TMP/libadd.a" "${link_flags[@]}" -o "$TMP/failing_host" \
       2> "$TMP/failing_host.link"; then
    run failing_host
    if [[ "$rc" -ne 101 ]]; then
        fail "failing_host: exit status $rc, expected 101"; cat "$TMP/failing_host.err"
    elif grep -q returned "$TMP/failing_host.out"; then
        fail "failing_host: control returned to C"
    elif ! python3 "$HELPER" "$TMP/failing_host.err" kind=runtime_error code=INTEGER_OVERFLOW \
            'file=/add\.qui$/' line=4 snippet=yes; then
        fail "failing_host: report"; cat "$TMP/failing_host.err"
    fi
else
    fail "failing_host: link failed"; cat "$TMP/failing_host.link" 2>/dev/null || true
fi

# The root of a library holds declarations only, and exports something.
expect_build_error() {
    local name=$1 code=$2
    shift 2
    if "$QUIDRA" build "$TMP/$name.qui" --lib -o "$TMP/lib$name.a" "$@" > "$TMP/$name.build" 2>&1; then
        fail "$name: library build accepted"
    elif ! grep -Fq "error[$code]" "$TMP/$name.build"; then
        fail "$name: expected $code"; cat "$TMP/$name.build"
    fi
}
printf 'export "C" int32 one()\n    return 1\n\nprint(one())\n' > "$TMP/statement.qui"
expect_build_error statement LIBRARY_TOP_LEVEL
printf 'int32 one()\n    return 1\n' > "$TMP/no_export.qui"
expect_build_error no_export FFI_EXPORT
printf 'cli Options\n    int64 count = option(default = 1)\n\nexport "C" int32 one()\n    return 1\n' \
    > "$TMP/with_cli.qui"
expect_build_error with_cli LIBRARY_TOP_LEVEL

# --print-link-flags belongs to --lib; an unusable archiver fails the build.
if "$QUIDRA" build "$TMP/add.qui" --print-link-flags -o "$TMP/add" > "$TMP/flags.only" 2>&1 ||
   ! grep -Fq -- "--print-link-flags requires --lib" "$TMP/flags.only"; then
    fail "--print-link-flags without --lib"; cat "$TMP/flags.only"
fi
for archiver in "$TMP/missing-archiver" /usr/bin/false; do
    if QUIDRA_LIBTOOL="$archiver" QUIDRA_AR="$archiver" \
           "$QUIDRA" build "$TMP/add.qui" --lib -o "$TMP/libbroken.a" > "$TMP/broken.build" 2>&1; then
        fail "archiver $archiver: build accepted"
    elif ! grep -Fq "static archiver" "$TMP/broken.build" || [[ -e "$TMP/libbroken.a" ]]; then
        fail "archiver $archiver: message or partial archive"; cat "$TMP/broken.build"
    fi
done

if [[ "$failures" -ne 0 ]]; then
    echo "c_export_tests: $failures failure(s)"
    exit 1
fi
echo "c_export_tests: ok"
