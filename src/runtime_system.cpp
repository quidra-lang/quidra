#include "device_backend.hpp"
#include "runtime_counters.hpp"
#include "runtime_location.hpp"
#include "runtime_report.hpp"
#include "quidra/abi/call_depth.hpp"
#include "quidra/abi/layout.hpp"
#include "quidra/abi/process_status.hpp"
#include "quidra/abi/runtime_entry_points.hpp"
#include "quidra/abi/runtime_failure.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <string>

// The ABI this runtime shares with generated code (include/quidra/abi).
namespace abi = quidra::abi;

namespace {
// The innermost statement (QUIDRA_COUNTERS sites, the provenance suffix):
// set by every statement with a public node, of user and of package code.
thread_local const abi::SourceProvenanceRecord* source_provenance{};
// The innermost statement of user code: where a failure without an exact
// site of its own (package code, the prelude's helpers without a location,
// deferred checks, holds, test assertions) is reported.
thread_local const abi::SourceProvenanceRecord* user_statement{};

bool source_site_of(const abi::SourceProvenanceRecord* record,
                    quidra::counters::SourceSite& site) {
    if (!record) return false;
    site.file = record->file;
    site.node_id = record->node_id;
    site.line = record->line;
    site.column = record->column;
    return true;
}

// Reports the statement currently executing to the device layer, which uses
// it to attribute host waits (QUIDRA_COUNTERS).
bool current_source_site(quidra::counters::SourceSite& site) {
    return source_site_of(source_provenance, site);
}

// Reports the user statement currently executing to the device layer, which
// names it as the origin of a deferred GPU check: its file as the source
// table shows it (the display path), its line and column.
bool current_user_source_site(quidra::counters::SourceSite& site) {
    if (!source_site_of(user_statement, site)) return false;
    if (const auto* entry = quidra::runtime::source_entry(user_statement->file))
        site.file = entry->display;
    return true;
}

struct SourceSiteRegistration {
    SourceSiteRegistration() {
        quidra::counters::set_source_site_provider(current_source_site);
        quidra::counters::set_user_source_site_provider(current_user_source_site);
    }
};
SourceSiteRegistration source_site_registration;

std::uint64_t random_next(void* generator) {
    auto* state=static_cast<std::uint64_t*>(generator);
    *state+=0x9e3779b97f4a7c15ULL;
    std::uint64_t value=*state;
    value=(value^(value>>30U))*0xbf58476d1ce4e5b9ULL;
    value=(value^(value>>27U))*0x94d049bb133111ebULL;
    return value^(value>>31U);
}
}

// Package encode holds (native_extension.h) end before the package code that
// began them returns. Where control comes back to Quidra code, a hold the
// thread still has stops the program at the statement that was running,
// which made the extern call. (A custom autograd backward callback is
// checked when it returns, at its backward() call: runtime.cpp.)
void quidra_runtime_check_package_hold(const char* returned) {
    if (quidra::device::open_package_holds.load(std::memory_order_relaxed) == 0)
        return;
    std::string message;
    if (quidra::device::end_left_package_hold(returned, message) ==
        quidra::device::LeftHold::None)
        return;
    quidra::runtime::report_at_statement(abi::FailureReason::package_scope_violation,
                                         abi::FailureArgs{.message = message},
                                         user_statement, {.provenance = true});
}

// The return of an exported function (export "C") that may leave device work
// behind, before control goes back to C: a package encode hold the thread
// left open stops the program (GPU_SCOPE), and deferred device checks still
// pending are drained, so that a failure they hold is reported now, as the
// canonical diagnostic with status 101, and not later inside unrelated host
// code. While nothing is pending it costs two relaxed loads.
extern "C" void quidra_runtime_export_leave() {
    if (quidra::device::open_package_holds.load(std::memory_order_relaxed) != 0)
        quidra_runtime_check_package_hold("an exported C function returned");
    if (quidra::device::pending_deferred_validations.load(std::memory_order_relaxed) == 0)
        return;
    std::string error;
    if (quidra::device::drain_deferred_validations(error)) return;
    quidra::counters::report_unlocated_failure(abi::FailureReason::deferred_check_failed, error,
                                               false);
}

