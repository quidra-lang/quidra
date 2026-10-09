#pragma once

// The runtime's one reporter of failures (runtime_system.cpp, through the
// location step of runtime_location.cpp). Every coded failure of the runtime
// library, quidra_runtime_fail_at for generated code, the uncoded
// runtime-library failures and the device layer's failures without a site
// print their report and end the program here:
// 1. a package encode hold the thread still has ends first, so exit does not
//    wait for its stream; a protocol violation the hold recorded is the
//    failure reported instead (GPU_SCOPE, in the spelling of the failure it
//    replaces);
// 2. the report "Quidra runtime error[CODE] at FILE:L:C", the source line
//    and a caret when the compiled file is present and unchanged, and the
//    message, formatted from the reason's template
//    (quidra/abi/runtime_failure.hpp), inside the gutter;
// 3. device::exit_failed_program(), which flushes and ends the process at
//    once on package callback and task.all threads (or the immediate exit,
//    for the callers that end that way and for a later report of the thread
//    that reported first).
// A site without a line of its own (line 0: package code, the prelude's
// helpers without a location, the codes without a site) is reported at the
// user's statement, the innermost statement of user code that runs on the
// thread, or names the root file without a line when none runs.

#include "quidra/abi/layout.hpp"
#include "quidra/abi/runtime_failure.hpp"

#include <string_view>

namespace quidra::runtime {

// How a report is spelled: each runtime site keeps the text it printed
// before it reported through here.
struct ReportStyle {
    // The JSON report carries the running statement's provenance
    // (source_revision, node_id, node_kind, source_file); human text never
    // does.
    bool provenance = false;
    // Ends the process at once after the report (flush every stream, then
    // _Exit): for the failures raised where exit handlers must not run.
    bool immediate_exit = false;
};

// Reports `reason` (its code is the reason's) at a site's immediates (the
// line encoded as (file index << 32) | line, or 0) and ends the program with
// abi::failure_exit_status.
[[noreturn]] void report_failure(abi::FailureReason reason, const abi::FailureArgs& args,
                                 unsigned long long line, unsigned long long column,
                                 ReportStyle style = {});

// Reports `reason` at a statement's record instead of a site's immediates:
// the callers that hold a record (a package hold left open, the Core work a
// package encoder scope refuses). Null reports no location.
[[noreturn]] void report_at_statement(abi::FailureReason reason, const abi::FailureArgs& args,
                                      const abi::SourceProvenanceRecord* statement,
                                      ReportStyle style = {});

// A numeric conversion failure (NUMERIC_CONVERSION) of `subject` to the type
// spelled `type`, for `reason`, at a site's immediates.
[[noreturn]] void report_conversion(abi::ConversionReason reason, std::string_view type,
                                    abi::ConversionSubject subject, unsigned long long line,
                                    unsigned long long column, ReportStyle style = {});

// The calling thread's reason of its last declined conversion: a
// try-conversion of the runtime that fails records why (only on failure),
// and the error block of a fallible conversion reads it right after
// (quidra_runtime_last_conversion_reason).
void record_conversion_reason(abi::ConversionReason reason);
abi::ConversionReason last_conversion_reason();

// An uncoded runtime-library failure: "Quidra runtime error at FILE:L:C",
// located at the user's statement, then the message.
[[noreturn]] void report_uncoded(std::string_view message);

// The innermost user statement of the calling thread, or null.
const abi::SourceProvenanceRecord* current_user_statement();

} // namespace quidra::runtime
