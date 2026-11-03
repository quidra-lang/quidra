#pragma once

#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

namespace quidra {
enum class DeclarationBindingMode : std::uint8_t { Off, Verify, On };
// Original source contract, before import renaming and specialization. Borrow
// inference is an ABI optimization and does not change this source contract.
struct DeclarationParameter {
    std::string name, type;
    bool writable{}, is_const{};
    bool operator==(const DeclarationParameter&) const = default;
};
struct DeclarationIdentity {
    enum class Kind : std::uint8_t { Function, Method, Constructor } kind{Kind::Function};
    std::string package, module_path, source_name, owner_class, signature;
    std::string source_file;
    std::vector<std::string> generic_parameters, generic_constraints, generic_args;
    std::vector<DeclarationParameter> parameters;
    std::string result_type;
    bool mutation_marker{}, external{};
    // Original declaration key. Instances and later optimizer clones retain
    // this key; no generated name or source-position hash is used to infer it.
    std::string specialization_of;
    bool operator==(const DeclarationIdentity&) const = default;
};
// Equality of the original declaration, excluding concrete type arguments.
inline bool same_declaration(const DeclarationIdentity& a, const DeclarationIdentity& b) {
    return std::tie(a.kind, a.package, a.module_path, a.source_name, a.owner_class,
                    a.signature, a.generic_parameters, a.generic_constraints, a.parameters,
                    a.result_type, a.mutation_marker, a.external) ==
           std::tie(b.kind, b.package, b.module_path, b.source_name, b.owner_class,
                    b.signature, b.generic_parameters, b.generic_constraints, b.parameters,
                    b.result_type, b.mutation_marker, b.external);
}
inline std::string declaration_key(const DeclarationIdentity& identity) {
    std::string result = std::to_string(static_cast<unsigned>(identity.kind));
    const auto part = [&](const std::string& value) { result += ":" + std::to_string(value.size()) + ":" + value; };
    for (const auto* value : {&identity.package, &identity.module_path, &identity.source_name,
                             &identity.owner_class, &identity.signature}) part(*value);
    result += ":g" + std::to_string(identity.generic_parameters.size());
    for (const auto& value : identity.generic_parameters) part(value);
    result += ":c" + std::to_string(identity.generic_constraints.size());
    for (const auto& value : identity.generic_constraints) part(value);
    result += ":p" + std::to_string(identity.parameters.size());
    for (const auto& parameter : identity.parameters) {
        part(parameter.name); part(parameter.type);
        result += parameter.writable ? ":w" : ":v";
        result += parameter.is_const ? ":c" : ":m";
    }
    part(identity.result_type);
    result += identity.mutation_marker ? ":mut" : ":pure-marker";
    result += identity.external ? ":external" : ":defined";
    return result;
}
} // namespace quidra
