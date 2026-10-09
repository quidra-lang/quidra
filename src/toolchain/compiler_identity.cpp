#include "toolchain/compiler_identity.hpp"

#include "platform/executable.hpp"
#include "platform/file_identity.hpp"
#include "platform/private_directory.hpp"
#include "platform/sha256.hpp"
#include "platform/strict_json.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <fstream>
#include <string_view>
#include <system_error>
#include <variant>

namespace fs = std::filesystem;

namespace quidra::toolchain {
namespace {

constexpr std::string_view memo_kind = "quidra-compiler-id";
constexpr std::size_t memo_limit = 64 * 1024;

// The text of a JSON string literal for `text`.
std::string json_string(std::string_view text) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string out = "\"";
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (byte < 0x20U) {
            out += "\\u00";
            out.push_back(hex[byte >> 4U]);
            out.push_back(hex[byte & 0xfU]);
        } else {
            out.push_back(c);
        }
    }
    out.push_back('"');
    return out;
}

std::string path_text(const fs::path& path) {
    const auto encoded = path.u8string();
    return std::string(encoded.begin(), encoded.end());
}

// Identity fields as decimal strings: an inode or a device number may not
// fit a JSON integer the strict reader accepts.
std::string identity_json(const platform::FileIdentity& identity) {
    return "{\"device\":\"" + std::to_string(identity.device) +
           "\",\"file\":\"" + std::to_string(identity.file) +
           "\",\"file_high\":\"" + std::to_string(identity.file_high) +
           "\",\"size\":\"" + std::to_string(identity.size) +
           "\",\"modified_ns\":\"" + std::to_string(identity.modified_ns) +
           "\",\"changed_ns\":\"" + std::to_string(identity.changed_ns) + "\"}";
}

const std::string* string_member(const platform::JsonValue::Object& object, const char* key) {
    const auto found = object.find(key);
    if (found == object.end()) return nullptr;
    return std::get_if<std::string>(&found->second.data);
}

bool hex_digest(const std::string& text) {
    if (text.size() != 64) return false;
    for (const char c : text) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

// The digest a memo holds for `real_path` with `identity`, or nullopt when
// the memo is missing, malformed, or describes anything else.
std::optional<std::string> memo_digest(
    const fs::path& memo, const fs::path& real_path, const platform::FileIdentity& identity) {
    const auto text = platform::read_private_file(memo, memo_limit);
    if (!text) return std::nullopt;
    try {
        const auto document = platform::read_strict_json(*text, {"compiler id memo", 8});
        const auto* object = std::get_if<platform::JsonValue::Object>(&document.data);
        if (!object || object->size() != 5) return std::nullopt;
        const auto schema = object->find("schema");
        if (schema == object->end() ||
            std::get_if<std::int64_t>(&schema->second.data) == nullptr ||
            *std::get_if<std::int64_t>(&schema->second.data) != 1) {
            return std::nullopt;
        }
        const auto* kind = string_member(*object, "kind");
        const auto* path = string_member(*object, "path");
        const auto* digest = string_member(*object, "sha256");
        const auto recorded_identity = object->find("identity");
        if (!kind || *kind != memo_kind || !path || *path != path_text(real_path) || !digest ||
            !hex_digest(*digest) || recorded_identity == object->end()) {
            return std::nullopt;
        }
        // The identity must be exactly the one this executable has now.
        const auto expected = platform::read_strict_json(identity_json(identity));
        const auto* fields = std::get_if<platform::JsonValue::Object>(&recorded_identity->second.data);
        const auto* wanted = std::get_if<platform::JsonValue::Object>(&expected.data);
        if (!fields || !wanted || fields->size() != wanted->size()) return std::nullopt;
        for (const auto& [name, value] : *wanted) {
            const auto* actual = string_member(*fields, name.c_str());
            if (!actual || *actual != std::get<std::string>(value.data)) return std::nullopt;
        }
        return *digest;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<std::string> file_digest(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    platform::Sha256 hash;
    std::array<char, 1 << 20> buffer{};
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = in.gcount();
        if (count > 0) hash.update(buffer.data(), static_cast<std::size_t>(count));
    }
    if (!in.eof()) return std::nullopt;
    return hash.finish_hex();
}

} // namespace

std::optional<std::string> compiler_build_id(
    const fs::path& executable, const std::optional<fs::path>& memo_directory,
    std::chrono::system_clock::time_point hashing_start) {
    std::error_code error;
    auto real_path = fs::canonical(executable, error);
    if (error) real_path = fs::absolute(executable, error).lexically_normal();
    const auto before = platform::file_identity(real_path);
    if (!before) return std::nullopt;

    std::optional<fs::path> memo;
    if (memo_directory) {
        memo = *memo_directory / (platform::sha256_hex(path_text(real_path)) + ".json");
        if (auto digest = memo_digest(*memo, real_path, *before)) return digest;
    }

    auto digest = file_digest(real_path);
    if (!digest || platform::file_identity(real_path) != before) return std::nullopt;

    const auto start_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              hashing_start.time_since_epoch())
                              .count();
    constexpr std::int64_t racy_window_ns = 2'000'000'000;
    if (memo && before->changed_ns < start_ns - racy_window_ns) {
        const auto text = "{\"schema\":1,\"kind\":" + json_string(memo_kind) +
                          ",\"path\":" + json_string(path_text(real_path)) +
                          ",\"identity\":" + identity_json(*before) +
                          ",\"sha256\":" + json_string(*digest) + "}\n";
        (void)platform::replace_private_file(*memo, text);
    }
    return digest;
}

std::optional<CompilerIdentity> compiler_identity(
    const CompilerVersions& versions, const std::optional<fs::path>& cache_root) {
    fs::path executable;
    try {
        executable = platform::executable_path();
    } catch (const std::exception&) {
        return std::nullopt;
    }
    std::optional<fs::path> memo_directory;
    if (cache_root) {
        const auto directory = platform::open_private_subdirectory(*cache_root, "compiler-id");
        memo_directory = directory.path;
    }
    auto build_id = compiler_build_id(executable, memo_directory);
    if (!build_id) return std::nullopt;
    return CompilerIdentity{versions, std::move(*build_id)};
}

} // namespace quidra::toolchain
