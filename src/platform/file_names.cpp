#include "platform/file_names.hpp"

namespace quidra::platform {

std::string executable_file_name(std::string_view stem) {
#ifdef _WIN32
    return std::string(stem) + ".exe";
#else
    return std::string(stem);
#endif
}

std::string object_file_name(std::string_view stem) {
#ifdef _WIN32
    return std::string(stem) + ".obj";
#else
    return std::string(stem) + ".o";
#endif
}

std::string static_library_file_name(std::string_view stem) {
#ifdef _WIN32
    return std::string(stem) + ".lib";
#else
    return "lib" + std::string(stem) + ".a";
#endif
}

std::string shared_library_file_name(std::string_view name) {
#ifdef _WIN32
    return std::string(name) + ".dll";
#elif defined(__APPLE__)
    return "lib" + std::string(name) + ".dylib";
#else
    return "lib" + std::string(name) + ".so";
#endif
}

} // namespace quidra::platform
