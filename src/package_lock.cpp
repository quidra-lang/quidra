#include "quidra/package_lock.hpp"
#include "quidra/package_manifest.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace quidra {
namespace {

namespace fs = std::filesystem;

class Sha256 {
public:
    void update(const void* data, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        if (size > (std::numeric_limits<std::uint64_t>::max() - bit_count_) / 8U)
            throw std::runtime_error("package is too large to hash");
        bit_count_ += static_cast<std::uint64_t>(size) * 8U;
        while (size > 0) {
            const auto take = std::min(size, buffer_.size() - buffered_);
            std::copy_n(bytes, take, buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_));
            buffered_ += take;
            bytes += take;
            size -= take;
            if (buffered_ == buffer_.size()) {
                transform(buffer_.data());
                buffered_ = 0;
            }
        }
    }

    void update(std::string_view text) {
        update(text.data(), text.size());
    }

    void update_u64(std::uint64_t value) {
        std::array<unsigned char, 8> bytes{};
        for (int i = 7; i >= 0; --i) {
            bytes[static_cast<std::size_t>(i)] = static_cast<unsigned char>(value & 0xffU);
            value >>= 8U;
        }
        update(bytes.data(), bytes.size());
    }

    std::string finish() {
        const auto original_bits = bit_count_;
        const unsigned char one = 0x80U;
        update_padding(&one, 1);
        const unsigned char zero = 0;
        while (buffered_ != 56) update_padding(&zero, 1);
        std::array<unsigned char, 8> length{};
        auto value = original_bits;
        for (int i = 7; i >= 0; --i) {
            length[static_cast<std::size_t>(i)] = static_cast<unsigned char>(value & 0xffU);
            value >>= 8U;
        }
        update_padding(length.data(), length.size());

        std::ostringstream output;
        output << std::hex << std::setfill('0');
        for (const auto word : state_) output << std::setw(8) << word;
        return output.str();
    }

private:
    std::array<std::uint32_t, 8> state_{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    std::array<unsigned char, 64> buffer_{};
    std::size_t buffered_{};
    std::uint64_t bit_count_{};

    static std::uint32_t rotate_right(std::uint32_t value, unsigned amount) {
        return (value >> amount) | (value << (32U - amount));
    }

    void update_padding(const void* data, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        while (size > 0) {
            const auto take = std::min(size, buffer_.size() - buffered_);
            std::copy_n(bytes, take, buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_));
            buffered_ += take;
            bytes += take;
            size -= take;
            if (buffered_ == buffer_.size()) {
                transform(buffer_.data());
                buffered_ = 0;
            }
        }
    }

    void transform(const unsigned char* block) {
        static constexpr std::array<std::uint32_t, 64> constants{
            0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
            0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
            0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
            0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
            0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
            0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
            0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
            0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U};

        std::array<std::uint32_t, 64> words{};
        for (std::size_t i = 0; i < 16; ++i) {
            const auto p = block + i * 4;
            words[i] = (static_cast<std::uint32_t>(p[0]) << 24U) |
                       (static_cast<std::uint32_t>(p[1]) << 16U) |
                       (static_cast<std::uint32_t>(p[2]) << 8U) |
                       static_cast<std::uint32_t>(p[3]);
        }
        for (std::size_t i = 16; i < words.size(); ++i) {
            const auto s0 = rotate_right(words[i - 15], 7) ^
                            rotate_right(words[i - 15], 18) ^
                            (words[i - 15] >> 3U);
            const auto s1 = rotate_right(words[i - 2], 17) ^
                            rotate_right(words[i - 2], 19) ^
                            (words[i - 2] >> 10U);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }

        auto a = state_[0];
        auto b = state_[1];
        auto c = state_[2];
        auto d = state_[3];
        auto e = state_[4];
        auto f = state_[5];
        auto g = state_[6];
        auto h = state_[7];

        for (std::size_t i = 0; i < words.size(); ++i) {
            const auto sum1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
            const auto choose = (e & f) ^ ((~e) & g);
            const auto temp1 = h + sum1 + choose + constants[i] + words[i];
            const auto sum0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto temp2 = sum0 + majority;

            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }
};

std::string portable_relative_path(const fs::path& path, const fs::path& root) {
    const auto relative = fs::relative(path, root).lexically_normal();
#if defined(__cpp_char8_t)
    const auto raw = relative.generic_u8string();
    return std::string(reinterpret_cast<const char*>(raw.data()), raw.size());
#else
    return relative.generic_u8string();
#endif
}

bool valid_lock_name(std::string_view name) {
    if (name.empty()) return false;
    for (const char c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

bool valid_sha256(std::string_view value) {
    if (value.size() != 64) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isdigit(c) || (c >= 'a' && c <= 'f');
    });
}

} // namespace

fs::path package_lock_path(const fs::path& project_root) {
    return fs::absolute(project_root).lexically_normal() / "quidra.lock";
}

