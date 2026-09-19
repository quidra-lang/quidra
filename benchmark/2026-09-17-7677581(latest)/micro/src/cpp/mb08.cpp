// MB-08 -- strings: text construction plus five character-level passes per round.
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
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

constexpr int kWordCount = 200000;
constexpr int kRounds = 20;
constexpr std::int64_t kQ = 1000000007;
constexpr std::int64_t kSeed = 20268917;

// Rolling hash, order-sensitive. Operates on the section 4.12(b) working buffer
// for cpp: std::vector<unsigned char>.
std::int64_t rhash(const std::vector<unsigned char>& seq) {
    std::int64_t h = 0;
    for (const unsigned char ch : seq) {
        h = (h * 131 + ch) % kQ;
    }
    return h;
}

std::string run() {
    Lcg g(kSeed);

    std::vector<std::string> words;
    for (int w = 0; w < kWordCount; ++w) {
        const std::int64_t len = 4 + g.next_int() % 13;  // length 4..16
        std::string word;
        for (std::int64_t i = 0; i < len; ++i) {
            word.push_back(static_cast<char>('a' + g.next_int() % 26));
        }
        words.push_back(std::move(word));
    }

    // Build phase: the ordinary string join, then the one string -> working-buffer
    // conversion, both outside the timed round loop (section 4.12(b)).
    std::string joined;
    for (std::size_t w = 0; w < words.size(); ++w) {
        if (w > 0) {
            joined.push_back(' ');
        }
        joined += words[w];
    }
    std::vector<unsigned char> text(joined.begin(), joined.end());
    const std::size_t len = text.size();

    std::int64_t acc = 0;
    std::int64_t cnt_ab = 0;
    std::int64_t cnt_w = 0;
    for (int r = 0; r < kRounds; ++r) {
        const std::size_t p = static_cast<std::size_t>(7 * r + 11);  // anti-elimination
        if (text[p] == ' ') {
            text[p] = 'x';
        } else {
            text[p] = static_cast<unsigned char>('a' + (text[p] - 'a' + 1) % 26);
        }

        const std::int64_t h1 = rhash(text);  // pass 1

        std::vector<unsigned char> upper(len);  // pass 2: upper-case
        for (std::size_t i = 0; i < len; ++i) {
            const unsigned char c = text[i];
            upper[i] = static_cast<unsigned char>((c >= 97 && c <= 122) ? c - 32 : c);
        }
        const std::int64_t h2 = rhash(upper);

        std::vector<unsigned char> reversed(len);  // pass 3: reverse
        for (std::size_t i = 0; i < len; ++i) {
            reversed[i] = text[len - 1 - i];
        }
        const std::int64_t h3 = rhash(reversed);

        cnt_ab = 0;  // pass 4: naive search
        for (std::size_t i = 0; i + 1 < len; ++i) {
            if (text[i] == 'a' && text[i + 1] == 'b') {
                ++cnt_ab;
            }
        }

        // pass 5: word count on the native string type, constructed fresh from the
        // working buffer inside the timed round and indexed with s[i] (section 4.12(b)).
        const std::string native(reinterpret_cast<const char*>(text.data()), len);
        cnt_w = 1;
        for (std::size_t i = 0; i < len; ++i) {
            if (native[i] == ' ') {
                ++cnt_w;
            }
        }

        for (const std::int64_t v : {h1, h2, h3, cnt_ab, cnt_w}) {
            acc = (acc * 31 + v) % kQ;
        }
    }

    return "MB08 " + std::to_string(acc) + " " + std::to_string(len) + " " +
           std::to_string(cnt_ab) + " " + std::to_string(cnt_w);
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
