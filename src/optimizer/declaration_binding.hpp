#pragma once

#include "quidra/compiler_extension_model.hpp"
#include "compiler_extension_binding.hpp"
#include "quidra/ir/module.hpp"

#include <map>
#include <string>
#include <vector>

namespace quidra::optimizer {
using ::quidra::DeclarationBindingMode;
struct BoundOperation {
    std::string reference, extension;
    std::vector<DeclarationIdentity> declarations;
    std::vector<std::string> functions;
    CompilerExtensionTable contract;
};
struct BoundSetter {
    std::string function, extension, policy;
};
struct BindingIssue {
    std::string reference, selector, message;
};
struct DeclarationBindings {
    std::map<std::string, BoundOperation, std::less<>> operations;
    std::vector<BoundSetter> setters;
    std::vector<BindingIssue> issues;
};
// Full descriptors always resolve exactly. Legacy exact results are diagnostic
// in Verify and selected in On; Off leaves their existing matching untouched.
DeclarationBindings bind_declarations(const ir::Module&, DeclarationBindingMode);
} // namespace quidra::optimizer
