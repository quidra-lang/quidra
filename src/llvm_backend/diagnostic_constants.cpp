#include "llvm_backend/diagnostic_constants.hpp"

#include "llvm_text/appendable_text.hpp"
#include "quidra/abi/call_depth.hpp"
#include "quidra/abi/runtime_failure.hpp"

#include <array>
#include <cstddef>
#include <string_view>

namespace quidra::llvm_backend {

namespace {

// The CALL_DEPTH_LIMIT message, "maximum recursion depth exceeded (limit
// 4096)": its reason's template with the limit the guards compare against
// (abi::self_call_depth_limit), formatted at compile time.
constexpr abi::FailureArgs recursion_limit_args{.limit = abi::self_call_depth_limit};

consteval std::size_t recursion_message_size() {
    std::size_t size = 0;
    abi::write_failure_message(abi::FailureReason::recursion_limit, recursion_limit_args,
                               [&](std::string_view piece) { size += piece.size(); });
    return size;
}

constexpr auto recursion_message = [] {
    std::array<char, recursion_message_size()> text{};
    std::size_t at = 0;
    abi::write_failure_message(abi::FailureReason::recursion_limit, recursion_limit_args,
                               [&](std::string_view piece) {
                                   for (const char c : piece) text[at++] = c;
                               });
    return text;
}();

// A string constant by its name (without the @) and its bytes.
struct DiagnosticConstant {
    std::string_view name;
    std::string_view bytes;
};

// In the order of the module text.
constexpr std::array diagnostic_constants{
    DiagnosticConstant{".fmt.int.raw", "%lld"},
    DiagnosticConstant{".fmt.uint.raw", "%llu"},
    DiagnosticConstant{".fmt.int.text", "%lld"},
    DiagnosticConstant{".fmt.uint.text", "%llu"},
    DiagnosticConstant{".fmt.address.raw", "%p"},
    DiagnosticConstant{".fmt.string.raw", "%s"},
    DiagnosticConstant{".fmt.repl.string", "\"%s\""},
    DiagnosticConstant{".fmt.repl.error", "error(\"%s\")"},
    DiagnosticConstant{".bool.true", "true"},
    DiagnosticConstant{".bool.false", "false"},
    DiagnosticConstant{".code.overflow", "INTEGER_OVERFLOW"},
    DiagnosticConstant{".msg.overflow", "integer overflow"},
    DiagnosticConstant{".code.divzero", "DIVISION_BY_ZERO"},
    DiagnosticConstant{".msg.divzero", "division by zero"},
    DiagnosticConstant{".code.shift", "SHIFT_COUNT"},
    DiagnosticConstant{".msg.shift", "shift count outside integer width"},
    DiagnosticConstant{".code.range.step", "RANGE_STEP_ZERO"},
    DiagnosticConstant{".msg.range.step", "range step is zero"},
    DiagnosticConstant{".code.stack", "CALL_DEPTH_LIMIT"},
    DiagnosticConstant{".msg.stack",
                       std::string_view(recursion_message.data(), recursion_message.size())},
    DiagnosticConstant{".code.unhandled.error", "UNHANDLED_ERROR"},
    DiagnosticConstant{".code.shape", "SHAPE_MISMATCH"},
    DiagnosticConstant{".msg.shape", "captured extent mismatch"},
    DiagnosticConstant{".err.parse", "numeric parse failed"},
    DiagnosticConstant{".err.utf8", "invalid UTF-8 text"},
    DiagnosticConstant{".err.input", "input failed"},
    DiagnosticConstant{".err.output", "output failed"},
    DiagnosticConstant{".err.json.type", "JSON value has incompatible kind"},
    DiagnosticConstant{".err.file", "file operation failed"},
    DiagnosticConstant{".code.time.sleep", "INVALID_SLEEP_DURATION"},
    DiagnosticConstant{".msg.time.sleep", "invalid sleep duration"},
    DiagnosticConstant{".code.random.range", "INVALID_RANDOM_RANGE"},
    DiagnosticConstant{".msg.random.range", "invalid random range"},
};

// The lines of the table, written at compile time with the spelling of
// every string constant (append_string_constant), into a FixedText of the
// size a TextSize counts.
consteval std::size_t diagnostic_lines_size() {
    llvm_text::TextSize size;
    for (const auto& constant : diagnostic_constants)
        llvm_text::append_string_constant(size, constant.name, constant.bytes);
    return size.size();
}

constexpr auto diagnostic_lines = [] {
    llvm_text::FixedText<diagnostic_lines_size()> text;
    for (const auto& constant : diagnostic_constants)
        llvm_text::append_string_constant(text, constant.name, constant.bytes);
    return text;
}();

} // namespace

void emit_diagnostic_constants(llvm_text::LlvmModule& module) {
    module.append(llvm_text::LlvmModule::Section::constants, diagnostic_lines.view());
}

} // namespace quidra::llvm_backend
