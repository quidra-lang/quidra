#!/usr/bin/env bash
set -euo pipefail

QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$TMP/packages/native_pkg/native"

cat > "$TMP/packages/native_pkg/main.qui" <<'QUI'
extern int native_answer() = "qtest_native_answer"
extern int tensor_probe(
    const tensor<float32> &value
) = "qtest_tensor_probe"
extern int32 scale3_native(
    const tensor<float32> &input,
    tensor<float32> &output
) = "qtest_scale3"
extern int32 scale_saved_native(
    const tensor<float32> &input,
    const tensor<float32> &factor,
    tensor<float32> &output
) = "qtest_scale_saved"
extern int32 square_native(
    const tensor<float32> &input,
    tensor<float32> &output
) = "qtest_square"
extern int32 increment_first_native(
    tensor<float32> &value
) = "qtest_increment_first"

tensor<float32> scale3(tensor<float32> input)
    tensor<float32> output = tensor.zeros<float32>(input.shape())
    int32 status = scale3_native(&input, &output)
    if status != int32(0)
        error("native custom autograd registration failed")
    return output

tensor<float32> scale_saved(
    tensor<float32> input,
    tensor<float32> factor
)
    tensor<float32> output = tensor.zeros<float32>(input.shape())
    int32 status = scale_saved_native(&input, &factor, &output)
    if status != int32(0)
        error("native custom autograd saved-tensor registration failed")
    return output

tensor<float32> square(tensor<float32> input)
    tensor<float32> output = tensor.zeros<float32>(input.shape())
    int32 status = square_native(&input, &output)
    if status != int32(0)
        error("native tracked custom autograd registration failed")
    return output

int32 increment_first(tensor<float32> &value)
    return increment_first_native(&value)

int answer()
    return native_answer()

int probe()
    tensor<float32> value = tensor.ones<float32>([2])
    return tensor_probe(&value)
QUI

cat > "$TMP/packages/native_pkg/quidra.package" <<'MANIFEST'
name = native_pkg
version = 0.1.0
native.source.bridge = native/bridge.cpp
MANIFEST

cat > "$TMP/packages/native_pkg/native/bridge.cpp" <<'CPP'
#include <quidra/native_extension.h>

static thread_local long long qtest_tls_answer = 42;

extern "C" long long qtest_native_answer() {
    return qcore_native_abi_version() == QUIDRA_NATIVE_ABI_VERSION
        ? qtest_tls_answer
        : -1;
}

static int qtest_scale3_backward(
    const void* const* saved_tensors,
    uint64_t saved_tensor_count,
    const void* gradient_output,
    void* const* gradient_inputs,
    uint64_t gradient_input_count,
    const void*,
    uint64_t) {
    if (!saved_tensors || saved_tensor_count != 1 ||
        !gradient_output || !gradient_inputs || gradient_input_count != 1)
        return 1;
    if (qcore_tensor_dtype(saved_tensors[0]) != QCORE_DTYPE_FLOAT32 ||
        qcore_tensor_dtype(gradient_output) != QCORE_DTYPE_FLOAT32 ||
        qcore_tensor_dtype(gradient_inputs[0]) != QCORE_DTYPE_FLOAT32)
        return 2;
    const auto count = qcore_tensor_element_count(saved_tensors[0]);
    if (qcore_tensor_element_count(gradient_output) != count ||
        qcore_tensor_element_count(gradient_inputs[0]) != count)
        return 3;
    const auto* gradient =
        static_cast<const float*>(qcore_tensor_cpu_data_const(gradient_output));
    auto* input_gradient =
        static_cast<float*>(qcore_tensor_cpu_data(gradient_inputs[0]));
    if (!gradient || !input_gradient) return 4;
    for (uint64_t index = 0; index < count; ++index)
        input_gradient[index] = gradient[index] * 3.0F;
    return 0;
}

