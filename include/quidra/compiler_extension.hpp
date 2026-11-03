#pragma once

#include <map>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace quidra {

struct CompilerExtensionModel;
class CompileInputs;

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
    // Legacy tensor-region descriptors already use wire version 2. Revision
    // 1 preserves that representation; revision 2 is the full tensor-program
    // schema. Wire version remains in the immutable descriptor/tables.
    std::uint32_t schema_revision{1};
    std::map<std::string, std::string> files{}; // Package-relative path -> snapshot.
    std::shared_ptr<const CompilerExtensionModel> model{};
};

// The descriptor snapshot has already been read by the frontend. Only full
// schema referenced files are read here through the CompileInputs boundary.
CompilerExtensionRegistration parse_compiler_extension_descriptor(
    std::string package, std::string name, const std::filesystem::path& package_root,
    const std::filesystem::path& descriptor_path, std::string descriptor,
    CompileInputs* inputs = nullptr);

} // namespace quidra
