#include "platform/process.hpp"
#include "platform/temporary_directory.hpp"
#include "platform/environment.hpp"

#include <cstdio>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <cerrno>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using quidra::platform::native_text;
using quidra::platform::run_native_program;

namespace {

using Environment = std::vector<std::pair<std::string, std::optional<std::string>>>;
constexpr const char* unicode_key = "QUIDRA_PROCESS_\xe7\x92\xb0\xe5\xa2\x83";
constexpr const char* unicode_value = "\xe9\x9b\xaa \xf0\x9f\x8c\x8d";

// The old API intentionally ignores these additions for the first RED run.
template<class E> int run_environment(const fs::path& program,
        const std::vector<std::string>& args, const fs::path& output, const E& environment) {
    if constexpr (requires { quidra::platform::run_program(program, args, output, std::nullopt, environment); })
        return quidra::platform::run_program(program, args, output, std::nullopt, environment);
    else return quidra::platform::run_program(program, args, output);
}
template<class E> int run_native_environment(const fs::path& program,
        const fs::path& output, const E& environment) {
    if constexpr (requires { run_native_program(program, std::vector<quidra::platform::NativeText>{},
            output, std::nullopt, std::nullopt, environment); })
        return run_native_program(program, {native_text("--environment")}, output, std::nullopt, std::nullopt, environment);
    else return run_native_program(program, {native_text("--environment")}, output);
}
template<class E> quidra::platform::ProgramRun reporting_environment(const fs::path& program,
        const std::vector<std::string>& args, const E& environment) {
    if constexpr (requires { quidra::platform::run_program_reporting_start(program, args, environment); })
        return quidra::platform::run_program_reporting_start(program, args, environment);
    else return quidra::platform::run_program_reporting_start(program, args);
}

std::optional<quidra::platform::NativeText> environment_value(const char* name) {
#ifdef _WIN32
    const auto key = native_text(name);
    for (const auto& record : quidra::platform::environment_native_snapshot()) {
        const auto separator = record.find(L'=', record.front() == L'=' ? 1 : 0);
        if (separator == std::wstring::npos) continue;
        const auto record_key = record.substr(0, separator);
        if (CompareStringOrdinal(key.c_str(), -1, record_key.c_str(), -1, TRUE) == CSTR_EQUAL)
            return record.substr(separator + 1);
    }
    return std::nullopt;
#else
    return quidra::platform::environment_value(name);
#endif
}
void set_environment(const char* name, const std::optional<quidra::platform::NativeText>& value) {
#ifdef _WIN32
    const auto key = native_text(name);
    if (!SetEnvironmentVariableW(key.c_str(), value ? value->c_str() : nullptr))
        throw std::runtime_error("cannot prepare process environment test");
#else
    const int result = value ? setenv(name, value->c_str(), 1) : unsetenv(name);
    if (result != 0) throw std::runtime_error("cannot prepare process environment test");
#endif
}
struct EnvironmentScope {
    std::vector<std::pair<const char*, std::optional<quidra::platform::NativeText>>> original;
    EnvironmentScope(std::initializer_list<const char*> names) {
        for (const auto name : names) original.emplace_back(name, environment_value(name));
    }
    ~EnvironmentScope() { for (const auto& [name, value] : original) set_environment(name, value); }
};
bool value_is(const char* name, const std::optional<std::string>& expected) {
    const auto actual = environment_value(name);
    return expected ? actual && *actual == native_text(*expected) : !actual;
}
int check_child_environment(bool quiet = false) {
    const bool correct = value_is("QUIDRA_PROCESS_REPLACE", "child") &&
        value_is("QUIDRA_PROCESS_REMOVE", std::nullopt) && value_is("QUIDRA_PROCESS_EMPTY", "") &&
        value_is("QUIDRA_PROCESS_INHERITED", "kept") && value_is("QUIDRA_PROCESS_ABSENT", "new") &&
        value_is(unicode_key, unicode_value);
    if (!quiet) {
#ifdef _WIN32
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        std::puts(correct ? "child environment correct" : "child environment incorrect");
    }
    return correct ? 37 : 38;
}
#ifdef _WIN32
std::string drive_records() {
    std::vector<std::wstring> records;
    for (const auto& record : quidra::platform::environment_native_snapshot()) {
        if (record.front() == L'=') records.push_back(record);
    }
    std::sort(records.begin(), records.end());
    std::string bytes;
    for (const auto& record : records)
        bytes.append(reinterpret_cast<const char*>(record.c_str()), (record.size() + 1) * sizeof(wchar_t));
    return bytes;
}
#endif

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void write_bytes(const fs::path& path, std::string_view bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(out), "cannot write process test input");
}

std::string read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    require(static_cast<bool>(in), "cannot read process test output");
    return std::string(std::istreambuf_iterator<char>(in), {});
}

