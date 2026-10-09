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
//   conversion's destination type and {subject} what was converted (a value,
//   an array element or a tensor element); {path} is the storage a failure
//   names, as the source writes it; {message} is a text the failing
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
    numeric_conversion,
    package_execution_policy,
    parallel_body,
    power_domain,
    range_step_zero,
    shape_mismatch,
    shift_count,
    task_array,
    task_null,
    task_start,
    tensor,
    unhandled_error,
    uninitialized,
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
    RuntimeFailureCodeInfo{RuntimeFailureCode::numeric_conversion, "NUMERIC_CONVERSION"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::package_execution_policy, "PACKAGE_EXECUTION_POLICY"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::parallel_body, "PARALLEL_BODY"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::power_domain, "POWER_DOMAIN"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::range_step_zero, "RANGE_STEP_ZERO"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::shape_mismatch, "SHAPE_MISMATCH"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::shift_count, "SHIFT_COUNT"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::task_array, "TASK_ARRAY"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::task_null, "TASK_NULL"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::task_start, "TASK_START"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::tensor, "TENSOR"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::unhandled_error, "UNHANDLED_ERROR"},
    RuntimeFailureCodeInfo{RuntimeFailureCode::uninitialized, "UNINITIALIZED"},
};

constexpr std::string_view spelling(RuntimeFailureCode code) {
    return runtime_failure_codes[static_cast<std::size_t>(code)].spelling;
}

// Whether the JSON details of a code's failures name their reason (its id,
// as "reason"): the codes whose reasons are details a program or tool tells
// apart (NUMERIC_CONVERSION: OUT_OF_RANGE, NON_FINITE, NOT_INTEGRAL,
// UNDECIDED).
constexpr bool reason_is_detail(RuntimeFailureCode code) {
    return code == RuntimeFailureCode::numeric_conversion;
}

// The reasons of the runtime library's coded failures, grouped by code.
enum class FailureReason : std::uint16_t {
    autograd_failure,
    recursion_limit,
    invalid_thread_count,
    division_by_zero,
    exact_numeric_failure,
    comparison_unproven,
    domain_unproven,
    evaluation_budget_exhausted,
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
    task_invalid_shared_storage,
    task_invalid_operation_array,
    task_null_operation_array,
    task_invalid_storage,
    task_null_operation,
    task_start_failed,
    tensor_failure,
    value_uninitialized,
    path_uninitialized,
    nat_negative,
    zero_power_zero,
    zero_power_negative,
    negative_base_fractional_exponent,
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
    // An exact real comparison or evaluation the finite proof budget could
    // not decide.
    FailureReasonInfo{FailureReason::comparison_unproven, RuntimeFailureCode::exact_unproven,
                      "COMPARISON_UNPROVEN",
                      "real comparison could not be proven within the finite proof budget"},
    FailureReasonInfo{FailureReason::domain_unproven, RuntimeFailureCode::exact_unproven,
                      "DOMAIN_UNPROVEN", "real comparison operand domain could not be proven"},
    FailureReasonInfo{FailureReason::evaluation_budget_exhausted,
                      RuntimeFailureCode::exact_unproven, "EVALUATION_BUDGET",
                      "real evaluation budget exhausted"},
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
    // text.slice and bin slices; every slice is half-open, [start, end).
    FailureReasonInfo{FailureReason::slice_out_of_bounds, RuntimeFailureCode::index_bounds,
                      "SLICE_OUT_OF_BOUNDS",
                      "slice [{start}, {end}) out of bounds for length {length}"},
    // Tensor element indexes and slices name the failing axis (from 0).
    FailureReasonInfo{FailureReason::axis_index_out_of_bounds, RuntimeFailureCode::index_bounds,
                      "AXIS_INDEX_OUT_OF_BOUNDS",
                      "index {index} out of bounds for axis {axis} with length {length}"},
    FailureReasonInfo{FailureReason::axis_slice_out_of_bounds, RuntimeFailureCode::index_bounds,
                      "AXIS_SLICE_OUT_OF_BOUNDS",
                      "slice [{start}, {end}) out of bounds for axis {axis} with length {length}"},
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
    FailureReasonInfo{FailureReason::task_invalid_shared_storage, RuntimeFailureCode::task_array,
                      "INVALID_SHARED_STORAGE", "task.all received invalid shared storage"},
    FailureReasonInfo{FailureReason::task_invalid_operation_array,
                      RuntimeFailureCode::task_array, "INVALID_OPERATION_ARRAY",
                      "task.all received an invalid operation array"},
    FailureReasonInfo{FailureReason::task_null_operation_array, RuntimeFailureCode::task_array,
                      "NULL_OPERATION_ARRAY", "task.all received a null operation array"},
    FailureReasonInfo{FailureReason::task_invalid_storage, RuntimeFailureCode::task_array,
                      "INVALID_STORAGE", "task.all received invalid storage"},
    FailureReasonInfo{FailureReason::task_null_operation, RuntimeFailureCode::task_null,
                      "NULL_OPERATION", "task.all received a null operation"},
    FailureReasonInfo{FailureReason::task_start_failed, RuntimeFailureCode::task_start,
                      "THREAD_START_FAILED", "cannot start task: {message}"},
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

constexpr std::string_view conversion_type_name(int type) {
    if (type == conversion_type_integer) return "int";
    if (type == conversion_type_real) return "real";
    if (type == conversion_type_natural) return "nat";
    if (type >= QCORE_DTYPE_INT64 && type <= QCORE_DTYPE_FLOAT32)
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
};

enum class FailureHole : std::uint8_t {
    index, length, start, end, axis, limit, type, subject, message, path, unknown
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
        case FailureHole::type:
        case FailureHole::subject:
        case FailureHole::message:
        case FailureHole::path:
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
