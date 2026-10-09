#include "platform/file_descriptor.hpp"

#ifndef _WIN32

#include <cerrno>
#include <unistd.h>

namespace quidra::platform {

bool write_all(int fd, std::string_view data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto written =
            ::write(fd, data.data() + offset, data.size() - offset);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (written == 0) return false;
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

bool read_exact(int fd, char* data, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
        const auto count = ::read(fd, data + offset, size - offset);
        if (count < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (count == 0) return false;
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

bool read_line(int fd, std::string& line, std::size_t limit) {
    line.clear();
    char c = 0;
    while (true) {
        const auto count = ::read(fd, &c, 1);
        if (count < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (count == 0) return false;
        if (c == '\n') return true;
        line.push_back(c);
        if (line.size() > limit) return false;
    }
}

} // namespace quidra::platform

#endif
