#include "lowering/assignment_order.hpp"
#include "lowering/borrow_inference.hpp"
#include "lowering/storage_writes.hpp"
#include "lowering/syntax_queries.hpp"
#include "semantics/argument_isolation.hpp"
#include "semantics/effect_summary.hpp"
#include "semantics/mutation_markers.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
using namespace quidra;
const Type integer = Type::simple(TypeKind::Int64);
const Type array = Type::array(integer);

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

ExprPtr expression(CheckedProgram& checked, Expr::Data data, const Type& type) {
    auto result = std::make_unique<Expr>();
    result->data = std::move(data);
    result->span.start.line = 7;
    result->span.start.offset = 70;
    result->span.end.offset = 80;
    checked.expr_types[result.get()] = type;
    checked.raw_types[result.get()] = type;
    return result;
}
ExprPtr name(CheckedProgram& checked, const char* value, const Type& type) {
    return expression(checked, NameExpr{value}, type);
}
ExprPtr number(CheckedProgram& checked) {
    return expression(checked, IntegerExpr{3, "3"}, integer);
}
ExprPtr call(CheckedProgram& checked, const char* target, std::vector<CallArg> arguments) {
    auto result = expression(checked, CallExpr{target, std::move(arguments)}, integer);
    checked.call_resolutions[result.get()] = CallResolution{CallKind::Function, target};
    return result;
}
StmtPtr statement(Stmt::Data data) {
    auto result = std::make_unique<Stmt>();
    result->data = std::move(data);
    return result;
}
Parameter parameter(const char* value, const char* type, bool writable = false) {
    Parameter result;
    result.name = value;
    result.type.name = type;
    result.writable = writable;
    return result;
}

enum class Site { Binding, Assignment, Parameter, ShapeAxis, MatchAxis, ForeignParameter };

