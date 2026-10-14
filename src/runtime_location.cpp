// The location step of the runtime's reporter; see runtime_location.hpp.

#include "runtime_location.hpp"

#include "runtime_trace.hpp"

#include "platform/environment.hpp"
#include "platform/sha256.hpp"
#include "quidra/abi/display_width.hpp"
#include "quidra/abi/process_status.hpp"
#include "quidra/abi/runtime_entry_points.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace abi = quidra::abi;

namespace {

std::atomic<const abi::SourceTable*> registered_table{nullptr};
thread_local const abi::SourceTable* selected_source_table{};
thread_local bool source_table_selected{};
const abi::SourceTable* current_registered_table() noexcept {
    return source_table_selected ? selected_source_table : registered_table.load(std::memory_order_acquire);
}


constexpr std::size_t buffer_size = 64 * 1024;

// --- the report's output: one static buffer, written to stderr when full and
// at the end.
char output_buffer[buffer_size];
std::size_t output_size = 0;

void flush_output() {
    if (output_size != 0) std::fwrite(output_buffer, 1, output_size, stderr);
    output_size = 0;
}

void put(std::string_view text) {
    while (!text.empty()) {
        if (output_size == buffer_size) flush_output();
        const auto n = (std::min)(text.size(), buffer_size - output_size);
        std::memcpy(output_buffer + output_size, text.data(), n);
        output_size += n;
        text.remove_prefix(n);
    }
}

void put_char(char c) { put(std::string_view(&c, 1)); }

void put_spaces(std::size_t count) {
    while (count-- != 0) put_char(' ');
}

void put_decimal(unsigned long long value) {
    char digits[24];
    std::size_t n = 0;
    do {
        digits[n++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (n != 0) put_char(digits[--n]);
}

std::size_t decimal_width(unsigned long long value) {
    std::size_t width = 1;
    while (value >= 10) {
        value /= 10;
        ++width;
    }
    return width;
}

// --- the message buffer.
thread_local char message_buffer[buffer_size];

// --- the source file of a snippet: opened without blocking (a FIFO or a
// device node at the path cannot block the open), checked through the open
// descriptor (a regular file of the recorded size), then read once through
// SHA-256 while line L is noted.
class SourceFile {
public:
    explicit SourceFile(const char* path) {
#ifdef _WIN32
        static wchar_t wide[32768];
        if (!path || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide,
                                         static_cast<int>(sizeof wide / sizeof wide[0])) == 0)
            return;
        handle_ = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
#else
        if (path) fd_ = ::open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
#endif
    }
    ~SourceFile() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
        if (fd_ >= 0) ::close(fd_);
#endif
    }
    SourceFile(const SourceFile&) = delete;
    SourceFile& operator=(const SourceFile&) = delete;

    // A regular file of `size` bytes.
    bool is_regular_file_of_size(unsigned long long size) const {
#ifdef _WIN32
        if (handle_ == INVALID_HANDLE_VALUE || GetFileType(handle_) != FILE_TYPE_DISK) return false;
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handle_, &info)) return false;
        if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
        const auto actual = (static_cast<unsigned long long>(info.nFileSizeHigh) << 32) |
                            info.nFileSizeLow;
        return actual == size;
#else
        struct stat status {};
        if (fd_ < 0 || ::fstat(fd_, &status) != 0 || !S_ISREG(status.st_mode)) return false;
        return static_cast<unsigned long long>(status.st_size) == size;
#endif
    }

    // Reads up to `size` bytes at the current position; 0 at the end, -1 on
    // an error.
    long long read(void* data, std::size_t size) {
#ifdef _WIN32
        DWORD done = 0;
        if (!ReadFile(handle_, data, static_cast<DWORD>(size), &done, nullptr)) return -1;
        return done;
#else
        for (;;) {
            const auto n = ::read(fd_, data, size);
            if (n < 0 && errno == EINTR) continue;
            return n;
        }
#endif
    }

    // Reads up to `size` bytes at `offset`.
    long long read_at(void* data, std::size_t size, unsigned long long offset) {
#ifdef _WIN32
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(handle_, position, nullptr, FILE_BEGIN)) return -1;
        return read(data, size);
