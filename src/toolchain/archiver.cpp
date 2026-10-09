#include "toolchain/archiver.hpp"

#include "platform/environment.hpp"
#include "platform/native_text.hpp"
#include "platform/process.hpp"

#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace quidra::toolchain {
namespace {

using platform::NativeText;

// The override in `variable`, or `fallback`.
std::string archiver_program(const char* variable, const char* fallback) {
    const auto configured = platform::environment_value(variable);
    return configured && !configured->empty() ? *configured : fallback;
}

#if !defined(__APPLE__) && !defined(_WIN32)
// An MRI script line names a path; ar's MRI reader takes it up to the end of
// the line, so a path that holds a line break cannot be named.
std::string mri_path(const fs::path& path) {
    const auto text = path.string();
    if (text.find_first_of("\r\n") != std::string::npos)
        throw std::runtime_error("cannot archive a path with a line break: " + text);
    return text;
}
#endif

} // namespace

void create_static_archive(const fs::path& output, const std::vector<fs::path>& objects,
                           const std::vector<fs::path>& archives) {
    for (const auto& input : objects) {
        if (!fs::is_regular_file(input))
            throw std::runtime_error("library member is not a regular file: " + input.string());
    }
    for (const auto& input : archives) {
        if (!fs::is_regular_file(input))
            throw std::runtime_error("library archive is not a regular file: " + input.string());
    }
    std::random_device random;
    auto partial = output;
    partial += ".partial-" + std::to_string(random());
    std::error_code ignored;
    fs::remove(partial, ignored);

#ifdef __APPLE__
    const auto program = archiver_program("QUIDRA_LIBTOOL", "libtool");
    std::vector<NativeText> arguments{"-static", "-o", partial.native()};
    for (const auto& input : objects) arguments.push_back(input.native());
    for (const auto& input : archives) arguments.push_back(input.native());
    const int status = platform::run_native_program(fs::path(program), arguments);
#elif defined(_WIN32)
    const auto program = archiver_program("QUIDRA_AR", "lib.exe");
    std::vector<NativeText> arguments{L"/NOLOGO", L"/OUT:" + partial.native()};
    for (const auto& input : objects) arguments.push_back(input.native());
    for (const auto& input : archives) arguments.push_back(input.native());
    const int status = platform::run_native_program(fs::path(program), arguments);
#else
    const auto program = archiver_program("QUIDRA_AR", "ar");
    auto script = output;
    script += ".mri-" + std::to_string(random());
    {
        std::ofstream mri(script, std::ios::binary | std::ios::trunc);
        mri << "CREATE " << mri_path(partial) << "\n";
        for (const auto& input : objects) mri << "ADDMOD " << mri_path(input) << "\n";
        for (const auto& input : archives) mri << "ADDLIB " << mri_path(input) << "\n";
        mri << "SAVE\nEND\n";
        if (!mri) throw std::runtime_error("cannot write the archiver script: " + script.string());
    }
    // ar reads an MRI script on its standard input only.
    const int status = platform::run_native_program(
        fs::path("/bin/sh"),
        {"-c", "exec \"$0\" -M < \"$1\"", program, script.native()});
    fs::remove(script, ignored);
#endif
    if (status != 0 || !fs::is_regular_file(partial)) {
        fs::remove(partial, ignored);
        if (status == 127)
            throw std::runtime_error("cannot run the static archiver '" + program + "'");
        throw std::runtime_error("the static archiver '" + program + "' failed with status " +
                                 std::to_string(status));
    }
    std::error_code error;
    fs::rename(partial, output, error);
    if (error) {
        fs::remove(partial, ignored);
        throw std::runtime_error("cannot write the library archive: " + output.string());
    }
}

} // namespace quidra::toolchain
