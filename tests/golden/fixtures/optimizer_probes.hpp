// Hand-built typed-IR modules for optimizer paths that no source program
// reaches (tests/golden/README.md, Inputs): descriptor tables the frontend
// refuses, calls whose arity differs from their callee's, chains whose blocks
// end without a terminator, intermediate values used twice or outside the
// chain, replacement results of other types, a release the conditional rules
// cannot transfer, and a function whose value numbers are used up. The golden
// tool optimizes each (`--fixture optimizer/NAME`); none of them is emitted.
//
// Each builder returns the module before ir::optimize().
#pragma once

#include "quidra/ir.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace quidra::optimizer_probes {

inline Type float32_tensor() { return Type::tensor(Type::simple(TypeKind::Real32), 1); }

// A float32 tensor whose extents its source shape pattern fixes.
inline Type float32_tensor_of(std::vector<long long> extents) {
    const auto rank = static_cast<long long>(extents.size());
    return Type::tensor(Type::simple(TypeKind::Real32), rank, extents, extents);
}

inline ir::Parameter tensor_parameter(std::string name, bool borrowed) {
    return ir::Parameter{std::move(name), float32_tensor(), false, borrowed, false};
}

// A package function: declared in the package's source, no body.
inline ir::Function package_function(std::string name, std::vector<ir::Parameter> parameters,
                                     Type result = float32_tensor()) {
    ir::Function function;
    function.name = std::move(name);
    function.source_file = "/virtual/probe/main.qui";
    function.parameters = std::move(parameters);
    function.result = std::move(result);
    return function;
}

inline ir::Call call(ir::ValueId out, std::string callee, std::vector<ir::ValueId> arguments,
                     Type result = float32_tensor()) {
    ir::Call call;
    call.out = out;
    call.callee = std::move(callee);
    for (const auto argument : arguments) call.args.push_back(ir::CallArgument{argument, std::nullopt});
    call.result = std::move(result);
    return call;
}

inline CompilerExtensionRegistration tensor_region_extension(
    std::string name, std::map<std::string, std::map<std::string, std::string>> tables) {
    return CompilerExtensionRegistration{
        "probe", std::move(name), "/virtual/probe", "/virtual/probe/compiler/graph.toml",
        "[extension]\nversion = 1\nphase = \"tensor-region\"\n", "tensor-region", std::move(tables)};
}

// Descriptor tables the frontend refuses: an extension of another phase, an
// execution policy without an id or a function, an operation without an id,
// fusion tables without operations, a rule without a replacement, and traits
// padded with carriage returns. One package function has a relative source
// path, which is in no package.
inline ir::Module descriptor_tables_the_frontend_refuses() {
    using namespace quidra::ir;
    Module module;
    module.compiler_extensions.push_back(CompilerExtensionRegistration{
        "probe", "later", "/virtual/probe", "/virtual/probe/compiler/later.toml",
        "[extension]\nversion = 1\nphase = \"later\"\n", "later",
        {
            {"operation.first", {{"function", "first"}, {"traits", "pure,tensor"}}},
            {"specialization.first", {{"operation", "first"}, {"replacement", "first"}}},
        }});
    module.compiler_extensions.push_back(tensor_region_extension(
        "graph",
        {
            {"execution_policy.", {{"function", "mode"}}},
            {"execution_policy.fast", {}},
            {"operation.", {{"function", "first"}, {"traits", "pure,tensor"}}},
            {"operation.first", {{"function", "first"}, {"traits", "pure\r,tensor, differentiable"}}},
            {"operation.second",
             {{"function", "second"}, {"traits", "\rpure,tensor,differentiable\r"}}},
            {"operation.fused", {{"function", "fused"}, {"traits", "pure,tensor,differentiable"}}},
            {"fusion.unlisted", {{"replacement", "fused"}}},
            {"fusion.blank", {{"operations", " , "}, {"replacement", "fused"}}},
            {"fusion.first_second", {{"operations", "first,second"}, {"replacement", "fused"}}},
            {"specialization.unfinished", {{"operation", "first"}}},
        }));

    module.functions.push_back(package_function("alias.first", {tensor_parameter("value", true)}));
    module.functions.push_back(package_function("alias.second", {tensor_parameter("value", true)}));
    module.functions.push_back(package_function("alias.fused", {tensor_parameter("value", true)}));
    auto relative = package_function("relative.first", {tensor_parameter("value", true)});
    relative.source_file = "relative/main.qui";
    module.functions.push_back(std::move(relative));

    Function user;
    user.name = "$entry";
    user.source_file = "/virtual/user/main.qui";
    user.result = Type::simple(TypeKind::Int64);
    user.blocks.push_back(Block{"entry", {}});
    auto& entry = user.blocks.back().instructions;
    entry.push_back(TensorCreate{1, 90, std::nullopt, float32_tensor(), 1, 0, 0});
    entry.push_back(call(2, "alias.first", {1}));
    entry.push_back(call(3, "alias.second", {2}));
    entry.push_back(ConstantInt{4, "0", Type::simple(TypeKind::Int64)});
    entry.push_back(Return{4, Type::simple(TypeKind::Int64)});
    module.functions.push_back(std::move(user));
    return module;
}