extern "C" int32_t qtest_scale3(const void* input, void* output) {
    if (!input || !output ||
        qcore_tensor_dtype(input) != QCORE_DTYPE_FLOAT32 ||
        qcore_tensor_dtype(output) != QCORE_DTYPE_FLOAT32 ||
        qcore_tensor_backend(input) != QCORE_BACKEND_CPU ||
        qcore_tensor_backend(output) != QCORE_BACKEND_CPU)
        return 10;
    const auto count = qcore_tensor_element_count(input);
    if (qcore_tensor_element_count(output) != count) return 11;
    const auto* source =
        static_cast<const float*>(qcore_tensor_cpu_data_const(input));
    auto* destination =
        static_cast<float*>(qcore_tensor_cpu_data(output));
    if (!source || !destination) return 12;
    for (uint64_t index = 0; index < count; ++index)
        destination[index] = source[index] * 3.0F;
    const void* inputs[] = {input};
    return qcore_tensor_attach_custom_autograd(
        output, inputs, 1, qtest_scale3_backward, nullptr, 0);
}

static int qtest_scale_saved_backward(
    const void* const* saved_tensors,
    uint64_t saved_tensor_count,
    const void* gradient_output,
    void* const* gradient_inputs,
    uint64_t gradient_input_count,
    const void*,
    uint64_t) {
    if (!saved_tensors || saved_tensor_count != 1 ||
        !gradient_output || !gradient_inputs || gradient_input_count != 1)
        return 21;
    const auto* factor =
        static_cast<const float*>(qcore_tensor_cpu_data_const(saved_tensors[0]));
    const auto* gradient =
        static_cast<const float*>(qcore_tensor_cpu_data_const(gradient_output));
    auto* input_gradient =
        static_cast<float*>(qcore_tensor_cpu_data(gradient_inputs[0]));
    if (!factor || !gradient || !input_gradient) return 22;
    const auto count = qcore_tensor_element_count(gradient_output);
    if (qcore_tensor_element_count(saved_tensors[0]) != count ||
        qcore_tensor_element_count(gradient_inputs[0]) != count)
        return 23;
    for (uint64_t index = 0; index < count; ++index)
        input_gradient[index] = gradient[index] * factor[index];
    return 0;
}

extern "C" int32_t qtest_scale_saved(
    const void* input, const void* factor, void* output) {
    if (!input || !factor || !output ||
        qcore_tensor_dtype(input) != QCORE_DTYPE_FLOAT32 ||
        qcore_tensor_dtype(factor) != QCORE_DTYPE_FLOAT32 ||
        qcore_tensor_dtype(output) != QCORE_DTYPE_FLOAT32 ||
        qcore_tensor_backend(input) != QCORE_BACKEND_CPU ||
        qcore_tensor_backend(factor) != QCORE_BACKEND_CPU ||
        qcore_tensor_backend(output) != QCORE_BACKEND_CPU)
        return 20;
    const auto count = qcore_tensor_element_count(input);
    if (qcore_tensor_element_count(factor) != count ||
        qcore_tensor_element_count(output) != count)
        return 24;
    const auto* source =
        static_cast<const float*>(qcore_tensor_cpu_data_const(input));
    const auto* factor_data =
        static_cast<const float*>(qcore_tensor_cpu_data_const(factor));
    auto* destination =
        static_cast<float*>(qcore_tensor_cpu_data(output));
    if (!source || !factor_data || !destination) return 25;
    for (uint64_t index = 0; index < count; ++index)
        destination[index] = source[index] * factor_data[index];

    const void* inputs[] = {input};
    const void* saved[] = {factor};
    return qcore_tensor_attach_custom_autograd_with_saved(
        output, inputs, 1, saved, 1,
        qtest_scale_saved_backward, nullptr, 0);
}

