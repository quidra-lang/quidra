// The runtime failure table (quidra/abi/runtime_failure.hpp): every code is
// spelled once, every reason belongs to one code with its own id and its own
// template, a printed message identifies its reason within its code, and the
// templates format the texts the runtime prints. The location step
// (src/runtime_location.cpp): encoded lines resolve against the registered
// source table, the snippet appears only for the identical file, and the
// caret is placed by display width.

#include "quidra/abi/display_width.hpp"
#include "quidra/abi/runtime_entry_points.hpp"
#include "quidra/abi/runtime_failure.hpp"
#include "platform/sha256.hpp"
#include "runtime_location.hpp"

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace abi = quidra::abi;

namespace {

int failures = 0;

void check(bool condition, std::string_view what) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << what << '\n';
}

bool is_code_spelling(std::string_view text) {
    if (text.empty() || text.front() < 'A' || text.front() > 'Z') return false;
    char previous = '\0';
    for (char c : text) {
        const bool valid = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!valid || (c == '_' && previous == '_')) return false;
        previous = c;
    }
    return text.back() != '_';
}

// Whether `text` could have been printed by `pattern`: the literal parts in
// order, each hole standing for any text.
bool template_matches(std::string_view pattern, std::string_view text) {
    if (pattern.empty()) return text.empty();
    if (pattern.front() == '{') {
        const auto close = pattern.find('}');
        const auto rest = pattern.substr(close + 1);
        for (std::size_t length = 0; length <= text.size(); ++length)
            if (template_matches(rest, text.substr(length))) return true;
        return false;
    }
    return !text.empty() && pattern.front() == text.front() &&
           template_matches(pattern.substr(1), text.substr(1));
}

void codes_are_spelled_once() {
    std::set<std::string_view> spellings;
    for (const auto& row : abi::runtime_failure_codes) {
        check(is_code_spelling(row.spelling), "code spelling is UPPER_SNAKE_CASE");
        check(spellings.insert(row.spelling).second, "code spelled once");
        check(abi::spelling(row.code) == row.spelling, "spelling() reads the row");
    }
}

void reasons_identify_themselves() {
    for (const auto& row : abi::failure_reasons) {
        check(is_code_spelling(row.id), "reason id is UPPER_SNAKE_CASE");
        check(&abi::failure_reason_info(row.reason) == &row, "reason row by enumerator");
        // A message printed for this reason, with sample arguments, matches
        // no other template of the same code.
        const auto printed = abi::format(
            row.reason, abi::FailureArgs{.index = 7, .length = 3, .message = "sample"});
        check(template_matches(row.message, printed), "a reason's text matches its template");
        for (const auto& other : abi::failure_reasons) {
            if (&other == &row || other.code != row.code) continue;
            check(other.id != row.id, "reason ids are unique within a code");
            check(!template_matches(other.message, printed),
                  "a printed message identifies its reason within its code");
        }
    }
}