#else
        for (;;) {
            const auto n = ::pread(fd_, data, size, static_cast<off_t>(offset));
            if (n < 0 && errno == EINTR) continue;
            return n;
        }
#endif
    }

private:
#ifdef _WIN32
    HANDLE handle_{INVALID_HANDLE_VALUE};
#else
    int fd_{-1};
#endif
};

unsigned char chunk_buffer[buffer_size];
char line_buffer[buffer_size];

// Line `line` of the compiled file, when the file is present and identical:
// its bytes (in line_buffer when they fit), without a trailing '\r'.
struct SnippetLine {
    unsigned long long offset{};
    unsigned long long length{};
    bool in_buffer{};
};

bool find_snippet_line(SourceFile& file, const abi::SourceTableEntry& entry,
                       unsigned long long line, SnippetLine& out) {
    if (!entry.revision || entry.revision[0] == '\0') return false;
    if (!file.is_regular_file_of_size(entry.byte_size)) return false;
    try {
        quidra::platform::Sha256 hash;
        unsigned long long current = 1;
        unsigned long long offset = 0;
        bool started = line == 1;
        bool ended = false;
        unsigned long long start = 0;
        unsigned long long end = 0;
        for (;;) {
            const auto n = file.read(chunk_buffer, sizeof chunk_buffer);
            if (n < 0) return false;
            if (n == 0) break;
            hash.update(chunk_buffer, static_cast<std::size_t>(n));
            for (long long i = 0; i < n && !ended; ++i) {
                if (chunk_buffer[i] != '\n') continue;
                const auto position = offset + static_cast<unsigned long long>(i);
                if (current == line) {
                    end = position;
                    ended = true;
                }
                ++current;
                if (current == line) {
                    start = position + 1;
                    started = true;
                }
            }
            offset += static_cast<unsigned long long>(n);
        }
        if (offset != entry.byte_size) return false;
        const auto digest = hash.finish();
        static constexpr char hex[] = "0123456789abcdef";
        char text[65];
        for (std::size_t i = 0; i < digest.size(); ++i) {
            text[2 * i] = hex[digest[i] >> 4];
            text[2 * i + 1] = hex[digest[i] & 0x0f];
        }
        text[64] = '\0';
        if (std::strcmp(text, entry.revision) != 0) return false;
        if (!started) return false;
        if (!ended) end = offset;
        out.offset = start;
        out.length = end - start;
        if (out.length != 0) {
            // A CRLF file: the '\r' before the line feed is not shown.
            unsigned char last = 0;
            if (file.read_at(&last, 1, end - 1) == 1 && last == '\r') --out.length;
        }
        out.in_buffer = out.length <= sizeof line_buffer;
        if (out.in_buffer && out.length != 0 &&
            file.read_at(line_buffer, static_cast<std::size_t>(out.length), out.offset) !=
                static_cast<long long>(out.length))
            return false;
        return true;
    } catch (...) {
        return false;
    }
}

// The caret's padding: the bytes of columns 1 .. column - 1 as display
// columns (a tab stays a tab; each code point takes its display width;
// continuation bytes add nothing).
class Padding {
public:
    void feed(unsigned char byte) {
        if (pending_ != 0) {
            if ((byte & 0xC0) == 0x80) {
                code_point_ = (code_point_ << 6) | (byte & 0x3F);
                if (--pending_ == 0) put_spaces(static_cast<std::size_t>(abi::display_width(code_point_)));
                return;
            }
            pending_ = 0;
        }
        if (byte == '\t') put_char('\t');
        else if (byte < 0x80) put_char(' ');
        else if ((byte & 0xE0) == 0xC0) start(byte & 0x1F, 1);
        else if ((byte & 0xF0) == 0xE0) start(byte & 0x0F, 2);
        else if ((byte & 0xF8) == 0xF0) start(byte & 0x07, 3);
        // A stray continuation byte adds nothing.
    }

private:
    void start(std::uint32_t bits, int continuation) {
        code_point_ = bits;
        pending_ = continuation;
    }
    std::uint32_t code_point_{};
    int pending_{};
};