static int qtest_mul2_backward(
    const void* const* saved_tensors,
    uint64_t saved_tensor_count,
    const void* gradient_output,
    void* const* gradient_inputs,
    uint64_t gradient_input_count,
    const void*,
    uint64_t) {
    if (!saved_tensors || saved_tensor_count != 2 ||
        !gradient_output || !gradient_inputs || gradient_input_count != 2)
        return 30;
    const auto count = qcore_tensor_element_count(saved_tensors[0]);
    if (qcore_tensor_element_count(saved_tensors[1]) != count ||
        qcore_tensor_element_count(gradient_output) != count ||
        qcore_tensor_element_count(gradient_inputs[0]) != count ||
        qcore_tensor_element_count(gradient_inputs[1]) != count)
        return 31;
    const auto* x = static_cast<const float*>(
        qcore_tensor_cpu_data_const(saved_tensors[0]));
    const auto* g = static_cast<const float*>(
        qcore_tensor_cpu_data_const(saved_tensors[1]));
    const auto* h = static_cast<const float*>(
        qcore_tensor_cpu_data_const(gradient_output));
    auto* dx = static_cast<float*>(qcore_tensor_cpu_data(gradient_inputs[0]));
    auto* dg = static_cast<float*>(qcore_tensor_cpu_data(gradient_inputs[1]));
    if (!x || !g || !h || !dx || !dg) return 32;
    for (uint64_t index = 0; index < count; ++index) {
        dx[index] = 2.0F * g[index] * h[index];
        dg[index] = 2.0F * x[index] * h[index];
    }
    return 0;
}

static int qtest_square_backward(
    const void* const* saved_tensors,
    uint64_t saved_tensor_count,
    const void* gradient_output,
    void* const* gradient_inputs,
    uint64_t gradient_input_count,
    const void*,
    uint64_t) {
    if (!saved_tensors || saved_tensor_count != 1 ||
        !gradient_output || !gradient_inputs || gradient_input_count != 1)
        return 40;
    const auto count = qcore_tensor_element_count(saved_tensors[0]);
    if (qcore_tensor_element_count(gradient_output) != count ||
        qcore_tensor_element_count(gradient_inputs[0]) != count)
        return 41;
    const auto* x = static_cast<const float*>(
        qcore_tensor_cpu_data_const(saved_tensors[0]));
    const auto* g = static_cast<const float*>(
        qcore_tensor_cpu_data_const(gradient_output));
    auto* dx = static_cast<float*>(qcore_tensor_cpu_data(gradient_inputs[0]));
    if (!x || !g || !dx) return 42;
    for (uint64_t index = 0; index < count; ++index)
        dx[index] = 2.0F * x[index] * g[index];
    return 0;
}

static int qtest_square_backward_tracked(
    const void* const* differentiable_inputs,
    uint64_t differentiable_input_count,
    const void* const* saved_tensors,
    uint64_t saved_tensor_count,
    const void* gradient_output,
    void* const* gradient_inputs,
    uint64_t gradient_input_count,
    const void* metadata,
    uint64_t metadata_size) {
    if (!differentiable_inputs || differentiable_input_count != 1)
        return 43;
    const int status = qtest_square_backward(
        saved_tensors, saved_tensor_count, gradient_output,
        gradient_inputs, gradient_input_count, metadata, metadata_size);
    if (status != 0) return status;
    const void* derivative_inputs[] = {
        differentiable_inputs[0], gradient_output
    };
    return qcore_tensor_attach_custom_autograd_ex(
        gradient_inputs[0], derivative_inputs, 2,
        qtest_mul2_backward, nullptr, nullptr, 0);
}

