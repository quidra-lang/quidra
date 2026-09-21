#include "runtime_internal.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

bool runtime_valid_text(const std::string& value) {
    return quidra_runtime_text_valid_bytes(
        value.data(), static_cast<unsigned long long>(value.size()));
}

char* runtime_copy_text(const std::string& value) {
    return quidra_runtime_copy_text_bytes(
        value.data(), static_cast<unsigned long long>(value.size()));
}

[[noreturn]] void runtime_process_failure(const char* message) {
    std::fprintf(stderr, "Quidra runtime error: %s\n", message);
    std::exit(101);
}

std::string read_process_stream(FILE* stream) {
    if (!stream) return {};
    std::fflush(stream);
    if (std::fseek(stream, 0, SEEK_SET) != 0) return {};
    std::string result;
    char buffer[4096];
    while (true) {
        const auto count = std::fread(buffer, 1, sizeof(buffer), stream);
        result.append(buffer, count);
        if (count < sizeof(buffer)) break;
    }
    return result;
}

void* make_process_result(long long status, std::string output, std::string error, bool started) {
    if (!runtime_valid_text(output) || !runtime_valid_text(error)) {
        runtime_process_failure(
            "process stdout/stderr must be valid UTF-8 text without NUL");
    }
    constexpr std::size_t bytes = 25;
    auto* raw = static_cast<unsigned char*>(quidra_managed_alloc(bytes));
    auto* output_text = runtime_copy_text(output);
    auto* error_text = runtime_copy_text(error);
    std::memcpy(raw, &status, sizeof(status));
    std::memcpy(raw + 8, &output_text, sizeof(output_text));
    std::memcpy(raw + 16, &error_text, sizeof(error_text));
    raw[24] = started ? 1 : 0;
    return raw;
}
} // namespace

extern "C" void* quidra_process_run(const char* program, void* args_array) {
    if (!program || !*program || !args_array) {
        return make_process_result(-1, "", "invalid process invocation", false);
    }

    long long count = 0;
    std::memcpy(&count, args_array, sizeof(count));
    if (count < 0 || count > 1048576) {
        return make_process_result(-1, "", "invalid process argument array", false);
    }

    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(count) + 1);
    arguments.emplace_back(program);
    auto* bytes = static_cast<unsigned char*>(args_array);
    for (long long i = 0; i < count; ++i) {
        char* value = nullptr;
        std::memcpy(&value, bytes + 8 + static_cast<std::size_t>(i) * sizeof(char*), sizeof(value));
        if (!value) return make_process_result(-1, "", "process argument is null", false);
        arguments.emplace_back(value);
    }

