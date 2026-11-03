#pragma once
#include "quidra/compiler_extension.hpp"
#include "quidra/declaration_identity.hpp"
#include <vector>
namespace quidra::optimizer {
// Descriptor assertions are checked against original declarations before
// generic demand removes unused templates. No typed IR or optimizer state is
// needed at the frontend boundary.
void validate_extension_declarations(const std::vector<CompilerExtensionRegistration>&,
                                     const std::vector<DeclarationIdentity>&);
}
