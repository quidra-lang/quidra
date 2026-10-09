#include "platform/native_text.hpp"

#ifdef _WIN32
#include <windows.h>

#include <stdexcept>
#endif

namespace quidra::platform {

NativeText native_text(std::string_view value) {
#ifdef _WIN32
    if (value.empty()) return {};
    const int needed = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0);
    if (needed <= 0) throw std::runtime_error("invalid UTF-8 in process argument");
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            wide.data(), needed) != needed) {
        throw std::runtime_error("cannot convert process argument to UTF-16");
    }
    return wide;
#else
    return NativeText(value);
#endif
}

} // namespace quidra::platform
