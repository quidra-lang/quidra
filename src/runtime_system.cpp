#include "device_backend.hpp"
#include "runtime_counters.hpp"
#include "runtime/profile.hpp"
#include "runtime/timeline.hpp"
#include "runtime/device_observation.hpp"
#include "runtime_internal.hpp"
#include "runtime_location.hpp"
#include "runtime_report.hpp"
#include "node_failure.hpp"
#include "quidra/abi/call_depth.hpp"
#include "quidra/abi/error_record.hpp"
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

void finish_observation() noexcept {
    quidra::timeline::note_point(quidra::timeline::Point::ExitBegin);
    quidra::profile::statement_changed(nullptr);
    quidra::device::finish_metal_capture(true);
    quidra::timeline::note_point(quidra::timeline::Point::ExitEnd);
    quidra::counters::flush_report();
    (void)quidra::timeline::flush_report();
    (void)quidra::profile::flush_report();
}

void begin_exit_observation() noexcept {
    quidra::timeline::note_point(quidra::timeline::Point::ExitBegin);
    quidra::profile::statement_changed(nullptr);
    // Host exit callbacks registered before program entry may still enter
    // Quidra. The library's original shutdown finalizers run after them.
}

void start_program_observation() noexcept {
    quidra::timeline::mark_program_started();
    quidra::profile::mark_program_started();
    // Registered at program entry, before device resources arm their exit
    // drains. Exit callbacks run in reverse registration order.
    if (quidra::timeline::enabled() || quidra::profile::enabled() ||
        quidra::profile::capture_requested()) {
        static std::atomic<bool> registered{false};
        if (!registered.exchange(true, std::memory_order_relaxed))
            (void)std::atexit(begin_exit_observation);
    }
}

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
        quidra::timeline::remember_runtime_initialized();
        quidra::profile::configure_runtime();
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
extern "C" void quidra_runtime_foreign_leave() {
    quidra::device::seal_native_package_work();
}

void quidra_runtime_check_package_hold(const char* returned) {
    quidra::device::seal_native_package_work();
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
    quidra::device::seal_native_package_work();
    if (quidra::device::open_package_holds.load(std::memory_order_relaxed) != 0)
        quidra_runtime_check_package_hold("an exported C function returned");
    if (quidra::device::pending_deferred_validations.load(std::memory_order_relaxed) == 0 &&
        !quidra::device::has_owned_native_work())
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
    if (provenance) {
        quidra::counters::mark_program_started();
        start_program_observation();
    }
    quidra::device::seal_native_package_work();
    if (quidra::device::open_package_holds.load(std::memory_order_relaxed) != 0)
        quidra_runtime_check_package_hold(
            "the package code returned to Quidra code");
    source_provenance = static_cast<const abi::SourceProvenanceRecord*>(provenance);
    user_statement = source_provenance;
    if (quidra::profile::enabled() || quidra::timeline::enabled()) {
        quidra::counters::SourceSite site{};
        const bool known = source_site_of(source_provenance, site);
        if (known) quidra::timeline::note_point(quidra::timeline::Point::FirstStatement, &site);
        quidra::profile::statement_changed(known ? &site : nullptr);
    }
}

// Runs at every statement of package code: it names the innermost statement
// and leaves the user's statement alone.
extern "C" void quidra_runtime_set_package_source_provenance(
    const void* provenance) {
    quidra::device::seal_native_package_work();
    if (quidra::device::open_package_holds.load(std::memory_order_relaxed) != 0)
        quidra_runtime_check_package_hold(
            "the package code returned to Quidra code");
    source_provenance = static_cast<const abi::SourceProvenanceRecord*>(provenance);
    if (quidra::profile::enabled()) {
        quidra::counters::SourceSite site{};
        quidra::profile::statement_changed(source_site_of(source_provenance, site) ? &site : nullptr);
    }
}

// Package code saves the user's statement before a call that may enter user
// code and restores it afterwards; a user statement without a public node,
// and a user statement about to make a call that may read it after a call
// into user code, set it to their own record.
extern "C" const void* quidra_runtime_user_statement() {
    return user_statement;
}

extern "C" void quidra_runtime_restore_user_statement(const void* statement) {
    quidra::device::seal_native_package_work();
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
                                  quidra::runtime::ReportSite raw_site,
                                  WriteMessage&& write_message) {
    if (quidra::runtime::node_failure_active()) {
        quidra::runtime::ReportMessage message;
        write_message(message);
        if (style.provenance) data.provenance = attributable_provenance();
        quidra::runtime::capture_node_failure(head, location, message.text(), data,raw_site);
    }
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
    quidra::runtime::preserve_managed_state_for_exit();
    quidra::device::exit_failed_program();
}

