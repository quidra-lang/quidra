#pragma once

// SHA-256 (FIPS 180-4), incremental, with the digest as 64 lowercase hex
// digits. The one implementation of the compiler and its tools: source
// revisions (inspect, patch), package tree hashes (quidra.lock) and the
// provenance of compiled sources.
//
// Engines compute the same digests. `portable` is plain C++ and runs
// everywhere, WebAssembly included; `cpu` uses the CPU's SHA-256
// instructions (ARMv8 SHA2, x86 SHA-NI) and exists only where the CPU has
// them, which is decided once, at run time. `automatic` takes `cpu` when it
// exists and `portable` otherwise.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace quidra::platform {

enum class Sha256Engine { automatic, portable, cpu };

// Whether this CPU has the instructions the `cpu` engine uses.
bool sha256_cpu_engine_available() noexcept;

class Sha256 {
public:
    // Throws std::invalid_argument for `cpu` where sha256_cpu_engine_available()
    // is false.
    explicit Sha256(Sha256Engine engine = Sha256Engine::automatic);

    // Throws std::overflow_error (a std::runtime_error) once the message would
    // exceed 2^64 - 1 bits.
    void update(const void* data, std::size_t size);
    void update(std::string_view text) { update(text.data(), text.size()); }

    // Ends the message. The object is not used afterwards.
    std::array<std::uint8_t, 32> finish();
    std::string finish_hex();

    using Compress = void (*)(std::uint32_t* state, const std::uint8_t* blocks,
                              std::size_t count);

private:
    Compress compress_;
    std::array<std::uint32_t, 8> state_;
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffered_{0};
    std::uint64_t bytes_{0};
};

// The digest of `data`, as 64 lowercase hex digits.
std::string sha256_hex(std::string_view data);

} // namespace quidra::platform