extern "C" int32_t qtest_square(const void* input, void* output) {
    if (!input || !output ||
        qcore_tensor_dtype(input) != QCORE_DTYPE_FLOAT32 ||
        qcore_tensor_dtype(output) != QCORE_DTYPE_FLOAT32 ||
        qcore_tensor_backend(input) != QCORE_BACKEND_CPU ||
        qcore_tensor_backend(output) != QCORE_BACKEND_CPU)
        return 50;
    const auto count = qcore_tensor_element_count(input);
    if (qcore_tensor_element_count(output) != count) return 51;
    const auto* source =
        static_cast<const float*>(qcore_tensor_cpu_data_const(input));
    auto* destination =
        static_cast<float*>(qcore_tensor_cpu_data(output));
    if (!source || !destination) return 52;
    for (uint64_t index = 0; index < count; ++index)
        destination[index] = source[index] * source[index];
    const void* inputs[] = {input};
    return qcore_tensor_attach_custom_autograd_ex(
        output, inputs, 1, qtest_square_backward,
        qtest_square_backward_tracked, nullptr, 0);
}

extern "C" int32_t qtest_increment_first(void* value) {
    if (!value || qcore_tensor_backend(value) != QCORE_BACKEND_CPU ||
        qcore_tensor_dtype(value) != QCORE_DTYPE_FLOAT32 ||
        qcore_tensor_element_count(value) == 0)
        return 1;
    auto* data = static_cast<float*>(qcore_tensor_cpu_data(value));
    if (!data) return 2;
    data[0] += 1.0F;
    return 0;
}

extern "C" long long qtest_tensor_probe(const void* value) {
    if (!value) return -1;
    if (qcore_tensor_backend(value) != QCORE_BACKEND_CPU) return -2;
    if (qcore_tensor_backend_device_index(value) != -1) return -8;
    if (qcore_tensor_device(value) != -1) return -3;
    if (!qcore_tensor_is_contiguous(value)) return -4;
    if (qcore_tensor_device_handle_const(value) != 0) return -5;
    if (qcore_tensor_device_offset_bytes(value) != 0) return -6;
    if (qcore_device_queue_handle(-1) != 0) return -9;
    if (qcore_execution_is_deterministic() != 0) return -7;
    return 43;
}
CPP

cat > "$TMP/use.qui" <<'QUI'
import package = native_pkg
print(package.answer())
print(NL)
print(package.probe())
print(NL)
// Native writable access must preserve source-level independent-value semantics.
tensor<float32> mutation_source = tensor.ones<float32>([1])
tensor<float32> mutation_copy = mutation_source
print(package.increment_first(&mutation_source) == int32(0))
print(NL)
print(mutation_source[0].item() == float32(2))
print(NL)
print(mutation_copy[0].item() == float32(1))
print(NL)
tensor<float32> tracked = tensor.ones<float32>([]).track()
tensor<float32> scaled = package.scale3(tracked)
scaled.backward(&tracked)
print(tracked.grad.item())
print(NL)
tensor<float32> tracked_saved = tensor.ones<float32>([]).track()
tensor<float32> factor = tensor.ones<float32>([1]) * float32(4)
tensor<float32> scaled_saved = package.scale_saved(tracked_saved, factor)
print(scaled_saved.untrack().item())
print(NL)
// Backward must consume the forward-time saved value, not this later mutation.
factor[0] = float32(9)
scaled_saved.backward(&tracked_saved)
print(tracked_saved.grad.item())
print(NL)

tensor<float32> nonlinear = (tensor.ones<float32>([]) * float32(3)).track()
tensor<float32> squared = package.square(nonlinear)
squared.backward(&nonlinear, track = true)
tensor<float32> first_grad = nonlinear.grad
print(first_grad.untrack().item() == float32(6))
print(NL)
nonlinear.clear_grad()
first_grad.backward(&nonlinear)
print(nonlinear.grad.untrack().item() == float32(2))
print(NL)
QUI

set +e
QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" "$TMP/use.qui" \
    >"$TMP/use.out" 2>"$TMP/use.err"
use_status=$?
set -e
if [[ "$use_status" -ne 0 ]]; then
    echo "native package execution failed with status $use_status" >&2
    cat "$TMP/use.err" >&2
    exit 1
fi
output="$(cat "$TMP/use.out")"
expected_output="$(printf '42\n43\ntrue\ntrue\ntrue\n3.0\n4.0\n4.0\ntrue\ntrue')"
if [[ "$output" != "$expected_output" ]]; then
    echo "unexpected native package output:" >&2
    printf '%s\n' "$output" >&2
    exit 1
