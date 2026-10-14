#pragma once

// The Lowerer: one checked program's typed AST to an ir::Module.
//
// It owns the state of lowering one program: the Module being built, the
// per-function components (the FunctionBuilder that appends the blocks and
// instructions of the function being lowered, its source locals and
// references, the facts that elide checks, the declared extents), the ones
// that follow the recursion (nesting depth, loop targets) or that callers
// set around a function (enclosing class, REPL replay), the per-program
// analyses (source locator, borrow inference, storage writes, assignment
// order) and the
// internal lowering switches (ir::LoweringOptions). It holds the checked
// program by const& (an unordered container whose iteration reaches the
// output is never copied). Member order is construction order: the source texts are
// inspected (locator_) before the borrowed parameters are classified
// (borrows_), and the check elision facts are constructed over the scope
// declared before them. The lowering units, one class per concept, each in
// its own header and source, do the lowering; the Lowerer owns them too:
//   FunctionLowering         functions, methods, constructors, the entry
//   StatementLowering        bodies and statements: the recursion roots
//                            (lower_block, lower_statement), one function
//                            per kind
//   AssignmentLowering       assignments
//   ControlFlowLowering      if, while, for and match; exit
//   TextIdioms,              statement sequences lowered as one operation
//   CollectionIdioms
//   ExpressionLowering       expressions: the recursion roots
//                            (lower_at_raw_type, lower, lower_into,
//                            lower_address), one function per kind
//   OperatorLowering         binary operators, exact-number builtins
//   CallLowering             arguments, class values, calls by
//                            resolution, method calls by receiver
//   BuiltinLowering          builtin calls, dispatched to their domain
//   ConversionLowering       conversions between types (convert), parse,
//                            numeric casts
//   LifetimeLowering         copies, owned temporaries, releases
//   ShapeConstraintLowering  declared extents and their checks
//   TextLowering, BinLowering, AggregateLowering, TensorLowering,
//   AutogradLowering, ConsoleLowering, FileLowering, SystemLowering,
//   JsonLowering, HttpLowering, ConcurrencyLowering, RandomLowering,
//   CheckLowering            each domain's methods and builtins
//   ReflectionLowering       reflected paths and values
//   ScanLowering             scan()
// Each unit holds references to the parts of the state it uses (const where
// it only reads them) and to the units it calls, which the Lowerer gives it
// when it constructs it; a unit's public members are what the other units
// and the Lowerer call. The recursion roots (lower_at_raw_type, lower,
// lower_into, lower_address, lower_statement, lower_block) are reached
// through the Lowerer. ir::lower (lower_module.cpp) constructs the Lowerer
// and calls its entry points; the Lowerer's constructor is in lowerer.cpp,
// and the analyses are in borrow_inference.cpp, check_elision_facts.cpp,
// storage_writes.cpp, assignment_order.cpp, syntax_queries.cpp and
// source_locator.cpp. The order
// in which the lowering allocates names (fresh, label, hidden,
// bind_source_local) is part of the output.

#include "ir/function_builder.hpp"
#include "ir/summaries.hpp"
#include "lowering/aggregate_lowering.hpp"
#include "lowering/assignment_lowering.hpp"
#include "lowering/assignment_order.hpp"
#include "lowering/autograd_lowering.hpp"
#include "lowering/bin_lowering.hpp"
#include "lowering/block_end.hpp"
#include "lowering/borrow_inference.hpp"
#include "lowering/builtin_lowering.hpp"
#include "lowering/call_lowering.hpp"
#include "lowering/check_elision_facts.hpp"
#include "lowering/check_lowering.hpp"
#include "lowering/collection_idioms.hpp"
#include "lowering/concurrency_lowering.hpp"
#include "lowering/console_lowering.hpp"
#include "lowering/control_flow_lowering.hpp"
#include "lowering/conversion_lowering.hpp"
#include "lowering/enclosing_class.hpp"
#include "lowering/error_lowering.hpp"
#include "lowering/expression_lowering.hpp"
#include "lowering/file_lowering.hpp"
#include "lowering/function_lowering.hpp"
#include "lowering/http_lowering.hpp"
#include "lowering/initialization_flags.hpp"
#include "lowering/ir_names.hpp"
#include "lowering/json_lowering.hpp"
#include "lowering/lifetime_lowering.hpp"
#include "lowering/local_scope.hpp"
#include "lowering/loop_targets.hpp"
#include "lowering/nesting_depth.hpp"
#include "lowering/operator_lowering.hpp"
#include "lowering/random_lowering.hpp"
#include "lowering/reflection_lowering.hpp"
#include "lowering/repl_replay_state.hpp"
#include "lowering/scan_lowering.hpp"
#include "lowering/shape_constraint_lowering.hpp"
#include "lowering/shape_constraint_state.hpp"
#include "lowering/source_locator.hpp"
#include "lowering/statement_sites.hpp"
#include "lowering/statement_lowering.hpp"
#include "lowering/storage_writes.hpp"
#include "lowering/syntax_queries.hpp"
#include "lowering/system_lowering.hpp"
#include "lowering/tensor_lowering.hpp"
#include "lowering/text_idioms.hpp"
#include "lowering/text_lowering.hpp"
#include "quidra/ir/module.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace quidra::lowering {

