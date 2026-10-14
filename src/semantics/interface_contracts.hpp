#pragma once
#include "semantics/interface_fingerprint.hpp"
#include "quidra/ast.hpp"

#include <functional>
#include <memory>

namespace quidra::semantics {
enum class InterfaceBindingKind : std::uint8_t { Class, Enum, Constant };
struct InterfaceSourceBinding {
    InterfaceBindingKind kind;
    std::string original_name, qualified_name;
};
struct InterfaceImportSource {
    std::string alias;
    bool is_public{};
    std::optional<ModuleSelector> destination;
};
struct OriginalModuleInterfaces {
    ModuleSelector module;
    InterfaceContracts contracts;
    CanonicalInterfacePayload original_tokens;
    std::vector<InterfaceSourceBinding> bindings;
    std::vector<InterfaceImportSource> imports;
};
struct InterfaceSources { std::vector<OriginalModuleInterfaces> modules; };
using CanonicalSourceType = std::function<std::string(const TypeName&)>;

// Called after original DeclarationIdentity assignment, before qualification,
// constant rewriting and generic expansion. This owns the full original data.
OriginalModuleInterfaces capture_original_interfaces(const Program&, const ModuleSelector&,
    std::string_view source, const CanonicalSourceType&);
// Lookup names are supplied at the existing qualification site, never decoded
// from an IR name. They are not included in any fingerprint.
void bind_interface_name(OriginalModuleInterfaces&, InterfaceBindingKind,
                         std::string_view original, std::string qualified);
void bind_interface_import(OriginalModuleInterfaces&, std::string_view alias,
                           ModuleSelector destination);
void finalize_original_interfaces(InterfaceSources&);

// Post-check/lowering enrichment reads exact original declarations and owned
// S1/effect/borrow/L13 records. Source data is copied, never mutated in place.
std::shared_ptr<const InterfaceSources> enrich_interface_contracts(
    const InterfaceSources&, const CheckedProgram&, const ir::Module&);
} // namespace quidra::semantics