fi

set +e
QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" repl < "$TMP/use.qui" \
    >"$TMP/repl.out" 2>"$TMP/repl.err"
repl_status=$?
set -e
if [[ "$repl_status" -ne 0 ]]; then
    echo "native package REPL execution failed with status $repl_status" >&2
    cat "$TMP/repl.err" >&2
    exit 1
fi
repl_output="$(cat "$TMP/repl.out")"
if ! grep -q '^42$' <<< "$repl_output"; then
    echo "native package REPL output missing expected answer:" >&2
    printf '%s\n' "$repl_output" >&2
    cat "$TMP/repl.err" >&2
    exit 1
fi

HOME_DIR="$TMP/home"
mkdir -p "$HOME_DIR"
HOME="$HOME_DIR" "$QUIDRA" package install "$TMP/packages/native_pkg" --force >/dev/null
HOME="$HOME_DIR" "$QUIDRA" package-info native_pkg --json > "$TMP/info.json"
python3 - "$TMP/info.json" <<'PY'
import json
import sys
info = json.load(open(sys.argv[1]))
assert info["native_source"]["bridge"] == "native/bridge.cpp"
PY

cat > "$TMP/installed-use.qui" <<'QUI'
import package = native_pkg
print(package.answer())
print(NL)
print(package.probe())
print(NL)
QUI
set +e
HOME="$HOME_DIR" "$QUIDRA" "$TMP/installed-use.qui" \
    >"$TMP/installed-use.out" 2>"$TMP/installed-use.err"
installed_use_status=$?
set -e
installed_use_output="$(cat "$TMP/installed-use.out")"
if [[ "$installed_use_status" -ne 0 || "$installed_use_output" != "$(printf '42\n43')" ]]; then
    echo "installed native package execution mismatch (status $installed_use_status):" >&2
    printf '%s\n' "$installed_use_output" >&2
    cat "$TMP/installed-use.err" >&2
    exit 1
fi

mkdir -p "$TMP/packages/invalid_extension/compiler"
cat > "$TMP/packages/invalid_extension/main.qui" <<'QUI'
tensor<float32> identity(tensor<float32> value)
    return value
QUI
cat > "$TMP/packages/invalid_extension/quidra.package" <<'MANIFEST'
name = invalid_extension
version = 0.1.0
repository = https://example.invalid/invalid_extension
requires.quidra = >=0.5.0 <0.6.0
MANIFEST
cat > "$TMP/packages/invalid_extension/project.toml" <<'TOML'
[package]
name = "test-invalid-extension"
import = "invalid_extension"
display_name = "Invalid Extension Test"
version = "0.1.0"
repository = "https://example.invalid/invalid_extension"

[requires]
quidra = ">=0.5.0 <0.6.0"
abi = 1

[compiler.extension]
graph = "compiler/graph.toml"
TOML
cat > "$TMP/packages/invalid_extension/compiler/graph.toml" <<'TOML'
[extension]
version = 1
phase = "tensor-region"

[operation.identity]
function = "identity"
traits = "pure,tensor"

[fusion.invalid]
operations = "identity,missing"
TOML
cat > "$TMP/invalid-extension-use.qui" <<'QUI'
import broken = invalid_extension

tensor<float32> value = broken.identity(tensor.ones<float32>([1]))
print(value[0].item())
print(NL)
QUI

set +e
QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" check "$TMP/invalid-extension-use.qui" --json \
    >"$TMP/invalid-extension.out" 2>&1
invalid_extension_status=$?
set -e
if [[ "$invalid_extension_status" -ne 1 ]]; then
    echo "invalid compiler extension descriptor unexpectedly passed" >&2
    cat "$TMP/invalid-extension.out" >&2
    exit 1
