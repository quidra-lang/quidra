#pragma once

// How the REPL cuts its input into submissions and decides how to compile
// them. The golden REPL session simulator (tests/golden/repl_session.cpp)
// calls these same functions, so the refactoring gates observe the REPL's real
// compile path.

#include <optional>
#include <string>
#include <string_view>

namespace quidra::ir {
struct Module;
}

namespace quidra::cli {

// Collects input lines into submissions the way the REPL prompt does. A line
// that leaves no string, parenthesis or bracket open completes a submission,
// unless the submission's first line opens a block (class, if, while, for,
// match, or a function header); a blank line ends the block.
class SubmissionAssembler {
public:
    // Adds one input line (without its newline) and returns the submission it
    // completes, if any. A blank line with nothing pending is dropped.
    std::optional<std::string> add_line(const std::string& line);
    // Whether a submission is in progress (the prompt shows "... ").
    bool pending() const { return !submission_.empty(); }
    // Drops the submission in progress (Ctrl-C).
    void discard();

private:
    std::string submission_;
    bool block_ = false;
};

// Whether redirected input holds a REPL command (a line starting with ':').
// Only then is it read line by line; otherwise it is one submission.
bool has_repl_command(std::string_view source);

// Whether running the module's entry point may reach an effect that running
// it again would repeat or could not reproduce: input, exit, the command
// line, flush, files, the environment, the clock, tasks, random numbers,
// processes or HTTP, an external function, an indirect call, or a call to a
// function the module does not define (also true without an entry point).
// The REPL runs the whole accepted source again for every executable
// submission, so before it runs such a module it sets the replay barrier,
// after which it refuses executable submissions until :reset
// (REPL_REPLAY_UNSAFE). The walk follows only the taken side of a branch on a
// constant boolean or on the equality of two constant strings.
bool module_requires_replay_barrier(const ir::Module& module);

// Whether a submission only declares (classes, enums, functions, imports): it
// parses to no top-level statement. The REPL checks such a submission together
// with the accepted source and keeps it without generating code or running
// anything, so it is accepted behind the replay barrier as well. A submission
// that does not parse is not declaration-only.
bool declaration_only_submission(std::string_view source);

} // namespace quidra::cli
