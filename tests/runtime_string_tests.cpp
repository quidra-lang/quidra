#include <cstring>

extern "C" char* quidra_string_index(const char*, long long, unsigned long long, unsigned long long);
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

    char* non_ascii = quidra_string_index("\xC3\xA9", 0, 1, 1);
    if (std::strcmp(non_ascii, "\xC3\xA9") != 0) return 1;
    quidra_managed_release(non_ascii, nullptr);
    return 0;
}
