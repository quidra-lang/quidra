#include "lowering/user_sources.hpp"

#include "platform/sha256.hpp"

#include <algorithm>
#include <filesystem>

namespace quidra::lowering {

std::vector<ir::UserSource> user_source_table(const Program& program) {
    namespace fs = std::filesystem;
    std::vector<ir::UserSource> table;
    if (program.user_sources.empty()) return table;
    const auto& display_root = program.source_display_path.empty()
                                   ? program.user_sources.front()
                                   : program.source_display_path;
    const bool file_backed = !(display_root.size() >= 2 && display_root.front() == '<' &&
                               display_root.back() == '>');
    const auto root_directory = fs::path(program.user_sources.front()).parent_path();
    const auto display_directory = fs::path(display_root).parent_path();
    for (std::size_t i = 0; i < program.user_sources.size(); ++i) {
        const auto& absolute = program.user_sources[i];
        ir::UserSource source;
        source.absolute_path = absolute;
        source.display_path =
            i == 0 ? display_root
                   : (display_directory / fs::path(absolute).lexically_relative(root_directory))
                         .lexically_normal()
                         .string();
        if (const auto text = program.source_texts.find(absolute);
            text != program.source_texts.end()) {
            if (file_backed) source.revision = platform::sha256_hex(text->second);
            source.line_count =
                static_cast<std::uint64_t>(std::count(text->second.begin(), text->second.end(), '\n')) + 1;
            source.byte_size = text->second.size();
        }
        table.push_back(std::move(source));
    }
    return table;
}

} // namespace quidra::lowering