// The lowering: its state and its units.
class Lowerer {
public:
    explicit Lowerer(
        const CheckedProgram& c, const Expr* repl = nullptr,
        std::size_t replay_prefix_offset = 0,
        const ir::LoweringOptions& options = {});
    // The facts refer to the scope beside them and the units to the state: the
    // Lowerer is neither copied nor moved.
    Lowerer(const Lowerer&) = delete;
    Lowerer& operator=(const Lowerer&) = delete;

    // The module being built, which ir::lower starts with the class layouts
    // and takes when every function is lowered.
    ir::Module& module() { return module_; }
    ir::analysis::SummaryBindings& summary_bindings() { return summary_bindings_; }
    StatementSites& statement_sites() { return statement_sites_; }
    void emit_statement_location(const Stmt& statement) {
        const auto& name = builder_.function()->name;
        builder_.emit(ir::StatementBoundary{statement_sites_.identity(name, statement)});
        builder_.emit(locator_.locate(statement.span, builder_.function()));
    }
    void flush_outlined_functions() {
        for(auto& function:outlined_functions_) module_.functions.push_back(std::move(function));
        outlined_functions_.clear();
    }
    bool parallel_hoisted(const Stmt& statement) const {return parallel_hoisted_.contains(&statement);}
    const ShapeConstraintLowering::BraceShape* parallel_brace_shape(const Stmt& statement) const {
        const auto found=parallel_brace_shapes_.find(&statement);
        return found==parallel_brace_shapes_.end()?nullptr:&found->second;
    }
    void lower_parallel(const Stmt& statement,const ParallelStmt& block){concurrency_.lower_parallel(statement,block);}
    // The internal switches this program is lowered with.
    const ir::LoweringOptions& options() const { return options_; }
    // The run-time initialization flags and checks (L13).
    InitializationFlags& initialization_flags() { return initialization_flags_; }

    // The entry points ir::lower calls.
    void lower_function(const FunctionDecl& source) { functions_.lower_function(source); }
    void lower_method(const std::string& class_name, const FunctionDecl& source) {
        functions_.lower_method(class_name, source);
    }
    void lower_constructor(const std::string& class_name, const FunctionDecl& source,
                           const std::string& internal) {
        functions_.lower_constructor(class_name, source, internal);
    }
    void lower_main(const std::vector<StmtPtr>& statements) {
        functions_.lower_main(statements);
    }

