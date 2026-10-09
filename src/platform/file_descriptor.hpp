#pragma once

// Reading and writing a POSIX file descriptor in full. Each function retries
// a call interrupted by a signal (EINTR) and stops at end of file or on any
// other error. POSIX only: nothing here is defined on Windows.

#include <cstddef>
#include <string>
#include <string_view>

namespace quidra::platform {

// Writes all of `data`; false on an error or when a write accepts nothing.
bool write_all(int fd, std::string_view data);
// Reads exactly `size` bytes into `data`; false on an error or at end of file.
bool read_exact(int fd, char* data, std::size_t size);
// Reads one line into `line`, without its "\n"; false on an error, at end of
// file, or once the line holds more than `limit` bytes.
bool read_line(int fd, std::string& line, std::size_t limit);

} // namespace quidra::platform
