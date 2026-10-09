#pragma once

// What the native toolchain prints about what it does, parsed. Each parser
// takes the text a tool printed and changes nothing; toolchain_identity
// runs the tools.
//
//   driver -###           the configuration files the driver read and the
//                         commands (jobs) it would run, each as its
//                         argument list
//   driver -E -v          the include search lists: quoted includes, then
//                         angle includes (framework directories in their
//                         place in that order)
//   ld64 -v               the library and framework search lists
//   -MD -MF depfile       the files a compile read
//   ld64 -t, --trace      the files a link opened
//
// The shadowing functions list the paths where a file that does not exist
// now would, once created, be found before the file a search found. A
// header found in search directory j under the relative name r would be
// shadowed by r in any earlier directory; a library found for -lNAME in
// directory j by any candidate file name of NAME in an earlier directory,
// or by an earlier candidate name in directory j itself; a framework by a
// framework directory of the same name in an earlier framework directory.
// The include directive's spelling is not in a depfile, so every way of
// splitting the header's path into a search directory and a relative name
// counts.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace quidra::toolchain {

struct DriverPlan {
    std::vector<std::filesystem::path> configuration_files;
    std::vector<std::vector<std::string>> jobs;
};

DriverPlan parse_driver_plan(std::string_view text);

// The value of `option` in a job: the argument after it ("-triple X"), or
// the text after "=" when `option` ends with "=" ("-target-sdk-version=X").
// The first occurrence; nullopt when there is none.
std::optional<std::string> job_option(
    const std::vector<std::string>& job, std::string_view option);
// Every value of `option`, in order.
std::vector<std::string> job_options(
    const std::vector<std::string>& job, std::string_view option);

struct IncludeSearch {
    std::vector<std::filesystem::path> quote;
    std::vector<std::filesystem::path> angle;
};

// nullopt when the text has no complete search list.
std::optional<IncludeSearch> parse_include_search(std::string_view text);

struct LibrarySearch {
    std::vector<std::filesystem::path> library;
    std::vector<std::filesystem::path> framework;
};

// nullopt when the text has no "Library search paths:" list.
std::optional<LibrarySearch> parse_linker_search(std::string_view text);

// The prerequisites of every rule, in order, without repeats. Relative
// paths stay relative.
std::vector<std::filesystem::path> parse_depfile(std::string_view text);

// The files a link opened, in order, without repeats: an archive member
// counts as its archive, and a linker's own status lines are skipped.
std::vector<std::filesystem::path> parse_link_trace(std::string_view text);

// The library file names a link tries for -lNAME, in the order it tries
// them in one directory: .tbd, .dylib, .so, .a on Apple platforms, .so and
// .a elsewhere.
std::vector<std::string> library_file_names(std::string_view name, bool apple);

std::vector<std::filesystem::path> header_shadowing_paths(
    const std::vector<std::filesystem::path>& search, const std::filesystem::path& found);
std::vector<std::filesystem::path> library_shadowing_paths(
    const std::vector<std::filesystem::path>& search, const std::filesystem::path& found,
    bool apple);
std::vector<std::filesystem::path> framework_shadowing_paths(
    const std::vector<std::filesystem::path>& search, const std::filesystem::path& found);

} // namespace quidra::toolchain
