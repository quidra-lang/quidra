#pragma once

#include "ir/content_hash.hpp"
#include "ir/summaries.hpp"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace quidra::semantics {
struct ModuleSelector {
    std::string package, module_path;
    bool operator==(const ModuleSelector&) const = default;
};
// Owned, versioned full canonical fields from the original AST/token stream,
// not a body digest. The producer removes attribution before supplying this.
struct CanonicalInterfacePayload { std::string format, fields; };
struct SignatureType {
    std::string syntax; // Original canonical abstract type (including shapes).
    std::optional<Type> semantic;
};
struct ForeignInterface { std::string abi, symbol; };
struct StorageInitializationContract {
    std::set<std::string> required, writes, initializes, invalidates;
};
struct InitializationContract {
    StorageInitializationContract receiver;
    std::map<std::string,StorageInitializationContract> references;
    std::set<std::string> return_initialized_fields;
};
struct BorrowParameterContract {
    std::optional<std::size_t> semantic_parameter;
    bool writable{}, borrowed{}, is_const{}, target_escapes{};
};
enum class OriginalSummaryState : std::uint8_t { Missing, Checked, NotCheckedGeneric };
struct FunctionInterface {
    DeclarationIdentity declaration;
    bool is_private{};
    std::optional<ForeignInterface> export_c;
    std::optional<std::string> external_symbol;
    std::vector<std::optional<CanonicalInterfacePayload>> parameter_defaults;
    std::optional<CanonicalInterfacePayload> generic_body;
    // The original generic declaration may have no lowered instance. These
    // facts must be captured explicitly, never inferred from a body digest.
    std::optional<EffectSummary> source_summary;
    std::optional<std::vector<ExtentOrigin>> source_result_extents;
    std::optional<std::vector<BorrowParameterContract>> source_borrow;
    std::optional<InitializationContract> initialization;
    // Checker L13 facts of existing instances can differ by concrete types.
    std::map<std::vector<std::string>,InitializationContract> instance_initialization;
    OriginalSummaryState summary_state{OriginalSummaryState::Missing};
};
struct FieldInterface {
    std::string name;
    SignatureType type;
    bool is_const{}, is_private{}, reference{};
    std::optional<CanonicalInterfacePayload> default_value;
};
struct ClassInterface {
    std::string source_name;
    bool is_private{};
    std::vector<std::string> generic_parameters, generic_constraints;
    std::vector<FieldInterface> fields; // Public and private fields in ABI order.
    std::set<std::size_t> init_mask_fields;
    struct Instance {
        std::vector<FieldInterface> fields;
        std::set<std::size_t> init_mask_fields;
    };
    std::map<std::vector<std::string>,Instance> instances;
};
struct AliasInterface { std::string source_name; bool is_private{}; SignatureType type; };
struct EnumVariantInterface { std::string name; std::optional<SignatureType> payload; };
struct EnumInterface {
    std::string source_name;
    bool is_private{};
    std::vector<EnumVariantInterface> variants; // Authored order is the ABI tag.
};
struct PublicImportInterface {
    std::string alias;
    ModuleSelector destination; // Resolved original identity, not filesystem path.
};
struct ConstantInterface {
    std::string source_name;
    bool is_private{}, literal{};
    SignatureType type;
    CanonicalInterfacePayload initializer;
};
struct NamedTypeIdentity {
    ModuleSelector module;
    std::string source_name;
    std::vector<std::string> generic_arguments;
};
struct InterfaceContracts {
    // Explicit completeness distinguishes an empty catalog from missing data.
    bool declarations_complete{}, classes_complete{}, constants_complete{}, aliases_complete{},
         enums_complete{}, reexports_complete{};
    std::vector<FunctionInterface> functions;
    std::vector<ClassInterface> classes;
    std::vector<AliasInterface> aliases;
    std::vector<ConstantInterface> constants;
    // Runtime/mangled names are lookup-only; only the typed original identity
    // enters an encoding. No reconstruction from generated spelling occurs.
    std::map<std::string,NamedTypeIdentity> named_types;
    std::vector<EnumInterface> enums;
    std::vector<PublicImportInterface> reexports;
    // Full original inline environment/dependencies, encoded once per module.
    // Required for opaque, not-yet-checked generic bodies; never an effect proof.
    std::optional<CanonicalInterfacePayload> generic_environment;
    bool signature_environment_required{};
};
enum class MissingComponent : std::uint8_t {
    DeclarationCatalog, ClassCatalog, AliasCatalog, ConstantCatalog, Visibility,
    GenericBody, GenericSummary, FunctionSummary, ResultExtents, BorrowContract,
    Initialization, ExternalSymbol, ConflictingInstances, NamedType,
    EnumCatalog, ReexportCatalog, GenericEnvironment
};
struct FingerprintIssue {
    MissingComponent component;
    std::string declaration; // Original logical identity, never IR clone/path.
    bool operator==(const FingerprintIssue&) const = default;
};
struct ModuleFingerprints {
    std::optional<ir::Digest> signature, summary;
    std::vector<FingerprintIssue> missing;
    std::size_t exported_functions{}, exported_classes{}, exported_aliases{}, exported_constants{};
    std::size_t exported_enums{}, exported_reexports{};
};
// A usable digest is emitted only for a complete corresponding catalog/fact
// set. No purity, freshness or clone authorization follows from these hashes.
ModuleFingerprints fingerprint_module(const ir::Module&, const ModuleSelector&,
                                      const InterfaceContracts& = {});
} // namespace quidra::semantics