// Chains that chain fusion rejects for reasons a lowered program never
// gives: a first or later call whose arity differs from its callee's, a
// bridge that copies the intermediate value, a block that ends without a
// terminator after the first call, an intermediate value passed twice, and
// one used in another block. The rejected chain of the last block still
// reaches target selection, where the replacement has four candidates with
// other result types: another element type, another rank, another second
// extent, and an unknown rank, the only compatible one. A second function
// forms one tensor region over two blocks.
inline ir::Module chain_rejections_no_source_program_reaches() {
    using namespace quidra::ir;
    Module module;
    module.compiler_extensions.push_back(tensor_region_extension(
        "graph",
        {
            {"operation.first", {{"function", "first"}, {"traits", "pure,tensor,differentiable"}}},
            {"operation.second", {{"function", "second"}, {"traits", "pure,tensor,differentiable"}}},
            {"operation.pair", {{"function", "pair"}, {"traits", "pure,tensor,differentiable"}}},
            {"operation.fused", {{"function", "fused"}, {"traits", "pure,tensor,differentiable"}}},
            {"fusion.first_second", {{"operations", "first,second"}, {"replacement", "fused"}}},
            {"fusion.first_pair", {{"operations", "first,pair"}, {"replacement", "fused"}}},
        }));

    module.functions.push_back(package_function("alias.first", {tensor_parameter("value", true)}));
    module.functions.push_back(package_function("alias.second", {tensor_parameter("value", true)}));
    module.functions.push_back(package_function(
        "alias.pair", {tensor_parameter("left", true), tensor_parameter("right", true)}));
    module.functions.push_back(package_function("alias.fused", {tensor_parameter("value", true)},
                                                Type::tensor(Type::simple(TypeKind::Real64), 1)));
    module.functions.push_back(package_function("rank.fused", {tensor_parameter("value", true)},
                                                float32_tensor_of({4, 4, 4})));
    module.functions.push_back(
        package_function("extent.fused", {tensor_parameter("value", true)}, float32_tensor_of({4, 3})));
    module.functions.push_back(package_function("any.fused", {tensor_parameter("value", true)},
                                                Type::tensor(Type::simple(TypeKind::Real32))));

    const auto int_type = Type::simple(TypeKind::Int64);
    Function user;
    user.name = "user";
    user.source_file = "/virtual/user/main.qui";
    user.result = int_type;
    // The first call has no argument; alias.first takes one.
    user.blocks.push_back(Block{"arity", {call(1, "alias.first", {}), Jump{"next_arity"}}});
    // The second call passes two arguments; alias.second takes one.
    user.blocks.push_back(
        Block{"next_arity", {call(11, "alias.first", {10}), call(12, "alias.second", {11, 11}),
                             Jump{"copied"}}});
    // A copy of the intermediate value sits between the calls.
    user.blocks.push_back(Block{"copied",
                                {call(21, "alias.first", {20}), Clone{22, 21, float32_tensor()},
                                 call(23, "alias.second", {22}), Jump{"twice"}}});
    // The intermediate value is both arguments of pair.
    user.blocks.push_back(
        Block{"twice", {call(41, "alias.first", {40}), call(42, "alias.pair", {41, 41}),
                        Jump{"escaping"}}});
    // The intermediate value is used again in the next block.
    user.blocks.push_back(Block{"escaping",
                                {call(51, "alias.first", {50}, float32_tensor_of({4, 4})),
                                 call(52, "alias.second", {51}, float32_tensor_of({4, 4})),
                                 Jump{"after"}}});
    user.blocks.push_back(Block{"after",
                                {Print{51, float32_tensor(), 53, int_type},
                                 ConstantInt{54, "0", int_type}, Return{54, int_type}}});
    // The block ends right after the first call.
    user.blocks.push_back(Block{"open", {call(31, "alias.first", {30})}});
    module.functions.push_back(std::move(user));

    Function spread;
    spread.name = "spread";
    spread.source_file = "/virtual/user/main.qui";
    spread.result = float32_tensor();
    spread.blocks.push_back(
        Block{"entry",
              {TensorBinary{3, "+", 1, 2, float32_tensor(), float32_tensor(), float32_tensor(), 0, 0},
               Jump{"later"}}});
    spread.blocks.push_back(
        Block{"later",
              {TensorBinary{4, "*", 3, 3, float32_tensor(), float32_tensor(), float32_tensor(), 0, 0},
               Return{4, float32_tensor()}}});
    module.functions.push_back(std::move(spread));
    return module;
}

