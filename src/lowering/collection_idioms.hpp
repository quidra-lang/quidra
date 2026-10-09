#pragma once

// CollectionIdioms: a match over a standard Map.get(key), lowered without
// materializing the optional value. Defined in collection_idioms.cpp.

#include "ir/function_builder.hpp"
#include "lowering/check_elision_facts.hpp"
#include "lowering/ir_names.hpp"
#include "lowering/local_scope.hpp"

namespace quidra::lowering {

class Lowerer;

class CollectionIdioms {
public:
    CollectionIdioms(
        Lowerer& lowerer, const CheckedProgram& checked,
        ir::FunctionBuilder& builder, LocalScope& scope,
        CheckElisionFacts& facts)
        : lowerer_(lowerer), checked_(checked), builder_(builder),
          scope_(scope), facts_(facts) {}

    // Map.get match
    bool lower_standard_map_get_match(const MatchStmt& match);

private:
    Lowerer& lowerer_;
    const CheckedProgram& checked_;
    ir::FunctionBuilder& builder_;
    LocalScope& scope_;
    CheckElisionFacts& facts_;
};

} // namespace quidra::lowering
