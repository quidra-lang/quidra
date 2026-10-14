#pragma once

#include "quidra/ir/function.hpp"

#include <optional>
#include <string>

namespace quidra::llvm_backend {
struct AuthoredDisplayName {std::string name;bool constructor{};};
// Display-only provenance. Never a callable identity, ABI/body rewrite,
// specialization eligibility rule, or constructor allocation proof.
inline std::optional<AuthoredDisplayName> authored_specialization_display_name(const ir::Function& function) {
    if(function.entrypoint || !function.declaration || function.declaration->specialization_of.empty())return {};
    const auto& source=*function.declaration;
    switch(source.kind) {
    case DeclarationIdentity::Kind::Function:
        if(!source.source_name.empty())return AuthoredDisplayName{source.source_name,false};
        break;
    case DeclarationIdentity::Kind::Method:
        if(!source.owner_class.empty() && !source.source_name.empty())
            return AuthoredDisplayName{source.owner_class+"."+source.source_name,false};
        break;
    case DeclarationIdentity::Kind::Constructor:
        if(!source.owner_class.empty())return AuthoredDisplayName{source.owner_class,true};
        break;
    }
    return {};
}
} // namespace quidra::llvm_backend
