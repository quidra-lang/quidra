#pragma once

#include <map>
#include <string>

namespace quidra {

// Domain-neutral registration captured from an imported package's project.toml.
// descriptor contains the immutable file snapshot read during frontend loading;
// later compiler stages never need to re-read package files.
struct CompilerExtensionRegistration {
    std::string package;
    std::string name;
    std::string package_root;
    std::string descriptor_path;
    std::string descriptor;
    // Generic compiler phase. Domain meaning remains inside descriptor data.
    std::string phase;
    // Parsed immutable descriptor tables. Core preserves generic key/value data
    // for later extension-aware compiler passes without interpreting domain
    // semantics or re-reading package files.
    std::map<std::string, std::map<std::string, std::string>> tables;
};

} // namespace quidra
