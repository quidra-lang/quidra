// platform::Sha256: the FIPS 180-4 / NIST CAVP vectors on every engine,
// whole and in pieces, and the CPU engine (where this CPU has one) against the
// portable engine on 100000 random messages.

#include "platform/sha256.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using quidra::platform::Sha256;
using quidra::platform::Sha256Engine;

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

std::string digest(Sha256Engine engine, std::string_view message, std::size_t piece) {
    Sha256 hash(engine);
    if (piece == 0) {
        hash.update(message);
    } else {
        for (std::size_t offset = 0; offset < message.size(); offset += piece) {
            hash.update(message.substr(offset, piece));
        }
    }
    return hash.finish_hex();
}

struct Vector {
    std::string message;
    const char* digest;
};

const char* engine_name(Sha256Engine engine) {
    switch (engine) {
        case Sha256Engine::automatic: return "automatic";
        case Sha256Engine::portable: return "portable";
        case Sha256Engine::cpu: return "cpu";
    }
    return "?";
}

} // namespace

int main() {
    const std::vector<Vector> vectors = {
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
        {"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
         "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"},
        {std::string(1000000, 'a'),
         "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"},
        // Lengths around the padding boundaries: 55, 56 and 64 bytes.
        {std::string(55, 'a'), "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
        {std::string(56, 'a'), "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
        {std::string(64, 'a'), "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
    };

    std::vector<Sha256Engine> engines = {Sha256Engine::automatic, Sha256Engine::portable};
    const bool cpu = quidra::platform::sha256_cpu_engine_available();
    if (cpu) {
        engines.push_back(Sha256Engine::cpu);
    } else {
        bool refused = false;
        try {
            Sha256 hash(Sha256Engine::cpu);
        } catch (const std::invalid_argument&) {
            refused = true;
        }
        expect(refused, "the cpu engine must be refused where the CPU has none");
    }

    for (const auto engine : engines) {
        for (const auto& vector : vectors) {
            for (const std::size_t piece : {std::size_t{0}, std::size_t{1}, std::size_t{3},
                                            std::size_t{63}, std::size_t{64}, std::size_t{65},
                                            std::size_t{1000}}) {
                if (piece == 1 && vector.message.size() > 100000) continue;
                expect(digest(engine, vector.message, piece) == vector.digest,
                       std::string(engine_name(engine)) + " engine, " +
                           std::to_string(vector.message.size()) + " bytes in pieces of " +
                           std::to_string(piece));
            }
        }
    }
    expect(quidra::platform::sha256_hex("abc") == vectors[1].digest, "sha256_hex(\"abc\")");

    if (cpu) {
        std::mt19937_64 random(20261009);
        std::uniform_int_distribution<std::size_t> length(0, 600);
        std::uniform_int_distribution<int> byte(0, 255);
        std::string message;
        for (int i = 0; i < 100000; ++i) {
            message.resize(length(random));
            for (auto& c : message) c = static_cast<char>(byte(random));
            const auto piece = i % 2 == 0 ? std::size_t{0} : std::size_t{1} + message.size() / 3;
            const auto portable = digest(Sha256Engine::portable, message, 0);
            if (digest(Sha256Engine::cpu, message, piece) != portable) {
                expect(false, "cpu and portable engines disagree on random message " +
                                  std::to_string(i) + " (" + std::to_string(message.size()) +
                                  " bytes)");
                break;
            }
        }
    }

    if (failures != 0) {
        std::cerr << failures << " platform SHA-256 test(s) failed\n";
        return 1;
    }
    std::cout << "platform SHA-256: ok (cpu engine " << (cpu ? "checked" : "unavailable")
              << ")\n";
    return 0;
}
