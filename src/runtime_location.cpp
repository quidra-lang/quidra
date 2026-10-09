// The location step of the runtime's reporter; see runtime_location.hpp.

#include "runtime_location.hpp"

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
char message_buffer[buffer_size];

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

// QUIDRA_ERROR_FORMAT=json, read when a report is written.
bool json_format_requested() {
    return quidra::platform::environment_value_is("QUIDRA_ERROR_FORMAT", "json");
}

// The report as one JSON line (schema version 1).
void put_json_report(const quidra::runtime::ReportHead& head,
                     const quidra::runtime::ReportLocation& location, std::string_view message,
                     const quidra::runtime::ReportData& data) {
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
    if (data.reason) {
        bool first = true;
        abi::visit_failure_holes(*data.reason, [&](std::string_view name, abi::FailureHole hole) {
            if (hole == abi::FailureHole::message || hole == abi::FailureHole::unknown) return;
            if (!first) put(", ");
            first = false;
            put_json_string(name);
            put(": ");
            if (hole == abi::FailureHole::path) put_json_string(data.args.path);
            else if (hole == abi::FailureHole::type) put_json_string(data.args.type);
            else if (hole == abi::FailureHole::subject)
                put_json_string(abi::conversion_subject_info(data.args.subject).id);
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
    put(", \"deferred_origin\": null, \"hops\": [], \"unhandled_at\": null, \"path\": [], "
        "\"path_complete\": true, \"causes\": []}\n");
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

const abi::SourceTableEntry* source_entry(const char* file) {
    const auto* table = registered_table.load(std::memory_order_acquire);
    if (!table || !file) return nullptr;
    const auto* entries = abi::source_table_entries(table);
    for (unsigned long long i = 0; i < table->count; ++i)
        if (entries[i].absolute == file) return &entries[i];
    for (unsigned long long i = 0; i < table->count; ++i)
        if (entries[i].absolute && std::strcmp(entries[i].absolute, file) == 0) return &entries[i];
    return nullptr;
}

ReportLocation locate_statement(const abi::SourceProvenanceRecord* statement) {
    const auto* table = registered_table.load(std::memory_order_acquire);
    if (!table) {
        return statement ? ReportLocation{nullptr, statement->line, statement->column}
                         : ReportLocation{};
    }
    const auto* root = table->count != 0 ? abi::source_table_entries(table) : nullptr;
    if (!statement) return {root, 0, 0};
    const auto* entry = source_entry(statement->file);
    if (!entry || statement->line == 0 || statement->line > entry->line_count) return {root, 0, 0};
    return {entry, statement->line, statement->column};
}

ReportLocation locate_site(unsigned long long encoded_line, unsigned long long column,
                           const abi::SourceProvenanceRecord* user_statement) {
    const auto* table = registered_table.load(std::memory_order_acquire);
    if (!table) return {nullptr, encoded_line & 0xffffffffULL, column};
    if (encoded_line != 0) {
        const auto index = encoded_line >> 32;
        const auto line = encoded_line & 0xffffffffULL;
        if (index < table->count) {
            const auto& entry = abi::source_table_entries(table)[index];
            if (line >= 1 && line <= entry.line_count) return {&entry, line, column};
        }
    }
    return locate_statement(user_statement);
}

void ReportMessage::append(std::string_view text) {
    const auto n = (std::min)(text.size(), buffer_size - size_);
    std::memcpy(message_buffer + size_, text.data(), n);
    size_ += n;
}

std::string_view ReportMessage::text() const { return {message_buffer, size_}; }

ReportMessage& report_message() {
    static ReportMessage message;
    return message;
}

void write_report(const ReportHead& head, const ReportLocation& location,
                  std::string_view message, const ReportData& data) {
#ifdef _WIN32
    _lock_file(stderr);
#else
    flockfile(stderr);
#endif
    output_size = 0;
    if (json_format_requested()) {
        put_json_report(head, location, message, data);
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

void immediate_failure_exit() {
    std::fflush(nullptr);
    std::_Exit(abi::failure_exit_status);
}

} // namespace quidra::runtime