// Calls that the conditional rules reject for reasons a lowered program never
// gives: a memory-reuse rule whose transfer finds no release of the owned
// temporary in its block (the temporary is released in another block, or not
// at all), an owned temporary used again after the call (in its block, and in
// another), a call whose arity differs from its callee's, and a rule that
// would clone an argument in a function whose value numbers are used up.
inline ir::Module conditional_rejections_no_source_program_reaches() {
    using namespace quidra::ir;
    Module module;
    module.compiler_extensions.push_back(tensor_region_extension(
        "graph",
        {
            {"operation.first", {{"function", "first"}, {"traits", "pure,tensor,differentiable"}}},
            {"operation.second", {{"function", "second"}, {"traits", "pure,tensor,differentiable"}}},
            {"operation.reuse", {{"function", "reuse"}, {"traits", "pure,tensor,differentiable"}}},
            {"operation.owned", {{"function", "owned"}, {"traits", "pure,tensor,differentiable"}}},
            {"memory.reuse",
             {{"operation", "first"}, {"replacement", "reuse"}, {"last_use", "true"}, {"owned", "true"}}},
            {"specialization.owned", {{"operation", "second"}, {"replacement", "owned"}}},
        }));

    module.functions.push_back(package_function("alias.first", {tensor_parameter("value", true)}));
    module.functions.push_back(package_function("alias.second", {tensor_parameter("value", true)}));
    module.functions.push_back(package_function("alias.reuse", {tensor_parameter("value", false)}));
    module.functions.push_back(package_function("alias.owned", {tensor_parameter("value", false)}));

    const auto int_type = Type::simple(TypeKind::Int64);
    Function user;
    user.name = "user";
    user.source_file = "/virtual/user/main.qui";
    user.result = int_type;
    // The owned temporary is never released.
    user.blocks.push_back(Block{"unreleased",
                                {TensorCreate{2, 1, std::nullopt, float32_tensor(), 1, 0, 0},
                                 call(3, "alias.first", {2}), Jump{"elsewhere"}}});
    // The owned temporary is released in the next block.
    user.blocks.push_back(Block{"elsewhere",
                                {TensorCreate{12, 11, std::nullopt, float32_tensor(), 1, 0, 0},
                                 call(13, "alias.first", {12}), Jump{"release"}}});
    user.blocks.push_back(Block{"release", {Release{12, float32_tensor()}, Jump{"arity"}}});
    // alias.first takes one argument.
    user.blocks.push_back(Block{"arity", {call(21, "alias.first", {}), Jump{"used_later"}}});
    // The owned temporary is used again after the call, in its block and
    // in the next one: the call is not its last use.
    user.blocks.push_back(Block{"used_later",
                                {TensorCreate{32, 31, std::nullopt, float32_tensor(), 1, 0, 0},
                                 call(33, "alias.first", {32}),
                                 Print{32, float32_tensor(), 34, int_type},
                                 TensorCreate{42, 41, std::nullopt, float32_tensor(), 1, 0, 0},
                                 call(43, "alias.first", {42}), Jump{"used_elsewhere"}}});
    user.blocks.push_back(Block{"used_elsewhere",
                                {Print{42, float32_tensor(), 44, int_type},
                                 ConstantInt{45, "0", int_type}, Return{45, int_type}}});
    module.functions.push_back(std::move(user));

    Function numbered;
    numbered.name = "numbered";
    numbered.source_file = "/virtual/user/main.qui";
    numbered.result = int_type;
    // The rule would clone the borrowed argument of alias.second into the
    // value after the largest number in the function, which does not exist.
    numbered.blocks.push_back(Block{
        "entry",
        {TensorCreate{2, 1, std::nullopt, float32_tensor(), 1, 0, 0}, call(3, "alias.second", {2}),
         Release{2, float32_tensor()},
         ConstantInt{std::numeric_limits<ValueId>::max(), "0", int_type},
         Return{std::numeric_limits<ValueId>::max(), int_type}}});
    module.functions.push_back(std::move(numbered));
    return module;
}

struct Fixture {
    std::string_view name;
    ir::Module (*build)();
};

// Every probe fixture, for the golden tool.
inline constexpr std::array<Fixture, 3> all{{
    {"descriptor_tables_the_frontend_refuses", descriptor_tables_the_frontend_refuses},
    {"chain_rejections_no_source_program_reaches", chain_rejections_no_source_program_reaches},
    {"conditional_rejections_no_source_program_reaches",
     conditional_rejections_no_source_program_reaches},
}};

} // namespace quidra::optimizer_probes
