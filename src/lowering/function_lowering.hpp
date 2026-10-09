#pragma once

// FunctionLowering: functions, methods, constructors and the entry, each
// begun with its per-function components reset. Defined in
// function_lowering.cpp.

#include "ir/function_builder.hpp"
#include "lowering/borrow_inference.hpp"
#include "lowering/check_elision_facts.hpp"
#include "lowering/enclosing_class.hpp"
#include "lowering/ir_names.hpp"
#include "lowering/local_scope.hpp"
#include "lowering/repl_replay_state.hpp"
#include "lowering/shape_constraint_state.hpp"
#include <cstddef>
#include <string>
#include <vector>

namespace quidra::lowering {

class Lowerer;
class ConversionLowering;
class ShapeConstraintLowering;
class CallLowering;

class FunctionLowering {
public:
    FunctionLowering(
        Lowerer& lowerer, const CheckedProgram& checked, ir::Module& module,
        ir::FunctionBuilder& builder, LocalScope& scope,
        CheckElisionFacts& facts, EnclosingClass& enclosing_class,
        ReplReplayState& replay, ShapeConstraintState& shapes,
        const BorrowInference& borrows, ConversionLowering& conversions,
        ShapeConstraintLowering& shape_constraints, CallLowering& calls)
        : lowerer_(lowerer), checked_(checked), module_(module),
          builder_(builder), scope_(scope), facts_(facts),
          enclosing_class_(enclosing_class), replay_(replay), shapes_(shapes),
          borrows_(borrows), conversions_(conversions),
          shape_constraints_(shape_constraints), calls_(calls) {}

    // functions, methods, constructors, the entry
    void lower_function(const FunctionDecl& source);
    void lower_method(const std::string& class_name,const FunctionDecl& source);
    void return_receiver();
    void lower_constructor(const std::string& class_name,const FunctionDecl& source,const std::string& internal);
    void lower_main(const std::vector<StmtPtr>& statements);

private:
    // A function begun, and the shape constraints its signature declares.
    void capture_signature_constraints(
        const FunctionDecl& source, std::size_t parameter_offset);
    void begin_function(Function out);

    // The header and the body prologue shared by functions, methods and
    // constructors.
    Function function_header(
        const std::string& name, const FunctionDecl& source, const FunctionType& sig,
        bool borrowed_receiver);
    void lower_prologue_and_body(
        const FunctionDecl& source, const FunctionType& sig, std::size_t parameter_offset);

    Lowerer& lowerer_;
    const CheckedProgram& checked_;
    ir::Module& module_;
    ir::FunctionBuilder& builder_;
    LocalScope& scope_;
    CheckElisionFacts& facts_;
    EnclosingClass& enclosing_class_;
    ReplReplayState& replay_;
    ShapeConstraintState& shapes_;
    const BorrowInference& borrows_;
    ConversionLowering& conversions_;
    ShapeConstraintLowering& shape_constraints_;
    CallLowering& calls_;
};

} // namespace quidra::lowering