int copy_standard_input() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    char buffer[4096];
    while (const auto size = std::fread(buffer, 1, sizeof(buffer), stdin)) {
        if (std::fwrite(buffer, 1, size, stdout) != size) return 2;
    }
    if (std::ferror(stdin)) return 3;
    std::fputs("child stderr\n", stderr);
    return 37;
}

void test_redirects(const fs::path& program, const fs::path& root) {
    const auto input = root / "input bytes.bin";
    const auto output = root / "output bytes.bin";
    const auto error = root / "error bytes.txt";
    constexpr char bytes[] = "first line\nsecond\0line\n";
    const std::string payload(bytes, sizeof(bytes) - 1);
    write_bytes(input, payload);
    write_bytes(output, "stale stdout");
    write_bytes(error, "stale stderr");

    require(run_native_program(program, {native_text("--copy")}, output, error, input) == 37,
            "stdin redirection changed the child's exit status");
    require(read_bytes(output) == payload, "redirected stdin bytes did not reach stdout");
    require(read_bytes(error) == "child stderr\n", "stdin redirection changed stderr capture");
    require(read_bytes(input) == payload, "stdin redirection modified its input file");

    // The original four-argument API inherits the redirected input of its
    // parent while still handling stdout and stderr redirection itself.
    require(run_native_program(program,
                {native_text("--forward"), output.native(), error.native()},
                std::nullopt, std::nullopt, input) == 37,
            "the four-argument API changed the child's exit status");
    require(read_bytes(output) == payload, "the four-argument API lost inherited stdin");
    require(read_bytes(error) == "child stderr\n", "the four-argument API changed stderr capture");
    require(run_native_program(program, {native_text("--quiet")}) == 19,
            "the default process API changed the child's exit status");

#ifndef _WIN32
    // When stdin was already closed, open() can return descriptor zero.
    // Redirecting that descriptor must leave the child's new stdin open.
    const int saved_input = ::dup(STDIN_FILENO);
    require(saved_input >= 0 || errno == EBADF, "cannot save process test stdin");
    ::close(STDIN_FILENO);
    const auto restore_input = [&] {
        if (saved_input >= 0) {
            require(::dup2(saved_input, STDIN_FILENO) >= 0, "cannot restore process test stdin");
            ::close(saved_input);
        }
    };
    int status;
    try {
        status = run_native_program(program, {native_text("--copy")}, output, error, input);
    } catch (...) {
        restore_input();
        throw;
    }
    restore_input();
    require(status == 37, "closed parent stdin prevented input redirection");
    require(read_bytes(output) == payload, "redirected descriptor zero was closed before exec");
#endif

    // The parent must release its redirected input handle when the child
    // exits; Windows denies this removal while a non-delete-shared handle lives.
    require(fs::remove(input), "the process runner retained its input handle");
}

void test_missing_files(const fs::path& program, const fs::path& root) {
    const auto input = root / "missing input";
#ifdef _WIN32
    bool rejected = false;
    try {
        (void)run_native_program(program, {native_text("--quiet")}, std::nullopt, std::nullopt, input);
    } catch (const std::runtime_error& error) {
        rejected = std::string_view(error.what()) == "cannot open process stdin file";
    }
    require(rejected, "missing stdin did not report an input-open failure");
#else
    require(run_native_program(program, {native_text("--quiet")}, std::nullopt, std::nullopt, input) == 126,
            "missing stdin did not preserve the redirection failure status");
#endif
    require(!fs::exists(input), "stdin redirection created its missing input file");

    write_bytes(input, "input stays readable");
    const auto invalid_output = root / "missing directory" / "output";
#ifdef _WIN32
    rejected = false;
    try {
        (void)run_native_program(program, {native_text("--quiet")}, invalid_output, std::nullopt, input);
    } catch (const std::runtime_error& error) {
        rejected = std::string_view(error.what()) == "cannot open process stdout file";
    }
    require(rejected, "stdout-open failure changed when stdin was redirected");
#else
    require(run_native_program(program, {native_text("--quiet")}, invalid_output, std::nullopt, input) == 126,
            "stdout-open failure changed when stdin was redirected");
#endif
    require(fs::remove(input), "a redirect failure retained the input handle");
}

