#pragma once

// The location step of the runtime's reporter (runtime_location.cpp): where
// a failure is reported, and the report's text.
//
// - The source table: generated code registers its module's table
//   (@.quidra.sources, abi::SourceTable) before its first statement. A
//   site's line immediate holds the file's index in its upper 32 bits and
//   the line in its lower 32 bits; line 0 means "no exact site": the
//   failure is reported at the user's statement, or names the root file
//   without a line when none runs.
// - The report: "Quidra runtime error[CODE] at FILE:L:C", then, when the
//   compiled file is present and identical (its size and SHA-256 recorded at
//   compile time), a blank line, the source line after its line number and a
//   caret under the column; then the message inside the gutter, one line per
//   line of the message. Without the snippet the message follows the header
//   in a zero-width gutter ("| message"). A failure before any table is
//   registered keeps the one-line "Quidra runtime error[CODE]: message".
// - The machine format: with QUIDRA_ERROR_FORMAT=json in the program's
//   environment, each report is one line of JSON on stderr in place of the
//   text (schema version 1, docs/spec/diagnostics.md): the code, the message,
//   the details, the location, the provenance of the innermost statement and
//   the trace fields. Human text carries no provenance.
// - One reporter at a time: the first report takes the reporter; another
//   thread waits until it is flushed and ends the process without printing;
//   a later report of the same thread prints and takes the immediate exit.
//
// Nothing here runs before a failure, and nothing allocates: the report is
// formatted in static buffers and the source file is read once (twice for a
// line longer than the line buffer) on the failure path.

#include "quidra/abi/layout.hpp"
#include "quidra/abi/runtime_failure.hpp"

#include <cstddef>
#include <string_view>

namespace quidra::runtime {

// A report's position, resolved against the registered source table.
struct ReportLocation {
    // Null: no source table is registered (the one-line form).
    const abi::SourceTableEntry* entry{};
    // 0: the file without a line (a failure with no site and no user
    // statement).
    unsigned long long line{};
    unsigned long long column{};
};

// The position of a site's immediates; line 0 takes `user_statement`.
ReportLocation locate_site(unsigned long long encoded_line, unsigned long long column,
                           const abi::SourceProvenanceRecord* user_statement);
// The position of a statement's record (null: the root file without a line).
ReportLocation locate_statement(const abi::SourceProvenanceRecord* statement);
// The entry of the source file a provenance record names, or null.
const abi::SourceTableEntry* source_entry(const char* file);

enum class ReportKind { coded, uncoded, test_assertion };

struct ReportHead {
    ReportKind kind{ReportKind::coded};
    std::string_view code;
};

// What the machine format adds to a report: the exit status, the details
// of a trap (the integer arguments its reason's template names, under the
// holes' names) and the innermost statement's provenance (null where none is
// attributable).
struct ReportData {
    int status{101};
    // The trap's reason, or null: no details.
    const abi::FailureReason* reason{};
    abi::FailureArgs args{};
    const abi::SourceProvenanceRecord* provenance{};
};

// Writes one report to stderr and flushes it: the header, the snippet under
// the hash rule and the message inside the gutter (a test assertion has
// none); or, with QUIDRA_ERROR_FORMAT=json, its JSON line.
void write_report(const ReportHead& head, const ReportLocation& location,
                  std::string_view message, const ReportData& data);

// The message buffer a report's message is formatted into before it is laid
// out (static; a longer message is cut at its size).
class ReportMessage {
public:
    void clear() { size_ = 0; }
    void append(std::string_view text);
    std::string_view text() const;

private:
    std::size_t size_{};
};
ReportMessage& report_message();

enum class ReporterTurn {
    // The first report of the process.
    first,
    // A later report of the thread that reported first, after its report
    // was flushed: printed, then the immediate exit.
    later,
    // A report while this thread's first report is still being written:
    // nothing more is printed; the immediate exit.
    nested,
};
// Takes the reporter. From another thread than the reporting one, waits
// until the first report is flushed, then flushes every stream and ends the
// process with the failure status without printing.
ReporterTurn take_reporter();
// The current report is written and flushed.
void reporter_flushed();
// Flushes every stream and ends the process at once with the failure status.
[[noreturn]] void immediate_failure_exit();

} // namespace quidra::runtime
