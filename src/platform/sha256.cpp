#include "platform/sha256.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

#if defined(__aarch64__) || defined(_M_ARM64)
#define QUIDRA_SHA256_ARM 1
#if defined(_MSC_VER) && !defined(__clang__)
#include <arm64_neon.h>
#else
#include <arm_neon.h>
#endif
#if defined(__APPLE__)
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <sys/auxv.h>
#elif defined(_WIN32)
#include <windows.h>
#endif
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define QUIDRA_SHA256_X86 1
#include <immintrin.h>
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

// The CPU engines' functions are compiled for their instructions whatever
// the target's baseline; they run only after the run-time check.
#if defined(QUIDRA_SHA256_ARM) && !defined(__ARM_FEATURE_SHA2) && \
    (defined(__GNUC__) || defined(__clang__))
#define QUIDRA_SHA256_ARM_TARGET __attribute__((target("+sha2")))
#else
#define QUIDRA_SHA256_ARM_TARGET
#endif
#if defined(QUIDRA_SHA256_X86) && (defined(__GNUC__) || defined(__clang__))
#define QUIDRA_SHA256_X86_TARGET __attribute__((target("sha,sse4.1")))
#else
#define QUIDRA_SHA256_X86_TARGET
#endif

namespace quidra::platform {
namespace {

alignas(16) constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

constexpr std::array<std::uint32_t, 8> kInitialState = {
    0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
    0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
};

// The longest message whose length in bits fits the 64-bit length field.
constexpr std::uint64_t kMaximumBytes = std::numeric_limits<std::uint64_t>::max() / 8U;

constexpr std::uint32_t rotate_right(std::uint32_t value, unsigned amount) {
    return (value >> amount) | (value << (32U - amount));
}

void compress_portable(std::uint32_t* state, const std::uint8_t* blocks, std::size_t count) {
    for (; count > 0; --count, blocks += 64) {
        std::array<std::uint32_t, 64> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(blocks[i * 4]) << 24U) |
                   (static_cast<std::uint32_t>(blocks[i * 4 + 1]) << 16U) |
                   (static_cast<std::uint32_t>(blocks[i * 4 + 2]) << 8U) |
                   static_cast<std::uint32_t>(blocks[i * 4 + 3]);
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const auto s0 = rotate_right(w[i - 15], 7) ^ rotate_right(w[i - 15], 18) ^ (w[i - 15] >> 3U);
            const auto s1 = rotate_right(w[i - 2], 17) ^ rotate_right(w[i - 2], 19) ^ (w[i - 2] >> 10U);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        auto a = state[0]; auto b = state[1]; auto c = state[2]; auto d = state[3];
        auto e = state[4]; auto f = state[5]; auto g = state[6]; auto h = state[7];
        for (std::size_t i = 0; i < 64; ++i) {
            const auto s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
            const auto ch = (e & f) ^ ((~e) & g);
            const auto temp1 = h + s1 + ch + kRoundConstants[i] + w[i];
            const auto s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
            const auto maj = (a & b) ^ (a & c) ^ (b & c);
            const auto temp2 = s0 + maj;
            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }
        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    }
}

#if defined(QUIDRA_SHA256_ARM)

bool cpu_has_sha256() noexcept {
#if defined(__ARM_FEATURE_SHA2)
    return true;
#elif defined(__APPLE__)
    int value = 0;
    std::size_t size = sizeof(value);
    return ::sysctlbyname("hw.optional.arm.FEAT_SHA256", &value, &size, nullptr, 0) == 0 &&
           value != 0;
#elif defined(__linux__)
    constexpr unsigned long hwcap_sha2 = 1UL << 6;  // HWCAP_SHA2 of <asm/hwcap.h>
    return (::getauxval(AT_HWCAP) & hwcap_sha2) != 0;
#elif defined(_WIN32)
    return IsProcessorFeaturePresent(PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE) != 0;
#else
    return false;
#endif
}

// Four rounds per step; the message schedule of the next four words is
// computed with vsha256su0/vsha256su1 for the first twelve steps.
QUIDRA_SHA256_ARM_TARGET
void compress_cpu(std::uint32_t* state, const std::uint8_t* blocks, std::size_t count) {
    uint32x4_t abcd = vld1q_u32(state);
    uint32x4_t efgh = vld1q_u32(state + 4);
    for (; count > 0; --count, blocks += 64) {
        const uint32x4_t abcd_start = abcd;
        const uint32x4_t efgh_start = efgh;
        uint32x4_t words[4] = {
            vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(blocks))),
            vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(blocks + 16))),
            vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(blocks + 32))),
            vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(blocks + 48))),
        };
        for (std::size_t step = 0; step < 16; ++step) {
            const uint32x4_t sum =
                vaddq_u32(words[step & 3U], vld1q_u32(kRoundConstants.data() + step * 4));
            const uint32x4_t abcd_before = abcd;
            abcd = vsha256hq_u32(abcd, efgh, sum);
            efgh = vsha256h2q_u32(efgh, abcd_before, sum);
            if (step < 12) {
                words[step & 3U] = vsha256su1q_u32(
                    vsha256su0q_u32(words[step & 3U], words[(step + 1) & 3U]),
                    words[(step + 2) & 3U], words[(step + 3) & 3U]);
            }
        }
        abcd = vaddq_u32(abcd, abcd_start);
        efgh = vaddq_u32(efgh, efgh_start);
    }
    vst1q_u32(state, abcd);
    vst1q_u32(state + 4, efgh);
}