// The texts the runtime printed before its sites reported through the table.
void templates_format_the_runtime_texts() {
    using R = abi::FailureReason;
    struct Case {
        R reason;
        abi::FailureArgs args;
        std::string_view text;
    };
    const Case expected[] = {
        {R::index_out_of_bounds, {.index = 3, .length = 3}, "index 3 out of bounds for length 3"},
        {R::recursion_limit, {.limit = 4096}, "maximum recursion depth exceeded (limit 4096)"},
        {R::index_out_of_bounds, {.index = -1, .length = 0}, "index -1 out of bounds for length 0"},
        {R::slice_out_of_bounds, {.length = 5, .start = 2, .end = 7},
         "slice [2, 7) out of bounds for length 5"},
        {R::axis_index_out_of_bounds, {.index = 4, .length = 4, .axis = 1},
         "index 4 out of bounds for axis 1 with length 4"},
        {R::axis_slice_out_of_bounds, {.length = 3, .start = 0, .end = 5, .axis = 0},
         "slice [0, 5) out of bounds for axis 0 with length 3"},
        {R::value_uninitialized, {}, "value is uninitialized"},
        {R::path_uninitialized, {.path = "box.x"}, "'box.x' is uninitialized"},
        {R::integer_overflow, {}, "integer overflow"},
        {R::conversion_out_of_range,
         {.type = "int8", .subject = abi::ConversionSubject::value},
         "numeric conversion out of range: value cannot be represented as int8"},
        {R::conversion_non_finite,
         {.type = "real32", .subject = abi::ConversionSubject::value},
         "numeric conversion failed: non-finite value cannot be represented as real32"},
        {R::conversion_not_integral,
         {.type = "bigint", .subject = abi::ConversionSubject::array_element},
         "numeric conversion failed: array element is not an integer and cannot be represented "
         "as bigint"},
        {R::conversion_undecided,
         {.type = "real64", .subject = abi::ConversionSubject::tensor_element},
         "numeric conversion failed: the exact tensor element could not be decided for real64"},
        {R::division_by_zero, {}, "division by zero"},
        {R::evaluation_budget_exhausted, {}, "real evaluation budget exhausted"},
        {R::nat_negative, {}, "nat subtraction result is negative"},
        {R::negative_integer_exponent, {}, "integer exponent must be non-negative"},
        {R::invalid_integer_power_width, {}, "invalid integer power width"},
        {R::zero_power_zero, {}, "0 ^ 0 is undefined"},
        {R::zero_power_negative, {}, "0 ^ a negative exponent is undefined"},
        {R::negative_base_fractional_exponent, {}, "a negative base requires an integer exponent"},
        {R::task_null_operation, {}, "task.all received a null operation"},
        {R::task_start_failed, {.message = "resource unavailable"},
         "cannot start task: resource unavailable"},
        {R::tensor_failure, {.message = "tensor shapes differ"}, "tensor shapes differ"},
        {R::exact_numeric_failure, {.message = "bigint is outside destination range"},
         "bigint is outside destination range"},
        {R::iterated_array_reshaped, {},
         "array iterated by reference was replaced or resized during the loop"},
    };
    for (const auto& item : expected) {
        const auto formatted = abi::format(item.reason, item.args);
        check(formatted == item.text,
              "format: " + std::string(item.text) + " (got " + formatted + ")");
    }
}

void display_widths_follow_the_wcwidth_convention() {
    check(abi::display_width(U'a') == 1, "ASCII is one column");
    check(abi::display_width(0x65E5) == 2, "CJK ideographs are two columns");
    check(abi::display_width(0xFF21) == 2, "fullwidth forms are two columns");
    check(abi::display_width(0x0301) == 0, "combining marks take no column");
    check(abi::display_width(0x200B) == 0, "zero width space takes no column");
    check(abi::display_width(0xFE0F) == 0, "variation selectors take no column");
    check(abi::display_width(0x00AD) == 1, "the soft hyphen is one column");
    for (std::size_t i = 1; i < std::size(abi::display_width_ranges); ++i)
        check(abi::display_width_ranges[i - 1].last < abi::display_width_ranges[i].first,
              "width ranges are sorted and disjoint");
}

// The table of two user files that the tests register.
struct TestTable {
    abi::SourceTable header{2};
    abi::SourceTableEntry entries[2];
};

void encoded_lines_resolve_against_the_table() {
    using quidra::runtime::locate_site;
    using quidra::runtime::locate_statement;
    check(locate_site(3, 4, nullptr).entry == nullptr, "no table: no entry");
    static const char root_path[] = "/work/main.qui";
    static const char import_path[] = "/work/lib/util.qui";
    static TestTable table{{2},
                           {{"main.qui", root_path, "", 10, 100},
                            {"lib/util.qui", import_path, "", 4, 40}}};
    quidra_runtime_register_sources(&table);
    const auto root_first = locate_site(1, 2, nullptr);
    check(root_first.entry == &table.entries[0] && root_first.line == 1 && root_first.column == 2,
          "the root's first line");
    const auto root_last = locate_site(10, 1, nullptr);
    check(root_last.entry == &table.entries[0] && root_last.line == 10, "the root's last line");
    const auto import_last = locate_site((1ULL << 32) | 4, 7, nullptr);
    check(import_last.entry == &table.entries[1] && import_last.line == 4 && import_last.column == 7,
          "the last index's last line");
    const auto outside_index = locate_site((2ULL << 32) | 1, 1, nullptr);
    check(outside_index.entry == &table.entries[0] && outside_index.line == 0,
          "an index outside the table is line 0: the root file without a line");
    const auto outside_line = locate_site((1ULL << 32) | 5, 1, nullptr);
    check(outside_line.entry == &table.entries[0] && outside_line.line == 0,
          "a line outside its file is line 0");
    const abi::SourceProvenanceRecord statement{import_path, "", "", "", 3, 9};
    const auto at_statement = locate_site(0, 0, &statement);
    check(at_statement.entry == &table.entries[1] && at_statement.line == 3 &&
              at_statement.column == 9,
          "line 0 takes the user statement");
    const std::string copy = import_path;
    const abi::SourceProvenanceRecord by_name{copy.c_str(), "", "", "", 2, 1};
    check(locate_statement(&by_name).entry == &table.entries[1],
          "a statement's file is found by name when the pointer differs");
    const auto no_statement = locate_site(0, 0, nullptr);
    check(no_statement.entry == &table.entries[0] && no_statement.line == 0,
          "line 0 without a user statement names the root file");
}