quidra::runtime::ReportHead coded_head(std::string_view code, quidra::runtime::ReportStyle) {
    return {quidra::runtime::ReportKind::coded, code};
}

// Why the calling thread's last failed parse, input read or declined
// try-conversion failed, as the reason's position in its code's list
// (written only on failure).
thread_local int last_failure_reason = 0;

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
        quidra::runtime::set_failure_flush_handler(finish_observation);
        quidra::profile::set_capture_finalizer(quidra::device::finish_metal_capture);
    }
};
DeviceFailureRegistration device_failure_registration;

} // namespace

namespace quidra::runtime {

NodeSourceState save_node_source_state() noexcept {
    return {source_provenance, user_statement, last_failure_reason};
}
void restore_node_source_state(NodeSourceState state) noexcept {
    source_provenance = state.provenance;
    user_statement = state.user_statement;
    last_failure_reason = state.failure_reason;
    if (quidra::profile::enabled()) {
        quidra::counters::SourceSite site{};
        quidra::profile::statement_changed(source_site_of(source_provenance, site) ? &site : nullptr);
    }
}

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
                    report_site(line,column,user_statement),
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
                    report_site(0,0,statement,true),
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
    last_failure_reason = static_cast<int>(reason);
}

abi::ConversionReason last_conversion_reason() {
    return conversion_reason_from(last_failure_reason);
}

void record_failure_reason(int position) {
    last_failure_reason = position;
}