    // The recursion roots the units call through the Lowerer, which breaks
    // the cycle expression -> statement -> expression between their headers.
    ValueId lower_at_raw_type(const Expr& e) { return expressions_.lower_at_raw_type(e); }
    void precompute(const Expr& e) { expressions_.precompute(e); }
    void lower_pattern_values(const PatternExpansion& expansion,ir::ValueId subject){assignments_.lower_pattern_values(expansion,subject);}
    void capture_loop_operand(const Expr& e) { expressions_.capture_loop_operand(e); }
    void forget_loop_operand(const Expr& e) { expressions_.forget_loop_operand(e); }
    ValueId lower(const Expr& e) { return expressions_.lower(e); }
    std::optional<ValueId> try_scaled_expression(const Expr& e,const Type* target=nullptr) { return expressions_.try_scaled_expression(e,target); }
    ValueId lower_int64(const Expr& e) { return expressions_.lower_int64(e); }
    SliceBounds lower_slice_bounds(const Expr& e, const IndexPart& item) {
        return expressions_.lower_slice_bounds(e, item);
    }
    // A value of one type converted to another (conversion_lowering.cpp).
    ValueId convert(ValueId value, const Type& from, const Type& to) {
        return conversions_.convert(value, from, to);
    }
    ValueId bare_from_int64(ValueId value, const Type& target, bool proven_inline) {
        return expressions_.bare_from_int64(value, target, proven_inline);
    }
    // The check elision facts of the function being lowered.
    const CheckElisionFacts& facts() const { return facts_; }
    EnclosingClass shape_class_context(const std::string& owner, const std::string& receiver,
                                       bool reference) {
        auto previous = enclosing_class_;
        if (owner.empty()) enclosing_class_.leave();
        else {
            enclosing_class_.enter(owner, checked_.classes.at(owner).standard_library);
            enclosing_class_.set_shape_receiver(receiver, reference);
        }
        enclosing_class_.set_in_constructor(false);
        return previous;
    }
    void restore_shape_class(EnclosingClass context) { enclosing_class_ = std::move(context); }
    bool isolate_shape_source_facts(bool isolated) {
        const auto previous = facts_.integer_ranges.isolate_source_facts(isolated);
        facts_.array_bounds.isolate_source_facts(isolated);
        facts_.array_initialization.isolate_source_facts(isolated);
        return previous;
    }
    ValueId lower_into(const Expr& expression, const Type& target) {
        return expressions_.lower_into(expression, target);
    }
    ValueId lower_address(const Expr& e) { return expressions_.lower_address(e); }
    ValueId lower_address(const Expr& e, bool may_write) {
        return expressions_.lower_address(e, may_write);
    }
    // error(...) and the message of an error (error_lowering.cpp).
    ValueId lower_error_constructor(const Expr& e, const CallExpr& n) {
        return errors_.lower_constructor(e, n);
    }
    ValueId error_message(ValueId value, bool owned) { return errors_.message_of(value, owned); }
    // The hop of an error leaving the call `e` (error_lowering.cpp).
    std::optional<ErrorHopCall> error_hop_call(const Expr& e) { return errors_.hop_call(e); }
    ValueId error_hop(ValueId container, const Type& container_type, const ErrorHopCall& call,
                      abi::ErrorHopKind kind) {
        return errors_.hop(container, container_type, call, kind);
    }
    void lower_statement(const Stmt& s) { statements_.lower_statement(s); }
    void release_temporary(const Expr& e, ValueId value) { lifetime_.release_temporary(e, value); }
    void lower_block(const std::vector<StmtPtr>& statements) {
        statements_.lower_block(statements);
    }
    void lower_block(
        const std::vector<StmtPtr>& statements,
        const std::function<void(std::size_t)>& before_statement) {
        statements_.lower_block(statements, before_statement);
    }

private:
    friend class ConcurrencyLowering;
    friend class AssignmentLowering;
    std::unordered_map<std::string,std::pair<ir::ValueId,ir::ValueId>> parallel_for_outputs_;
    friend class ControlFlowLowering;
    // The state of lowering one program, in construction order.
    const CheckedProgram& checked_;
    const ir::LoweringOptions options_;
    ir::Module module_;
    ir::analysis::SummaryBindings summary_bindings_;
    std::vector<ir::Function> outlined_functions_;
    std::unordered_set<const Stmt*> parallel_hoisted_;
    std::unordered_map<const Stmt*,ShapeConstraintLowering::BraceShape> parallel_brace_shapes_;
    // The function being lowered: its blocks and name counters.
    ir::FunctionBuilder builder_;
    // The source locals and references of the function being lowered.
    LocalScope scope_;
    // Proofs that remove runtime checks in the function being lowered.
    CheckElisionFacts facts_;
    // The depth of the expression and statement recursions.
    NestingDepth nesting_depth_;
    // Where break and continue jump.
    LoopTargets loop_targets_;
    // The class of the method or constructor being lowered.
    EnclosingClass enclosing_class_;
    // The REPL submission being lowered, if any.
    ReplReplayState replay_;
    // The declared extents of the function being lowered.
    ShapeConstraintState shapes_;
    // The public syntax nodes of every source text, for source locations.
    StatementSites statement_sites_;
    SourceLocator locator_;
    // Which by-value parameters callers pass borrowed (after the locator: the
    // source texts are inspected first).
    BorrowInference borrows_;
    // What an expression may write (after the borrows it builds on).
    StorageWrites storage_writes_;
    // Which plain assignments have an observable evaluation order.
    AssignmentOrder assignment_order_;
    // Which borrowed arguments a call must pass as copies.
    semantics::ArgumentIsolation isolation_;
    // The run-time initialization flags of the function being lowered.
    InitializationFlags initialization_flags_;

    // The units, in construction order; each holds references only.
    LifetimeLowering lifetime_;
    ConversionLowering conversions_;
    ErrorLowering errors_;
    ShapeConstraintLowering shape_constraints_;
    ExpressionLowering expressions_;
    OperatorLowering operators_;
    CallLowering calls_;
    BuiltinLowering builtins_;
    TextLowering text_;
    BinLowering bin_;
    AggregateLowering aggregates_;
    TensorLowering tensors_;
    AutogradLowering autograd_;
    ConsoleLowering console_;
    FileLowering files_;
    SystemLowering system_;
    JsonLowering json_;
    HttpLowering http_;
    ConcurrencyLowering concurrency_;
    RandomLowering random_;
    CheckLowering checks_;
    ReflectionLowering reflection_;
    ScanLowering scan_;
    StatementLowering statements_;
    AssignmentLowering assignments_;
    ControlFlowLowering control_flow_;
    TextIdioms text_idioms_;
    CollectionIdioms collection_idioms_;
    FunctionLowering functions_;
};

} // namespace quidra::lowering