// Runs at every statement of user code, so the hold check is one relaxed load
// unless some thread holds a stream. The record names both the innermost
// statement and the user's statement; null (the entry's return) clears both.
extern "C" void quidra_runtime_set_source_provenance(
    const void* provenance) {
    if (quidra::device::open_package_holds.load(std::memory_order_relaxed) != 0)
        quidra_runtime_check_package_hold(
            "the package code returned to Quidra code");
    source_provenance = static_cast<const abi::SourceProvenanceRecord*>(provenance);
    user_statement = source_provenance;
}

// Runs at every statement of package code: it names the innermost statement
// and leaves the user's statement alone.
extern "C" void quidra_runtime_set_package_source_provenance(
    const void* provenance) {
    if (quidra::device::open_package_holds.load(std::memory_order_relaxed) != 0)
        quidra_runtime_check_package_hold(
            "the package code returned to Quidra code");
    source_provenance = static_cast<const abi::SourceProvenanceRecord*>(provenance);
}

// Package code saves the user's statement before a call that may enter user
// code and restores it afterwards; a user statement without a public node,
// and a user statement about to make a call that may read it after a call
// into user code, set it to their own record.
extern "C" const void* quidra_runtime_user_statement() {
    return user_statement;
}

extern "C" void quidra_runtime_restore_user_statement(const void* statement) {
    user_statement = static_cast<const abi::SourceProvenanceRecord*>(statement);
}

namespace {

// Ends a package encode hold the calling thread still has, so that exit does
// not wait for its stream (native_extension.h). Returns true with the
// protocol violation the hold recorded in `violation`: that is the failure
// to report (GPU_SCOPE).
bool end_hold_before_report(std::string& violation) {
    return quidra::device::open_package_holds.load(std::memory_order_relaxed) != 0 &&
           quidra::device::end_left_package_hold("the program failed", violation) ==
               quidra::device::LeftHold::Violated;
}

// The innermost statement's provenance record, which the JSON report
// carries; null without a statement that has a public node.
const abi::SourceProvenanceRecord* attributable_provenance() {
    const auto* provenance = source_provenance;
    if (!provenance || !provenance->node_id || provenance->node_id[0] == '\0') return nullptr;
    return provenance;
}

// Writes one report through the location step and ends the program: one
// reporter at a time (runtime_location.hpp), then exit_failed_program, or
// the immediate exit for a later report of the reporting thread and for the
// callers that end that way.
template <class WriteMessage>
[[noreturn]] void report_and_exit(const quidra::runtime::ReportHead& head,
                                  const quidra::runtime::ReportLocation& location,
                                  quidra::runtime::ReportStyle style,
                                  quidra::runtime::ReportData data,
                                  WriteMessage&& write_message) {
    using quidra::runtime::ReporterTurn;
    const auto turn = quidra::runtime::take_reporter();
    if (turn == ReporterTurn::nested) quidra::runtime::immediate_failure_exit();
    auto& message = quidra::runtime::report_message();
    message.clear();
    write_message(message);
    if (style.provenance) data.provenance = attributable_provenance();
    quidra::runtime::write_report(head, location, message.text(), data);
    quidra::runtime::reporter_flushed();
    if (turn == ReporterTurn::later || style.immediate_exit)
        quidra::runtime::immediate_failure_exit();
    // The program is ending: a check at exit runs no statement, so it names
    // the root file.
    source_provenance = nullptr;
    user_statement = nullptr;
    quidra::device::exit_failed_program();
}

quidra::runtime::ReportHead coded_head(std::string_view code, quidra::runtime::ReportStyle) {
    return {quidra::runtime::ReportKind::coded, code};
}

// Why the calling thread's last try-conversion declined (written only on
// failure).
thread_local abi::ConversionReason declined_conversion_reason = abi::ConversionReason::out_of_range;

abi::ConversionReason conversion_reason_from(int reason) {
    switch (reason) {
        case static_cast<int>(abi::ConversionReason::non_finite):
            return abi::ConversionReason::non_finite;
        case static_cast<int>(abi::ConversionReason::not_integral):
            return abi::ConversionReason::not_integral;
        case static_cast<int>(abi::ConversionReason::undecided):
            return abi::ConversionReason::undecided;
        default: return abi::ConversionReason::out_of_range;
    }
}

abi::ConversionSubject conversion_subject_from(int subject) {
    switch (subject) {
        case static_cast<int>(abi::ConversionSubject::array_element):
            return abi::ConversionSubject::array_element;
        case static_cast<int>(abi::ConversionSubject::tensor_element):
            return abi::ConversionSubject::tensor_element;
        default: return abi::ConversionSubject::value;
    }
}

// A trap's details: the integer arguments its message template names.
quidra::runtime::ReportData trap_details(const abi::FailureReason& reason,
                                         const abi::FailureArgs& args) {
    quidra::runtime::ReportData data;
    data.reason = &reason;
    data.args = args;
    return data;
}

// The device layer's failures without a site of their own (it is also part
// of the compiler, which has no reporter): reported at the user's statement,
// or naming the root file when none runs.
[[noreturn]] void report_device_failure(abi::FailureReason reason, std::string_view message,
                                        bool immediate_exit) {
    quidra::runtime::report_failure(reason, abi::FailureArgs{.message = message}, 0, 0,
                                    {.immediate_exit = immediate_exit});
}

struct DeviceFailureRegistration {
    DeviceFailureRegistration() {
        quidra::counters::set_unlocated_failure_reporter(report_device_failure);
    }
};
DeviceFailureRegistration device_failure_registration;

} // namespace