void report_uncoded(std::string_view text) {
    std::string violation;
    if (end_hold_before_report(violation)) {
        report_failure(abi::FailureReason::package_scope_violation,
                       abi::FailureArgs{.message = violation}, 0, 0);
    }
    report_and_exit({ReportKind::uncoded, {}}, locate_site(0, 0, user_statement), {}, {},report_site(0,0,user_statement),
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
                    quidra::runtime::report_site(line,column,user_statement),
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

// Generated code's finite checks of real arithmetic: the real type (a dtype
// code), the operation (abi::RealOperation) and whether a division's
// divisor was zero; the result of the operation at line and column was not
// finite.
extern "C" void quidra_runtime_real_fail(int type, int operation, bool zero_divisor,
                                         unsigned long long line, unsigned long long column) {
    const auto index = operation >= 0 &&
                               static_cast<std::size_t>(operation) < abi::real_operations.size()
                           ? static_cast<std::size_t>(operation)
                           : static_cast<std::size_t>(abi::RealOperation::power);
    const auto reason = zero_divisor ? abi::FailureReason::real_zero_divisor
                                     : abi::FailureReason::real_invalid;
    quidra::runtime::report_failure(
        reason,
        abi::FailureArgs{.type = abi::conversion_type_name(type),
                         .operation = abi::real_operations[index].name},
        line, column, {.provenance = true});
}

// An extern function's real result is not finite: its type (a dtype code)
// and the name the program calls the function by; at the call.
extern "C" void quidra_runtime_extern_nonfinite(int type, const char* name,
                                                unsigned long long line,
                                                unsigned long long column) {
    quidra::runtime::report_failure(
        abi::FailureReason::extern_nonfinite,
        abi::FailureArgs{.type = abi::conversion_type_name(type),
                         .operation = name ? std::string_view(name) : std::string_view("?")},
        line, column, {.provenance = true});
}

// Why the calling thread's last failed parse, input read or declined
// try-conversion failed: the error block of a site whose cause the generated
// code cannot tell reads it right after the failing call, as the index into
// the site's record array.
extern "C" int quidra_runtime_last_reason() {
    return last_failure_reason;
}

// A fail-fast stop of an error value: its own code (ERROR for a codeless
// user error) and message, at its origin (where it was made; for an error
// made in package code, the user statement that led into it); the JSON
// report carries its details. The consumption site is used only for an
// error with no origin.
extern "C" void quidra_runtime_fail_error(const void* error, unsigned long long line,
                                          unsigned long long column) {
    const quidra::runtime::ReportStyle style{.provenance = true};
    std::string violation;
    if (end_hold_before_report(violation)) {
        quidra::runtime::report_failure(abi::FailureReason::package_scope_violation,
                                        abi::FailureArgs{.message = violation}, line,
                                        column, style);
    }
    const auto* record = static_cast<const abi::ErrorRecord*>(error);
    quidra::runtime::ReportSite raw_site;
    if(record && record->line!=0)
        raw_site=quidra::runtime::report_site(record->line,record->column,user_statement);
    else if(record && record->statement)
        raw_site=quidra::runtime::report_site(0,0,static_cast<const abi::SourceProvenanceRecord*>(record->statement),true);
    else raw_site=quidra::runtime::report_site(line,column,user_statement);
    const auto location=quidra::runtime::locate_report_site(raw_site);
    quidra::runtime::ReportData data;
    data.error = record;
    // The stop is printed ("unhandled at") when the error stops away from
    // where it was made and no hop names the stop: a stored error consumed
    // later. A fail-fast of a call records the call's hop instead.
    const bool stop_hop = record && record->hops && record->hops->count != 0 &&
                          record->hops->entries[record->hops->count - 1].site->kind ==
                              static_cast<std::uint64_t>(abi::ErrorHopKind::fail_fast);
    if (record && line != 0 && !stop_hop) {
        const auto stop = quidra::runtime::locate_site(line, column, nullptr);
        bool same = false;
        if (record->line != 0) {
            same = record->line == line && record->column == column;
        } else if (record->statement) {
            const auto origin = quidra::runtime::locate_statement(
                static_cast<const abi::SourceProvenanceRecord*>(record->statement));
            same = origin.entry == stop.entry && origin.line == stop.line;
        }
        if (!same) {
            data.unhandled = true;
            data.unhandled_line = line;
            data.unhandled_column = column;
        }
    }
    const std::string_view code =
        record && record->code ? std::string_view(record->code) : abi::user_error_code;
    report_and_exit(coded_head(code, style), location, style, data,raw_site,
                    [&](quidra::runtime::ReportMessage& text) {
                        text.append(record && record->message ? record->message : "");
                    });
}

extern "C" void quidra_runtime_bounds_fail(
    long long index, long long length, unsigned long long line,
    unsigned long long column) {
    quidra::runtime::report_failure(abi::FailureReason::index_out_of_bounds,
                                    abi::FailureArgs{.index = index, .length = length},
                                    line, column, {.provenance = true});
}

extern "C" [[noreturn]] void quidra_process_exit(int status) {
    if (quidra::runtime::node_failure_active()) {
        std::string violation;
        if (end_hold_before_report(violation)) {
            quidra::runtime::report_failure(abi::FailureReason::package_scope_violation,
                abi::FailureArgs{.message=violation},0,0);
        }
        quidra::runtime::capture_node_exit(status);
    }
    std::exit(status);
}

// A failed test.check: the assertion's statement, the snippet, no message;
// status 1.
extern "C" void quidra_test_assert(bool condition) {
    if(condition) return;
    if(quidra::runtime::node_failure_active()) {
        std::string violation;
        if (end_hold_before_report(violation)) {
            quidra::runtime::report_failure(abi::FailureReason::package_scope_violation,
                abi::FailureArgs{.message=violation},0,0);
        }
        const auto site=quidra::runtime::report_site(0,0,user_statement,true);
        quidra::runtime::capture_node_failure({quidra::runtime::ReportKind::test_assertion,{}},
            quidra::runtime::locate_report_site(site),{},
            {.status=1,.provenance=attributable_provenance()},site);
    }
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
    quidra::timeline::note_point(quidra::timeline::Point::TimeMark);
    quidra::counters::time_mark();
    if (quidra::timeline::time_marks_are_steps() ||
        quidra::profile::capture_time_marks_are_steps()) {
        quidra::device::observation_step_boundary();
        quidra::profile::advance_capture_step();
        quidra::timeline::note_point(quidra::timeline::Point::StepBoundary);
    }
    using clock=std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

extern "C" void quidra_runtime_start_observation() noexcept {
    start_program_observation();
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
extern "C" unsigned long long quidra_random_bits(void* generator){return random_next(generator);}

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

extern "C" void* quidra_parallel_frame_create(unsigned long long bytes,
    unsigned long long alignment,void (*drop)(void*)) {
    return quidra::runtime::create_node_frame(bytes,alignment,drop);
}
extern "C" void quidra_parallel_frame_leave(void* frame) {
    quidra::runtime::leave_node_frame(frame);
}

extern "C" void quidra_parallel_frame_cleanup(void (*drop)(void*),void* slot) {
    quidra::runtime::cleanup_node_slot(drop,slot);
}

extern "C" bool quidra_parallel_cleanup_active() {
    return quidra::runtime::node_failure_active();
}

extern "C" void quidra_parallel_cleanup_rethrow() {
    const auto* owner=quidra::runtime::current_node_failure;
    if(owner && (owner->report || owner->exit_status))
        throw quidra::runtime::CapturedNodeFailure{};
}
