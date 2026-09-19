#include <cstdint>
#include <cstring>

extern "C" char* quidra_string_index(const char*, long long, unsigned long long, unsigned long long);
extern "C" bool quidra_string_index_equal_ascii(const char*, long long, unsigned char, unsigned long long, unsigned long long);
extern "C" long long quidra_string_count_ascii_prefix(
    const char*, long long, unsigned char, bool, long long,
    unsigned long long, unsigned long long,
    unsigned long long, unsigned long long);
extern "C" char* quidra_runtime_try_copy_text_bytes(const char*, unsigned long long);
extern "C" void* quidra_string_split(const char*, const char*);
extern "C" void* quidra_string_split_iter_begin(const char*, const char*);
extern "C" void* quidra_string_split_iter_begin_move(const char*, const char*);
extern "C" char* quidra_string_split_iter_next(void*);
extern "C" void quidra_string_split_iter_end(void*);
extern "C" unsigned long long quidra_runtime_text_byte_length(const char*);
extern "C" long long quidra_string_length(const char*);
extern "C" bool quidra_string_parse_two_signed(
    const char*, unsigned char, long long*, long long*);
extern "C" char* quidra_string_build_append_move_unique_direct(
    char*, const unsigned char*, const unsigned long long*,
    unsigned long long, const char*, long long*);
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
    if (quidra_string_count_ascii_prefix(
            managed_ascii, 10, 'a', false, 4, 1, 1, 1, 1) != 7)
        return 1;
    if (quidra_string_count_ascii_prefix(
            managed_ascii, 10, ' ', true, 0, 1, 1, 1, 1) != 9)
        return 1;
    if (quidra_string_count_ascii_prefix(
            managed_ascii, 0, 'x', false, 5, 1, 1, 1, 1) != 5)
        return 1;
    quidra_managed_release(managed_space, nullptr);
    quidra_managed_release(managed_ascii, nullptr);

    const char ascii_blocks[] =
        "0123456789abcdef0123456789abcdef0123456789abcdef";
    char* managed_blocks = quidra_runtime_try_copy_text_bytes(
        ascii_blocks, sizeof(ascii_blocks) - 1);
    if (!managed_blocks ||
        quidra_runtime_text_byte_length(managed_blocks) !=
            sizeof(ascii_blocks) - 1 ||
        quidra_string_length(managed_blocks) !=
            static_cast<long long>(sizeof(ascii_blocks) - 1))
        return 1;
    quidra_managed_release(managed_blocks, nullptr);

    char* managed_unicode = quidra_runtime_try_copy_text_bytes("\xC3\xA9x", 3);
    if (!managed_unicode) return 1;
    if (!quidra_string_index_equal_ascii(managed_unicode, 1, 'x', 1, 1)) return 1;
    if (quidra_string_index_equal_ascii(managed_unicode, 0, 'x', 1, 1)) return 1;
    char* managed_x = quidra_string_index(managed_unicode, 1, 1, 1);
    if (std::strcmp(managed_x, "x") != 0) return 1;
    if (quidra_string_count_ascii_prefix(
            managed_unicode, 2, 'x', false, 0, 1, 1, 1, 1) != 1)
        return 1;
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

    void* split_iter = quidra_string_split_iter_begin("a::b::", "::");
    char* iter_a = quidra_string_split_iter_next(split_iter);
    // Keep the iterator's backing-allocation identity valid even if unrelated
    // managed allocations grow/rehash the allocation table between slices.
    char* split_cache_churn[256]{};
    for (auto& value : split_cache_churn) {
        value = quidra_runtime_try_copy_text_bytes("cache", 5);
        if (!value) return 1;
    }
    char* iter_b = quidra_string_split_iter_next(split_iter);
    for (char* value : split_cache_churn)
        quidra_managed_release(value, nullptr);
    char* iter_empty = quidra_string_split_iter_next(split_iter);
    if (!iter_a || !iter_b || !iter_empty ||
        std::strcmp(iter_a, "a") != 0 ||
        std::strcmp(iter_b, "b") != 0 ||
        std::strcmp(iter_empty, "") != 0 ||
        quidra_runtime_text_byte_length(iter_a) != 1 ||
        quidra_runtime_text_byte_length(iter_b) != 1 ||
        quidra_runtime_text_byte_length(iter_empty) != 0 ||
        quidra_string_split_iter_next(split_iter) != nullptr) return 1;
    quidra_managed_retain(iter_b);
    quidra_string_split_iter_end(split_iter);
    if (std::strcmp(iter_b, "b") != 0) return 1;
    quidra_managed_release(iter_b, nullptr);

    // A compiler-proven dead, uniquely owned split source can become the
    // iterator slab directly. The iterator keeps its own owner while pieces are borrowed.
    char* movable = quidra_runtime_try_copy_text_bytes("left right", 10);
    if (!movable) return 1;
    void* move_iter = quidra_string_split_iter_begin_move(movable, " ");
    char* move_left = quidra_string_split_iter_next(move_iter);
    if (move_left != movable || std::strcmp(move_left, "left") != 0 ||
        quidra_string_length(move_left) != 4) return 1;
    quidra_managed_release(movable, nullptr);
    char* move_right = quidra_string_split_iter_next(move_iter);
    if (!move_right || std::strcmp(move_right, "right") != 0 ||
        quidra_string_length(move_right) != 5 ||
        quidra_string_split_iter_next(move_iter) != nullptr) return 1;
    quidra_managed_retain(move_right);
    quidra_string_split_iter_end(move_iter);
    if (std::strcmp(move_right, "right") != 0) return 1;
    quidra_managed_release(move_right, nullptr);

    // An observable alias forces the old private-slab path and leaves source text intact.
    char* aliased = quidra_runtime_try_copy_text_bytes("left right", 10);
    if (!aliased) return 1;
    quidra_managed_retain(aliased);
    void* alias_iter = quidra_string_split_iter_begin_move(aliased, " ");
    char* alias_left = quidra_string_split_iter_next(alias_iter);
    if (!alias_left || alias_left == aliased ||
        std::strcmp(alias_left, "left") != 0 ||
        std::strcmp(aliased, "left right") != 0) return 1;
    quidra_string_split_iter_end(alias_iter);
    quidra_managed_release(aliased, nullptr);
    quidra_managed_release(aliased, nullptr);

    // Slice byte-length caching must never leak code-point metadata between
    // different slices that share one backing slab.
    void* unicode_iter =
        quidra_string_split_iter_begin("\xC3\xA9::abc::\xE3\x81\x82\xE3\x81\x84", "::");
    char* unicode_one = quidra_string_split_iter_next(unicode_iter);
    char* unicode_ascii = quidra_string_split_iter_next(unicode_iter);
    char* unicode_two = quidra_string_split_iter_next(unicode_iter);
    if (!unicode_one || !unicode_ascii || !unicode_two ||
        quidra_string_length(unicode_one) != 1 ||
        quidra_string_length(unicode_ascii) != 3 ||
        quidra_string_length(unicode_two) != 2 ||
        quidra_runtime_text_byte_length(unicode_one) != 2 ||
        quidra_runtime_text_byte_length(unicode_ascii) != 3 ||
        quidra_runtime_text_byte_length(unicode_two) != 6 ||
        quidra_string_split_iter_next(unicode_iter) != nullptr) return 1;
    quidra_string_split_iter_end(unicode_iter);

    void* empty_iter = quidra_string_split_iter_begin("", ",");
    char* only_empty = quidra_string_split_iter_next(empty_iter);
    if (!only_empty || std::strcmp(only_empty, "") != 0 ||
        quidra_string_split_iter_next(empty_iter) != nullptr) return 1;
    quidra_string_split_iter_end(empty_iter);

    long long parsed_left = -1;
    long long parsed_right = -1;
    if (!quidra_string_parse_two_signed(
            "12 -34", ' ', &parsed_left, &parsed_right) ||
        parsed_left != 12 || parsed_right != -34) return 1;
    if (!quidra_string_parse_two_signed(
            "9223372036854775807 -9223372036854775808 tail", ' ',
            &parsed_left, &parsed_right) ||
        parsed_left != 9223372036854775807LL ||
        parsed_right != (-9223372036854775807LL - 1)) return 1;
    parsed_left = 7;
    parsed_right = 9;
    if (quidra_string_parse_two_signed(
            "12 nope", ' ', &parsed_left, &parsed_right) ||
        parsed_left != 0 || parsed_right != 0) return 1;
    parsed_left = 7;
    parsed_right = 9;
    if (quidra_string_parse_two_signed(
            "9223372036854775808 1", ' ', &parsed_left, &parsed_right) ||
        parsed_left != 0 || parsed_right != 0) return 1;
    parsed_left = 7;
    parsed_right = 9;
    if (quidra_string_parse_two_signed(
            "+12 1", ' ', &parsed_left, &parsed_right) ||
        parsed_left != 0 || parsed_right != 0) return 1;
    parsed_left = 7;
    parsed_right = 9;
    if (quidra_string_parse_two_signed(
            "12  34", ' ', &parsed_left, &parsed_right) ||
        parsed_left != 0 || parsed_right != 0) return 1;
    parsed_left = 7;
    parsed_right = 9;
    if (quidra_string_parse_two_signed(
            "-9223372036854775809 1", ' ', &parsed_left, &parsed_right) ||
        parsed_left != 0 || parsed_right != 0) return 1;

    // The compiler's typed string append fast path returns its appended
    // code-point count through caller-owned storage and can trust proven
    // one-byte ASCII literal parts without managed-string metadata lookups.
    char* build_target = quidra_runtime_try_copy_text_bytes("", 0);
    if (!build_target) return 1;
    const unsigned char build_kinds[4] = {1, 4, 1, 4};
    const char build_space[] = " ";
    const char build_enter[] = "\n";
    const unsigned long long build_values[4] = {
        12ULL,
        static_cast<unsigned long long>(
            reinterpret_cast<std::uintptr_t>(build_space)),
        34ULL,
        static_cast<unsigned long long>(
            reinterpret_cast<std::uintptr_t>(build_enter)),
    };
    long long build_added = -1;
    char* built = quidra_string_build_append_move_unique_direct(
        build_target, build_kinds, build_values, 4, "", &build_added);
    if (!built || std::strcmp(built, "12 34\n") != 0 ||
        build_added != 6) return 1;
    quidra_managed_release(built, nullptr);
    return 0;
}
