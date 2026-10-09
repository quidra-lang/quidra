#!/usr/bin/env bash
# qcore_parallel_for end to end: a package kernel gives bitwise-identical
# results for every QUIDRA_CPU_THREADS value, reports the effective thread
# count (reduced to the hardware count), runs serially when nested or inside
# task.all, reports the lowest failing chunk, rejects invalid thread counts
# wherever the first call is made, allows only pure data-access qcore_* calls
# inside bodies (any other one is runtime error PARALLEL_BODY at every thread
# count), and links under AOT and JIT. A plain Quidra program can read the
# count through an extern binding.
set -euo pipefail

QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/quidra-cache"  # a run cache of this suite run only

fail() {
    echo "cpu parallel test failed: $*" >&2
    exit 1
}

mkdir -p "$TMP/packages/parallel_pkg/native"

cat > "$TMP/packages/parallel_pkg/quidra.package" <<'MANIFEST'
name = parallel_pkg
version = 0.1.0
native.source.bridge = native/bridge.cpp
MANIFEST

cat > "$TMP/packages/parallel_pkg/main.qui" <<'QUI'
extern int32 fill_native(
    tensor<real32> &value,
    int64 seed
) = "qtest_parallel_fill"
extern int32 kernel_native(
    const tensor<real32> &input,
    tensor<real32> &output,
    int64 grain
) = "qtest_parallel_kernel"
extern int64 checksum_native(
    const tensor<real32> &value
) = "qtest_parallel_checksum"
extern int64 threads_native() = "qtest_parallel_threads"
extern int64 failure_native(int64 status_chunk, int64 throw_chunk) = "qtest_parallel_failure"
extern int64 body_calls_native(
    const tensor<real32> &value,
    int64 kind
) = "qtest_parallel_body_calls"

// Checksum of a parallel float32 kernel over 40000 independent outputs, or a
// negative value when a native step reported an error.
int run(int grain)
    tensor<real32> input = tensor.zeros<real32>([40000])
    int32 filled = fill_native(&input, 7)
    if filled != int32(0)
        return -1
    tensor<real32> output = tensor.zeros<real32>([40000])
    int32 computed = kernel_native(&input, &output, int64(grain))
    if computed != int32(0)
        return -2
    return int(checksum_native(&output))

// 1000 * (thread count seen by the caller) + (largest count seen in a body).
int threads()
    return int(threads_native())

int failure(int status_chunk, int throw_chunk)
    return int(failure_native(int64(status_chunk), int64(throw_chunk)))

// Bodies that call qcore_* functions: kind 0 only pure data-access ones,
// kinds 1..5 one that is not (see body_calls_body).
int body_calls(int kind)
    tensor<real32> value = tensor.zeros<real32>([64])
    return int(body_calls_native(&value, int64(kind)))
QUI

cat > "$TMP/packages/parallel_pkg/native/bridge.cpp" <<'CPP'
#include <quidra/native_extension.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace {

struct Kernel {
    const float* input;
    float* output;
    uint64_t size;
};

// Each output is an order-sensitive fma chain plus transcendental terms, so a
// change in how any single output is computed changes its bits.
int kernel_body(uint64_t begin, uint64_t end, void* raw) {
    const auto& kernel = *static_cast<const Kernel*>(raw);
    for (uint64_t index = begin; index < end; ++index) {
        float accumulator = 0.0F;
        for (uint64_t tap = 0; tap < 29; ++tap) {
            const float value = kernel.input[(index * 13 + tap * 101) % kernel.size];
            accumulator = std::fma(value, 1.0F / static_cast<float>(tap + 2), accumulator);
        }
        kernel.output[index] =
            accumulator * std::exp(-std::fabs(accumulator)) + std::tanh(kernel.input[index]);
    }
    return 0;
}

bool cpu_float32(const void* tensor) {
    return tensor && qcore_tensor_dtype(tensor) == QCORE_DTYPE_FLOAT32 &&
        qcore_tensor_backend(tensor) == QCORE_BACKEND_CPU &&
        qcore_tensor_is_contiguous(tensor);
}

struct Threads {
    std::atomic<uint64_t> inner_max{0};
    std::atomic<int> nested_errors{0};
};

int count_body(uint64_t begin, uint64_t end, void* raw) {
    *static_cast<uint64_t*>(raw) += end - begin;
    return 0;
}

