#include "platform/os_version.hpp"

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#else
#include <sys/utsname.h>
#if defined(__linux__) && defined(__GLIBC__)
#include <gnu/libc-version.h>
#endif
#endif

#include <cstddef>
#include <string>
#include <vector>

namespace quidra::platform {

#ifdef _WIN32

std::string os_version() {
    // RtlGetVersion reports the real version; GetVersionEx is subject to the
    // compatibility shims of the application manifest.
    using RtlGetVersionFunction = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return {};
    const auto get_version = reinterpret_cast<RtlGetVersionFunction>(
        reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
    if (!get_version) return {};
    RTL_OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (get_version(&info) != 0) return {};
    return std::to_string(info.dwMajorVersion) + "." +
           std::to_string(info.dwMinorVersion) + "." +
           std::to_string(info.dwBuildNumber);
}

#elif defined(__APPLE__)

namespace {

std::string sysctl_text(const char* name) {
    std::size_t size = 0;
    if (::sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0) return {};
    std::vector<char> buffer(size);
    if (::sysctlbyname(name, buffer.data(), &size, nullptr, 0) != 0) return {};
    std::string text(buffer.data(), size);
    while (!text.empty() && text.back() == '\0') text.pop_back();
    return text;
}

} // namespace

std::string os_version() {
    const auto product = sysctl_text("kern.osproductversion");
    const auto build = sysctl_text("kern.osversion");
    if (product.empty() && build.empty()) return {};
    return product + " (" + build + ")";
}

#else

std::string os_version() {
    struct utsname name {};
    if (::uname(&name) != 0) return {};
#if defined(__linux__)
#if defined(__GLIBC__)
    return std::string(name.release) + " glibc " + gnu_get_libc_version();
#else
    return std::string(name.release) + " musl";
#endif
#else
    return std::string(name.sysname) + " " + name.release;
#endif
}

#endif

} // namespace quidra::platform
