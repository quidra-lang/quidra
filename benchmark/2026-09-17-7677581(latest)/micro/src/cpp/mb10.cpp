// MB-10 -- file I/O: buffered text write, read-back, integer formatting and parsing.
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::int64_t kModulus = 2147483647;
constexpr std::int64_t kMultiplier = 48271;

// Lehmer / MINSTD generator: s = (48271 * s) mod (2^31 - 1).
class Lcg {
public:
    explicit Lcg(std::int64_t seed) : state_(seed) {}

    std::int64_t next_int() {
        state_ = (kMultiplier * state_) % kModulus;
        return state_;
    }

private:
    std::int64_t state_;
};

constexpr std::int64_t kN = 1000000;
constexpr int kRounds = 3;
constexpr std::int64_t kSeed = 20270917;

// Section 4.12(c): one frozen 65536-byte stream buffer for every configuration.
constexpr std::streamsize kBufSize = 65536;

std::string run() {
    Lcg g(kSeed);  // the stream continues across rounds
    std::int64_t sum_v = 0;
    std::int64_t chk = 0;
    std::int64_t nbytes = 0;
    std::int64_t lines = 0;

    for (int r = 0; r < kRounds; ++r) {
        const std::string name = "mb10_round_" + std::to_string(r) + ".txt";

        std::vector<char> wbuf(static_cast<std::size_t>(kBufSize));
        std::ofstream out;
        out.rdbuf()->pubsetbuf(wbuf.data(), kBufSize);  // must precede open()
        out.open(name);
        if (!out) {
            std::cerr << "MB10: cannot open " << name << " for writing\n";
            std::exit(1);
        }
        for (std::int64_t i = 0; i < kN; ++i) {
            const std::int64_t v = g.next_int();
            const std::string line = std::to_string(i) + " " + std::to_string(v) + "\n";
            out << line;
            nbytes += static_cast<std::int64_t>(line.size());
        }
        out.flush();
        out.close();

        std::vector<char> rbuf(static_cast<std::size_t>(kBufSize));
        std::ifstream in;
        in.rdbuf()->pubsetbuf(rbuf.data(), kBufSize);  // must precede open()
        in.open(name);
        if (!in) {
            std::cerr << "MB10: cannot open " << name << " for reading\n";
            std::exit(1);
        }
        std::int64_t idx = 0;
        std::string line;
        while (std::getline(in, line)) {
            const std::string_view lv(line);
            const std::size_t space = lv.find(' ');
            std::int64_t a = 0;
            std::int64_t v = 0;
            std::from_chars(lv.data(), lv.data() + space, a);
            std::from_chars(lv.data() + space + 1, lv.data() + lv.size(), v);
            if (a != idx) {
                std::cerr << "MB10: index mismatch at line " << idx << "\n";
                std::exit(1);
            }
            ++idx;
            ++lines;
            sum_v = (sum_v + v) % 1000000007;
            chk = (chk * 31 + v % 1000003) % 1000003;
        }
        in.close();
    }

    return "MB10 " + std::to_string(sum_v) + " " + std::to_string(chk) + " " +
           std::to_string(nbytes) + " " + std::to_string(lines);
}

}  // namespace

int main(int argc, char** argv) {
    // Section 5.2 program modes: argv[1] in {once, steady}, argv[2] = K (default 7).
    const std::string mode = argc > 1 ? argv[1] : "once";
    std::string line;
    if (mode == "steady") {
        int iterations = argc > 2 ? std::atoi(argv[2]) : 7;
        if (iterations < 1) {
            iterations = 7;
        }
        for (int k = 0; k < iterations; ++k) {
            const auto t0 = std::chrono::steady_clock::now();
            line = run();
            const auto t1 = std::chrono::steady_clock::now();
            std::cout << "ITER " << k << " "
                      << std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()
                      << "\n"
                      << std::flush;  // section 5.2: each ITER line is flushed immediately
        }
    } else {
        line = run();
    }
    std::cout << line << std::endl;
    return 0;
}