void put_snippet(SourceFile& file, const SnippetLine& line, unsigned long long number,
                 unsigned long long column) {
    const auto width = decimal_width(number);
    // The header line ends; an empty line sets the snippet off.
    put("\n\n");
    put_decimal(number);
    put(" | ");
    if (line.in_buffer) {
        put(std::string_view(line_buffer, static_cast<std::size_t>(line.length)));
    } else {
        // A line longer than the line buffer: read again from the same file.
        unsigned long long done = 0;
        while (done < line.length) {
            const auto want = static_cast<std::size_t>(
                (std::min<unsigned long long>)(sizeof chunk_buffer, line.length - done));
            const auto n = file.read_at(chunk_buffer, want, line.offset + done);
            if (n <= 0) break;
            put(std::string_view(reinterpret_cast<const char*>(chunk_buffer),
                                 static_cast<std::size_t>(n)));
            done += static_cast<unsigned long long>(n);
        }
    }
    put("\n");
    put_spaces(width);
    put(" | ");
    Padding padding;
    const auto before = column > 0 ? column - 1 : 0;
    const auto in_line = (std::min)(before, line.length);
    if (line.in_buffer) {
        for (unsigned long long i = 0; i < in_line; ++i)
            padding.feed(static_cast<unsigned char>(line_buffer[i]));
    } else {
        unsigned long long done = 0;
        while (done < in_line) {
            const auto want = static_cast<std::size_t>(
                (std::min<unsigned long long>)(sizeof chunk_buffer, in_line - done));
            const auto n = file.read_at(chunk_buffer, want, line.offset + done);
            if (n <= 0) break;
            for (long long i = 0; i < n; ++i) padding.feed(chunk_buffer[i]);
            done += static_cast<unsigned long long>(n);
        }
    }
    // A column past the end of the line: one space per missing byte.
    put_spaces(static_cast<std::size_t>(before - in_line));
    put("^");
}

// The body lines: each LF-separated line of `text` after the gutter: `gutter`
// spaces (the line number's width and its space with a snippet, none
// without one), the bar, a space and the line.
void put_body(std::size_t gutter, std::string_view text) {
    for (;;) {
        const auto newline = text.find('\n');
        const auto piece = text.substr(0, newline);
        put("\n");
        put_spaces(gutter);
        put("|");
        if (!piece.empty()) {
            put(" ");
            put(piece);
        }
        if (newline == std::string_view::npos) break;
        text.remove_prefix(newline + 1);
    }
}

// A JSON string: the text between quotes, with '"', '\\' and control
// characters escaped.
void put_json_string(std::string_view text) {
    static constexpr char hex[] = "0123456789abcdef";
    put("\"");
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        switch (c) {
            case '"': put("\\\""); break;
            case '\\': put("\\\\"); break;
            case '\n': put("\\n"); break;
            case '\r': put("\\r"); break;
            case '\t': put("\\t"); break;
            default:
                if (byte < 0x20) {
                    put("\\u00");
                    put_char(hex[byte >> 4]);
                    put_char(hex[byte & 0x0f]);
                } else {
                    put_char(c);
                }
        }
    }
    put("\"");
}

void put_json_string_or_null(const char* text) {
    if (text) put_json_string(text);
    else put("null");
}

void put_signed(long long value) {
    if (value < 0) {
        put_char('-');
        put_decimal(0ULL - static_cast<unsigned long long>(value));
    } else {
        put_decimal(static_cast<unsigned long long>(value));
    }
}

// The details of an error value by its schema's names: text and name
// details as strings, integer details as numbers, absent ones left out; a
// user or package error's one detail, its text.
void put_error_details(const abi::ErrorRecord& record) {
    const auto* schema = abi::error_value_schema(record.schema);
    if (!schema) {
        put("\"text\": ");
        put_json_string(record.message ? record.message : "");
        return;
    }
    const auto details = abi::error_record_details(record, *schema);
    bool first = true;
    for (std::size_t i = 0; i < schema->details.size(); ++i) {
        if (!details[i].present) continue;
        if (!first) put(", ");
        first = false;
        put_json_string(schema->details[i].name);
        put(": ");
        if (details[i].kind == abi::ErrorDetailKind::integer) put_signed(details[i].integer);
        else put_json_string(details[i].text);
    }
}