#ifdef _WIN32
    auto utf8_to_wide = [](std::string_view value) -> std::wstring {
        if (value.empty()) return {};
        const int needed = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            nullptr, 0);
        if (needed <= 0) return {};
        std::wstring wide(static_cast<std::size_t>(needed), L'\0');
        if (MultiByteToWideChar(
                CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                wide.data(), needed) != needed) {
            return {};
        }
        return wide;
    };
    auto quote_windows_argument = [](std::wstring_view value) {
        if (!value.empty() && value.find_first_of(L" \\t\\n\\v\\\"") == std::wstring_view::npos) {
            return std::wstring(value);
        }
        std::wstring out = L"\"";
        std::size_t backslashes = 0;
        for (wchar_t ch : value) {
            if (ch == L'\\') {
                ++backslashes;
                continue;
            }
            if (ch == L'"') {
                out.append(backslashes * 2 + 1, L'\\');
                out.push_back(L'"');
                backslashes = 0;
                continue;
            }
            out.append(backslashes, L'\\');
            backslashes = 0;
            out.push_back(ch);
        }
        out.append(backslashes * 2, L'\\');
        out.push_back(L'"');
        return out;
    };
    auto windows_error = [](DWORD code) {
        char* raw = nullptr;
        const DWORD count = FormatMessageA(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            reinterpret_cast<char*>(&raw), 0, nullptr);
        std::string message =
            count && raw ? std::string(raw, static_cast<std::size_t>(count))
                         : "Windows error " + std::to_string(code);
        if (raw) LocalFree(raw);
        while (!message.empty() &&
               (message.back() == '\r' || message.back() == '\n' || message.back() == ' ')) {
            message.pop_back();
        }
        return message;
    };

    std::wstring command_line;
    const bool cmd_shell =
        arguments.size() == 5 &&
        arguments[0] == "cmd.exe" &&
        arguments[1] == "/D" &&
        arguments[2] == "/S" &&
        arguments[3] == "/C";
    if (cmd_shell) {
        const auto command = utf8_to_wide(arguments[4]);
        if (command.empty() && !arguments[4].empty()) {
            return make_process_result(
                -1, "", "process arguments must be valid UTF-8", false);
        }
        // cmd.exe owns the syntax after /C. Do not run the command body
        // through C-runtime argv quoting: backslash-before-quote is not cmd
        // escaping and changes commands containing quotes or metacharacters.
        // /S strips the first and last quotes around the /C string and leaves
        // the interior unchanged. Always provide that outer pair so commands
        // beginning with a quoted executable path keep their own quotes.
        command_line = L"cmd.exe /D /S /C \"";
        command_line += command;
        command_line.push_back(L'"');
    } else {
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            const auto wide = utf8_to_wide(arguments[i]);
            if (wide.empty() && !arguments[i].empty()) {
                return make_process_result(
                    -1, "", "process arguments must be valid UTF-8", false);
            }
            if (i) command_line.push_back(L' ');
            command_line += quote_windows_argument(wide);
        }
    }
    command_line.push_back(L'\0');

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE output_read = nullptr;
    HANDLE output_write = nullptr;
    HANDLE error_read = nullptr;
    HANDLE error_write = nullptr;
    if (!CreatePipe(&output_read, &output_write, &security, 0) ||
        !CreatePipe(&error_read, &error_write, &security, 0)) {
        const auto message = windows_error(GetLastError());
        if (output_read) CloseHandle(output_read);
        if (output_write) CloseHandle(output_write);
        if (error_read) CloseHandle(error_read);
        if (error_write) CloseHandle(error_write);
        return make_process_result(-1, "", "cannot create process capture pipes: " + message, false);
    }
    SetHandleInformation(output_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(error_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = output_write;
    startup.hStdError = error_write;
    PROCESS_INFORMATION process{};

    const BOOL created = CreateProcessW(
        nullptr, command_line.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
        &startup, &process);
    const DWORD create_error = created ? ERROR_SUCCESS : GetLastError();
    CloseHandle(output_write);
    CloseHandle(error_write);

    if (!created) {
        CloseHandle(output_read);
        CloseHandle(error_read);
        return make_process_result(
            -1, "", "cannot execute process: " + windows_error(create_error), false);
    }

    auto read_pipe = [](HANDLE handle) {
        std::string result;
        char buffer[4096];
        DWORD received = 0;
        while (ReadFile(handle, buffer, sizeof(buffer), &received, nullptr) && received != 0) {
            result.append(buffer, static_cast<std::size_t>(received));
        }
        CloseHandle(handle);
        return result;
    };

    std::string output;
    std::string error_output;
    std::thread output_thread([&] { output = read_pipe(output_read); });
    std::thread error_thread([&] { error_output = read_pipe(error_read); });

    const DWORD wait_result = WaitForSingleObject(process.hProcess, INFINITE);
    DWORD status = 0;
    const BOOL status_ok =
        wait_result == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &status);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    output_thread.join();
    error_thread.join();

    if (!status_ok) {
        return make_process_result(-1, output, "cannot wait for process completion", false);
    }
    return make_process_result(static_cast<long long>(status), output, error_output, true);
#else
    std::vector<char*> native_arguments;
    native_arguments.reserve(arguments.size() + 1);
    for (auto& argument : arguments) native_arguments.push_back(argument.data());
    native_arguments.push_back(nullptr);

    FILE* output_file = std::tmpfile();
    FILE* error_file = std::tmpfile();
    if (!output_file || !error_file) {
        if (output_file) std::fclose(output_file);
        if (error_file) std::fclose(error_file);
        return make_process_result(-1, "", "cannot create process capture files", false);
    }

    int exec_error_pipe[2]{-1, -1};
    if (::pipe(exec_error_pipe) != 0) {
        const std::string message =
            std::string("cannot create process control pipe: ") + std::strerror(errno);
        std::fclose(output_file);
        std::fclose(error_file);
        return make_process_result(-1, "", message, false);
    }
    const int flags = ::fcntl(exec_error_pipe[1], F_GETFD);
    if (flags < 0 || ::fcntl(exec_error_pipe[1], F_SETFD, flags | FD_CLOEXEC) != 0) {
        const std::string message =
            std::string("cannot configure process control pipe: ") + std::strerror(errno);
        ::close(exec_error_pipe[0]);
        ::close(exec_error_pipe[1]);
        std::fclose(output_file);
        std::fclose(error_file);
        return make_process_result(-1, "", message, false);
    }

    const pid_t child = ::fork();
    if (child < 0) {
        const std::string message = std::string("cannot start process: ") + std::strerror(errno);
        ::close(exec_error_pipe[0]);
        ::close(exec_error_pipe[1]);
        std::fclose(output_file);
        std::fclose(error_file);
        return make_process_result(-1, "", message, false);
    }

    if (child == 0) {
        ::close(exec_error_pipe[0]);
        if (::dup2(::fileno(output_file), STDOUT_FILENO) < 0 ||
            ::dup2(::fileno(error_file), STDERR_FILENO) < 0) {
            const int error = errno;
            const auto written = ::write(exec_error_pipe[1], &error, sizeof(error));
            (void)written;
            ::_exit(127);
        }
        ::execvp(program, native_arguments.data());
        const int error = errno;
        const auto written = ::write(exec_error_pipe[1], &error, sizeof(error));
        (void)written;
        ::_exit(127);
    }

    ::close(exec_error_pipe[1]);
    int exec_error = 0;
    const ssize_t exec_error_bytes =
        ::read(exec_error_pipe[0], &exec_error, sizeof(exec_error));
    ::close(exec_error_pipe[0]);

    int wait_status = 0;
    bool waited = false;
    while (true) {
        const pid_t result = ::waitpid(child, &wait_status, 0);
        if (result == child) {
            waited = true;
            break;
        }
        if (result < 0 && errno == EINTR) continue;
        break;
    }

    const std::string output = read_process_stream(output_file);
    const std::string error_output = read_process_stream(error_file);
    std::fclose(output_file);
    std::fclose(error_file);

    if (exec_error_bytes > 0) {
        return make_process_result(
            -1, output,
            std::string("cannot execute process: ") + std::strerror(exec_error),
            false);
    }
    if (!waited) {
        return make_process_result(-1, output, "cannot wait for process completion", false);
    }

    long long status = -1;
    if (WIFEXITED(wait_status)) status = WEXITSTATUS(wait_status);
    else if (WIFSIGNALED(wait_status)) status = 128 + WTERMSIG(wait_status);
    return make_process_result(status, output, error_output, true);
#endif
}


extern "C" void* quidra_process_shell(const char* command) {
    if (!command) {
        return make_process_result(-1, "", "invalid shell invocation", false);
    }

#ifdef _WIN32
    const char* program = "cmd.exe";
    std::vector<std::string> arguments{"/D", "/S", "/C", command};
#else
    const char* program = "/bin/sh";
    std::vector<std::string> arguments{"-c", command};
#endif

    const long long count = static_cast<long long>(arguments.size());
    std::vector<unsigned char> encoded(
        8 + arguments.size() * sizeof(char*));
    std::memcpy(encoded.data(), &count, sizeof(count));
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        char* value = arguments[i].data();
        std::memcpy(
            encoded.data() + 8 + i * sizeof(char*),
            &value, sizeof(value));
    }
    return quidra_process_run(program, encoded.data());
}