namespace quidra::runtime {

const abi::SourceProvenanceRecord* current_user_statement() {
    return user_statement;
}

void report_failure(abi::FailureReason reason, const abi::FailureArgs& args,
                    unsigned long long line, unsigned long long column,
                    ReportStyle style) {
    std::string violation;
    if (end_hold_before_report(violation)) {
        report_failure(abi::FailureReason::package_scope_violation,
                       abi::FailureArgs{.message = violation}, line, column, style);
    }
    report_and_exit(coded_head(abi::spelling(abi::failure_reason_info(reason).code), style),
                    locate_site(line, column, user_statement), style, trap_details(reason, args),
                    [&](ReportMessage& message) {
                        abi::write_failure_message(
                            reason, args, [&](std::string_view piece) { message.append(piece); });
                    });
}

void report_at_statement(abi::FailureReason reason, const abi::FailureArgs& args,
                         const abi::SourceProvenanceRecord* statement, ReportStyle style) {
    std::string violation;
    if (end_hold_before_report(violation)) {
        report_at_statement(abi::FailureReason::package_scope_violation,
                            abi::FailureArgs{.message = violation}, statement, style);
    }
    report_and_exit(coded_head(abi::spelling(abi::failure_reason_info(reason).code), style),
                    locate_statement(statement), style, trap_details(reason, args),
                    [&](ReportMessage& message) {
                        abi::write_failure_message(
                            reason, args, [&](std::string_view piece) { message.append(piece); });
                    });
}

void report_conversion(abi::ConversionReason reason, std::string_view type,
                       abi::ConversionSubject subject, unsigned long long line,
                       unsigned long long column, ReportStyle style) {
    report_failure(abi::conversion_failure_reason(reason),
                   abi::FailureArgs{.type = type, .subject = subject}, line, column, style);
}

void record_conversion_reason(abi::ConversionReason reason) {
    declined_conversion_reason = reason;
}

abi::ConversionReason last_conversion_reason() {
    return declined_conversion_reason;
}

void report_uncoded(std::string_view text) {
    std::string violation;
    if (end_hold_before_report(violation)) {
        report_failure(abi::FailureReason::package_scope_violation,
                       abi::FailureArgs{.message = violation}, 0, 0);
    }
    report_and_exit({ReportKind::uncoded, {}}, locate_site(0, 0, user_statement), {}, {},
                    [&](ReportMessage& message) { message.append(text); });
}

} // namespace quidra::runtime