// The location of a site's immediates for a trace line: the display path,
// with the line and the column when the site has them.
quidra::runtime::ReportLocation trace_location(unsigned long long line, unsigned long long column);
quidra::runtime::ReportLocation origin_location(const abi::ErrorRecord& record);

void put_location_text(const quidra::runtime::ReportLocation& location) {
    if (!location.entry) {
        put("?");
        return;
    }
    put(location.entry->display ? location.entry->display : "");
    if (location.line != 0) {
        put(":");
        put_decimal(location.line);
        put(":");
        put_decimal(location.column);
    }
}

void begin_trace_line(std::size_t gutter, std::string_view indent) {
    put("\n");
    put_spaces(gutter);
    put("| ");
    put(indent);
}

// One list of trace lines with the elision of the report: more than 20
// lines print the first 10, "… N more …" and the last 10.
template <class PutLine>
void put_elided(std::size_t gutter, std::string_view indent, std::size_t count, PutLine&& put_line) {
    if (count <= 20) {
        for (std::size_t i = 0; i < count; ++i) put_line(i);
        return;
    }
    for (std::size_t i = 0; i < 10; ++i) put_line(i);
    begin_trace_line(gutter, indent);
    put("\u2026 ");
    put_decimal(count - 20);
    put(" more \u2026");
    for (std::size_t i = count - 10; i < count; ++i) put_line(i);
}

void put_times(std::uint64_t repeat) {
    if (repeat > 1) {
        put(" (");
        put_decimal(repeat);
        put(" times)");
    }
}

// The name of the function a hop left: through the call-site table for a
// call through an `func` value (the function actually called), else the
// site's.
std::string_view hop_function(const abi::ErrorHopEntry& entry) {
    if (entry.callee) {
        const auto name = quidra::runtime::function_at(entry.callee);
        if (!name.empty()) return name;
    }
    return entry.site->name ? entry.site->name : "?";
}

// "propagated through F() at FILE:L:C" for one hop entry; an entry the
// error left N times in a row says "(N times)".
void put_hop_line(std::size_t gutter, std::string_view indent, const abi::ErrorHopEntry& entry) {
    begin_trace_line(gutter, indent);
    put("propagated through ");
    put(hop_function(entry));
    put(" at ");
    put_location_text(trace_location(entry.site->line, entry.site->column));
    put_times(entry.repeat);
}

// One hop entry per line, inner to outer.
void put_hop_lines(std::size_t gutter, std::string_view indent, const abi::ErrorHopList* hops) {
    if (!hops) return;
    put_elided(gutter, indent, static_cast<std::size_t>(hops->count),
               [&](std::size_t i) { put_hop_line(gutter, indent, hops->entries[i]); });
}

// One line of the walked path (runtime_trace.hpp): a call, a merged call
// with every candidate site, the parallel node boundary, or the
// marker of an incomplete path.
void put_path_line(std::size_t gutter, const quidra::runtime::CallPath& path,
                   const quidra::runtime::PathItem& item) {
    using quidra::runtime::PathItemKind;
    begin_trace_line(gutter, "");
    if (item.kind == PathItemKind::marker) {
        put("\u2026 call path incomplete \u2026");
        return;
    }
    put("propagated through ");
    put(item.function);
    put(" at ");
    if (item.kind == PathItemKind::merged) {
        for (std::uint32_t i = 0; i < item.candidate_count; ++i) {
            if (i != 0) put(" or ");
            const auto& candidate = path.candidates[item.first_candidate + i];
            put_location_text(trace_location(candidate.line, candidate.column));
        }
    } else {
        put_location_text(trace_location(item.line, item.column));
    }
    put_times(item.repeat);
}

// The trace of a report: the error's hops, "unhandled at" when the stop is
// printed, the walked path (one list for elision), then each cause,
// outermost first, "caused by: [CODE] message (at FILE:L:C)" with its hops
// indented under it. False when there is none.
bool has_trace(const quidra::runtime::ReportData& data, const quidra::runtime::CallPath& path) {
    if (path.count != 0) return true;
    return data.error &&
           ((data.error->hops && data.error->hops->count != 0) || data.unhandled || data.error->cause);
}