fi
grep -Fq "PACKAGE_COMPILER_EXTENSION" "$TMP/invalid-extension.out"
grep -Fq "references unknown operation 'missing'" "$TMP/invalid-extension.out"

cat > "$TMP/packages/invalid_extension/compiler/graph.toml" <<'TOML'
[extension]
version = 1
phase = "tensor-region"

[operation.identity]
function = "identity"
traits = "pure,tensor"

[specialization.invalid]
operation = "identity"
replacement = "missing"
dtype = "float32"
TOML

set +e
QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" check "$TMP/invalid-extension-use.qui" --json \
    >"$TMP/invalid-conditional-extension.out" 2>&1
invalid_conditional_extension_status=$?
set -e
if [[ "$invalid_conditional_extension_status" -ne 1 ]]; then
    echo "invalid compiler conditional extension unexpectedly passed" >&2
    cat "$TMP/invalid-conditional-extension.out" >&2
    exit 1
fi
grep -Fq "PACKAGE_COMPILER_EXTENSION" "$TMP/invalid-conditional-extension.out"
grep -Fq "specialization 'invalid' references unknown replacement operation 'missing'" \
    "$TMP/invalid-conditional-extension.out"

cat > "$TMP/packages/invalid_extension/compiler/graph.toml" <<'TOML'
[extension]
version = 1
phase = "tensor-region"

[execution_policy.fast]
function = "fast"

[operation.identity]
function = "identity"
traits = "pure,tensor"

[specialization.invalid_policy]
operation = "identity"
replacement = "identity"
policy = "deterministic"
TOML

set +e
QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" check "$TMP/invalid-extension-use.qui" --json \
    >"$TMP/invalid-policy-extension.out" 2>&1
invalid_policy_extension_status=$?
set -e
if [[ "$invalid_policy_extension_status" -ne 1 ]]; then
    echo "invalid compiler execution policy unexpectedly passed" >&2
    cat "$TMP/invalid-policy-extension.out" >&2
    exit 1
fi
grep -Fq "PACKAGE_COMPILER_EXTENSION" "$TMP/invalid-policy-extension.out"
grep -Fq "references unknown execution policy 'deterministic'" \
    "$TMP/invalid-policy-extension.out"

mkdir -p "$TMP/packages/policy_extension/compiler"
cat > "$TMP/packages/policy_extension/main.qui" <<'QUI'
void fast()
    return

void deterministic()
    return

void opaque()
    return

tensor<float32> portable(tensor<float32> value)
    return value

tensor<float32> fast_target(tensor<float32> value)
    return value

tensor<float32> deterministic_target(tensor<float32> value)
    return value
QUI
cat > "$TMP/packages/policy_extension/quidra.package" <<'MANIFEST'
name = policy_extension
version = 0.1.0
repository = https://example.invalid/policy_extension
requires.quidra = >=0.5.0 <0.6.0
MANIFEST
cat > "$TMP/packages/policy_extension/project.toml" <<'TOML'
[package]
name = "test-policy-extension"
import = "policy_extension"
display_name = "Policy Extension Test"
version = "0.1.0"
repository = "https://example.invalid/policy_extension"

[requires]
quidra = ">=0.5.0 <0.6.0"
abi = 1

[compiler.extension]
graph = "compiler/graph.toml"
TOML
cat > "$TMP/packages/policy_extension/compiler/graph.toml" <<'TOML'
[extension]
version = 1
phase = "tensor-region"

[execution_policy.fast]
function = "fast"

[execution_policy.deterministic]
function = "deterministic"

[operation.portable]
function = "portable"
traits = "pure,tensor,differentiable,higher-order"

[operation.fast_target]
function = "fast_target"
traits = "pure,tensor,differentiable,higher-order,backend-target"

[operation.deterministic_target]
function = "deterministic_target"
traits = "pure,tensor,differentiable,higher-order,backend-target"

[backend.fast]
operation = "portable"
replacement = "fast_target"
policy = "fast"
dtype = "float32"
device = "cpu"
layout = "contiguous"
tracked = "false"