// The details generated code's failures carry: CALL_DEPTH_LIMIT's limit (its
// message is the module's constant built from the same template), none for
// the others.
static quidra::runtime::ReportData generated_failure_details(const char* code) {
    static constexpr auto recursion = abi::FailureReason::recursion_limit;
    if (code && std::string_view(code) == abi::spelling(abi::RuntimeFailureCode::call_depth_limit))
        return trap_details(recursion, abi::FailureArgs{.limit = abi::self_call_depth_limit});
    return {};
}

// Generated code names the code and the message of its failures (string
// constants of the module).
extern "C" void quidra_runtime_fail_at(
    const char* code, const char* message, unsigned long long line,
    unsigned long long column) {
    const quidra::runtime::ReportStyle style{.provenance = true};
    std::string violation;
    if (end_hold_before_report(violation)) {
        quidra::runtime::report_failure(abi::FailureReason::package_scope_violation,
                                        abi::FailureArgs{.message = violation}, line,
                                        column, style);
    }
    report_and_exit(coded_head(code ? code : "RUNTIME", style),
                    quidra::runtime::locate_site(line, column, user_statement), style,
                    generated_failure_details(code),
                    [&](quidra::runtime::ReportMessage& text) {
                        text.append(message ? message : "runtime failure");
                    });
}

// Generated code's numeric conversion failures: the destination type code
// (a dtype code or abi::conversion_type_integer/real/natural), the subject and the
// reason (abi::ConversionSubject, abi::ConversionReason).
extern "C" void quidra_runtime_conversion_fail(int type, int subject, int reason,
                                               unsigned long long line,
                                               unsigned long long column) {
    quidra::runtime::report_conversion(conversion_reason_from(reason),
                                       abi::conversion_type_name(type),
                                       conversion_subject_from(subject), line, column,
                                       {.provenance = true});
}

// The message of a fallible conversion's error value: immortal (outside the
// managed allocations, so retain and release ignore it), one per
// destination type, subject and reason, formatted on first use.
extern "C" const char* quidra_runtime_conversion_message(int type, int subject, int reason) {
    static constexpr int types[] = {
        QCORE_DTYPE_INT64,  QCORE_DTYPE_INT8,    QCORE_DTYPE_INT16,  QCORE_DTYPE_INT32,
        QCORE_DTYPE_UINT8,  QCORE_DTYPE_UINT16,  QCORE_DTYPE_UINT32, QCORE_DTYPE_UINT64,
        QCORE_DTYPE_FLOAT64, QCORE_DTYPE_FLOAT32, abi::conversion_type_integer,
        abi::conversion_type_real, abi::conversion_type_natural};
    static constexpr std::size_t type_count = sizeof types / sizeof types[0];
    static constexpr std::size_t subject_count = abi::conversion_subjects.size();
    static constexpr std::size_t reason_count = 4;
    static const auto* messages = [] {
        auto* table = new std::array<std::string, type_count * subject_count * reason_count>;
        for (std::size_t t = 0; t < type_count; ++t)
            for (std::size_t s = 0; s < subject_count; ++s)
                for (std::size_t r = 0; r < reason_count; ++r)
                    (*table)[(t * subject_count + s) * reason_count + r] = abi::format(
                        abi::conversion_failure_reason(static_cast<abi::ConversionReason>(r)),
                        abi::FailureArgs{.type = abi::conversion_type_name(types[t]),
                                         .subject = static_cast<abi::ConversionSubject>(s)});
        return table;
    }();
    std::size_t type_index = 0;
    while (type_index < type_count && types[type_index] != type) ++type_index;
    if (type_index == type_count) type_index = 0;
    const auto subject_index = static_cast<std::size_t>(conversion_subject_from(subject));
    const auto reason_index = static_cast<std::size_t>(conversion_reason_from(reason));
    return (*messages)[(type_index * subject_count + subject_index) * reason_count + reason_index]
        .c_str();
}