void put_trace(std::size_t gutter, const quidra::runtime::ReportData& data,
               const quidra::runtime::CallPath& path) {
    const abi::ErrorHopList* hops = data.error ? data.error->hops : nullptr;
    const auto hop_count = hops ? static_cast<std::size_t>(hops->count) : 0;
    const std::size_t unhandled = data.error && data.unhandled ? 1 : 0;
    put_elided(gutter, "", hop_count + unhandled + path.count, [&](std::size_t i) {
        if (i < hop_count) {
            put_hop_line(gutter, "", hops->entries[i]);
        } else if (i < hop_count + unhandled) {
            begin_trace_line(gutter, "");
            put("unhandled at ");
            put_location_text(trace_location(data.unhandled_line, data.unhandled_column));
        } else {
            put_path_line(gutter, path, path.items[i - hop_count - unhandled]);
        }
    });
    if (!data.error) return;
    const auto& record = *data.error;
    std::size_t causes = 0;
    for (const auto* cause = record.cause; cause; cause = cause->cause) ++causes;
    const abi::ErrorRecord* cause = record.cause;
    std::size_t position = 0;
    put_elided(gutter, "", causes, [&](std::size_t i) {
        while (position < i) {
            cause = cause->cause;
            ++position;
        }
        begin_trace_line(gutter, "");
        put("caused by: [");
        put(cause->code ? cause->code : "ERROR");
        put("] ");
        for (const char* c = cause->message ? cause->message : ""; *c; ++c) {
            if (*c == '\n') put("\\n");
            else put_char(*c);
        }
        const auto origin = origin_location(*cause);
        if (origin.entry && origin.line != 0) {
            put(" (at ");
            put_location_text(origin);
            put(")");
        }
        put_hop_lines(gutter, "    ", cause->hops);
    });
}

// QUIDRA_ERROR_FORMAT=json, read when a report is written.
bool json_format_requested() {
    return quidra::platform::environment_value_is("QUIDRA_ERROR_FORMAT", "json");
}

// A location as JSON: the file, and with a line its line, column, absolute
// path and revision; null without a source table.
void put_json_location(const quidra::runtime::ReportLocation& location) {
    if (!location.entry) {
        put("null");
        return;
    }
    put("{\"file\": ");
    put_json_string_or_null(location.entry->display);
    if (location.line != 0) {
        put(", \"line\": ");
        put_decimal(location.line);
        put(", \"column\": ");
        put_decimal(location.column);
        put(", \"source_file\": ");
        put_json_string_or_null(location.entry->absolute);
        put(", \"source_revision\": ");
        put_json_string_or_null(location.entry->revision);
    }
    put("}");
}

// Every hop, unelided: the function (without "()"), its file, line and
// column, the count and how the error left it (via).
void put_json_hops(const abi::ErrorHopList* hops) {
    put("[");
    for (std::uint64_t i = 0; hops && i < hops->count; ++i) {
        const auto& entry = hops->entries[i];
        if (i != 0) put(", ");
        std::string_view name = hop_function(entry);
        if (name.size() >= 2 && name.substr(name.size() - 2) == "()")
            name.remove_suffix(2);
        put("{\"function\": ");
        put_json_string(name);
        const auto location = trace_location(entry.site->line, entry.site->column);
        put(", \"file\": ");
        put_json_string_or_null(location.entry ? location.entry->display : nullptr);
        put(", \"line\": ");
        put_decimal(location.line);
        put(", \"column\": ");
        put_decimal(location.column);
        put(", \"count\": ");
        put_decimal(entry.repeat);
        put(", \"via\": ");
        put_json_string(abi::error_hop_via(entry.site->kind));
        put("}");
    }
    put("]");
}

