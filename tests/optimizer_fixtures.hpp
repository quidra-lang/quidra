// Hand-built typed-IR modules that exercise the optimizer's package extension
// rewrites (fusion chains, memory reuse, conditional specialization, backend
// selection). No source program reaches these paths, so tests/compiler_tests.cpp
// checks the optimized results and the golden tool (tests/golden) compares
// them byte for byte across refactoring commits (`--fixture optimizer/NAME`).
//
// Each builder returns the module before ir::optimize().
#pragma once

#include "quidra/ir.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace quidra::optimizer_fixtures {
inline ir::Module calls_form_tensor_regions() {
    using namespace quidra;
    using namespace quidra::ir;

    const auto tensor =
        Type::tensor(Type::simple(TypeKind::Real32), 1);

    Module module;
    module.compiler_extensions.push_back(
        CompilerExtensionRegistration{
            "sample", "graph", "/virtual/sample",
            "/virtual/sample/compiler/graph.toml",
            "[extension]\nversion = 1\nphase = \"tensor-region\"\n",
            "tensor-region",
            {
                {"operation.first",
                 {{"function", "first"}, {"semantic", "opaque.first"},
                  {"traits", "pure,tensor"}}},
                {"operation.second",
                 {{"function", "second"}, {"semantic", "opaque.second"},
                  {"traits", "tensor, pure, differentiable"}}},
                {"operation.conv",
                 {{"function", "$method.sample.Conv.forward"},
                  {"semantic", "opaque.conv"},
                  {"traits", "pure,differentiable,tensor"}}},
                {"operation.stateful",
                 {{"function", "stateful"}, {"semantic", "opaque.stateful"},
                  {"traits", "tensor,stateful"}}},
                {"fusion.chain",
                 {{"operations", "first,second,conv"},
                  {"semantic", "opaque.chain"}}},
                {"optimization.fuse",
                 {{"stage", "fusion"}, {"backend", "sample"}}},
            }});

    Function first;
    first.name = "alias.first";
    first.source_file = "/virtual/sample/main.qui";
    first.result = tensor;
    module.functions.push_back(first);

    Function second;
    second.name = "__quidra_fs_alias_second_0123456789abcdef";
    second.source_file = "/virtual/sample/main.qui";
    second.result = tensor;
    module.functions.push_back(second);

    Function convolution;
    convolution.name = "$method.alias.Conv.forward";
    convolution.source_file = "/virtual/sample/main.qui";
    convolution.result = tensor;
    module.functions.push_back(convolution);

    Function stateful;
    stateful.name = "alias.stateful";
    stateful.source_file = "/virtual/sample/main.qui";
    stateful.result = tensor;
    module.functions.push_back(stateful);

    Function bridged;
    bridged.name = "bridged";
    bridged.source_file = "/virtual/user/bridged.qui";
    bridged.result = Type::simple(TypeKind::Int64);
    bridged.blocks.push_back(Block{"entry", {}});

    Call bridged_first;
    bridged_first.out = 1;
    bridged_first.callee = "alias.first";
    bridged_first.result = tensor;
    bridged.blocks.back().instructions.push_back(bridged_first);

    bridged.blocks.back().instructions.push_back(
        TensorContiguous{2, 1, tensor, 0, 0});

    Call bridged_second;
    bridged_second.out = 3;
    bridged_second.callee = "__quidra_fs_alias_second_0123456789abcdef";
    bridged_second.args.push_back(CallArgument{2, std::nullopt});
    bridged_second.result = tensor;
    bridged.blocks.back().instructions.push_back(bridged_second);

    Call bridged_convolution;
    bridged_convolution.out = 4;
    bridged_convolution.callee = "$method.alias.Conv.forward";
    bridged_convolution.args.push_back(CallArgument{3, std::nullopt});
    bridged_convolution.result = tensor;
    bridged.blocks.back().instructions.push_back(bridged_convolution);

    module.functions.push_back(std::move(bridged));

    Function user;
    user.name = "$entry";
    user.source_file = "/virtual/user/main.qui";
    user.result = Type::simple(TypeKind::Int64);
    user.blocks.push_back(Block{"entry", {}});

    Call first_call;
    first_call.out = 1;
    first_call.callee = "alias.first";
    first_call.result = tensor;
    user.blocks.back().instructions.push_back(first_call);

    Call second_call;
    second_call.out = 2;
    second_call.callee = "__quidra_fs_alias_second_0123456789abcdef";
    second_call.args.push_back(CallArgument{1, std::nullopt});
    second_call.result = tensor;
    user.blocks.back().instructions.push_back(second_call);

    Call convolution_call;
    convolution_call.out = 3;
    convolution_call.callee = "$method.alias.Conv.forward";
    convolution_call.args.push_back(CallArgument{2, std::nullopt});
    convolution_call.result = tensor;
    user.blocks.back().instructions.push_back(convolution_call);

    Call stateful_call;
    stateful_call.out = 4;
    stateful_call.callee = "alias.stateful";
    stateful_call.args.push_back(CallArgument{3, std::nullopt});
    stateful_call.result = tensor;
    user.blocks.back().instructions.push_back(stateful_call);

    module.functions.push_back(std::move(user));
    return module;
}
inline ir::Module replacement_rewrites_pure_chain() {
    using namespace quidra;
    using namespace quidra::ir;

    const auto tensor =
        Type::tensor(Type::simple(TypeKind::Real32), 1);
    const auto generic_tensor =
        Type::tensor(Type::simple(TypeKind::Real32));
    const auto side_type = Type::class_type("Side");

    Module module;
    module.compiler_extensions.push_back(
        CompilerExtensionRegistration{
            "sample", "graph", "/virtual/sample",
            "/virtual/sample/compiler/graph.toml",
            "[extension]\nversion = 1\nphase = \"tensor-region\"\n",
            "tensor-region",
            {
                {"operation.first",
                 {{"function", "first"},
                  {"traits",
                   "pure,tensor,differentiable,training-sensitive,effect:stable"}}},
                {"operation.relu",
                 {{"function", "relu"},
                  {"traits", "pure,tensor,differentiable"}}},
                {"operation.fused",
                 {{"function", "fused"},
                  {"traits",
                   "pure,tensor,differentiable,training-sensitive,effect:stable,fusion-target"}}},
                {"fusion.first_relu",
                 {{"operations", "first,relu"},
                  {"replacement", "fused"}}},
            }});

    Function first;
    first.name = "alias.first";
    first.source_file = "/virtual/sample/main.qui";
    first.result = tensor;
    module.functions.push_back(first);

    Function relu;
    relu.name = "alias.relu";
    relu.source_file = "/virtual/sample/main.qui";
    relu.parameters.push_back(
        ir::Parameter{"value", tensor, false, false, false});
    // Model a later method receiver: borrowed and non-writable, but not
    // source-spelled const.
    relu.parameters.push_back(
        ir::Parameter{"side", side_type, false, true, false});
    relu.result = tensor;
    module.functions.push_back(relu);

    Function fused;
    fused.name = "alias.fused";
    fused.source_file = "/virtual/sample/main.qui";
    // A package replacement may safely strengthen a borrowed side input to
    // const; it must never gain mutation authority.
    fused.parameters.push_back(
        ir::Parameter{"side", side_type, false, true, true});
    fused.result = generic_tensor;
    fused.blocks.push_back(Block{"entry", {}});

    Call fallback_first;
    fallback_first.out = 10;
    fallback_first.callee = "alias.first";
    fallback_first.result = tensor;
    fused.blocks.back().instructions.push_back(fallback_first);

    Call fallback_relu;
    fallback_relu.out = 11;
    fallback_relu.callee = "alias.relu";
    fallback_relu.args.push_back(
        CallArgument{10, std::nullopt});
    fallback_relu.result = tensor;
    fused.blocks.back().instructions.push_back(fallback_relu);
    module.functions.push_back(std::move(fused));

    Function user;
    user.name = "$entry";
    user.source_file = "/virtual/user/main.qui";
    user.result = Type::simple(TypeKind::Int64);
    user.blocks.push_back(Block{"entry", {}});

    Call first_call;
    first_call.out = 1;
    first_call.callee = "alias.first";
    first_call.result = tensor;
    user.blocks.back().instructions.push_back(first_call);

    Call relu_call;
    relu_call.out = 2;
    relu_call.callee = "alias.relu";
    relu_call.args.push_back(
        CallArgument{1, std::nullopt});
    relu_call.args.push_back(
        CallArgument{99, std::nullopt});
    relu_call.result = tensor;
    user.blocks.back().instructions.push_back(relu_call);

    module.functions.push_back(std::move(user));

    return module;
}
inline ir::Module replacement_crosses_eliminated_release() {
    using namespace quidra;
    using namespace quidra::ir;

    const auto tensor =
        Type::tensor(Type::simple(TypeKind::Real32), 1);

    Module module;
    module.compiler_extensions.push_back(
        CompilerExtensionRegistration{
            "sample", "graph", "/virtual/sample",
            "/virtual/sample/compiler/graph.toml",
            "[extension]\nversion = 1\nphase = \"tensor-region\"\n",
            "tensor-region",
            {
                {"operation.first",
                 {{"function", "first"},
                  {"traits", "pure,tensor,differentiable,higher-order"}}},
                {"operation.second",
                 {{"function", "second"},
                  {"traits", "pure,tensor,differentiable,higher-order"}}},
                {"operation.third",
                 {{"function", "third"},
                  {"traits", "pure,tensor,differentiable,higher-order"}}},
                {"operation.fused",
                 {{"function", "fused"},
                  {"traits",
                   "pure,tensor,differentiable,higher-order,fusion-target"}}},
                {"fusion.chain",
                 {{"operations", "first,second,third"},
                  {"replacement", "fused"}}},
            }});

    for (const auto& name :
         {std::string("first"), std::string("second"),
          std::string("third"), std::string("fused")}) {
        Function function;
        function.name = "alias." + name;
        function.source_file = "/virtual/sample/main.qui";
        function.parameters.push_back(
            ir::Parameter{"value", tensor, false, false, false});
        function.result = tensor;
        module.functions.push_back(std::move(function));
    }

    Function user;
    user.name = "$entry";
    user.source_file = "/virtual/user/main.qui";
    user.result = Type::simple(TypeKind::Int64);
    user.blocks.push_back(Block{"entry", {}});

    Call first;
    first.out = 1;
    first.callee = "alias.first";
    first.args.push_back(CallArgument{99, std::nullopt});
    first.result = tensor;
    user.blocks.back().instructions.push_back(first);

    Call second;
    second.out = 2;
    second.callee = "alias.second";
    second.args.push_back(CallArgument{1, std::nullopt});
    second.result = tensor;
    user.blocks.back().instructions.push_back(second);

    // Ordinary lowering releases the first intermediate after its final
    // consumer. A longer fusion must be able to cross and eliminate this
    // lifetime-only instruction rather than falling back to a shorter prefix.
    user.blocks.back().instructions.push_back(Release{1, tensor});

    Call third;
    third.out = 3;
    third.callee = "alias.third";
    third.args.push_back(CallArgument{2, std::nullopt});
    third.result = tensor;
    user.blocks.back().instructions.push_back(third);

    module.functions.push_back(std::move(user));
    return module;
}
inline ir::Module memory_reuse_respects_tensor_storage_aliases() {
    using namespace quidra;
    using namespace quidra::ir;

    const auto tensor =
        Type::tensor(Type::simple(TypeKind::Real32), 1);

    Module module;
    module.compiler_extensions.push_back(
        CompilerExtensionRegistration{
            "sample", "graph", "/virtual/sample",
            "/virtual/sample/compiler/graph.toml",
            "[extension]\nversion = 1\nphase = \"tensor-region\"\n",
            "tensor-region",
            {
                {"operation.relu",
                 {{"function", "relu"},
                  {"traits", "pure,tensor,differentiable,higher-order"}}},
                {"operation.relu_reuse",
                 {{"function", "relu_reuse"},
                  {"traits",
                   "pure,tensor,differentiable,higher-order,memory-reuse-target"}}},
                {"memory.relu",
                 {{"operation", "relu"},
                  {"replacement", "relu_reuse"},
                  {"dtype", "real32"},
                  {"device", "cpu"},
                  {"layout", "contiguous"},
                  {"tracked", "false"},
                  {"last_use", "true"},
                  {"owned", "true"}}},
            }});

    Function relu;
    relu.name = "alias.relu";
    relu.source_file = "/virtual/sample/main.qui";
    relu.parameters.push_back(
        ir::Parameter{"value", tensor, false, false, false});
    relu.result = tensor;
    module.functions.push_back(relu);

    Function reuse;
    reuse.name = "alias.relu_reuse";
    reuse.source_file = "/virtual/sample/main.qui";
    reuse.parameters.push_back(
        ir::Parameter{"value", tensor, false, false, false});
    reuse.result = tensor;
    module.functions.push_back(reuse);

    Function user;
    user.name = "$entry";
    user.source_file = "/virtual/user/main.qui";
    user.result = Type::simple(TypeKind::Int64);
    user.blocks.push_back(Block{"entry", {}});

    // A fresh temporary is unique and may use the package's in-place target.
    user.blocks.back().instructions.push_back(
        TensorCreate{1, 90, std::nullopt, tensor, 1, 0, 0});
    Call temporary_relu;
    temporary_relu.out = 2;
    temporary_relu.callee = "alias.relu";
    temporary_relu.args.push_back(CallArgument{1, std::nullopt});
    temporary_relu.result = tensor;
    user.blocks.back().instructions.push_back(temporary_relu);

    // A named tensor load is cloned for value independence. The clone has a
    // distinct descriptor but still shares TensorStorage. Native mutable access
    // now detaches through Core's copy-on-write boundary, but that allocation is
    // not buffer reuse, so a shared clone must not satisfy owned=true.
    user.blocks.back().instructions.push_back(
        TensorCreate{3, 91, std::nullopt, tensor, 1, 0, 0});
    user.blocks.back().instructions.push_back(
        StoreLocal{"named", 3, tensor});
    user.blocks.back().instructions.push_back(
        LoadLocal{4, "named", tensor});
    user.blocks.back().instructions.push_back(
        Clone{5, 4, tensor});
    Call aliased_relu;
    aliased_relu.out = 6;
    aliased_relu.callee = "alias.relu";
    aliased_relu.args.push_back(CallArgument{5, std::nullopt});
    aliased_relu.result = tensor;
    user.blocks.back().instructions.push_back(aliased_relu);

    module.functions.push_back(std::move(user));
    return module;
}
inline ir::Module conditional_replacement_preserves_owned_argument_lifetime() {
    using namespace quidra;
    using namespace quidra::ir;

    const auto tensor =
        Type::tensor(Type::simple(TypeKind::Real32), 1);

    Module module;
    module.compiler_extensions.push_back(
        CompilerExtensionRegistration{
            "sample", "graph", "/virtual/sample",
            "/virtual/sample/compiler/graph.toml",
            "[extension]\nversion = 1\nphase = \"tensor-region\"\n",
            "tensor-region",
            {
                {"operation.portable",
                 {{"function", "portable"},
                  {"traits", "pure,tensor,differentiable,higher-order"}}},
                {"operation.inference",
                 {{"function", "inference"},
                  {"traits",
                   "pure,tensor,differentiable,higher-order,specialization-target"}}},
                {"specialization.inference",
                 {{"operation", "portable"},
                  {"replacement", "inference"},
                  {"dtype", "real32"},
                  {"tracked", "false"}}},
            }});

    Function portable;
    portable.name = "alias.portable";
    portable.source_file = "/virtual/sample/main.qui";
    portable.parameters.push_back(
        ir::Parameter{"value", tensor, false, false, false});
    portable.result = tensor;
    module.functions.push_back(portable);

    Function inference;
    inference.name = "alias.inference";
    inference.source_file = "/virtual/sample/main.qui";
    // Borrowing is inferred independently from the replacement body. It must
    // not block a source-signature-compatible compiler replacement.
    inference.parameters.push_back(
        ir::Parameter{"value", tensor, false, true, false});
    inference.result = tensor;
    module.functions.push_back(inference);

    Function user;
    user.name = "$entry";
    user.source_file = "/virtual/user/main.qui";
    user.result = Type::simple(TypeKind::Int64);
    user.blocks.push_back(Block{"entry", {}});

    user.blocks.back().instructions.push_back(
        TensorCreate{1, 90, std::nullopt, tensor, 1, 0, 0});
    Call call;
    call.out = 2;
    call.callee = "alias.portable";
    call.args.push_back(CallArgument{1, std::nullopt});
    call.result = tensor;
    user.blocks.back().instructions.push_back(call);

    module.functions.push_back(std::move(user));
    return module;
}
inline ir::Module conditional_replacement_clones_borrowed_argument() {
    using namespace quidra;
    using namespace quidra::ir;

    const auto tensor =
        Type::tensor(Type::simple(TypeKind::Real32), 1);

    Module module;
    module.compiler_extensions.push_back(
        CompilerExtensionRegistration{
            "sample", "graph", "/virtual/sample",
            "/virtual/sample/compiler/graph.toml",
            "[extension]\nversion = 1\nphase = \"tensor-region\"\n",
            "tensor-region",
            {
                {"operation.portable",
                 {{"function", "portable"},
                  {"traits", "pure,tensor,differentiable,higher-order"}}},
                {"operation.training",
                 {{"function", "training"},
                  {"traits",
                   "pure,tensor,differentiable,higher-order,training-target"}}},
                {"specialization.training",
                 {{"operation", "portable"},
                  {"replacement", "training"},
                  {"dtype", "real32"},
                  {"tracked", "false"}}},
            }});

    Function portable;
    portable.name = "alias.portable";
    portable.source_file = "/virtual/sample/main.qui";
    portable.parameters.push_back(
        ir::Parameter{"value", tensor, false, true, false});
    portable.result = tensor;
    module.functions.push_back(portable);

    Function training;
    training.name = "alias.training";
    training.source_file = "/virtual/sample/main.qui";
    training.parameters.push_back(
        ir::Parameter{"value", tensor, false, false, false});
    training.result = tensor;
    module.functions.push_back(training);

    Function user;
    user.name = "$entry";
    user.source_file = "/virtual/user/main.qui";
    user.result = Type::simple(TypeKind::Int64);
    user.blocks.push_back(Block{"entry", {}});
    user.blocks.back().instructions.push_back(
        TensorCreate{1, 90, std::nullopt, tensor, 1, 0, 0});
    Call call;
    call.out = 2;
    call.callee = "alias.portable";
    call.args.push_back(CallArgument{1, std::nullopt});
    call.result = tensor;
    user.blocks.back().instructions.push_back(call);
    user.blocks.back().instructions.push_back(Release{1, tensor});

    module.functions.push_back(std::move(user));
    return module;
}
inline ir::Module backend_selection_tracks_transfer_layout() {
    using namespace quidra;
    using namespace quidra::ir;

    const auto tensor =
        Type::tensor(Type::simple(TypeKind::Real32), 1);

    Module module;
    module.compiler_extensions.push_back(
        CompilerExtensionRegistration{
            "sample", "graph", "/virtual/sample",
            "/virtual/sample/compiler/graph.toml",
            "[extension]\nversion = 1\nphase = \"tensor-region\"\n",
            "tensor-region",
            {
                {"operation.portable",
                 {{"function", "portable"},
                  {"traits", "pure,tensor,differentiable,higher-order"}}},
                {"operation.gpu",
                 {{"function", "gpu"},
                  {"traits",
                   "pure,tensor,differentiable,higher-order,backend-target"}}},
                {"backend.gpu",
                 {{"operation", "portable"},
                  {"replacement", "gpu"},
                  {"device", "gpu"},
                  {"layout", "contiguous"},
                  {"tracked", "false"}}},
            }});

    Function portable;
    portable.name = "alias.portable";
    portable.source_file = "/virtual/sample/main.qui";
    portable.parameters.push_back(
        ir::Parameter{"value", tensor, false, false, false});
    portable.result = tensor;
    module.functions.push_back(portable);

    Function gpu;
    gpu.name = "alias.gpu";
    gpu.source_file = "/virtual/sample/main.qui";
    gpu.parameters.push_back(
        ir::Parameter{"value", tensor, false, false, false});
    gpu.result = tensor;
    module.functions.push_back(gpu);

    Function user;
    user.name = "$entry";
    user.source_file = "/virtual/user/main.qui";
    user.result = Type::simple(TypeKind::Int64);
    user.blocks.push_back(Block{"entry", {}});

    user.blocks.back().instructions.push_back(
        TensorCreate{1, 90, std::nullopt, tensor, 1, 0, 0});
    user.blocks.back().instructions.push_back(
        TensorTransfer{2, 1, ValueId{91}, tensor, 0, 0});
    Call call;
    call.out = 3;
    call.callee = "alias.portable";
    call.args.push_back(CallArgument{2, std::nullopt});
    call.result = tensor;
    user.blocks.back().instructions.push_back(call);

    module.functions.push_back(std::move(user));
    return module;
}
// The variant of replacement_rewrites_pure_chain whose fusion target lacks the
// safety traits of the chain it replaces; the rewrite must be rejected.
inline void drop_fused_safety_traits(ir::Module& module) {
    module.compiler_extensions.front()
        .tables["operation.fused"]["traits"] =
        "pure,tensor,fusion-target";
}

inline ir::Module replacement_rewrites_pure_chain_unsafe() {
    ir::Module module = replacement_rewrites_pure_chain();
    drop_fused_safety_traits(module);
    return module;
}

struct Fixture {
    std::string_view name;
    ir::Module (*build)();
};

// Every fixture, for the golden tool.
inline constexpr std::array<Fixture, 8> all{{
    {"calls_form_tensor_regions", calls_form_tensor_regions},
    {"replacement_rewrites_pure_chain", replacement_rewrites_pure_chain},
    {"replacement_rewrites_pure_chain_unsafe", replacement_rewrites_pure_chain_unsafe},
    {"replacement_crosses_eliminated_release", replacement_crosses_eliminated_release},
    {"memory_reuse_respects_tensor_storage_aliases", memory_reuse_respects_tensor_storage_aliases},
    {"conditional_replacement_preserves_owned_argument_lifetime", conditional_replacement_preserves_owned_argument_lifetime},
    {"conditional_replacement_clones_borrowed_argument", conditional_replacement_clones_borrowed_argument},
    {"backend_selection_tracks_transfer_layout", backend_selection_tracks_transfer_layout},
}};

} // namespace quidra::optimizer_fixtures