void test_child_environment(const fs::path& program, const fs::path& root) {
    const EnvironmentScope scope{"QUIDRA_PROCESS_REPLACE", "QUIDRA_PROCESS_REMOVE", "QUIDRA_PROCESS_EMPTY",
        "QUIDRA_PROCESS_INHERITED", "QUIDRA_PROCESS_ABSENT", unicode_key};
    set_environment("QUIDRA_PROCESS_REPLACE", native_text("parent"));
    set_environment("QUIDRA_PROCESS_REMOVE", native_text("parent"));
    set_environment("QUIDRA_PROCESS_EMPTY", native_text("parent"));
    set_environment("QUIDRA_PROCESS_INHERITED", native_text("kept"));
    set_environment("QUIDRA_PROCESS_ABSENT", std::nullopt);
    set_environment(unicode_key, std::nullopt);
    Environment overrides{{"QUIDRA_PROCESS_REPLACE", "first"}, {"QUIDRA_PROCESS_REMOVE", std::nullopt},
        {"QUIDRA_PROCESS_EMPTY", ""}, {"QUIDRA_PROCESS_ABSENT", "new"}, {unicode_key, unicode_value},
        {"QUIDRA_PROCESS_REPLACE", "child"}};
#ifdef _WIN32
    overrides.push_back({"quidra_process_replace", "child"});
    overrides.push_back({"quidra_process_remove", std::nullopt});
#endif
    const auto output = root / "environment.txt";
    require(run_environment(program, {"--environment"}, output, overrides) == 37,
            "child did not receive replacement/removal/empty/Unicode environment");
    require(read_bytes(output) == "child environment correct\n", "child environment output changed");
    require(run_native_environment(program, output, overrides) == 37, "native child overrides lost");
#ifdef _WIN32
    const auto inherited_drives = drive_records();
    require(run_environment(program, {"--drive-records"}, output, overrides) == 0 &&
            read_bytes(output) == inherited_drives, "Windows inherited drive records lost");
#endif
    require(value_is("QUIDRA_PROCESS_REPLACE", "parent") && value_is("QUIDRA_PROCESS_REMOVE", "parent") &&
            value_is("QUIDRA_PROCESS_EMPTY", "parent") && value_is("QUIDRA_PROCESS_INHERITED", "kept") &&
            value_is("QUIDRA_PROCESS_ABSENT", std::nullopt) && value_is(unicode_key, std::nullopt),
            "child overrides modified parent environment");
    const auto started = reporting_environment(program, {"--environment-quiet"}, overrides);
    require(started.started && started.status == 37 && started.failure.empty(), "reporting child environment/status lost");
    for (const int status : {101, 126, 127}) {
        const auto result = reporting_environment(program, {"--status", std::to_string(status)}, overrides);
        require(result.started && result.status == status && result.failure.empty(), "real exit mistaken for start failure");
    }
    const auto missing = reporting_environment(root / "missing-program", {}, overrides);
    require(!missing.started && !missing.failure.empty(), "missing executable falsely reported started");
#ifdef _WIN32
    require(missing.status == 1, "missing Windows program status changed");
#else
    require(missing.status == 127, "missing POSIX program status changed");
#endif
    for (const auto& invalid : {Environment{{"", "value"}}, Environment{{"A=B", "value"}},
            Environment{{std::string("A\0B", 3), "value"}}, Environment{{"QUIDRA_PROCESS_BAD", std::string("a\0b", 3)}}}) {
        bool rejected = false;
        try { (void)run_environment(program, {"--quiet"}, output, invalid); }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "malformed child environment accepted");
    }
#ifdef _WIN32
    bool malformed_utf8 = false;
    try { (void)run_environment(program, {"--quiet"}, output, Environment{{"QUIDRA_PROCESS_BAD", "\xff"}}); }
    catch (const std::runtime_error&) { malformed_utf8 = true; }
    require(malformed_utf8, "malformed UTF-8 environment accepted");
#else
    bool rejected = false;
    try { (void)run_environment("sh", {"--quiet"}, output, overrides); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "overridden environment accepted a nonabsolute executable");
    require(run_environment("sh", {"-c", "exit 23"}, output, Environment{}) == 23,
            "empty environment overrides changed PATH lookup");
    const auto searched = reporting_environment("sh", {"-c", "exit 24"}, Environment{});
    require(searched.started && searched.status == 24, "empty overrides changed reporting PATH lookup");
#endif
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--copy") return copy_standard_input();
    if (argc == 2 && std::string_view(argv[1]) == "--quiet") return 19;
    if (argc == 2 && std::string_view(argv[1]) == "--environment") return check_child_environment();
    if (argc == 2 && std::string_view(argv[1]) == "--environment-quiet") {
        const auto status = check_child_environment(true);
        return status;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--status") return std::stoi(argv[2]);
#ifdef _WIN32
    if (argc == 2 && std::string_view(argv[1]) == "--drive-records") {
        _setmode(_fileno(stdout), _O_BINARY);
        const auto bytes = drive_records();
        return std::fwrite(bytes.data(), 1, bytes.size(), stdout) == bytes.size() ? 0 : 2;
    }
#endif
    if (argc == 4 && std::string_view(argv[1]) == "--forward")
        return run_native_program(fs::absolute(argv[0]), {native_text("--copy")},
                                  fs::path(argv[2]), fs::path(argv[3]));
    try {
        const quidra::platform::TemporaryDirectory root(
            "quidra process tests ", "cannot create process test directory");
        const auto program = fs::absolute(argv[0]);
        test_redirects(program, root.path());
        test_missing_files(program, root.path());
        test_child_environment(program, root.path());
    } catch (const std::exception& error) {
        std::fprintf(stderr, "process test failed: %s\n", error.what());
        return 1;
    }
    std::puts("process redirection tests passed");
    return 0;
}