int threads_body(uint64_t, uint64_t, void* raw) {
    auto& threads = *static_cast<Threads*>(raw);
    const auto inner = qcore_parallel_thread_count();
    uint64_t seen = threads.inner_max.load();
    while (inner > seen && !threads.inner_max.compare_exchange_weak(seen, inner)) {}
    // A nested call runs serially on this thread, so a plain counter is safe.
    uint64_t counted = 0;
    if (qcore_parallel_for(0, 8, 1, count_body, &counted) != QCORE_PARALLEL_OK || counted != 8)
        threads.nested_errors.fetch_add(1);
    return 0;
}

struct Failure {
    long long status_chunk;
    long long throw_chunk;
};

int failure_body(uint64_t begin, uint64_t, void* raw) {
    const auto& failure = *static_cast<const Failure*>(raw);
    const auto chunk = static_cast<long long>(begin / 4);
    if (chunk == failure.status_chunk) return static_cast<int>(1000 + chunk);
    if (chunk == failure.throw_chunk) throw std::runtime_error("chunk failed");
    return 0;
}

struct BodyCalls {
    const void* tensor;
    uint64_t count;
    long long kind;
    std::atomic<int> wrong{0};
};

// Checks every pure data-access call against what the caller knows.
void check_data_access(BodyCalls& calls) {
    const void* tensor = calls.tensor;
    const bool expected =
        qcore_native_abi_version() == QUIDRA_NATIVE_ABI_VERSION &&
        qcore_parallel_thread_count() == 1 &&
        qcore_execution_is_deterministic() ==
            (qcore_execution_policy_get() == QCORE_EXECUTION_DETERMINISTIC ? 1 : 0) &&
        qcore_tensor_dtype(tensor) == QCORE_DTYPE_FLOAT32 &&
        qcore_tensor_device(tensor) == -1 && qcore_tensor_rank(tensor) == 1 &&
        qcore_tensor_extent(tensor, 0) == static_cast<long long>(calls.count) &&
        qcore_tensor_extent(tensor, 1) == -1 &&
        qcore_tensor_element_count(tensor) == calls.count &&
        qcore_tensor_is_contiguous(tensor) == 1 &&
        qcore_tensor_backend(tensor) == QCORE_BACKEND_CPU &&
        qcore_tensor_backend_device_index(tensor) == -1 &&
        qcore_tensor_device_offset_bytes(tensor) == 0;
    if (!expected) calls.wrong.fetch_add(1);
}

int nested_body_calls_body(uint64_t, uint64_t, void* raw) {
    auto& calls = *static_cast<BodyCalls*>(raw);
    if (calls.kind == 3)
        return qcore_tensor_device_handle_const(calls.tensor) == 0 ? 0 : 1;
    check_data_access(calls);
    return 0;
}

// Kind 0 makes every pure data-access call, here and in a nested body. Kinds
// 1..5 each make one call that is not pure data access (kind 3 in a nested
// body, which runs serially).
int body_calls_body(uint64_t begin, uint64_t, void* raw) {
    auto& calls = *static_cast<BodyCalls*>(raw);
    switch (calls.kind) {
    case 1:
        return qcore_tensor_cpu_data_const(calls.tensor) ? 0 : 1;
    case 2:
        qcore_counter_note_refusal("parallel_pkg", "body", 7);
        return 0;
    case 3:
        return qcore_parallel_for(begin, begin + 2, 1, nested_body_calls_body, raw);
    case 4:
        return qcore_device_wait(0) == 0 ? 0 : 1;
    case 5:
        return qcore_device_encode_begin(0) ? 1 : 0;
    default:
        check_data_access(calls);
        return qcore_parallel_for(begin, begin + 2, 1, nested_body_calls_body, raw);
    }
}

} // namespace

extern "C" int32_t qtest_parallel_fill(void* value, long long seed) {
    if (!cpu_float32(value)) return 1;
    auto* data = static_cast<float*>(qcore_tensor_cpu_data(value));
    if (!data) return 2;
    const auto count = qcore_tensor_element_count(value);
    uint64_t state = static_cast<uint64_t>(seed);
    for (uint64_t index = 0; index < count; ++index) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        const auto bits = static_cast<uint32_t>(state >> 32);
        data[index] = static_cast<float>(static_cast<int32_t>(bits)) / 2147483648.0F;
    }
    if (count > 4) {
        data[1] = -0.0F;
        data[2] = std::numeric_limits<float>::infinity();
        data[3] = std::numeric_limits<float>::denorm_min();
        data[4] = std::numeric_limits<float>::quiet_NaN();
    }
    return 0;
}

