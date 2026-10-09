#pragma once

// Child processes: run a program with arguments and wait for it, with stdout
// and stderr optionally redirected to files (created or truncated).
//
// POSIX forks and execs (execvp, so a bare program name is searched for in
// PATH); the child's redirect files are created with mode 0666 before the
// umask, and a child that cannot redirect or exec exits with 126, or 127 when
// the program does not exist. The result is the exit status, or 128 + the
// signal of a child killed by one, or 1. Windows starts the program with
// CreateProcessW from a quoted command line and inherits handles only when a
// stream is redirected.

#include "platform/native_text.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace quidra::platform {

// Runs `program` with arguments in native text (native_text, or a path's
// native()), passed to the OS as they are.
int run_native_program(
    const std::filesystem::path& program,
    const std::vector<NativeText>& arguments,
    const std::optional<std::filesystem::path>& stdout_path = std::nullopt,
    const std::optional<std::filesystem::path>& stderr_path = std::nullopt);

// Runs `program` with UTF-8 arguments on every platform: each is converted
// to native text once.
int run_program(
    const std::filesystem::path& program,
    const std::vector<std::string>& arguments = {},
    const std::optional<std::filesystem::path>& stdout_path = std::nullopt,
    const std::optional<std::filesystem::path>& stderr_path = std::nullopt);

// How a program run through run_program_reporting_start ended. `started`
// is false when the program could not be started at all (POSIX reports the
// exec failure from the child over a close-on-exec pipe, so it is never
// confused with a program that exits 126 or 127). `status` is then what
// run_program gives for that failure (126, or 127 for a missing program;
// 1 on Windows), and `failure` says why.
struct ProgramRun {
    bool started{true};
    int status{};
    std::string failure;
};

// run_program with the start of the program reported apart from its exit.
// Throws only where run_program throws for anything but a failed start.
ProgramRun run_program_reporting_start(
    const std::filesystem::path& program, const std::vector<std::string>& arguments = {});

// Writes `data` to this process's standard error unchanged (no newline
// translation on Windows), for relaying a child's captured stderr. Returns
// false when the write fails.
bool write_standard_error(std::string_view data);

// The exit status of a std::system or waitpid status: -1 stays -1, a normal
// exit gives its code, a signal 128 + the signal, anything else 1 (Windows
// passes the status through).
int system_status(int status);

} // namespace quidra::platform
