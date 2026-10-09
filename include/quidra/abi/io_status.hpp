#pragma once

// The status codes the runtime's console and file reads and path queries
// return, which generated code branches on.

namespace quidra::abi {

// quidra_input_read and quidra_file_handle_read_line_raw: a line was read
// (and stored through the output pointer), the input ended, or reading
// failed.
namespace read_line_status {
inline constexpr int failed = -1;
inline constexpr int end_of_input = 0;
inline constexpr int line = 1;
} // namespace read_line_status

// quidra_file_exists_raw and quidra_file_is_directory_raw: the query failed
// (any negative status), or its answer.
namespace path_query_status {
inline constexpr int failed = -1;
inline constexpr int no = 0;
inline constexpr int yes = 1;
} // namespace path_query_status

} // namespace quidra::abi