// Construct checked nodes directly so these regressions isolate the semantic
// visitors from parser/checker support for the range syntax.
void bounds_keep_effects(Site site, bool exposed) {
    CheckedProgram checked;
    FunctionDecl foreign;
    foreign.name = "touch";
    foreign.external_symbol = "touch";
    foreign.mutation_marker = site == Site::ForeignParameter;
    foreign.parameters.push_back(parameter("values", "int64", true));
    checked.program.functions.push_back(std::move(foreign));
    FunctionType foreign_type;
    foreign_type.external = true;
    foreign_type.result = integer;
    foreign_type.parameters.push_back({"values", array, true});
    checked.functions["touch"] = foreign_type;

    // Lexically after a_outer: the bound's call-graph edge must make this
    // summary available before its caller is analyzed.
    FunctionDecl leaf;
    leaf.name = "z_leaf";
    leaf.parameters.push_back(parameter("snapshot", "int64"));
    leaf.parameters.push_back(parameter("values", "int64", true));
    std::vector<CallArg> foreign_args;
    foreign_args.push_back({std::nullopt, true, name(checked, "values", array)});
    leaf.body.push_back(statement(ReturnStmt{call(checked, "touch", std::move(foreign_args))}));
    checked.program.functions.push_back(std::move(leaf));
    FunctionType leaf_type;
    leaf_type.result = integer;
    leaf_type.parameters = {{"snapshot", array, false, nullptr, true}, {"values", array, true}};
    checked.functions["z_leaf"] = leaf_type;

    FunctionDecl outer;
    outer.name = "a_outer";
    outer.parameters.push_back(parameter("values", "int64", exposed));
    outer.parameters.push_back(parameter("candidate", "int64"));
    FunctionType outer_type;
    outer_type.parameters = {{"values", array, exposed}, {"candidate", integer}};
    checked.functions["a_outer"] = outer_type;

    std::vector<CallArg> bound_args;
    bound_args.push_back({std::nullopt, false, name(checked, "values", array)});
    const Expr* borrowed_argument = bound_args.front().value.get();
    bound_args.push_back({std::nullopt, true, name(checked, "values", array)});
    auto bound = std::shared_ptr<Expr>(call(checked, "z_leaf", std::move(bound_args)));
    RangeConstraint range;
    range.upper = bound;
    const Stmt* guarded = nullptr;
    if (site == Site::Binding) {
        BindingStmt binding;
        binding.name = "local";
        binding.value = number(checked);
        binding.range_constraint = range;
        auto node = statement(std::move(binding));
        checked.binding_types[node.get()] = integer;
        guarded = node.get();
        outer.body.push_back(std::move(node));
    } else if (site == Site::Assignment) {
        AssignStmt assignment;
        assignment.target = name(checked, "candidate", integer);
        assignment.value = number(checked);
        assignment.range_constraint = range;
        auto node = statement(std::move(assignment));
        guarded = node.get();
        outer.body.push_back(std::move(node));
    } else if (site == Site::Parameter || site == Site::ForeignParameter) {
        outer.parameters[1].range_constraint = range;
        if (site == Site::ForeignParameter) {
            outer.external_symbol = "foreign_outer";
            checked.functions["a_outer"].external = true;
        }
    } else {
        TypeName tensor;
        tensor.name = "tensor";
        ShapeAxis axis;
        axis.kind = ShapeAxisKind::Declare;
        axis.name = "N";
        axis.range_constraint = range;
        tensor.shape_axes.push_back(std::move(axis));
        if (site == Site::ShapeAxis) {
            BindingStmt binding;
            binding.name = "local";
            binding.value = number(checked);
            binding.declared_type = std::move(tensor);
            auto node = statement(std::move(binding));
            checked.binding_types[node.get()] = integer;
            guarded = node.get();
            outer.body.push_back(std::move(node));
        } else {
            MatchStmt match;
            match.value = number(checked);
            MatchCase arm;
            arm.type = std::move(tensor);
            match.cases.push_back(std::move(arm));
            auto node = statement(std::move(match));
            guarded = node.get();
            outer.body.push_back(std::move(node));
        }
    }
    checked.program.functions.push_back(std::move(outer));
    checked.effects = std::make_shared<semantics::EffectSummaries>(semantics::summarize_effects(checked));
    const auto* summary = checked.effects->find("a_outer");
    require(summary && summary->exposed_writes.size() == 2, "missing outer effect summary");
    require(!summary->exposed_writes[0].empty() == exposed, "bound lost transitive exposed write");
    if (site == Site::ForeignParameter) {
        require(summary->mutates_ambient, "foreign precondition lost its callee's ambient effect");
        require(summary->result.kind == semantics::ResultProvenance::Kind::Existing,
                "foreign precondition erased opaque result provenance");
    }
    require(semantics::ArgumentIsolation(checked, false).copies(*borrowed_argument),
            "aliased value argument in bound must be isolated");

    lowering::BorrowInference borrows(checked);
    require(!borrows.parameter_is_borrowed("a_outer", 0), "bound mutation incorrectly borrows value parameter");
    if (guarded) {
        require(lowering::statement_mentions_name(*guarded, "values"), "name query lost bound reference");
        require(lowering::block_may_replace_array_reference(checked.program.functions.back().body, {"values"}, &checked),
                "array replacement query lost bound mutation");
        lowering::LocalScope scope;
        scope.bind_parameter("values", array);
        ir::FunctionBuilder builder;
        lowering::StorageWrites writes(checked, borrows, builder, scope);
        auto iterable = name(checked, "values", array);
        require(writes.statement_writes_iterable(*guarded, *iterable) == lowering::StorageWrites::IterableWrite::in_place,
                "storage write query lost bound call");
    }
    checked.program.functions[1].mutation_marker = true;
    const auto diagnostics = semantics::check_mutation_markers(checked);
    require(std::any_of(diagnostics.begin(), diagnostics.end(), [&](const auto& diagnostic) {
        return diagnostic.code == "MUTATION_MARKER" && diagnostic.span.start.offset == bound->span.start.offset &&
               diagnostic.message.find("z_leaf") != std::string::npos;
    }), "mutation marker check missed bound call");
}

void assignment_order_includes_bound() {
    CheckedProgram checked;
    lowering::BorrowInference borrows(checked);
    AssignStmt assignment;
    IndexExpr target;
    target.base = name(checked, "values", array);
    IndexPart coordinate;
    coordinate.index = name(checked, "i", integer);
    target.items.push_back(std::move(coordinate));
    assignment.target = expression(checked, std::move(target), integer);
    assignment.value = number(checked);
    std::vector<CallArg> args;
    args.push_back({std::nullopt, true, name(checked, "i", integer)});
    RangeConstraint range;
    range.upper = std::shared_ptr<Expr>(call(checked, "change_index", std::move(args)));
    assignment.range_constraint = std::move(range);
    require(lowering::AssignmentOrder(checked, borrows).classify(assignment) ==
                lowering::AssignmentOrder::Tier::every_execution,
            "assignment ordering ignored bound mutation of target index");
}
} // namespace

int main() {
    try {
        for (const auto site : {Site::Binding, Site::Assignment, Site::Parameter, Site::ShapeAxis, Site::MatchAxis, Site::ForeignParameter}) {
            bounds_keep_effects(site, true);
            bounds_keep_effects(site, false);
        }
        assignment_order_includes_bound();
        std::cout << "Range constraint traversal passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