extern "C" int32_t qtest_parallel_kernel(const void* input, void* output, long long grain) {
    if (!cpu_float32(input) || !cpu_float32(output) || grain <= 0) return 1;
    const auto count = qcore_tensor_element_count(input);
    if (qcore_tensor_element_count(output) != count) return 2;
    // Data pointers are obtained before the parallel region; bodies call no
    // other qcore_* function.
    Kernel kernel{
        static_cast<const float*>(qcore_tensor_cpu_data_const(input)),
        static_cast<float*>(qcore_tensor_cpu_data(output)),
        count};
    if (!kernel.input || !kernel.output) return 3;
    return qcore_parallel_for(0, count, static_cast<uint64_t>(grain), kernel_body, &kernel);
}

extern "C" long long qtest_parallel_checksum(const void* value) {
    if (!cpu_float32(value)) return -1;
    const auto* bytes = static_cast<const unsigned char*>(qcore_tensor_cpu_data_const(value));
    if (!bytes) return -2;
    uint64_t hash = 1469598103934665603ull;
    for (uint64_t index = 0; index < qcore_tensor_element_count(value) * sizeof(float); ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
    return static_cast<long long>(hash & 0x7fffffffffffffffull);
}

extern "C" long long qtest_parallel_threads() {
    const auto outer = qcore_parallel_thread_count();
    Threads threads;
    if (qcore_parallel_for(0, 64, 1, threads_body, &threads) != QCORE_PARALLEL_OK ||
        threads.nested_errors.load() != 0)
        return -1;
    return static_cast<long long>(outer * 1000 + threads.inner_max.load());
}

extern "C" long long qtest_parallel_failure(long long status_chunk, long long throw_chunk) {
    Failure failure{status_chunk, throw_chunk};
    return qcore_parallel_for(0, 400, 4, failure_body, &failure);
}

extern "C" long long qtest_parallel_body_calls(const void* value, long long kind) {
    if (!cpu_float32(value)) return -1;
    BodyCalls calls{value, qcore_tensor_element_count(value), kind};
    const int status = qcore_parallel_for(0, 64, 1, body_calls_body, &calls);
    if (status != QCORE_PARALLEL_OK) return status;
    return calls.wrong.load();
}
CPP

cat > "$TMP/use.qui" <<'QUI'
import par = parallel_pkg

int task_checksum()
    return par.run(100)

int task_threads()
    return par.threads()

print(par.run(1))
print(NL)
print(par.run(100))
print(NL)
print(par.run(4096))
print(NL)
print(par.run(40000))
print(NL)
print(par.failure(-1, -1))
print(NL)
print(par.failure(30, 5))
print(NL)
print(par.failure(5, 30))
print(NL)
print(par.failure(99, -1))
print(NL)
int[] results = task.all([task_checksum, task_threads])
print(results[0])
print(NL)
print(results[1])
print(NL)
print(par.threads())
print(NL)
QUI

# The package's kernels are reached only from task.all operations here.
cat > "$TMP/task_only.qui" <<'QUI'
import par = parallel_pkg

int task_checksum()
    return par.run(100)

int task_threads()
    return par.threads()

int[] results = task.all([task_checksum, task_threads])
print(results[0])
print(NL)
print(results[1])
print(NL)
QUI

cat > "$TMP/body_calls.qui" <<'QUI'
import par = parallel_pkg

cli args
    int kind = option(default = 0)

print("before{NL}")
print(par.body_calls(args.kind))
print(NL)
QUI

# No package: the program binds the Core symbol itself.
cat > "$TMP/count.qui" <<'QUI'
extern nat64 cpu_threads() = "qcore_parallel_thread_count"

print(cpu_threads())
print(NL)
QUI

for program in use task_only body_calls; do
    QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" build "$TMP/$program.qui" -o "$TMP/$program" \
        >"$TMP/build.out" 2>"$TMP/build.err" || {
        cat "$TMP/build.err" >&2
        fail "building the parallel package program $program"
    }
done
"$QUIDRA" build "$TMP/count.qui" -o "$TMP/count" >"$TMP/build.out" 2>"$TMP/build.err" || {
    cat "$TMP/build.err" >&2
    fail "building the thread-count program"
}

run_with_threads() {
    local threads="$1"
    local output="$2"
    if [[ "$threads" == "unset" ]]; then
        env -u QUIDRA_CPU_THREADS "$TMP/use" >"$output" 2>"$output.err" ||
            { cat "$output.err" >&2; fail "run with QUIDRA_CPU_THREADS unset"; }
    else
        QUIDRA_CPU_THREADS="$threads" "$TMP/use" >"$output" 2>"$output.err" ||
            { cat "$output.err" >&2; fail "run with QUIDRA_CPU_THREADS=$threads"; }
    fi
}