#elif defined(QUIDRA_SHA256_X86)

// CPUID leaf 1 ECX: SSSE3 (bit 9) and SSE4.1 (bit 19); leaf 7 EBX: SHA (bit 29).
bool cpu_has_sha256() noexcept {
    unsigned int leaf1_ecx = 0;
    unsigned int leaf7_ebx = 0;
#if defined(_MSC_VER) && !defined(__clang__)
    int registers[4] = {};
    __cpuid(registers, 0);
    if (registers[0] < 7) return false;
    __cpuid(registers, 1);
    leaf1_ecx = static_cast<unsigned int>(registers[2]);
    __cpuidex(registers, 7, 0);
    leaf7_ebx = static_cast<unsigned int>(registers[1]);
#else
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (__get_cpuid_max(0, nullptr) < 7) return false;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) return false;
    leaf1_ecx = ecx;
    if (!__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) return false;
    leaf7_ebx = ebx;
#endif
    const bool ssse3 = (leaf1_ecx & (1U << 9)) != 0;
    const bool sse41 = (leaf1_ecx & (1U << 19)) != 0;
    const bool sha = (leaf7_ebx & (1U << 29)) != 0;
    return ssse3 && sse41 && sha;
}

// The state lives as ABEF and CDGH, as sha256rnds2 takes it. Each step does
// four rounds; the message schedule of the next four words is computed with
// sha256msg1/sha256msg2 for the first twelve steps.
QUIDRA_SHA256_X86_TARGET
void compress_cpu(std::uint32_t* state, const std::uint8_t* blocks, std::size_t count) {
    const __m128i byte_swap = _mm_set_epi64x(0x0c0d0e0f08090a0bLL, 0x0405060700010203LL);
    __m128i dcba = _mm_loadu_si128(reinterpret_cast<const __m128i*>(state));
    __m128i hgfe = _mm_loadu_si128(reinterpret_cast<const __m128i*>(state + 4));
    const __m128i cdab = _mm_shuffle_epi32(dcba, 0xB1);
    const __m128i efgh = _mm_shuffle_epi32(hgfe, 0x1B);
    __m128i abef = _mm_alignr_epi8(cdab, efgh, 8);
    __m128i cdgh = _mm_blend_epi16(efgh, cdab, 0xF0);
    for (; count > 0; --count, blocks += 64) {
        const __m128i abef_start = abef;
        const __m128i cdgh_start = cdgh;
        __m128i words[4];
        for (std::size_t i = 0; i < 4; ++i) {
            words[i] = _mm_shuffle_epi8(
                _mm_loadu_si128(reinterpret_cast<const __m128i*>(blocks + i * 16)), byte_swap);
        }
        for (std::size_t step = 0; step < 16; ++step) {
            __m128i sum = _mm_add_epi32(
                words[step & 3U],
                _mm_load_si128(reinterpret_cast<const __m128i*>(kRoundConstants.data() + step * 4)));
            cdgh = _mm_sha256rnds2_epu32(cdgh, abef, sum);
            sum = _mm_shuffle_epi32(sum, 0x0E);
            abef = _mm_sha256rnds2_epu32(abef, cdgh, sum);
            if (step < 12) {
                __m128i next = _mm_sha256msg1_epu32(words[step & 3U], words[(step + 1) & 3U]);
                next = _mm_add_epi32(
                    next, _mm_alignr_epi8(words[(step + 3) & 3U], words[(step + 2) & 3U], 4));
                words[step & 3U] = _mm_sha256msg2_epu32(next, words[(step + 3) & 3U]);
            }
        }
        abef = _mm_add_epi32(abef, abef_start);
        cdgh = _mm_add_epi32(cdgh, cdgh_start);
    }
    const __m128i feba = _mm_shuffle_epi32(abef, 0x1B);
    const __m128i dchg = _mm_shuffle_epi32(cdgh, 0xB1);
    dcba = _mm_blend_epi16(feba, dchg, 0xF0);
    hgfe = _mm_alignr_epi8(dchg, feba, 8);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(state), dcba);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(state + 4), hgfe);
}