[backend.deterministic]
operation = "portable"
replacement = "deterministic_target"
policy = "deterministic"
dtype = "float32"
device = "cpu"
layout = "contiguous"
tracked = "false"
TOML
cat > "$TMP/policy-extension-use.qui" <<'QUI'
import policy = policy_extension

policy.fast()
tensor<float32> first = policy.portable(tensor.ones<float32>([1]))
policy.deterministic()
tensor<float32> second = policy.portable(tensor.ones<float32>([1]))
policy.fast()
policy.opaque()
tensor<float32> third = policy.portable(tensor.ones<float32>([1]))
print(first[0].item() == float32(1))
print(NL)
print(second[0].item() == float32(1))
print(NL)
print(third[0].item() == float32(1))
print(NL)
QUI
policy_output="$(QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" "$TMP/policy-extension-use.qui")"
if [[ "$policy_output" != "$(printf 'true\ntrue\ntrue')" ]]; then
    echo "policy extension execution mismatch:" >&2
    printf '%s\n' "$policy_output" >&2
    exit 1
fi
policy_ir="$(QUIDRA_PACKAGE_PATH="$TMP/packages" "$QUIDRA" ir "$TMP/policy-extension-use.qui")"
policy_entry="$(awk '/^function \$entry\(/,/^end$/' <<< "$policy_ir")"
policy_fast_count="$(grep -Fc "call policy.fast_target(" <<< "$policy_entry")"
policy_deterministic_count="$(grep -Fc "call policy.deterministic_target(" <<< "$policy_entry")"
policy_portable_count="$(grep -Fc "call policy.portable(" <<< "$policy_entry")"
if [[ "$policy_fast_count" -ne 1 || "$policy_deterministic_count" -ne 1 || "$policy_portable_count" -lt 1 ]]; then
    echo "policy extension IR mismatch: fast=$policy_fast_count deterministic=$policy_deterministic_count portable=$policy_portable_count" >&2
    printf '%s\n' "$policy_entry" >&2
    exit 1
fi

if [[ "$(uname -s)" == "Linux" ]]; then
    mkdir -p "$TMP/fake-cuda/bin" "$TMP/fake-cuda/lib64"
    cat > "$TMP/fake-cuda/bin/nvcc" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
args=()
for argument in "$@"; do
    case "$argument" in
        -Xcompiler=-fPIC)
            args+=("-fPIC")
            ;;
        *.cu)
            args+=("-x" "c++" "$argument")
            ;;
        *)
            args+=("$argument")
            ;;
    esac
done
exec clang++ "${args[@]}"
SH
    chmod +x "$TMP/fake-cuda/bin/nvcc"
    printf '%s\n' 'extern "C" void qtest_cudart_stub() {}' |
        clang++ -shared -fPIC -x c++ - -o "$TMP/fake-cuda/lib64/libcudart.so"

    mkdir -p "$TMP/packages/cuda_pkg/native"
    cat > "$TMP/packages/cuda_pkg/main.qui" <<'QUI'
extern int cuda_answer() = "qtest_cuda_answer"

int answer()
    return cuda_answer()
QUI
    cat > "$TMP/packages/cuda_pkg/quidra.package" <<'MANIFEST'
name = cuda_pkg
version = 0.1.0
native.source.cuda = native/answer.cu
MANIFEST
    cat > "$TMP/packages/cuda_pkg/native/answer.cu" <<'CU'
#include <quidra/native_extension.h>

extern "C" long long qtest_cuda_answer() {
    return QUIDRA_NATIVE_ABI_VERSION == 1u ? 44 : -1;
}
CU
    cat > "$TMP/cuda-use.qui" <<'QUI'
