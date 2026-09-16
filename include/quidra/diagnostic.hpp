#pragma once
#include <cstddef>
#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace quidra {

struct SourcePos {
    std::size_t offset{};
    std::size_t line{1};
    std::size_t column{1};
};

struct SourceSpan {
    SourcePos start{};
    SourcePos end{};
};

struct Diagnostic {
    std::string code;
    std::string message;
    SourceSpan span{};
};

class CompileError final : public std::exception {
public:
    explicit CompileError(Diagnostic diagnostic) : diagnostic_(std::move(diagnostic)) {}
    const char* what() const noexcept override { return diagnostic_.message.c_str(); }
    const Diagnostic& diagnostic() const noexcept { return diagnostic_; }

private:
    Diagnostic diagnostic_;
};

class CompileErrors final : public std::exception {
public:
    explicit CompileErrors(std::vector<Diagnostic> diagnostics, bool truncated = false)
        : diagnostics_(std::move(diagnostics)),
          truncated_(truncated),
          message_("compilation failed with " + std::to_string(diagnostics_.size()) + " error(s)") {}

    const char* what() const noexcept override { return message_.c_str(); }
    const std::vector<Diagnostic>& diagnostics() const noexcept { return diagnostics_; }
    bool truncated() const noexcept { return truncated_; }

private:
    std::vector<Diagnostic> diagnostics_;
    bool truncated_{};
    std::string message_;
};

} // namespace quidra