#else

bool cpu_has_sha256() noexcept { return false; }

void compress_cpu(std::uint32_t*, const std::uint8_t*, std::size_t) {}

#endif

bool cpu_engine_available() noexcept {
    static const bool available = cpu_has_sha256();
    return available;
}

} // namespace

bool sha256_cpu_engine_available() noexcept { return cpu_engine_available(); }

Sha256::Sha256(Sha256Engine engine) : compress_(compress_portable), state_(kInitialState) {
    if (engine == Sha256Engine::cpu && !cpu_engine_available()) {
        throw std::invalid_argument("this CPU has no SHA-256 instructions");
    }
    if (engine != Sha256Engine::portable && cpu_engine_available()) compress_ = compress_cpu;
}

void Sha256::update(const void* data, std::size_t size) {
    if (size > kMaximumBytes - bytes_) {
        throw std::overflow_error("message is too long for SHA-256");
    }
    bytes_ += size;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    if (buffered_ > 0) {
        const auto take = std::min(size, buffer_.size() - buffered_);
        std::copy_n(bytes, take, buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_));
        buffered_ += take;
        bytes += take;
        size -= take;
        if (buffered_ < buffer_.size()) return;
        compress_(state_.data(), buffer_.data(), 1);
        buffered_ = 0;
    }
    if (size >= 64) {
        const auto blocks = size / 64;
        compress_(state_.data(), bytes, blocks);
        bytes += blocks * 64;
        size -= blocks * 64;
    }
    std::copy_n(bytes, size, buffer_.begin());
    buffered_ = size;
}

std::array<std::uint8_t, 32> Sha256::finish() {
    const auto bits = bytes_ * 8U;
    buffer_[buffered_++] = 0x80U;
    if (buffered_ > 56) {
        std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_), buffer_.end(), std::uint8_t{0});
        compress_(state_.data(), buffer_.data(), 1);
        buffered_ = 0;
    }
    std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_), buffer_.begin() + 56, std::uint8_t{0});
    for (unsigned i = 0; i < 8; ++i) {
        buffer_[63 - i] = static_cast<std::uint8_t>((bits >> (i * 8U)) & 0xffU);
    }
    compress_(state_.data(), buffer_.data(), 1);
    buffered_ = 0;

    std::array<std::uint8_t, 32> digest{};
    for (std::size_t i = 0; i < 8; ++i) {
        digest[i * 4] = static_cast<std::uint8_t>(state_[i] >> 24U);
        digest[i * 4 + 1] = static_cast<std::uint8_t>(state_[i] >> 16U);
        digest[i * 4 + 2] = static_cast<std::uint8_t>(state_[i] >> 8U);
        digest[i * 4 + 3] = static_cast<std::uint8_t>(state_[i]);
    }
    return digest;
}

std::string Sha256::finish_hex() {
    static constexpr char digits[] = "0123456789abcdef";
    const auto digest = finish();
    std::string text;
    text.reserve(digest.size() * 2);
    for (const auto byte : digest) {
        text.push_back(digits[byte >> 4U]);
        text.push_back(digits[byte & 0xfU]);
    }
    return text;
}

std::string sha256_hex(std::string_view data) {
    Sha256 hash;
    hash.update(data);
    return hash.finish_hex();
}

} // namespace quidra::platform
