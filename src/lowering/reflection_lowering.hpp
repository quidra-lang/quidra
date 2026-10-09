#pragma once

// ReflectionLowering: reflected paths and values, and the reflect builtins.
// Defined in reflection_lowering.cpp.

#include "ir/function_builder.hpp"
#include "lowering/ir_names.hpp"
#include "lowering/local_scope.hpp"
#include <functional>
#include <string>
#include <unordered_set>

namespace quidra::lowering {

class Lowerer;
class LifetimeLowering;

class ReflectionLowering {
public:
    ReflectionLowering(
        Lowerer& lowerer, const CheckedProgram& checked,
        ir::FunctionBuilder& builder, LocalScope& scope,
        LifetimeLowering& lifetime)
        : lowerer_(lowerer), checked_(checked), builder_(builder),
          scope_(scope), lifetime_(lifetime) {}

    // reflected paths and values; the reflect builtins
    std::string collected_array_local(
        const Type& element_type,const std::string& prefix);
    void append_collected_value(
        const std::string& destination,const Type& element_type,ValueId value);
    void for_each_collected_array_element(
        ValueId array,const Type& array_type,const std::string& prefix,
        const std::function<void(ValueId,const Type&,ValueId)>& visit);
    ValueId lower_reflect_type_name(const CallExpr& n);
    ValueId lower_reflect_collection(
        const Expr& e, const CallExpr& n, const CallResolution& resolution);

private:
    // the walk over a value that lower_reflect_collection collects from
    bool reflected_type_contains(
        const Type& type,const Type& target,
        std::unordered_set<std::string>& active) const;
    ValueId reflected_index_path(ValueId prefix,ValueId index);
    ValueId reflected_field_path(
        ValueId prefix,bool prefix_empty,const std::string& field_name);
    void collect_reflected_paths(
        ValueId object,const Type& type,const Type& target,
        ValueId prefix,bool prefix_empty,const std::string& destination,
        std::unordered_set<std::string>& active);
    void collect_reflected_values(
        ValueId object,const Type& type,const Type& target,
        const std::string& destination,
        std::unordered_set<std::string>& active);

    Lowerer& lowerer_;
    const CheckedProgram& checked_;
    ir::FunctionBuilder& builder_;
    LocalScope& scope_;
    LifetimeLowering& lifetime_;
};

} // namespace quidra::lowering