# Everything except the final thread-count line must be identical.
run_with_threads 1 "$TMP/serial.out"
head -n 10 "$TMP/serial.out" > "$TMP/serial.common"
checksum_1="$(sed -n 1p "$TMP/serial.out")"
checksum_100="$(sed -n 2p "$TMP/serial.out")"
for line in 1 2 3 4; do
    value="$(sed -n "${line}p" "$TMP/serial.out")"
    [[ "$value" =~ ^[0-9]+$ ]] || fail "kernel checksum line $line is '$value'"
    [[ "$value" == "$checksum_1" ]] || fail "grain changes the kernel bits ($value vs $checksum_1)"
done
expected_failures="$(printf '0\n-2\n1005\n1099')"
[[ "$(sed -n 5,8p "$TMP/serial.out")" == "$expected_failures" ]] ||
    fail "failure statuses: $(sed -n 5,8p "$TMP/serial.out" | tr '\n' ' ')"
[[ "$(sed -n 9p "$TMP/serial.out")" == "$checksum_100" ]] || fail "task.all kernel bits differ"
[[ "$(sed -n 10p "$TMP/serial.out")" == "1001" ]] ||
    fail "task.all operation must run serially, got $(sed -n 10p "$TMP/serial.out")"
[[ "$(sed -n 11p "$TMP/serial.out")" == "1001" ]] ||
    fail "QUIDRA_CPU_THREADS=1 reports $(sed -n 11p "$TMP/serial.out")"

# Unset means the hardware thread count; every requested count is reduced to it.
hardware=""
for threads in unset 2 3 4 8 16 256; do
    run_with_threads "$threads" "$TMP/threads-$threads.out"
    head -n 10 "$TMP/threads-$threads.out" > "$TMP/threads-$threads.common"
    cmp -s "$TMP/serial.common" "$TMP/threads-$threads.common" ||
        fail "QUIDRA_CPU_THREADS=$threads changes results: $(tr '\n' ' ' < "$TMP/threads-$threads.out")"
    reported="$(sed -n 11p "$TMP/threads-$threads.out")"
    if [[ "$threads" == "unset" ]]; then
        [[ "$reported" =~ ^[1-9][0-9]*001$ ]] ||
            fail "default thread count report '$reported' (expected N*1000 + 1)"
        hardware="$(( (reported - 1) / 1000 ))"
    else
        expected="$(( threads < hardware ? threads : hardware ))"
        [[ "$reported" == "$((expected * 1000 + 1))" ]] ||
            fail "QUIDRA_CPU_THREADS=$threads reports '$reported' (expected $expected; bodies must see 1)"
    fi
done

# An empty value means unset.
QUIDRA_CPU_THREADS= "$TMP/use" > "$TMP/empty.out" 2>"$TMP/empty.err" ||
    { cat "$TMP/empty.err" >&2; fail "empty QUIDRA_CPU_THREADS"; }
[[ "$(sed -n 11p "$TMP/empty.out")" == "$(sed -n 11p "$TMP/threads-unset.out")" ]] ||
    fail "empty QUIDRA_CPU_THREADS differs from unset"

# Kernels reached only from task.all run serially and still give the same bits.
QUIDRA_CPU_THREADS=4 "$TMP/task_only" > "$TMP/task_only.out" 2>"$TMP/task_only.err" ||
    { cat "$TMP/task_only.err" >&2; fail "task.all-only program"; }
[[ "$(cat "$TMP/task_only.out")" == "$(printf '%s\n1001' "$checksum_100")" ]] ||
    fail "task.all-only program printed $(tr '\n' ' ' < "$TMP/task_only.out")"

# The thread count is readable from Quidra code.
for threads in unset 1 2 256; do
    if [[ "$threads" == "unset" ]]; then
        reported="$(env -u QUIDRA_CPU_THREADS "$TMP/count")" || fail "count program, unset"
        expected="$hardware"
    else
        reported="$(QUIDRA_CPU_THREADS="$threads" "$TMP/count")" ||
            fail "count program, QUIDRA_CPU_THREADS=$threads"
        expected="$(( threads < hardware ? threads : hardware ))"
    fi
    [[ "$reported" == "$expected" ]] ||
        fail "extern thread count with QUIDRA_CPU_THREADS=$threads is '$reported', expected $expected"
done