// Why the calling thread's last try-conversion declined: the error block of a
// fallible conversion whose cause the generated code cannot tell reads it.
extern "C" int quidra_runtime_last_conversion_reason() {
    return static_cast<int>(declined_conversion_reason);
}

extern "C" void quidra_runtime_bounds_fail(
    long long index, long long length, unsigned long long line,
    unsigned long long column) {
    quidra::runtime::report_failure(abi::FailureReason::index_out_of_bounds,
                                    abi::FailureArgs{.index = index, .length = length},
                                    line, column, {.provenance = true});
}

// A failed test.check: the assertion's statement, the snippet, no message;
// status 1.
extern "C" void quidra_test_assert(bool condition) {
    if(condition) return;
    using quidra::runtime::ReporterTurn;
    const auto turn = quidra::runtime::take_reporter();
    if (turn == ReporterTurn::nested) quidra::runtime::immediate_failure_exit();
    quidra::runtime::write_report({quidra::runtime::ReportKind::test_assertion, {}},
                                  quidra::runtime::locate_statement(user_statement), {},
                                  {.status = 1, .provenance = attributable_provenance()});
    quidra::runtime::reporter_flushed();
    if (turn == ReporterTurn::later) quidra::runtime::immediate_failure_exit();
    std::exit(1);
}
// flush() returns void | error: 0 here is success, anything else becomes
// the error alternative, which fails fast when the statement discards it.
extern "C" int quidra_flush() {
    if (std::fflush(stdout) == 0 && !std::ferror(stdout)) return 0;
    return 1;
}
// print reports the standard output stream's error state after writing. The
// flag is sticky, so a failed write is never silently lost: the next print or
// flush() reports it.
extern "C" int quidra_output_status() {
    return std::ferror(stdout) ? 1 : 0;
}
extern "C" double quidra_time_now(bool sync) {
    if (sync) {
        std::string error;
        if (!quidra::device::synchronize_all(error)) {
            quidra::runtime::report_failure(abi::FailureReason::gpu_sync_failed,
                                            abi::FailureArgs{.message = error}, 0, 0);
        }
    }
    using clock=std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}
extern "C" void quidra_gpu_sync(long long index,
                                unsigned long long line,
                                unsigned long long column) {
    if (index < 0) {
        quidra::runtime::report_failure(
            abi::FailureReason::gpu_sync_failed,
            abi::FailureArgs{.message = "GPU index must be non-negative"}, line, column,
            {.provenance = true});
    }
    std::string error;
    if (!quidra::device::synchronize(static_cast<int>(index), error)) {
        quidra::runtime::report_failure(abi::FailureReason::gpu_sync_failed,
                                        abi::FailureArgs{.message = error}, line, column,
                                        {.provenance = true});
    }
}
extern "C" bool quidra_time_sleep(double seconds) {
    if(!std::isfinite(seconds)||seconds<0.0) return false;
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    return true;
}
extern "C" long long quidra_random_int(void* generator,long long start,long long end) {
    const auto span=static_cast<std::uint64_t>(end)-static_cast<std::uint64_t>(start);
    const auto threshold=(std::uint64_t{0}-span)%span;
    std::uint64_t sample=0;
    do{sample=random_next(generator);}while(sample<threshold);
    const auto result_bits=static_cast<std::uint64_t>(start)+(sample%span);
    return std::bit_cast<long long>(result_bits);
}
extern "C" double quidra_random_float(void* generator) {
    const auto sample=random_next(generator)>>11U;
    return static_cast<double>(sample)*(1.0/9007199254740992.0);
}
extern "C" bool quidra_random_bool(void* generator) {
    return (random_next(generator)&1ULL)!=0;
}