// The report written for `source` (a file at `path`) failing at line:column,
// as stderr receives it.
std::string report_text(const std::filesystem::path& directory, const std::string& source,
                        unsigned long long line, unsigned long long column,
                        unsigned long long recorded_size = 0) {
    const auto path = directory / "program.qui";
    {
        std::ofstream out(path, std::ios::binary);
        out << source;
    }
    static std::string absolute;
    static std::string revision;
    absolute = path.string();
    revision = quidra::platform::sha256_hex(source);
    unsigned long long lines = 1;
    for (const char c : source) lines += c == '\n';
    static TestTable table;
    table = TestTable{{1},
                      {{"program.qui", absolute.c_str(), revision.c_str(), lines,
                        recorded_size ? recorded_size : source.size()},
                       {}}};
    quidra_runtime_register_sources(&table);
    const auto captured = directory / "stderr.txt";
    std::fflush(stderr);
#ifdef _WIN32
    const int saved = _dup(2);
#else
    const int saved = dup(2);
#endif
    if (saved < 0 || !std::freopen(captured.string().c_str(), "w", stderr)) return {};
    quidra::runtime::write_report({quidra::runtime::ReportKind::coded, "INDEX_BOUNDS"},
                                  quidra::runtime::locate_site(line, column, nullptr),
                                  "index 3 out of bounds for length 3", {});
    std::fflush(stderr);
#ifdef _WIN32
    _dup2(saved, 2);
    _close(saved);
#else
    dup2(saved, 2);
    close(saved);
#endif
    std::ifstream in(captured, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void snippets_show_the_identical_file() {
    const auto directory = std::filesystem::temp_directory_path() / "quidra-runtime-location-tests";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    const auto shown = report_text(directory, "int[] values = [1, 2, 3]\nprint(values[3])\n", 2, 14);
    check(shown == "Quidra runtime error[INDEX_BOUNDS] at program.qui:2:14\n"
                   "\n"
                   "2 | print(values[3])\n"
                   "  |              ^\n"
                   "  | index 3 out of bounds for length 3\n",
          "the snippet, the caret and the message in the gutter: " + shown);
    const auto other_size =
        report_text(directory, "int[] values = [1, 2, 3]\nprint(values[3])\n", 2, 14, 5);
    check(other_size == "Quidra runtime error[INDEX_BOUNDS] at program.qui:2:14\n"
                        "| index 3 out of bounds for length 3\n",
          "a file of another size: no snippet, a zero-width gutter: " + other_size);
    // print("<CJK ideograph>e<combining acute><tab>", v[3]): v is byte 18.
    const auto wide = report_text(directory, "x\nprint(\"\xe6\x97\xa5" "e\xcc\x81\t\", v[3])\n", 2, 18);
    check(wide.find("  | " + std::string(10, ' ') + "\t   ^\n") != std::string::npos,
          "the caret padding by display width: " + wide);
    const auto crlf = report_text(directory, "x\r\nprint(values[3])\r\n", 2, 7);
    check(crlf.find("2 | print(values[3])\n") != std::string::npos, "a CRLF line without its \\r");
    std::string long_line = "print(\"" + std::string(70000, 'y') + "\", v[3])";
    const auto long_report = report_text(directory, "x\n" + long_line + "\n", 2, 70011);
    check(long_report.find("2 | " + long_line + "\n") != std::string::npos,
          "a line longer than the buffer is written in full");
    check(long_report.find(std::string(70010, ' ') + "^") != std::string::npos,
          "and its caret stands under the column");
    std::filesystem::remove_all(directory);
}

} // namespace

int main() {
    codes_are_spelled_once();
    reasons_identify_themselves();
    templates_format_the_runtime_texts();
    display_widths_follow_the_wcwidth_convention();
    encoded_lines_resolve_against_the_table();
    snippets_show_the_identical_file();
    if (failures != 0) {
        std::cerr << failures << " runtime failure table check(s) failed\n";
        return 1;
    }
    std::cout << "runtime failure table: " << abi::runtime_failure_codes.size() << " codes, "
              << abi::failure_reasons.size() << " reasons\n";
    return 0;
}