// The walked path: one object per item, unelided (a run of identical
// frames is one object with its count), the marker left out; merged calls
// list their candidate sites.
void put_json_path(const quidra::runtime::CallPath& path) {
    using quidra::runtime::PathItemKind;
    put("[");
    bool first = true;
    for (std::size_t i = 0; i < path.count; ++i) {
        const auto& item = path.items[i];
        if (item.kind == PathItemKind::marker) continue;
        if (!first) put(", ");
        first = false;
        auto name = item.function;
        if (name.size() >= 2 && name.substr(name.size() - 2) == "()") name.remove_suffix(2);
        put("{\"function\": ");
        put_json_string(name);
        const auto put_position = [&](unsigned long long line, unsigned long long column) {
            const auto location = trace_location(line, column);
            put("\"file\": ");
            put_json_string_or_null(location.entry ? location.entry->display : nullptr);
            put(", \"line\": ");
            put_decimal(location.line);
            put(", \"column\": ");
            put_decimal(location.column);
        };
        if (item.kind == PathItemKind::merged) {
            put(", \"sites\": [");
            for (std::uint32_t c = 0; c < item.candidate_count; ++c) {
                if (c != 0) put(", ");
                put("{");
                const auto& candidate = path.candidates[item.first_candidate + c];
                put_position(candidate.line, candidate.column);
                put("}");
            }
            put("]");
        } else {
            put(", ");
            put_position(item.line, item.column);
        }
        put(", \"count\": ");
        put_decimal(item.repeat);
        put(", \"via\": ");
        put(item.via == quidra::runtime::PathVia::inline_call ? "\"inline\"" : "\"call\"");
        put("}");
    }
    put("]");
}

// The report as one JSON line (schema version 1).
void put_json_report(const quidra::runtime::ReportHead& head,
                     const quidra::runtime::ReportLocation& location, std::string_view message,
                     const quidra::runtime::ReportData& data,
                     const quidra::runtime::CallPath& path) {
    using quidra::runtime::ReportKind;
    put("{\"schema_version\": 1, \"kind\": ");
    put(head.kind == ReportKind::test_assertion ? "\"test_assertion\"" : "\"runtime_error\"");
    put(", \"status\": ");
    put_signed(data.status);
    put(", \"code\": ");
    if (head.kind == ReportKind::coded) put_json_string(head.code);
    else put("null");
    put(", \"message\": ");
    put_json_string(head.kind == ReportKind::test_assertion ? std::string_view{} : message);
    put(", \"details\": {");
    if (data.error) {
        put_error_details(*data.error);
    } else if (data.reason) {
        bool first = true;
        abi::visit_failure_holes(*data.reason, [&](std::string_view name, abi::FailureHole hole) {
            if (hole == abi::FailureHole::message || hole == abi::FailureHole::unknown) return;
            if (!first) put(", ");
            first = false;
            put_json_string(name);
            put(": ");
            if (hole == abi::FailureHole::path) put_json_string(data.args.path);
            else if (hole == abi::FailureHole::slice) put_json_string(data.args.slice);
            else if (hole == abi::FailureHole::marker) put_json_string(data.args.marker);
            else if (hole == abi::FailureHole::name) put_json_string(data.args.name);
            else if (hole == abi::FailureHole::type) put_json_string(data.args.type);
            else if (hole == abi::FailureHole::operation) put_json_string(data.args.operation);
            else if (hole == abi::FailureHole::subject)
                put_json_string(abi::conversion_subject_info(data.args.subject).id);
            else if (hole == abi::FailureHole::step && !data.args.step_text.empty())
                put(data.args.step_text);
            else put_signed(abi::failure_hole_value(hole, data.args));
        });
        const auto& info = abi::failure_reason_info(*data.reason);
        if (abi::reason_is_detail(info.code)) {
            if (!first) put(", ");
            put("\"reason\": ");
            put_json_string(info.id);
        }
    }
    put("}, \"location\": ");
    if (!location.entry) {
        put("null");
    } else {
        put("{\"file\": ");
        put_json_string_or_null(location.entry->display);
        if (location.line != 0) {
            put(", \"line\": ");
            put_decimal(location.line);
            put(", \"column\": ");
            put_decimal(location.column);
            put(", \"source_file\": ");
            put_json_string_or_null(location.entry->absolute);
            put(", \"source_revision\": ");
            put_json_string_or_null(location.entry->revision);
        }
        put("}");
    }
    put(", \"provenance\": ");
    if (const auto* provenance = data.provenance) {
        put("{\"source_revision\": ");
        put_json_string_or_null(provenance->revision);
        put(", \"node_id\": ");
        put_json_string_or_null(provenance->node_id);
        put(", \"node_kind\": ");
        put_json_string_or_null(provenance->node_kind);
        put(", \"source_file\": ");
        put_json_string_or_null(provenance->file);
        put("}");
    } else {
        put("null");
    }
    put(", \"deferred_origin\": null, \"hops\": ");
    put_json_hops(data.error ? data.error->hops : nullptr);
    put(", \"unhandled_at\": ");
    if (data.unhandled) put_json_location(trace_location(data.unhandled_line, data.unhandled_column));
    else put("null");
    put(", \"path\": ");
    put_json_path(path);
    put(", \"path_complete\": ");
    put(path.complete ? "true" : "false");
    put(", \"causes\": [");
    bool first_cause = true;
    for (const auto* cause = data.error ? data.error->cause : nullptr; cause; cause = cause->cause) {
        if (!first_cause) put(", ");
        first_cause = false;
        put("{\"code\": ");
        put_json_string(cause->code ? cause->code : "ERROR");
        put(", \"message\": ");
        put_json_string(cause->message ? cause->message : "");
        put(", \"details\": {");
        put_error_details(*cause);
        put("}, \"location\": ");
        put_json_location(origin_location(*cause));
        put(", \"hops\": ");
        put_json_hops(cause->hops);
        put("}");
    }
    put("]}\n");
}

