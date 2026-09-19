#include <cstring>

extern "C" char* quidra_string_index(const char*, long long, unsigned long long, unsigned long long);
extern "C" bool quidra_string_index_equal_ascii(const char*, long long, unsigned char, unsigned long long, unsigned long long);
extern "C" char* quidra_runtime_try_copy_text_bytes(const char*, unsigned long long);
extern "C" void* quidra_string_split(const char*, const char*);
extern "C" void quidra_managed_retain(void*);
extern "C" void quidra_managed_release(void*, void*);

int main() {
    char* first = quidra_string_index("abc", 1, 1, 1);
    char* second = quidra_string_index("xbx", 1, 1, 1);
    if (std::strcmp(first, "b") != 0 || std::strcmp(second, "b") != 0) return 1;

    // ASCII index values are immutable runtime singletons. Pointer reuse is an
    // internal performance contract: release remains a no-op for unmanaged
    // static values, just as it is for compiler string literals.
    if (first != second) return 1;
    quidra_managed_release(first, nullptr);
    quidra_managed_release(second, nullptr);

    char* space = quidra_string_index("x x", 1, 1, 1);
    if (std::strcmp(space, " ") != 0) return 1;
    quidra_managed_release(space, nullptr);

    char* managed_ascii = quidra_runtime_try_copy_text_bytes("alpha beta", 10);
    if (!managed_ascii) return 1;
    if (!quidra_string_index_equal_ascii(managed_ascii, 5, ' ', 1, 1)) return 1;
    if (quidra_string_index_equal_ascii(managed_ascii, 5, 'x', 1, 1)) return 1;
    char* managed_space = quidra_string_index(managed_ascii, 5, 1, 1);
    if (std::strcmp(managed_space, " ") != 0) return 1;
    quidra_managed_release(managed_space, nullptr);
    quidra_managed_release(managed_ascii, nullptr);

    char* managed_unicode = quidra_runtime_try_copy_text_bytes("\xC3\xA9x", 3);
    if (!managed_unicode) return 1;
    if (!quidra_string_index_equal_ascii(managed_unicode, 1, 'x', 1, 1)) return 1;
    if (quidra_string_index_equal_ascii(managed_unicode, 0, 'x', 1, 1)) return 1;
    char* managed_x = quidra_string_index(managed_unicode, 1, 1, 1);
    if (std::strcmp(managed_x, "x") != 0) return 1;
    quidra_managed_release(managed_x, nullptr);
    quidra_managed_release(managed_unicode, nullptr);

    char* non_ascii = quidra_string_index("\xC3\xA9", 0, 1, 1);
    if (std::strcmp(non_ascii, "\xC3\xA9") != 0) return 1;
    quidra_managed_release(non_ascii, nullptr);

    // split() pieces share one backing allocation, but each element keeps normal
    // value lifetime semantics. Retaining one piece must keep it alive after all
    // sibling element owners and the result array itself are released.
    void* split_raw = quidra_string_split("alpha beta gamma", " ");
    long long split_count = 0;
    std::memcpy(&split_count, split_raw, sizeof(split_count));
    if (split_count != 3) return 1;
    char* pieces[3]{};
    for (int i = 0; i < 3; ++i) {
        std::memcpy(&pieces[i],
                    static_cast<unsigned char*>(split_raw) + 8 + i * sizeof(char*),
                    sizeof(char*));
    }
    if (std::strcmp(pieces[0], "alpha") != 0 ||
        std::strcmp(pieces[1], "beta") != 0 ||
        std::strcmp(pieces[2], "gamma") != 0) return 1;

    quidra_managed_retain(pieces[1]);
    for (char* piece : pieces) quidra_managed_release(piece, nullptr);
    quidra_managed_release(split_raw, nullptr);
    if (std::strcmp(pieces[1], "beta") != 0) return 1;
    quidra_managed_release(pieces[1], nullptr);
    return 0;
}