import package = cuda_pkg
print(package.answer())
print(NL)
QUI

    set +e
    QUIDRA_PACKAGE_PATH="$TMP/packages" \
    QUIDRA_NVCC="$TMP/fake-cuda/bin/nvcc" \
    QUIDRA_CUDA_HOME="$TMP/fake-cuda" \
    "$QUIDRA" "$TMP/cuda-use.qui" \
        >"$TMP/cuda-use.out" 2>"$TMP/cuda-use.err"
    cuda_status=$?
    set -e
    cuda_output="$(cat "$TMP/cuda-use.out")"
    if [[ "$cuda_status" -ne 0 || "$cuda_output" != "44" ]]; then
        echo "CUDA package execution mismatch (status $cuda_status):" >&2
        printf '%s\n' "$cuda_output" >&2
        cat "$TMP/cuda-use.err" >&2
        exit 1
    fi

    set +e
    QUIDRA_PACKAGE_PATH="$TMP/packages" \
    QUIDRA_NVCC="$TMP/fake-cuda/bin/nvcc" \
    QUIDRA_CUDA_HOME="$TMP/fake-cuda" \
    "$QUIDRA" repl < "$TMP/cuda-use.qui" \
        >"$TMP/cuda-repl.out" 2>"$TMP/cuda-repl.err"
    cuda_repl_status=$?
    set -e
    cuda_repl_output="$(cat "$TMP/cuda-repl.out")"
    if [[ "$cuda_repl_status" -ne 0 ]] || ! grep -q '^44$' <<< "$cuda_repl_output"; then
        echo "CUDA package REPL mismatch (status $cuda_repl_status):" >&2
        printf '%s\n' "$cuda_repl_output" >&2
        cat "$TMP/cuda-repl.err" >&2
        exit 1
    fi
fi

if [[ "$(uname -s)" == "Linux" && "$(uname -m)" == "x86_64" ]]; then
    mkdir -p "$TMP/packages/asm_pkg/native"
    cat > "$TMP/packages/asm_pkg/main.qui" <<'QUI'
extern int asm_answer() = "qtest_asm_answer"

int answer()
    return asm_answer()
QUI
    cat > "$TMP/packages/asm_pkg/quidra.package" <<'MANIFEST'
name = asm_pkg
version = 0.1.0
native.source.asm = native/answer.S
MANIFEST
    cat > "$TMP/packages/asm_pkg/native/answer.S" <<'ASM'
.text
.globl qtest_asm_answer
.type qtest_asm_answer,@function
qtest_asm_answer:
    mov $45, %rax
    ret
.section .note.GNU-stack,"",@progbits
ASM
    cat > "$TMP/asm-use.qui" <<'QUI'
import package = asm_pkg
print(package.answer())
print(NL)
QUI

    set +e
    QUIDRA_PACKAGE_PATH="$TMP/packages" \
    "$QUIDRA" "$TMP/asm-use.qui" \
        >"$TMP/asm-use.out" 2>"$TMP/asm-use.err"
    asm_status=$?
    set -e
    asm_output="$(cat "$TMP/asm-use.out")"
    if [[ "$asm_status" -ne 0 || "$asm_output" != "45" ]]; then
        echo "assembly package execution mismatch (status $asm_status):" >&2
        printf '%s\n' "$asm_output" >&2
        cat "$TMP/asm-use.err" >&2
        exit 1
    fi

    set +e
    QUIDRA_PACKAGE_PATH="$TMP/packages" \
    "$QUIDRA" repl < "$TMP/asm-use.qui" \
        >"$TMP/asm-repl.out" 2>"$TMP/asm-repl.err"
    asm_repl_status=$?
    set -e
    asm_repl_output="$(cat "$TMP/asm-repl.out")"
    if [[ "$asm_repl_status" -ne 0 ]] || ! grep -q '^45$' <<< "$asm_repl_output"; then
        echo "assembly package REPL mismatch (status $asm_repl_status):" >&2
        printf '%s\n' "$asm_repl_output" >&2
        cat "$TMP/asm-repl.err" >&2
        exit 1
    fi
fi

echo "package native tests: ok"