# An invalid value stops the program at the first call, wherever it is made:
# at top level, inside task.all, or from Quidra code with no package kernel.
for program in use task_only count; do
    for invalid in 0 -3 abc 4x 257 " 2"; do
        set +e
        QUIDRA_CPU_THREADS="$invalid" "$TMP/$program" >"$TMP/invalid.out" 2>"$TMP/invalid.err"
        status=$?
        set -e
        [[ "$status" -eq 101 ]] || fail "$program: QUIDRA_CPU_THREADS='$invalid' exited with $status"
        { grep -q '^Quidra runtime error\[CPU_THREADS\] at ' "$TMP/invalid.err" &&
          grep -q '| QUIDRA_CPU_THREADS must be an integer from 1 to 256' "$TMP/invalid.err"; } ||
            { cat "$TMP/invalid.err" >&2; fail "$program: message for '$invalid'"; }
        # Reading the count first, as a harness does, checks the value before
        # the program prints anything.
        if [[ "$program" == "count" && -s "$TMP/invalid.out" ]]; then
            fail "count: output before the CPU_THREADS check for '$invalid'"
        fi
    done
done

# Inside a body only pure data-access qcore_* calls are allowed, nested or
# not. Any other call stops the program with runtime error PARALLEL_BODY at
# every thread count, 1 included, before it touches Core state; output
# printed before is flushed.
for threads in 1 4 unset; do
    if [[ "$threads" == "unset" ]]; then
        run_env=(env -u QUIDRA_CPU_THREADS)
    else
        run_env=(env QUIDRA_CPU_THREADS="$threads")
    fi
    "${run_env[@]}" "$TMP/body_calls" --kind 0 >"$TMP/body-calls.out" 2>"$TMP/body-calls.err" ||
        { cat "$TMP/body-calls.err" >&2; fail "data-access calls in bodies (threads $threads)"; }
    [[ "$(cat "$TMP/body-calls.out")" == "$(printf 'before\n0')" ]] ||
        fail "data-access calls in bodies (threads $threads) printed $(tr '\n' ' ' < "$TMP/body-calls.out")"
    for kind in 1 2 3 4 5; do
        case "$kind" in
            1) function=qcore_tensor_cpu_data_const ;;
            2) function=qcore_counter_note_refusal ;;
            3) function=qcore_tensor_device_handle_const ;;
            4) function=qcore_device_wait ;;
            5) function=qcore_device_encode_begin ;;
        esac
        set +e
        "${run_env[@]}" "$TMP/body_calls" --kind "$kind" >"$TMP/body-calls.out" 2>"$TMP/body-calls.err"
        status=$?
        set -e
        [[ "$status" -eq 101 ]] ||
            { cat "$TMP/body-calls.err" >&2; fail "$function in a body (threads $threads) exited with $status"; }
        { grep -Fq "Quidra runtime error[PARALLEL_BODY] at " "$TMP/body-calls.err" &&
          grep -Fq "| $function was called inside a qcore_parallel_for body" "$TMP/body-calls.err"; } ||
            { cat "$TMP/body-calls.err" >&2; fail "$function in a body (threads $threads): message"; }
        [[ "$(cat "$TMP/body-calls.out")" == "before" ]] ||
            fail "$function in a body (threads $threads) printed $(tr '\n' ' ' < "$TMP/body-calls.out")"
    done
done

# The REPL JIT resolves the same Core symbols from the shared runtime.
set +e
QUIDRA_CPU_THREADS=3 QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" repl < "$TMP/use.qui" \
    >"$TMP/repl.out" 2>"$TMP/repl.err"
repl_status=$?
set -e
[[ "$repl_status" -eq 0 ]] || { cat "$TMP/repl.err" >&2; fail "REPL exited with $repl_status"; }
grep -q "^${checksum_1}\$" "$TMP/repl.out" ||
    { cat "$TMP/repl.out" "$TMP/repl.err" >&2; fail "REPL kernel bits differ"; }
repl_threads="$(( 3 < hardware ? 3 : hardware ))"
grep -q "^${repl_threads}001\$" "$TMP/repl.out" ||
    { cat "$TMP/repl.out" >&2; fail "REPL thread count report"; }
set +e
QUIDRA_CPU_THREADS=2 "$QUIDRA" repl < "$TMP/count.qui" >"$TMP/repl-count.out" 2>"$TMP/repl-count.err"
repl_status=$?
set -e
[[ "$repl_status" -eq 0 ]] || { cat "$TMP/repl-count.err" >&2; fail "REPL count exited with $repl_status"; }
grep -q "^$(( 2 < hardware ? 2 : hardware ))\$" "$TMP/repl-count.out" ||
    { cat "$TMP/repl-count.out" >&2; fail "REPL extern thread count"; }

echo "cpu parallel tests passed"
