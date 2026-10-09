#pragma once

// The version of the operating system a native executable is built on, as
// text that changes whenever the system's libraries and headers may have:
//   macOS    "<product version> (<build>)", from kern.osproductversion and
//            kern.osversion, e.g. "15.4 (24E248)";
//   Linux    "<kernel release> <C library>", from uname(2) and
//            gnu_get_libc_version(), e.g. "6.8.0-45-generic glibc 2.39";
//            "musl" stands for a C library that is not glibc;
//   Windows  "<major>.<minor>.<build>", from RtlGetVersion;
//   others   "<system name> <release>" from uname(2).
// Empty when the system does not tell.

#include <string>

namespace quidra::platform {

std::string os_version();

} // namespace quidra::platform
