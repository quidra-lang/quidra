#pragma once

// The coded runtime failures: every code a runtime failure prints
// ("Quidra runtime error[CODE] at FILE:LINE:COLUMN", then the message), and
// every reason
// the runtime library or generated code reports through these rows, with the
// message template it prints.
//
// Owns: the runtime failure codes and their spellings, the reasons with
// their stable ids and message templates, and the formatting of a template
// with its arguments. The runtime's one reporter
// (quidra::runtime::report_failure, runtime_system.cpp) formats every coded
// failure of the runtime library from these rows; generated code passes the
// spelling of its code and its message to quidra_runtime_fail_at, taken from
// these rows for the reasons it reports itself (FOR_ITERATION).
//
// Rules, checked here and in tests/runtime_failure_tests.cpp:
// - within a code, each reason has exactly one template, and no two reasons
//   share one, so a printed message identifies its reason;
// - a reason's id is unique within its code;
// - a template's holes are named: {index}, {length}, {start}, {end}, {axis}
//   and {limit} are integer arguments; {type} is the spelling of a
//   conversion's destination type or a real operation's type; {operation} is
//   the operation whose result was not finite; {subject} is what was converted (a value,
//   an array element or a tensor element); {path} is the storage a failure
//   names, as the source writes it; {slice} is a slice as the source writes
//   it, with the values of its operands (`2:<7`, `10>:0:-2`, `:7`);
//   {step} is a slice's step and {marker} the slice marker it points
//   against (`:<`); {given} is how many values a slice assignment got;
//   {message} is a text the failing
//   operation describes at run time. Every hole but {message} is a detail of
//   the JSON report under the hole's name ({subject} by its id).
// A code names a category: its messages may become more specific while the
// code stays (docs/spec/diagnostics.md).

#include "quidra/abi/dtype.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>

