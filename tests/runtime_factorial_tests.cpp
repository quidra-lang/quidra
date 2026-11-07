#include "quidra/abi/layout.hpp"
#include "quidra/abi/process_status.hpp"
#include "quidra/abi/runtime_entry_points.hpp"
#include "runtime_internal.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>

#ifdef _WIN32
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
namespace abi = quidra::abi;
namespace layout = abi::bare_integer_layout;
int failures = 0;

void expect(bool condition, const std::string& what) {
    if (condition) return;
    std::fprintf(stderr, "runtime factorial test failed: %s\n", what.c_str());
    ++failures;
}

void* word_box(long long value) {
    if ((value & layout::boxed_bit) == 0) return nullptr;
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(value) &
                                   ~static_cast<std::uintptr_t>(layout::boxed_bit));
}

void release_word(long long value) {
    if (auto* box = word_box(value))
        quidra_managed_release(box, reinterpret_cast<void*>(&quidra_bigint_drop));
}

std::string word_text(long long value) {
    char* text = quidra_int_text(value);
    const std::string result(text);
    quidra_managed_release(text, nullptr);
    return result;
}

void exact_factorials() {
    struct Case { long long input; const char* output; };
    const Case cases[] = {
        {0, "1"}, {1, "1"}, {5, "120"}, {20, "2432902008176640000"},
        {21, "51090942171709440000"},
        {50, "30414093201713378043612608166064768844377641568960512000000000000"},
        {100, "9332621544394415268169923885626670049071596826438162146859296389521759999322"
              "9915608941463976156518286253697920827223758251185210916864000000000000000000000000"},
    };
    for (const auto& test : cases) {
        const auto input = quidra_int_from_i64(test.input);
        const auto result = quidra_int_factorial(input, 17, 23);
        const auto label = std::to_string(test.input) + "!";
        expect(word_text(result) == test.output, label + " is exact");
        expect(word_text(input) == std::to_string(test.input), label + " preserves operand");
        expect((word_box(result) == nullptr) == (test.input <= 20), label + " is canonical");
        if (auto* box = word_box(result)) {
            expect(quidra::runtime::managed_owners(box) == 1, label + " owns one box");
            const auto second = quidra_int_factorial(input, 17, 23);
            expect(word_box(second) != box, label + " has independent result ownership");
            release_word(second);
            expect(word_text(result) == test.output, label + " survives releasing another result");
            release_word(result);
            expect(!quidra::runtime::is_managed(box), label + " releases its box");
        } else {
            release_word(result);
        }
        release_word(input);
    }
}

void fixed_factorials() {
    struct Case { int bits; long long signed_input; unsigned long long unsigned_input, output; };
    const Case cases[] = {{8, 5, 5, 120}, {16, 7, 8, 40320},
                          {32, 12, 12, 479001600}, {64, 20, 20, 2432902008176640000ULL}};
    for (const auto& test : cases) {
        const auto label = std::to_string(test.bits) + "-bit factorial";
        expect(quidra_integer_factorial_signed(0, test.bits, 17, 23) == 1, label + " signed zero");
        expect(quidra_integer_factorial_unsigned(0, test.bits, 17, 23) == 1, label + " unsigned zero");
        const long long signed_output = test.bits == 16 ? 5040 : static_cast<long long>(test.output);
        expect(quidra_integer_factorial_signed(test.signed_input, test.bits, 17, 23) == signed_output,
               label + " largest signed result");
        expect(quidra_integer_factorial_unsigned(test.unsigned_input, test.bits, 17, 23) == test.output,
               label + " largest unsigned result");
    }
}

int fail_child(const std::string& kind, const std::string& value, int bits, const char* errors) {
#ifdef _WIN32
    std::FILE* stream = nullptr;
    if (freopen_s(&stream, errors, "wb", stderr) != 0) return 126;
#else
    if (!std::freopen(errors, "wb", stderr)) return 126;
#endif
    static const struct {
        abi::SourceTable header;
        abi::SourceTableEntry entry;
    } source{{1}, {"factorial.qui", "/quidra-factorial-test.qui", "", 100, 0}};
    quidra_runtime_register_sources(&source);
    if (kind == "word") {
        long long input = 0;
        if (!quidra_int_parse(value.c_str(), &input)) return 125;
        release_word(quidra_int_factorial(input, 17, 23));
        release_word(input);
    } else if (kind == "signed") {
        (void)quidra_integer_factorial_signed(std::stoll(value), bits, 17, 23);
    } else {
        (void)quidra_integer_factorial_unsigned(std::stoull(value), bits, 17, 23);
    }
    return 0;
}

int child_status(const std::string& executable, const char* kind, const std::string& value,
                 int bits, const std::filesystem::path& errors) {
    const auto width = std::to_string(bits), path = errors.string();
#ifdef _WIN32
    return static_cast<int>(_spawnl(_P_WAIT, executable.c_str(), executable.c_str(),
        "--failure", kind, value.c_str(), width.c_str(), path.c_str(), static_cast<char*>(nullptr)));
#else
    const auto pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execl(executable.c_str(), executable.c_str(), "--failure", kind, value.c_str(),
              width.c_str(), path.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

void checked_failures(const std::string& executable) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = std::filesystem::temp_directory_path() /
                           ("quidra-factorial-tests-" + std::to_string(stamp));
    std::filesystem::create_directory(directory);
    const auto errors = directory / "stderr.txt";
    const auto failure = [&](const char* kind, const std::string& input, int bits, const char* code) {
        const auto label = std::string(kind) + " " + input + "! in " + std::to_string(bits) + " bits";
        const int status = child_status(executable, kind, input, bits, errors);
        expect(status == abi::failure_exit_status, label + " stops with failure status");
        std::ifstream in(errors, std::ios::binary);
        const std::string report{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        expect(report.find(std::string("error[") + code + "]") != std::string::npos,
               label + " reports " + code + ": " + report);
        expect(report.find("factorial.qui:17:23") != std::string::npos,
               label + " preserves source location: " + report);
    };
    failure("word", "-1", 0, "FACTORIAL_DOMAIN");
    failure("word", "-1000000000000000000000000000000", 0, "FACTORIAL_DOMAIN");
    for (int bits : {8, 16, 32, 64}) {
        failure("signed", "-1", bits, "FACTORIAL_DOMAIN");
        failure("signed", bits == 8 ? "6" : bits == 16 ? "8" : bits == 32 ? "13" : "21",
                bits, "INTEGER_OVERFLOW");
        failure("unsigned", bits == 8 ? "6" : bits == 16 ? "9" : bits == 32 ? "13" : "21",
                bits, "INTEGER_OVERFLOW");
    }
    failure("signed", std::to_string(std::numeric_limits<long long>::min()), 64, "FACTORIAL_DOMAIN");
    failure("signed", std::to_string(std::numeric_limits<long long>::max()), 64, "INTEGER_OVERFLOW");
    failure("unsigned", std::to_string(std::numeric_limits<unsigned long long>::max()),
            64, "INTEGER_OVERFLOW");
    std::filesystem::remove_all(directory);
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 6 && std::string(argv[1]) == "--failure")
        return fail_child(argv[2], argv[3], std::stoi(argv[4]), argv[5]);
    exact_factorials();
    fixed_factorials();
    checked_failures(std::filesystem::absolute(argv[0]).string());
    if (failures != 0) return 1;
    std::puts("runtime factorial tests passed");
    return 0;
}
