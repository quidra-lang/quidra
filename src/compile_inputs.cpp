#include "quidra/compile_inputs.hpp"

#include "platform/sha256.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

namespace quidra {

void CompileInputs::record(CompileInput input) {
    if (std::find(records_.begin(), records_.end(), input) != records_.end()) return;
    records_.push_back(std::move(input));
}

void CompileInputs::mark_incomplete(std::string reason) {
    if (!incomplete_reason_.empty()) return;
    incomplete_reason_ = reason.empty() ? std::string("incomplete") : std::move(reason);
}

const char* input_file_kind_name(InputFileKind kind) {
    switch (kind) {
        case InputFileKind::source: return "source";
        case InputFileKind::manifest: return "manifest";
        case InputFileKind::project: return "project";
        case InputFileKind::descriptor: return "descriptor";
        case InputFileKind::lock: return "lock";
    }
    return "source";
}

const char* input_path_state_name(InputPathState state) {
    switch (state) {
        case InputPathState::missing: return "missing";
        case InputPathState::regular_file: return "regular_file";
        case InputPathState::other: return "other";
    }
    return "other";
}

InputPathState input_path_state(const fs::path& path) {
    std::error_code error;
    const auto status = fs::status(path, error);
    if (error) {
        return error == std::errc::no_such_file_or_directory ||
                       error == std::errc::not_a_directory
                   ? InputPathState::missing
                   : InputPathState::other;
    }
    switch (status.type()) {
        case fs::file_type::regular: return InputPathState::regular_file;
        case fs::file_type::not_found: return InputPathState::missing;
        default: return InputPathState::other;
    }
}

InputPathState probe_input_path(const fs::path& path, CompileInputs* inputs) {
    const auto state = input_path_state(path);
    if (inputs && state != InputPathState::regular_file) {
        inputs->record(AbsentInput{path, state});
    }
    return state;
}

std::optional<std::string> read_input_file(
    const fs::path& path, InputFileKind kind, CompileInputs* inputs) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (inputs) {
            const auto state = input_path_state(path);
            if (state == InputPathState::regular_file) {
                inputs->mark_incomplete("cannot read " + path.string());
            } else {
                inputs->record(AbsentInput{path, state});
            }
        }
        return std::nullopt;
    }
    std::ostringstream out;
    out << in.rdbuf();
    auto content = out.str();
    if (inputs) {
        inputs->record(InputFile{
            kind, path, static_cast<std::uint64_t>(content.size()),
            platform::sha256_hex(content)});
    }
    return content;
}

} // namespace quidra