namespace quidra::abi {

enum class RuntimeFailureCode : std::uint8_t {
    autograd,
    call_depth_limit,
    cpu_threads,
    division_by_zero,
    exact_numeric,
    exact_unproven,
    for_iteration,
    gpu_async,
    gpu_scope,
    gpu_sync,
    index_bounds,
    integer_overflow,
    invalid_random_range,
    invalid_sleep_duration,
    nonfinite_result,
    numeric_conversion,
    package_execution_policy,
    parallel_body,
    power_domain,
    range_step_zero,
    real_overflow,
    shape_mismatch,
    shift_count,
    slice_assignment,
    slice_step,
    tensor,
    uninitialized,
    for_step,
    parallel_start,
    parallel_ownership,
    unbound_reference,
    reference_target,
    for_length,
    parallel_for,
};

struct RuntimeFailureCodeInfo {
    RuntimeFailureCode code;
    std::string_view spelling;
};

// In enumerator order.
inline constexpr std::array runtime_failure_codes{
    RuntimeFailureCodeInfo{RuntimeFailureCode::autograd, "AUTOGRAD"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::call_depth_limit, "CALL_DEPTH_LIMIT"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::cpu_threads, "CPU_THREADS"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::division_by_zero, "DIVISION_BY_ZERO"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::exact_numeric, "EXACT_NUMERIC"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::exact_unproven, "EXACT_UNPROVEN"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::for_iteration, "FOR_ITERATION"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::gpu_async, "GPU_ASYNC"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::gpu_scope, "GPU_SCOPE"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::gpu_sync, "GPU_SYNC"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::index_bounds, "INDEX_BOUNDS"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::integer_overflow, "INTEGER_OVERFLOW"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::invalid_random_range, "INVALID_RANDOM_RANGE"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::invalid_sleep_duration, "INVALID_SLEEP_DURATION"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::nonfinite_result, "NONFINITE_RESULT"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::numeric_conversion, "NUMERIC_CONVERSION"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::package_execution_policy, "PACKAGE_EXECUTION_POLICY"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::parallel_body, "PARALLEL_BODY"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::power_domain, "POWER_DOMAIN"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::range_step_zero, "RANGE_STEP_ZERO"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::real_overflow, "REAL_OVERFLOW"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::shape_mismatch, "SHAPE_MISMATCH"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::shift_count, "SHIFT_COUNT"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::slice_assignment, "SLICE_ASSIGNMENT"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::slice_step, "SLICE_STEP"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::tensor, "TENSOR"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::uninitialized, "UNINITIALIZED"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::for_step, "FOR_STEP"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::parallel_start, "PARALLEL_START"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::parallel_ownership, "PARALLEL_OWNERSHIP"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::unbound_reference, "UNBOUND_REFERENCE"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::reference_target, "REFERENCE_TARGET"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::for_length, "FOR_LENGTH"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::parallel_for, "PARALLEL_FOR"},
};

constexpr std::string_view spelling(RuntimeFailureCode code) {
    return runtime_failure_codes[static_cast<std::size_t>(code)].spelling;
}

// Whether the JSON details of a code's failures name their reason (its id,
// as "reason"): the codes whose reasons are details a program or tool tells
// apart (NUMERIC_CONVERSION: OUT_OF_RANGE, NON_FINITE, NOT_INTEGRAL,
// UNDECIDED; EXACT_UNPROVEN: ZERO_UNDECIDED, BUDGET_EXHAUSTED,
// NO_REFINEMENT).
constexpr bool reason_is_detail(RuntimeFailureCode code) {
    return code == RuntimeFailureCode::numeric_conversion ||
           code == RuntimeFailureCode::exact_unproven;
}

// The reasons of the runtime library's coded failures, grouped by code.
enum class FailureReason : std::uint16_t {
    autograd_failure,
    recursion_limit,
    invalid_thread_count,
    division_by_zero,
    exact_numeric_failure,
    exact_zero_undecided,
    exact_budget_exhausted,
    exact_no_refinement,
    iterated_array_reshaped,
    deferred_check_failed,
    package_scope_violation,
    gpu_sync_failed,
    index_out_of_bounds,
    slice_out_of_bounds,
    axis_index_out_of_bounds,
    axis_slice_out_of_bounds,
    integer_overflow,
    conversion_out_of_range,
    conversion_non_finite,
    conversion_not_integral,
    conversion_undecided,
    invalid_execution_policy,
    core_call_in_parallel_body,
    negative_integer_exponent,
    invalid_integer_power_width,
    tensor_failure,
    value_uninitialized,
    path_uninitialized,
    nat_negative,
    zero_power_zero,
    zero_power_negative,
    negative_base_fractional_exponent,
    slice_step_zero,
    slice_step_direction,
    slice_length_mismatch,
    tensor_slice_step_negative,
    real_overflow,
    real_invalid,
    real_zero_divisor,
    extern_nonfinite,
    package_nonfinite,
    shape_variable_mismatch,
    for_step_zero,
    for_step_ascending,
    for_step_descending,
    parallel_start_failed,
    parallel_call_unaccepted,
    reference_unbound,
    reference_interior,
    reference_serial_exhausted,
    for_lengths_mismatch,
    parallel_for_unsupported,
};

struct FailureReasonInfo {
    FailureReason reason;
    RuntimeFailureCode code;
    // Stable within its code: names the reason where a program or a tool
    // reads it instead of the message.
    std::string_view id;
    // The message, with named holes.
    std::string_view message;
};

// In enumerator order.
inline constexpr std::array failure_reasons{
    FailureReasonInfo{FailureReason::autograd_failure, RuntimeFailureCode::autograd,
                      "AUTOGRAD_FAILURE", "{message}"},
    // Generated code prints it from a constant (the module's @.msg.stack)
    // with {limit} = abi::self_call_depth_limit.
    FailureReasonInfo{FailureReason::recursion_limit, RuntimeFailureCode::call_depth_limit,
                      "RECURSION_LIMIT", "maximum recursion depth exceeded (limit {limit})"},
    FailureReasonInfo{FailureReason::invalid_thread_count, RuntimeFailureCode::cpu_threads,
                      "INVALID_THREAD_COUNT", "{message}"},
    // Exact division; generated code prints the same text for fixed-width
    // integers from its own constant.
    FailureReasonInfo{FailureReason::division_by_zero, RuntimeFailureCode::division_by_zero,
                      "ZERO_DIVISOR", "division by zero"},
    // The exact-number substrate: a provider's availability or domain, and
    // internal invariants.
    FailureReasonInfo{FailureReason::exact_numeric_failure, RuntimeFailureCode::exact_numeric,
                      "EXACT_NUMERIC_FAILURE", "{message}"},
    // An exact real decision (a comparison, the digits of a value, the
    // rounding of a contextual constant) that refinement could not prove;
    // {message} names the decision ("exact comparison could not be
    // proven"), the reason says why: the enclosure never separated from
    // zero or from the rounding or integer boundary in question within the
    // maximum precision, the refinement budget ran out, or a value has no
    // certified refinement.
    FailureReasonInfo{FailureReason::exact_zero_undecided, RuntimeFailureCode::exact_unproven,
                      "ZERO_UNDECIDED", "{message} within the maximum precision"},
    FailureReasonInfo{FailureReason::exact_budget_exhausted, RuntimeFailureCode::exact_unproven,
                      "BUDGET_EXHAUSTED", "{message}: the refinement budget ran out"},
    FailureReasonInfo{FailureReason::exact_no_refinement, RuntimeFailureCode::exact_unproven,
                      "NO_REFINEMENT", "{message}: a value has no certified refinement"},
    FailureReasonInfo{FailureReason::iterated_array_reshaped, RuntimeFailureCode::for_iteration,
                      "REPLACED_OR_RESIZED",
                      "array iterated by reference was replaced or resized during the loop"},
    FailureReasonInfo{FailureReason::deferred_check_failed, RuntimeFailureCode::gpu_async,
                      "DEFERRED_CHECK_FAILED", "{message}"},
    FailureReasonInfo{FailureReason::package_scope_violation, RuntimeFailureCode::gpu_scope,
                      "PACKAGE_SCOPE_VIOLATION", "{message}"},
    FailureReasonInfo{FailureReason::gpu_sync_failed, RuntimeFailureCode::gpu_sync,
                      "SYNC_FAILED", "{message}"},
    // Arrays, fixed arrays, strings and bin.
    FailureReasonInfo{FailureReason::index_out_of_bounds, RuntimeFailureCode::index_bounds,
                      "INDEX_OUT_OF_BOUNDS", "index {index} out of bounds for length {length}"},
    // Slices of strings and bins, in their written form.
    FailureReasonInfo{FailureReason::slice_out_of_bounds, RuntimeFailureCode::index_bounds,
                      "SLICE_OUT_OF_BOUNDS",
                      "slice {slice} out of bounds for length {length}"},
    // Tensor element indexes and slices name the failing axis (from 0).
    FailureReasonInfo{FailureReason::axis_index_out_of_bounds, RuntimeFailureCode::index_bounds,
                      "AXIS_INDEX_OUT_OF_BOUNDS",
                      "index {index} out of bounds for axis {axis} with length {length}"},
    FailureReasonInfo{FailureReason::axis_slice_out_of_bounds, RuntimeFailureCode::index_bounds,
                      "AXIS_SLICE_OUT_OF_BOUNDS",
                      "slice {slice} out of bounds for axis {axis} with length {length}"},
    FailureReasonInfo{FailureReason::integer_overflow, RuntimeFailureCode::integer_overflow,
                      "INTEGER_OVERFLOW", "integer overflow"},
    // Numeric conversions: scalar (value), array element and tensor element
    // casts, and the conversions of exact numbers.
    FailureReasonInfo{FailureReason::conversion_out_of_range,
                      RuntimeFailureCode::numeric_conversion, "OUT_OF_RANGE",
                      "numeric conversion out of range: {subject} cannot be represented as {type}"},
    FailureReasonInfo{FailureReason::conversion_non_finite, RuntimeFailureCode::numeric_conversion,
                      "NON_FINITE",
                      "numeric conversion failed: non-finite {subject} cannot be represented as "
                      "{type}"},
    FailureReasonInfo{FailureReason::conversion_not_integral,
                      RuntimeFailureCode::numeric_conversion, "NOT_INTEGRAL",
                      "numeric conversion failed: {subject} is not an integer and cannot be "
                      "represented as {type}"},
    FailureReasonInfo{FailureReason::conversion_undecided, RuntimeFailureCode::numeric_conversion,
                      "UNDECIDED",
                      "numeric conversion failed: the exact {subject} could not be decided for "
                      "{type}"},
    FailureReasonInfo{FailureReason::invalid_execution_policy,
                      RuntimeFailureCode::package_execution_policy, "INVALID_POLICY",
                      "invalid package execution policy"},
    FailureReasonInfo{FailureReason::core_call_in_parallel_body, RuntimeFailureCode::parallel_body,
                      "CORE_CALL_IN_BODY", "{message}"},
    FailureReasonInfo{FailureReason::negative_integer_exponent, RuntimeFailureCode::power_domain,
                      "NEGATIVE_EXPONENT", "integer exponent must be non-negative"},
    FailureReasonInfo{FailureReason::invalid_integer_power_width,
                      RuntimeFailureCode::power_domain, "INVALID_WIDTH",
                      "invalid integer power width"},
    FailureReasonInfo{FailureReason::tensor_failure, RuntimeFailureCode::tensor,
                      "TENSOR_FAILURE", "{message}"},
    FailureReasonInfo{FailureReason::value_uninitialized, RuntimeFailureCode::uninitialized,
                      "VALUE_UNINITIALIZED", "value is uninitialized"},
    FailureReasonInfo{FailureReason::path_uninitialized, RuntimeFailureCode::uninitialized,
                      "PATH_UNINITIALIZED", "'{path}' is uninitialized"},
    FailureReasonInfo{FailureReason::nat_negative, RuntimeFailureCode::integer_overflow,
                      "NAT_NEGATIVE", "nat subtraction result is negative"},
    FailureReasonInfo{FailureReason::zero_power_zero, RuntimeFailureCode::power_domain,
                      "ZERO_POWER_ZERO", "0 ^ 0 is undefined"},
    FailureReasonInfo{FailureReason::zero_power_negative, RuntimeFailureCode::power_domain,
                      "ZERO_POWER_NEGATIVE", "0 ^ a negative exponent is undefined"},
    FailureReasonInfo{FailureReason::negative_base_fractional_exponent,
                      RuntimeFailureCode::power_domain, "NEGATIVE_BASE",
                      "a negative base requires an integer exponent"},
    // A slice's run-time step: 0, or against the direction of its marker.
    FailureReasonInfo{FailureReason::slice_step_zero, RuntimeFailureCode::slice_step,
                      "STEP_ZERO", "slice step 0 is invalid"},
    FailureReasonInfo{FailureReason::slice_step_direction, RuntimeFailureCode::slice_step,
                      "STEP_DIRECTION", "slice step {step} cannot be used with '{marker}'"},
    // A slice assignment whose value has another number of elements.
    FailureReasonInfo{FailureReason::slice_length_mismatch, RuntimeFailureCode::slice_assignment,
                      "LENGTH_MISMATCH", "slice assignment needs {length} values, got {given}"},
    FailureReasonInfo{FailureReason::tensor_slice_step_negative, RuntimeFailureCode::slice_step,
                      "TENSOR_STEP_NEGATIVE", "tensor slices currently require a positive step"},
    // A fixed-width real operation on finite operands whose result is not
    // finite: {type} is the real type (a tensor's is tensor<T>), {operation}
    // the operation's name (abi::real_operations).
    FailureReasonInfo{FailureReason::real_overflow, RuntimeFailureCode::real_overflow,
                      "REAL_OVERFLOW", "{type} {operation} overflow"},
    // A NaN-producing arithmetic operation. Infinities are valid results.
    FailureReasonInfo{FailureReason::real_invalid, RuntimeFailureCode::nonfinite_result,
                      "REAL_INVALID", "{type} {operation} produced NaN"},
    // An indeterminate real division (0.0 / 0.0).
    FailureReasonInfo{FailureReason::real_zero_divisor, RuntimeFailureCode::division_by_zero,
                      "REAL_ZERO_DIVISOR", "{type} division by zero"},
    // A real value that C code gave Quidra code is not finite: the result of
    // an extern function ({operation}, the name the program calls it by).
    FailureReasonInfo{FailureReason::extern_nonfinite, RuntimeFailureCode::nonfinite_result,
                      "EXTERN_RESULT", "extern '{operation}' produced a NaN {type} value"},
    // A package function wrote a real tensor that holds NaN or an infinity
    // ({operation}: the function as the program calls it).
    FailureReasonInfo{FailureReason::package_nonfinite, RuntimeFailureCode::nonfinite_result,
                      "PACKAGE_OPERATION",
                      "package operation {operation} produced a NaN {type} value"},
    // A tensor stored in a binding whose brace shape fixes an axis by a
    // shape variable (L17): the variable, its value and the axis's extent.
    FailureReasonInfo{FailureReason::shape_variable_mismatch, RuntimeFailureCode::shape_mismatch,
                      "SHAPE_VARIABLE",
                      "shape variable '{name}' is {expected}, but axis {axis} of '{path}' is "
                      "{actual}"},
    FailureReasonInfo{FailureReason::for_step_zero, RuntimeFailureCode::for_step,
                     "STEP_ZERO", "for step 0 is invalid"},
    FailureReasonInfo{FailureReason::for_step_ascending, RuntimeFailureCode::for_step,
                     "STEP_ASCENDING", "for step {step} does not match an ascending loop"},
    FailureReasonInfo{FailureReason::for_step_descending, RuntimeFailureCode::for_step,
                     "STEP_DESCENDING", "for step {step} does not match a descending loop"},
    FailureReasonInfo{FailureReason::parallel_start_failed, RuntimeFailureCode::parallel_start,
                      "THREAD_START_FAILED", "cannot start parallel block: {message}"},
    FailureReasonInfo{FailureReason::parallel_call_unaccepted, RuntimeFailureCode::parallel_ownership,
                      "CALL_UNACCEPTED", "consuming call did not accept its arguments"},
    FailureReasonInfo{FailureReason::reference_unbound, RuntimeFailureCode::unbound_reference,
                      "REFERENCE_UNBOUND", "reference '{path}' is not bound"},
    FailureReasonInfo{FailureReason::reference_interior, RuntimeFailureCode::reference_target,
                      "INTERIOR_SELF_REFERENCE", "interior self-reference: '{path}' would refer into the value that holds it"},
    FailureReasonInfo{FailureReason::reference_serial_exhausted, RuntimeFailureCode::reference_target,
                      "ROOT_SERIAL_EXHAUSTED", "reference root serial exhausted"},
    FailureReasonInfo{FailureReason::for_lengths_mismatch, RuntimeFailureCode::for_length,
                      "LENGTH_MISMATCH", "all arrays in a for header must have the same length"},
    FailureReasonInfo{FailureReason::parallel_for_unsupported, RuntimeFailureCode::parallel_for,
                      "UNPROVEN_ITERATION", "{message}"},
};

constexpr const FailureReasonInfo& failure_reason_info(FailureReason reason) {
    return failure_reasons[static_cast<std::size_t>(reason)];
}

// What a numeric conversion converted ({subject}): its id names it in the
// JSON details, its text in the message.
enum class ConversionSubject : std::uint8_t { value, array_element, tensor_element };

struct ConversionSubjectInfo {
    ConversionSubject subject;
    std::string_view id;
    std::string_view text;
};

// In enumerator order; generated code passes the enumerator's value.
inline constexpr std::array conversion_subjects{
    ConversionSubjectInfo{ConversionSubject::value, "VALUE", "value"},
    ConversionSubjectInfo{ConversionSubject::array_element, "ARRAY_ELEMENT", "array element"},
    ConversionSubjectInfo{ConversionSubject::tensor_element, "TENSOR_ELEMENT", "tensor element"},
};

constexpr const ConversionSubjectInfo& conversion_subject_info(ConversionSubject subject) {
    return conversion_subjects[static_cast<std::size_t>(subject)];
}

// Why a numeric conversion failed, as generated code passes it (a stable
// code, independent of the reasons' order in failure_reasons).
enum class ConversionReason : std::uint8_t { out_of_range, non_finite, not_integral, undecided };

constexpr FailureReason conversion_failure_reason(ConversionReason reason) {
    switch (reason) {
        case ConversionReason::non_finite: return FailureReason::conversion_non_finite;
        case ConversionReason::not_integral: return FailureReason::conversion_not_integral;
        case ConversionReason::undecided: return FailureReason::conversion_undecided;
        case ConversionReason::out_of_range: break;
    }
    return FailureReason::conversion_out_of_range;
}

// A conversion's destination type ({type}) as generated code and the
// runtime pass it: a dtype code (QCORE_DTYPE_*) for a fixed-width type, or
// one of the exact types below.
inline constexpr int conversion_type_integer = 0x100;
inline constexpr int conversion_type_real = 0x101;
inline constexpr int conversion_type_natural = 0x102;
inline constexpr int conversion_type_complex = 0x103;

constexpr std::string_view conversion_type_name(int type) {
    if (type == conversion_type_integer) return "int";
    if (type == conversion_type_real) return "real";
    if (type == conversion_type_natural) return "nat";
    if (type == conversion_type_complex) return "com";
    if (numeric_dtype_code(type) || complex_dtype_code(type))
        return dtype_info(static_cast<Dtype>(type)).quidra_name;
    return "number";
}

// The fixed-width integer type of `bits` and signedness, as a conversion
// type.
constexpr int integer_conversion_type(int bits, bool is_signed) {
    switch (bits) {
        case 8: return is_signed ? QCORE_DTYPE_INT8 : QCORE_DTYPE_UINT8;
        case 16: return is_signed ? QCORE_DTYPE_INT16 : QCORE_DTYPE_UINT16;
        case 32: return is_signed ? QCORE_DTYPE_INT32 : QCORE_DTYPE_UINT32;
        default: return is_signed ? QCORE_DTYPE_INT64 : QCORE_DTYPE_UINT64;
    }
}

// A fixed-width real operation whose result may not be finite
// ({operation}), as generated code and the runtime pass it: the enumerator's
// value.
enum class RealOperation : std::uint8_t {
    addition, subtraction, multiplication, division, power,
    // A gradient a backward pass delivers (tensors).
    gradient
};

struct RealOperationInfo {
    RealOperation operation;
    std::string_view name;
};

// In enumerator order.
inline constexpr std::array real_operations{
    RealOperationInfo{RealOperation::addition, "addition"},
    RealOperationInfo{RealOperation::subtraction, "subtraction"},
    RealOperationInfo{RealOperation::multiplication, "multiplication"},
    RealOperationInfo{RealOperation::division, "division"},
    RealOperationInfo{RealOperation::power, "power"},
    RealOperationInfo{RealOperation::gradient, "gradient"},
};

constexpr const RealOperationInfo& real_operation_info(RealOperation operation) {
    return real_operations[static_cast<std::size_t>(operation)];
}

// The arguments a template's holes name.
struct FailureArgs {
    long long index{};
    long long length{};
    long long start{};
    long long end{};
    long long axis{};
    long long limit{};
    std::string_view type{};
    ConversionSubject subject{};
    std::string_view message{};
    // The storage a failure names, as the source writes it.
    std::string_view path{};
    // A slice as the source writes it, with its operands' values.
    std::string_view slice{};
    long long step{};
    long long given{};
    std::string_view marker{};
    // The operation that failed.
    std::string_view operation{};
    // A shape variable, the extent it requires and the extent found.
    std::string_view name{};
    long long expected{};
    long long actual{};
    // An arbitrary-precision for step, in canonical decimal notation. Empty
    // keeps the fixed-width step field used by slices and existing clients.
    std::string_view step_text{};
};

enum class FailureHole : std::uint8_t {
    index, length, start, end, axis, limit, type, subject, message, path, slice, step, given,
    marker, operation, name, expected, actual, unknown
};

constexpr FailureHole failure_hole(std::string_view name) {
    if (name == "index") return FailureHole::index;
    if (name == "length") return FailureHole::length;
    if (name == "start") return FailureHole::start;
    if (name == "end") return FailureHole::end;
    if (name == "axis") return FailureHole::axis;
    if (name == "limit") return FailureHole::limit;
    if (name == "type") return FailureHole::type;
    if (name == "subject") return FailureHole::subject;
    if (name == "message") return FailureHole::message;
    if (name == "path") return FailureHole::path;
    if (name == "slice") return FailureHole::slice;
    if (name == "step") return FailureHole::step;
    if (name == "given") return FailureHole::given;
    if (name == "marker") return FailureHole::marker;
    if (name == "operation") return FailureHole::operation;
    if (name == "name") return FailureHole::name;
    if (name == "expected") return FailureHole::expected;
    if (name == "actual") return FailureHole::actual;
    return FailureHole::unknown;
}

// The value of an integer hole.
constexpr long long failure_hole_value(FailureHole hole, const FailureArgs& args) {
    switch (hole) {
        case FailureHole::index: return args.index;
        case FailureHole::length: return args.length;
        case FailureHole::start: return args.start;
        case FailureHole::end: return args.end;
        case FailureHole::axis: return args.axis;
        case FailureHole::limit: return args.limit;
        case FailureHole::step: return args.step;
        case FailureHole::given: return args.given;
        case FailureHole::expected: return args.expected;
        case FailureHole::actual: return args.actual;
        case FailureHole::type:
        case FailureHole::subject:
        case FailureHole::message:
        case FailureHole::path:
        case FailureHole::slice:
        case FailureHole::marker:
        case FailureHole::operation:
        case FailureHole::name:
        case FailureHole::unknown: break;
    }
    return 0;
}


// Writes `value` in decimal into `digits` (at least 20 bytes) and returns the
// number of bytes written.
constexpr std::size_t write_decimal(char* digits, long long value) {
    auto magnitude = value < 0 ? 0ULL - static_cast<unsigned long long>(value)
                               : static_cast<unsigned long long>(value);
    char reversed[20]{};
    std::size_t count = 0;
    do {
        reversed[count++] = static_cast<char>('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude != 0);
    std::size_t size = 0;
    if (value < 0) digits[size++] = '-';
    while (count != 0) digits[size++] = reversed[--count];
    return size;
}

// Calls visit(name, hole) for each hole of `reason`'s template, in order: the
// JSON report writes the integer ones as the failure's details.
template <class Visit>
void visit_failure_holes(FailureReason reason, Visit&& visit) {
    const auto text = failure_reason_info(reason).message;
    std::size_t start = 0;
    while (true) {
        const auto open = text.find('{', start);
        if (open == std::string_view::npos) return;
        const auto close = text.find('}', open);
        const auto name = text.substr(open + 1, close - open - 1);
        visit(name, failure_hole(name));
        start = close + 1;
    }
}

// Writes the message of `reason` with `args` in pieces, each through
// write(std::string_view): the runtime's reporter writes them to stderr
// without allocating, and a constant expression can build a message from
// constant arguments (the compiler's diagnostic constants).
template <class Write>
constexpr void write_failure_message(FailureReason reason, const FailureArgs& args,
                                     Write&& write) {
    const auto text = failure_reason_info(reason).message;
    std::size_t start = 0;
    while (start < text.size()) {
        const auto open = text.find('{', start);
        if (open == std::string_view::npos) {
            write(text.substr(start));
            return;
        }
        if (open > start) write(text.substr(start, open - start));
        const auto close = text.find('}', open);
        const auto hole = failure_hole(text.substr(open + 1, close - open - 1));
        if (hole == FailureHole::message) {
            write(args.message);
        } else if (hole == FailureHole::path) {
            write(args.path);
        } else if (hole == FailureHole::slice) {
            write(args.slice);
        } else if (hole == FailureHole::step && !args.step_text.empty()) {
            write(args.step_text);
        } else if (hole == FailureHole::marker) {
            write(args.marker);
        } else if (hole == FailureHole::operation) {
            write(args.operation);
        } else if (hole == FailureHole::name) {
            write(args.name);
        } else if (hole == FailureHole::type) {
            write(args.type);
        } else if (hole == FailureHole::subject) {
            write(conversion_subject_info(args.subject).text);
        } else {
            char digits[24]{};
            const auto value = failure_hole_value(hole, args);
            write(std::string_view(digits, write_decimal(digits, value)));
        }
        start = close + 1;
    }
}

inline std::string format(FailureReason reason, const FailureArgs& args) {
    std::string text;
    write_failure_message(reason, args, [&](std::string_view piece) { text += piece; });
    return text;
}

namespace detail {

constexpr bool runtime_failure_tables_are_ordered() {
    for (std::size_t i = 0; i < runtime_failure_codes.size(); ++i)
        if (static_cast<std::size_t>(runtime_failure_codes[i].code) != i) return false;
    for (std::size_t i = 0; i < conversion_subjects.size(); ++i)
        if (static_cast<std::size_t>(conversion_subjects[i].subject) != i) return false;
    for (std::size_t i = 0; i < failure_reasons.size(); ++i)
        if (static_cast<std::size_t>(failure_reasons[i].reason) != i) return false;
    for (std::size_t i = 0; i < real_operations.size(); ++i)
        if (static_cast<std::size_t>(real_operations[i].operation) != i) return false;
    return true;
}

constexpr bool failure_templates_name_known_holes() {
    for (const auto& row : failure_reasons) {
        const auto text = row.message;
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '}') return false;
            if (text[i] != '{') continue;
            const auto close = text.find('}', i);
            if (close == std::string_view::npos) return false;
            if (failure_hole(text.substr(i + 1, close - i - 1)) == FailureHole::unknown)
                return false;
            i = close;
        }
    }
    return true;
}

constexpr bool failure_reasons_are_distinct_per_code() {
    for (std::size_t i = 0; i < failure_reasons.size(); ++i)
        for (std::size_t j = i + 1; j < failure_reasons.size(); ++j) {
            if (failure_reasons[i].code != failure_reasons[j].code) continue;
            if (failure_reasons[i].id == failure_reasons[j].id ||
                failure_reasons[i].message == failure_reasons[j].message)
                return false;
        }
    return true;
}

} // namespace detail

static_assert(detail::runtime_failure_tables_are_ordered(),
              "runtime failure codes and reasons are listed in enumerator order");
static_assert(detail::failure_templates_name_known_holes(),
              "every hole of a failure message template has a known name");
static_assert(detail::failure_reasons_are_distinct_per_code(),
              "within a code, every reason has its own id and its own template");

} // namespace quidra::abi
