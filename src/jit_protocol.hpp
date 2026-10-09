#pragma once

// The persistent JIT protocol: the REPL (repl_cli.cpp, PersistentReplJit) and
// the JIT worker it starts (`quidra __jit-server`, jit.cpp run_server) talk
// over the worker's standard input and output.
//
// The worker first writes `ready` and "\n", or `start_error`, the message's
// size, "\n" and the message. A request is `run`, the size of the LLVM text,
// "\n" and the text; `quit` ends the worker. The answer is `ok`, the program's
// exit status and "\n"; or `error` (the generated entry point did not start,
// so the REPL may run the submission another way) or `post_error` (it
// started), then the message's size, "\n" and the message. The REPL reads
// answer lines of at most response_line_limit bytes.

#include <cstddef>
#include <string_view>

namespace quidra::jit_protocol {

inline constexpr std::string_view ready = "READY";
inline constexpr std::string_view quit = "QUIT";
inline constexpr std::string_view run = "RUN ";
inline constexpr std::string_view ok = "OK ";
inline constexpr std::string_view error = "ERR ";
inline constexpr std::string_view post_error = "POSTERR ";
inline constexpr std::string_view start_error = "STARTERR ";
inline constexpr std::size_t response_line_limit = 4096;

} // namespace quidra::jit_protocol
