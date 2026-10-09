#include "platform/environment.hpp"

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

// platform::environment is the only code in Core that reads the process
// environment (docs/development.md, "Environment variables"). These tests pin
// its contract on every host, the _dupenv_s branch on Windows included: an
// unset variable has no value and is not set, a set variable reads back byte
// for byte (long values and non-ASCII bytes too), every read sees the current
// environment, and a variable set to the empty string has an empty value and
// is set.

namespace {

using quidra::platform::environment_has;
using quidra::platform::environment_value;

constexpr const char* name = "QUIDRA_TEST_PLATFORM_ENVIRONMENT";

int failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "platform environment test failed: %s\n", what);
        ++failures;
    }
}

void expect_value(const std::string& expected, const char* what) {
    const std::optional<std::string> value = environment_value(name);
    expect(value.has_value() && *value == expected, what);
    expect(environment_has(name), what);
}

bool set_variable(const char* value) {
#ifdef _WIN32
    return _putenv_s(name, value) == 0;
#else
    return setenv(name, value, 1) == 0;
#endif
}

void unset_variable() {
#ifdef _WIN32
    // The CRT removes a variable that _putenv_s sets to the empty string.
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

void expect_unset(const char* what) {
    expect(!environment_value(name).has_value(), what);
    expect(!environment_has(name), what);
}

} // namespace

int main() {
    unset_variable();
    expect_unset("an unset variable has no value and is not set");

    expect(set_variable("value"), "setting a variable");
    expect_value("value", "a set variable reads back");

    expect(set_variable("changed"), "changing a variable");
    expect_value("changed", "a read sees the current value");

    // _dupenv_s reports a size that counts the terminating NUL; the value
    // must not include it, nor lose its last byte.
    const std::string long_value = std::string(4096, 'q') + "end";
    expect(set_variable(long_value.c_str()), "setting a long value");
    expect_value(long_value, "a long value reads back whole");

    // Windows may refuse bytes its code page cannot carry into the wide
    // environment; whatever it stores must read back unchanged.
    const char* bytes = "\xc3\xa9t\xc3\xa9 \xff";
    if (set_variable(bytes)) expect_value(bytes, "non-ASCII bytes read back unchanged");

#ifndef _WIN32
    // A running Windows process cannot create an empty variable through the
    // CRT (see unset_variable), so this case is POSIX only.
    expect(set_variable(""), "setting an empty value");
    expect_value("", "a variable set to the empty string is set and empty");
#endif

    unset_variable();
    expect_unset("an unset variable stays unset after it had a value");

    if (failures == 0) std::puts("platform environment: ok");
    return failures == 0 ? 0 : 1;
}