std::atomic<int> reporter_state{0};  // 0 free, 1 writing, 2 flushed
thread_local bool reporting_thread = false;

} // namespace

// Generated code registers its module's table before its first statement;
// the failure path reads it with acquire.
extern "C" void quidra_runtime_register_sources(const void* table) {
    registered_table.store(static_cast<const abi::SourceTable*>(table), std::memory_order_release);
}

namespace quidra::runtime {

ReportSourceScope::ReportSourceScope(const abi::SourceTable* table) noexcept
    : previous_(selected_source_table),was_selected_(source_table_selected) {
    selected_source_table=table;source_table_selected=true;
}
ReportSourceScope::~ReportSourceScope() {
    selected_source_table=previous_;source_table_selected=was_selected_;
}
namespace {
const abi::SourceTableEntry* entry_in(const abi::SourceTable* table,const char* file) {
    if(!table || !file) return nullptr;
    const auto* entries=abi::source_table_entries(table);
    for(unsigned long long i=0;i<table->count;++i)
        if(entries[i].absolute==file) return &entries[i];
    for(unsigned long long i=0;i<table->count;++i)
        if(entries[i].absolute && std::strcmp(entries[i].absolute,file)==0) return &entries[i];
    return nullptr;
}
ReportLocation statement_in(const abi::SourceTable* table,
                            const abi::SourceProvenanceRecord* statement) {
    if(!table) return statement ? ReportLocation{nullptr,statement->line,statement->column} : ReportLocation{};
    const auto* root=table->count ? abi::source_table_entries(table) : nullptr;
    if(!statement) return {root,0,0};
    const auto* entry=entry_in(table,statement->file);
    if(!entry || statement->line==0 || statement->line>entry->line_count) return {root,0,0};
    return {entry,statement->line,statement->column};
}
}
const abi::SourceTableEntry* source_entry(const char* file) {
    return entry_in(current_registered_table(),file);
}
ReportSite report_site(unsigned long long line,unsigned long long column,
                       const abi::SourceProvenanceRecord* statement,bool statement_only) {
    return {current_registered_table(),line,column,statement,statement_only};
}
ReportLocation locate_report_site(const ReportSite& site) {
    if(site.statement_only) return statement_in(site.table,site.statement);
    if(!site.table) return {nullptr,site.encoded_line & 0xffffffffULL,site.column};
    if(site.encoded_line) {
        const auto index=site.encoded_line>>32;
        const auto line=site.encoded_line & 0xffffffffULL;
        if(index<site.table->count) {
            const auto& entry=abi::source_table_entries(site.table)[index];
            if(line>=1 && line<=entry.line_count) return {&entry,line,site.column};
        }
    }
    return statement_in(site.table,site.statement);
}
ReportLocation locate_statement(const abi::SourceProvenanceRecord* statement) {
    return locate_report_site(report_site(0,0,statement,true));
}
ReportLocation locate_site(unsigned long long line,unsigned long long column,
                           const abi::SourceProvenanceRecord* statement) {
    return locate_report_site(report_site(line,column,statement));
}

} // namespace quidra::runtime

