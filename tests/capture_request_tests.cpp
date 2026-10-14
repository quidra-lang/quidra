#include <cstdio>
#if __has_include("runtime/capture_request.hpp")
#include "runtime/capture_request.hpp"
#include <limits>
int main() {
    using quidra::profile::parse_capture_step;
    int failures{};
    const auto check = [&](bool value, const char* message) {
        if (!value) { std::fprintf(stderr, "%s\n", message); ++failures; }
    };
    check(parse_capture_step("step:0") == 0, "step zero is valid");
    check(parse_capture_step("step:0012") == 12, "decimal leading zeros are valid");
    check(parse_capture_step("step:18446744073709551615") ==
        std::numeric_limits<std::uint64_t>::max(), "full uint64 request preserved");
    for (const char* invalid : {"", "step:", "step:-1", "step:+1", "step: 1",
            "step:1 ", "step:1x", "STEP:1", "1", "step:18446744073709551616"})
        check(!parse_capture_step(invalid), "invalid capture request accepted");
    const std::string nul{"step:1\0", 7};
    check(!parse_capture_step(nul), "embedded NUL accepted");
    const quidra::platform::RuntimeEnvironment base{{"QUIDRA_PROFILE", "trace.json"},
        {"MTL_CAPTURE_ENABLED", "0"}};
    using quidra::profile::capture_child_environment;
    check(capture_child_environment(base, "step:0", false) == base, "non-Apple child changed");
    check(capture_child_environment(base, "step:-1", true) == base, "invalid request changed child");
    check(capture_child_environment(base, "", true) == base, "disabled request changed child");
    const auto enabled = capture_child_environment(base, "step:0", true);
    check(enabled.size() == base.size() + 1 && enabled.back().first == "MTL_CAPTURE_ENABLED" &&
        enabled.back().second == "1", "valid Apple request lacks final child-only override");
    check(base.back().second == "0", "parent override list changed");
    return failures ? 1 : 0;
}
#else
int main() { std::fputs("capture request and child-only environment policy missing\n", stderr); return 1; }
#endif