std::string package_tree_sha256(const fs::path& package_main) {
    const auto main = fs::absolute(package_main).lexically_normal();
    std::error_code error;
    if (!fs::is_regular_file(main, error) || error || main.filename() != "main.qui") {
        throw std::runtime_error("package hash requires a package main.qui file");
    }
    const auto root = main.parent_path();

    std::vector<fs::path> files;
    for (fs::recursive_directory_iterator iterator(root), end; iterator != end; ++iterator) {
        const auto status = iterator->symlink_status(error);
        if (error) throw std::runtime_error("cannot inspect package tree: " + error.message());
        if (fs::is_directory(status) && iterator->path().filename() == ".git") {
            iterator.disable_recursion_pending();
            continue;
        }
        if (fs::is_symlink(status)) {
            throw std::runtime_error(
                "package hashing rejects symbolic links: " + iterator->path().string());
        }
        if (fs::is_regular_file(status)) files.push_back(iterator->path());
        else if (!fs::is_directory(status)) {
            throw std::runtime_error(
                "package hashing encountered unsupported filesystem entry: " +
                iterator->path().string());
        }
    }
    std::sort(files.begin(), files.end(), [&](const fs::path& left, const fs::path& right) {
        return portable_relative_path(left, root) < portable_relative_path(right, root);
    });

    Sha256 hash;
    hash.update("quidra-package-tree-v1");
    for (const auto& file : files) {
        const auto relative = portable_relative_path(file, root);
        hash.update_u64(static_cast<std::uint64_t>(relative.size()));
        hash.update(relative);

        const auto size = fs::file_size(file, error);
        if (error) throw std::runtime_error("cannot stat package file: " + file.string());
        hash.update_u64(static_cast<std::uint64_t>(size));

        std::ifstream input(file, std::ios::binary);
        if (!input) throw std::runtime_error("cannot read package file: " + file.string());
        std::array<char, 64 * 1024> buffer{};
        while (input) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = input.gcount();
            if (count > 0) hash.update(buffer.data(), static_cast<std::size_t>(count));
        }
        if (!input.eof()) throw std::runtime_error("cannot finish reading package file: " + file.string());
    }
    return hash.finish();
}

std::optional<PackageLockEntries> read_package_lock(const fs::path& project_root) {
    const auto path = package_lock_path(project_root);
    std::error_code error;
    if (!fs::exists(path, error) && !error) return std::nullopt;
    if (error || !fs::is_regular_file(path, error) || error) {
        throw std::runtime_error("quidra.lock exists but is not a regular file");
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read quidra.lock");

    std::string line;
    if (!std::getline(input, line)) {
        throw std::runtime_error("quidra.lock is empty");
    }
    const bool legacy = line == "quidra-lock-v1";
    if (!legacy && line != "quidra-lock-v2") {
        throw std::runtime_error(
            "quidra.lock must begin with 'quidra-lock-v2'");
    }

    PackageLockEntries entries;
    std::size_t line_number = 1;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.empty()) continue;

        std::istringstream fields(line);
        std::string name;
        std::string version = "-";
        std::string digest;
        std::string extra;
        if (legacy) {
            if (!(fields >> name >> digest) || (fields >> extra)) {
                throw std::runtime_error(
                    "invalid quidra.lock entry on line " +
                    std::to_string(line_number));
            }
        } else {
            if (!(fields >> name >> version >> digest) || (fields >> extra)) {
                throw std::runtime_error(
                    "invalid quidra.lock entry on line " +
                    std::to_string(line_number));
            }
        }

        if (!valid_lock_name(name) || !valid_sha256(digest)) {
            throw std::runtime_error(
                "invalid quidra.lock entry on line " +
                std::to_string(line_number));
        }
        if (version != "-") {
            try {
                (void)parse_semantic_version(version);
            } catch (const std::exception&) {
                throw std::runtime_error(
                    "invalid package version in quidra.lock on line " +
                    std::to_string(line_number));
            }
        }

        if (!entries
                 .emplace(
                     name,
                     PackageLockEntry{
                         std::move(version), std::move(digest)})
                 .second) {
            throw std::runtime_error(
                "duplicate package in quidra.lock: " + name);
        }
    }
    return entries;
}

std::string package_lock_text(
    const std::map<std::string, fs::path>& packages) {
    std::ostringstream output;
    output << "quidra-lock-v2\n";

    for (const auto& [name, main] : packages) {
        if (!valid_lock_name(name)) {
            throw std::runtime_error(
                "invalid package name in resolved dependency set");
        }

        std::string version = "-";
        if (const auto manifest =
                try_read_package_manifest(main.parent_path())) {
            version = manifest->version.str();
        }

        output
            << name << ' '
            << version << ' '
            << package_tree_sha256(main) << '\n';
    }

    return output.str();
}

} // namespace quidra