namespace {

quidra::runtime::ReportLocation trace_location(unsigned long long line, unsigned long long column) {
    return quidra::runtime::locate_site(line, column, nullptr);
}

// Where an error was made: its site, or the user statement that led into
// the package code that made it.
quidra::runtime::ReportLocation origin_location(const abi::ErrorRecord& record) {
    if (record.line != 0) return quidra::runtime::locate_site(record.line, record.column, nullptr);
    if (record.statement)
        return quidra::runtime::locate_statement(
            static_cast<const abi::SourceProvenanceRecord*>(record.statement));
    return {};
}

} // namespace

namespace quidra::runtime {

void ReportMessage::append(std::string_view text) {
    const auto n = (std::min)(text.size(), buffer_size - size_);
    std::memcpy(message_buffer + size_, text.data(), n);
    size_ += n;
}

std::string_view ReportMessage::text() const { return {message_buffer, size_}; }

ReportMessage& report_message() {
    static thread_local ReportMessage message;
    return message;
}

void write_report(const ReportHead& head, const ReportLocation& location,
                  std::string_view message, const ReportData& data) {
    // The user call path of the failing thread, walked from here.
    const auto path = data.path ? *data.path : walk_call_path();
#ifdef _WIN32
    _lock_file(stderr);
#else
    flockfile(stderr);
#endif
    output_size = 0;
    if (json_format_requested()) {
        put_json_report(head, location, message, data, path);
        flush_output();
#ifdef _WIN32
        _unlock_file(stderr);
#else
        funlockfile(stderr);
#endif
        std::fflush(stderr);
        return;
    }
    switch (head.kind) {
        case ReportKind::coded:
            put("Quidra runtime error[");
            put(head.code);
            put("]");
            break;
        case ReportKind::uncoded:
            put("Quidra runtime error");
            break;
        case ReportKind::test_assertion:
            put("Quidra test assertion failed");
            break;
    }
    const bool has_message = head.kind != ReportKind::test_assertion;
    if (!location.entry) {
        // Before any source table is registered: one line.
        if (head.kind == ReportKind::test_assertion) {
            put(" at ");
            put_decimal(location.line);
            put(":");
            put_decimal(location.column);
        } else {
            put(": ");
            put(message);
        }
        put("\n");
    } else {
        put(" at ");
        put(location.entry->display ? location.entry->display : "");
        std::size_t gutter = 0;
        if (location.line != 0) {
            put(":");
            put_decimal(location.line);
            put(":");
            put_decimal(location.column);
            SourceFile file(location.entry->absolute);
            SnippetLine line;
            if (find_snippet_line(file, *location.entry, location.line, line)) {
                put_snippet(file, line, location.line, location.column);
                // The body lines' bar stands under the gutter line's.
                gutter = decimal_width(location.line) + 1;
            }
        }
        if (has_message) put_body(gutter, message);
        if (has_trace(data, path)) {
            // An empty body line, then the trace.
            put("\n");
            put_spaces(gutter);
            put("|");
            put_trace(gutter, data, path);
        }
        put("\n");
    }
    flush_output();
#ifdef _WIN32
    _unlock_file(stderr);
#else
    funlockfile(stderr);
#endif
    std::fflush(stderr);
}

ReporterTurn take_reporter() {
    int expected = 0;
    if (reporter_state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        reporting_thread = true;
        return ReporterTurn::first;
    }
    if (reporting_thread) {
        return reporter_state.load(std::memory_order_acquire) == 2 ? ReporterTurn::later
                                                                   : ReporterTurn::nested;
    }
    while (reporter_state.load(std::memory_order_acquire) != 2) std::this_thread::yield();
    immediate_failure_exit();
}

void reporter_flushed() { reporter_state.store(2, std::memory_order_release); }

namespace {
std::atomic<FailureFlushHandler> failure_flush_handler{nullptr};
}

void set_failure_flush_handler(FailureFlushHandler handler) {
    failure_flush_handler.store(handler, std::memory_order_release);
}

void immediate_failure_exit() {
    if (const auto flush = failure_flush_handler.load(std::memory_order_acquire)) flush();
    std::fflush(nullptr);
    std::_Exit(abi::failure_exit_status);
}

} // namespace quidra::runtime
