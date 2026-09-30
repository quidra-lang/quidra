#include "quidra/compiler.hpp"
#include "quidra/ir.hpp"
#include "quidra/source_tools.hpp"
#include <iostream>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>
static void good(const std::string& s) {
    try {
        auto c=quidra::compile(s);
        (void)quidra::inspect_source_json(s,c.checked,"test.qui");
    } catch(const quidra::CompileErrors& e) {
        std::cerr<<"unexpected rejection: "<<e.what()<<"\n";
        for(const auto& d:e.diagnostics()) {
            std::cerr<<d.span.start.line<<":"<<d.span.start.column
                     <<" ["<<d.code<<"] "<<d.message<<"\n";
        }
        std::cerr<<s;
        std::exit(1);
    } catch(const quidra::CompileError& e) {
        const auto& d=e.diagnostic();
        std::cerr<<"unexpected rejection: "<<d.span.start.line<<":"<<d.span.start.column
                 <<" ["<<d.code<<"] "<<d.message<<"\n"<<s;
        std::exit(1);
    } catch(const std::exception& e){
        std::cerr<<"unexpected rejection: "<<e.what()<<"\n"<<s;
        std::exit(1);
    }
}
static void bad(const std::string& s) { try {(void)quidra::compile(s);}catch(const quidra::CompileErrors&){return;}catch(const quidra::CompileError&){return;}std::cerr<<"unexpected acceptance:\n"<<s;std::exit(1); }
static void inspect_contains(const std::string& s, const std::string& expected) {
    try {
        auto c = quidra::compile(s);
        const auto json = quidra::inspect_source_json(s, c.checked, "test.qui");
        if (json.find(expected) != std::string::npos) return;
        std::cerr << "inspect output missing expected fragment: " << expected << "\n" << json << "\n";
    } catch (const std::exception& e) {
        std::cerr << "unexpected inspect rejection: " << e.what() << "\n" << s;
    }
    std::exit(1);
}
static void inspect_no_source_contains(const std::string& s, const std::string& expected) {
    try {
        auto c = quidra::compile(s);
        quidra::InspectOptions options;
        options.include_source = false;
        const auto json = quidra::inspect_source_json(s, c.checked, "test.qui", options);
        if (json.find("\"source\":") != std::string::npos) {
            std::cerr << "inspect --no-source unexpectedly emitted source text\n" << json << "\n";
            std::exit(1);
        }
        if (json.find(expected) != std::string::npos) return;
        std::cerr << "inspect --no-source output missing expected fragment: " << expected << "\n"
                  << json << "\n";
    } catch (const std::exception& e) {
        std::cerr << "unexpected inspect --no-source rejection: " << e.what() << "\n" << s;
    }
    std::exit(1);
}
static void repl_ir_contains(
    const std::string& source, std::size_t replay_prefix_bytes,
    const std::string& expected) {
    const auto root = std::filesystem::temp_directory_path() / "quidra-compiler-tests";
    const auto path = root / "repl_ir_contains.qui";
    try {
        std::filesystem::create_directories(root);
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        auto c = quidra::compile_repl_file_source(
            path, source, {}, root, replay_prefix_bytes);
        if (std::filesystem::exists(path)) {
            std::cerr << "REPL root-source overlay unexpectedly created a source file\n";
            std::exit(1);
        }
        const auto dumped = quidra::ir::dump(c.compilation.ir);
        if (dumped.find(expected) != std::string::npos) return;
        std::cerr << "REPL typed IR output missing expected fragment: " << expected << "\n"
                  << dumped << "\n";
    } catch (const std::exception& e) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::cerr << "unexpected REPL IR dump rejection: " << e.what() << "\n" << source;
    }
    std::exit(1);
}

static void ir_contains(const std::string& s, const std::string& expected) {
    const auto root = std::filesystem::temp_directory_path() / "quidra-compiler-tests";
    const auto path = root / "ir_contains.qui";
    try {
        std::filesystem::create_directories(root);
        {
            std::ofstream out(path, std::ios::binary);
            if (!out) throw std::runtime_error("cannot create temporary Quidra source");
            out << s;
        }
        auto c = quidra::compile_file(path, {}, root);
        std::filesystem::remove(path);
        const auto dumped = quidra::ir::dump(c.ir);
        if (dumped.find(expected) != std::string::npos) return;
        std::cerr << "typed IR output missing expected fragment: " << expected << "\n"
                  << dumped << "\n";
    } catch (const std::exception& e) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::cerr << "unexpected IR dump rejection: " << e.what() << "\n" << s;
    }
    std::exit(1);
}
static void tensor_region_at_least(
    const std::string& source, std::size_t minimum_values,
    bool require_backward) {
    try {
        const auto compilation = quidra::compile(source);
        std::size_t largest = 0;
        bool reaches_backward = false;
        for (const auto& function : compilation.ir.functions) {
            for (const auto& region : function.tensor_regions) {
                if (region.values.size() > largest)
                    largest = region.values.size();
                reaches_backward =
                    reaches_backward || region.reaches_backward;
            }
        }
        if (largest >= minimum_values &&
            (!require_backward || reaches_backward))
            return;
        std::cerr << "tensor optimization region planning mismatch: largest="
                  << largest << ", backward=" << reaches_backward << "\n"
                  << source;
    } catch (const std::exception& e) {
        std::cerr << "unexpected tensor optimization planning rejection: "
                  << e.what() << "\n" << source;
    }
    std::exit(1);
}
static void ir_function_not_contains(
    const std::string& s, const std::string& function_marker,
    const std::string& unexpected) {
    try {
        const auto c = quidra::compile(s);
        const auto dumped = quidra::ir::dump(c.ir);
        const auto marker = dumped.find(function_marker);
        if (marker == std::string::npos) {
            std::cerr << "typed IR output missing function marker: " << function_marker << "\n"
                      << dumped << "\n";
            std::exit(1);
        }
        const auto begin = dumped.rfind("function ", marker);
        const auto end = dumped.find("\nend\n", marker);
        if (begin == std::string::npos || end == std::string::npos) {
            std::cerr << "cannot isolate typed IR function for marker: " << function_marker << "\n"
                      << dumped << "\n";
            std::exit(1);
        }
        const auto body = dumped.substr(begin, end + 5 - begin);
        if (body.find(unexpected) == std::string::npos) return;
        std::cerr << "typed IR function unexpectedly contains fragment: " << unexpected << "\n"
                  << body << "\n";
    } catch (const std::exception& e) {
        std::cerr << "unexpected typed IR generation rejection: " << e.what() << "\n" << s;
    }
    std::exit(1);
}
static void llvm_contains(const std::string& s, const std::string& expected) {
    try {
        const auto c = quidra::compile(s);
        if (c.llvm.find(expected) != std::string::npos) return;
        std::cerr << "LLVM output missing expected fragment: " << expected << "\n"
                  << c.llvm << "\n";
    } catch (const std::exception& e) {
        std::cerr << "unexpected LLVM generation rejection: " << e.what() << "\n" << s;
    }
    std::exit(1);
}
static void llvm_not_contains(const std::string& s, const std::string& unexpected) {
    try {
        const auto c = quidra::compile(s);
        if (c.llvm.find(unexpected) == std::string::npos) return;
        std::cerr << "LLVM output unexpectedly contains fragment: " << unexpected << "\n"
                  << c.llvm << "\n";
    } catch (const std::exception& e) {
        std::cerr << "unexpected LLVM generation rejection: " << e.what() << "\n" << s;
    }
    std::exit(1);
}
static void llvm_function_not_contains(
    const std::string& s, const std::string& function_marker,
    const std::string& unexpected) {
    try {
        const auto c = quidra::compile(s);
        const auto marker = c.llvm.find(function_marker);
        if (marker == std::string::npos) {
            std::cerr << "LLVM output missing function marker: " << function_marker << "\n"
                      << c.llvm << "\n";
            std::exit(1);
        }
        const auto begin = c.llvm.rfind("define ", marker);
        const auto end = c.llvm.find("\n}", marker);
        if (begin == std::string::npos || end == std::string::npos) {
            std::cerr << "cannot isolate LLVM function for marker: " << function_marker << "\n"
                      << c.llvm << "\n";
            std::exit(1);
        }
        const auto body = c.llvm.substr(begin, end + 2 - begin);
        if (body.find(unexpected) == std::string::npos) return;
        std::cerr << "LLVM function unexpectedly contains fragment: " << unexpected << "\n"
                  << body << "\n";
    } catch (const std::exception& e) {
        std::cerr << "unexpected LLVM generation rejection: " << e.what() << "\n" << s;
    }
    std::exit(1);
}
static void llvm_file_contains(const std::string& s, const std::string& expected) {
    const auto root = std::filesystem::temp_directory_path() / "quidra-compiler-tests";
    const auto path = root / "llvm_file_contains.qui";
    try {
        std::filesystem::create_directories(root);
        {
            std::ofstream out(path, std::ios::binary);
            if(!out) throw std::runtime_error("cannot create temporary Quidra source");
            out << s;
        }
        const auto c = quidra::compile_file(path, {}, root);
        std::filesystem::remove(path);
        if(c.llvm.find(expected) != std::string::npos) return;
        std::cerr << "file-aware LLVM output missing expected fragment: " << expected << "\n"
                  << c.llvm << "\n";
    } catch(const std::exception& e) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::cerr << "unexpected file-aware LLVM generation rejection: " << e.what() << "\n" << s;
    }
    std::exit(1);
}
static void bad_code(const std::string& s, const std::string& expected) {
    try { (void)quidra::compile(s); }
    catch (const quidra::CompileErrors& errors) {
        for (const auto& d : errors.diagnostics()) if (d.code == expected) return;
        std::cerr<<"wrong diagnostic code, expected "<<expected<<", got";
        for (const auto& d : errors.diagnostics()) std::cerr<<" "<<d.code;
        std::cerr<<"\n"<<s;
        std::exit(1);
    }
    catch (const quidra::CompileError& error) {
        if (error.diagnostic().code == expected) return;
        std::cerr<<"wrong diagnostic code, expected "<<expected<<" but got "<<error.diagnostic().code<<"\n"<<s; std::exit(1);
    }
    std::cerr<<"unexpected acceptance, expected "<<expected<<":\n"<<s; std::exit(1);
}
static void bad_message(const std::string& s, const std::string& code,
                        const std::string& fragment) {
    try { (void)quidra::compile(s); }
    catch (const quidra::CompileErrors& errors) {
        for (const auto& d : errors.diagnostics()) {
            if (d.code == code && d.message.find(fragment) != std::string::npos) return;
        }
        std::cerr << "missing diagnostic " << code << " containing '" << fragment << "'\n" << s;
        std::exit(1);
    }
    catch (const quidra::CompileError& error) {
        const auto& d = error.diagnostic();
        if (d.code == code && d.message.find(fragment) != std::string::npos) return;
        std::cerr << "missing diagnostic " << code << " containing '" << fragment << "'\n" << s;
        std::exit(1);
    }
    std::cerr << "unexpected acceptance, expected " << code << " containing '" << fragment
              << "':\n" << s;
    std::exit(1);
}
static void root_source_override_with_import() {
    const auto root = std::filesystem::temp_directory_path() / "quidra-root-source-override";
    const auto main_path = root / "main.qui";
    const auto lib_path = root / "lib.qui";
    try {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        {
            std::ofstream out(lib_path, std::ios::binary);
            out << "int doubled(int value)\n    return value * 2\n";
        }
        {
            std::ofstream out(main_path, std::ios::binary);
            out << "print(unknown_name)\n";
        }
        const std::string source =
            "import lib = \"./lib.qui\"\n"
            "int value = lib.doubled(21)\n"
            "print(value)\n";
        (void)quidra::check_file_source(main_path, source, {}, root);
        std::filesystem::remove_all(root);
    } catch (const std::exception& e) {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        std::cerr << "root source override with import failed: " << e.what() << "\n";
        std::exit(1);
    }
}


static void string_input_ignores_package_lock() {
    const auto root = std::filesystem::temp_directory_path() / "quidra-string-package-lock";
    const auto original = std::filesystem::current_path();
    try {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        {
            std::ofstream out(root / "quidra.lock", std::ios::binary);
            if (!out) throw std::runtime_error("cannot create temporary package lock");
            // Deliberately invalid content proves the in-memory string APIs do
            // not consult a project lockfile at all. This test is not a legacy
            // lockfile compatibility fixture.
            out << "not-a-valid-package-lock\n";
        }
        std::filesystem::current_path(root);
        (void)quidra::check("print(int(1))\n");
        (void)quidra::compile("print(int(1))\n");
        std::filesystem::current_path(original);
        std::filesystem::remove_all(root);
    } catch (const std::exception& e) {
        std::error_code ignored;
        std::filesystem::current_path(original, ignored);
        std::filesystem::remove_all(root, ignored);
        std::cerr << "string input package lock isolation failed: " << e.what() << "\n";
        std::exit(1);
    }
}


static void package_extension_calls_form_tensor_regions() {
    using namespace quidra;
    using namespace quidra::ir;

    const auto tensor =
        Type::tensor(Type::simple(TypeKind::Float32), 1);

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
    bridged.result = Type::simple(TypeKind::Int);
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
    user.result = Type::simple(TypeKind::Int);
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
    module = optimize(std::move(module));

    const auto& bridged_entry =
        module.functions[module.functions.size() - 2];
    if (bridged_entry.tensor_regions.size() != 1 ||
        !bridged_entry.tensor_regions.front()
             .compiler_fusion_candidates.empty()) {
        std::cerr
            << "package compiler fusion crossed an undeclared tensor operation\n";
        std::exit(1);
    }

    const auto& entry = module.functions.back();
    if (entry.tensor_regions.size() != 1 ||
        entry.tensor_regions.front().values !=
            std::vector<ValueId>({1, 2, 3}) ||
        entry.tensor_regions.front().compiler_extensions !=
            std::vector<std::string>{"sample.graph"} ||
        entry.tensor_regions.front().compiler_operations !=
            std::vector<std::string>{
                "sample.graph:first",
                "sample.graph:second",
                "sample.graph:conv"} ||
        entry.tensor_regions.front().compiler_fusion_candidates !=
            std::vector<std::string>{"sample.graph:fusion.chain"} ||
        std::find(
            entry.tensor_regions.front().compiler_extension_tables.begin(),
            entry.tensor_regions.front().compiler_extension_tables.end(),
            "sample.graph:optimization.fuse") ==
            entry.tensor_regions.front().compiler_extension_tables.end()) {
        std::cerr
            << "package compiler extension did not preserve pure tensor call boundaries\n";
        std::exit(1);
    }
}


static void package_extension_replacement_rewrites_pure_chain() {
    using namespace quidra;
    using namespace quidra::ir;

    const auto tensor =
        Type::tensor(Type::simple(TypeKind::Float32), 1);
    const auto generic_tensor =
        Type::tensor(Type::simple(TypeKind::Float32));
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
    user.result = Type::simple(TypeKind::Int);
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

    Module unsafe = module;
    unsafe.compiler_extensions.front()
        .tables["operation.fused"]["traits"] =
        "pure,tensor,fusion-target";
    unsafe = optimize(std::move(unsafe));
    const auto& unsafe_entry = unsafe.functions.back();
    if (unsafe_entry.blocks.size() != 1 ||
        unsafe_entry.blocks.front().instructions.size() != 2) {
        std::cerr
            << "package compiler replacement discarded safety traits\n";
        std::exit(1);
    }

    module = optimize(std::move(module));

    const auto& fallback = module.functions[2];
    if (fallback.blocks.size() != 1 ||
        fallback.blocks.front().instructions.size() != 2 ||
        !std::holds_alternative<Call>(
            fallback.blocks.front().instructions[0]) ||
        !std::holds_alternative<Call>(
            fallback.blocks.front().instructions[1])) {
        std::cerr
            << "package compiler replacement rewrote its own fallback\n";
        std::exit(1);
    }

    const auto& entry = module.functions.back();
    if (entry.blocks.size() != 1 ||
        entry.blocks.front().instructions.size() != 1) {
        std::cerr
            << "package compiler replacement did not collapse the pure chain\n";
        std::exit(1);
    }
    const auto* replacement =
        std::get_if<Call>(&entry.blocks.front().instructions.front());
    if (!replacement ||
        replacement->callee != "alias.fused" ||
        replacement->out != 2 ||
        replacement->result != tensor ||
        replacement->args.size() != 1 ||
        replacement->args.front().value != 99) {
        std::cerr
            << "package compiler replacement emitted the wrong target call\n";
        std::exit(1);
    }
    if (entry.tensor_regions.size() != 1 ||
        entry.tensor_regions.front().compiler_operations !=
            std::vector<std::string>{"sample.graph:fused"}) {
        std::cerr
            << "package compiler replacement did not rebuild tensor regions\n";
        std::exit(1);
    }
}


static void package_extension_replacement_crosses_eliminated_release() {
    using namespace quidra;
    using namespace quidra::ir;

    const auto tensor =
        Type::tensor(Type::simple(TypeKind::Float32), 1);

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
    user.result = Type::simple(TypeKind::Int);
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
    module = optimize(std::move(module));

    const auto& entry = module.functions.back();
    if (entry.blocks.size() != 1 ||
        entry.blocks.front().instructions.size() != 1) {
        std::cerr
            << "package compiler fusion could not cross an eliminated release\n";
        std::exit(1);
    }
    const auto* replacement =
        std::get_if<Call>(&entry.blocks.front().instructions.front());
    if (!replacement ||
        replacement->callee != "alias.fused" ||
        replacement->out != 3 ||
        replacement->args.size() != 1 ||
        replacement->args.front().value != 99) {
        std::cerr
            << "package compiler fusion emitted the wrong long-chain replacement\n";
        std::exit(1);
    }
}



static void package_extension_memory_reuse_respects_tensor_storage_aliases() {
    using namespace quidra;
    using namespace quidra::ir;

    const auto tensor =
        Type::tensor(Type::simple(TypeKind::Float32), 1);

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
                  {"dtype", "float32"},
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
    user.result = Type::simple(TypeKind::Int);
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
    module = optimize(std::move(module));

    const auto& entry = module.functions.back();
    std::vector<std::string> callees;
    for (const auto& instruction : entry.blocks.front().instructions) {
        if (const auto* call = std::get_if<Call>(&instruction))
            callees.push_back(call->callee);
    }
    if (callees !=
        std::vector<std::string>{"alias.relu_reuse", "alias.relu"}) {
        std::cerr
            << "package memory planning reused aliased tensor storage\n";
        std::exit(1);
    }
}



static void package_extension_conditional_replacement_preserves_owned_argument_lifetime() {
    using namespace quidra;
    using namespace quidra::ir;

    const auto tensor =
        Type::tensor(Type::simple(TypeKind::Float32), 1);

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
                  {"dtype", "float32"},
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
    user.result = Type::simple(TypeKind::Int);
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
    module = optimize(std::move(module));

    const auto& entry = module.functions.back();
    if (entry.blocks.size() != 1 ||
        entry.blocks.front().instructions.size() != 3) {
        std::cerr
            << "package specialization did not preserve owned argument lifetime\n";
        std::exit(1);
    }
    const auto* selected =
        std::get_if<Call>(&entry.blocks.front().instructions[1]);
    const auto* release =
        std::get_if<Release>(&entry.blocks.front().instructions[2]);
    if (!selected || selected->callee != "alias.inference" ||
        !release || release->value != 1 || release->type != tensor) {
        std::cerr
            << "package specialization mishandled owned-to-borrowed replacement\n";
        std::exit(1);
    }
}


static void package_extension_conditional_replacement_clones_borrowed_argument() {
    using namespace quidra;
    using namespace quidra::ir;

    const auto tensor =
        Type::tensor(Type::simple(TypeKind::Float32), 1);

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
                  {"dtype", "float32"},
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
    user.result = Type::simple(TypeKind::Int);
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
    module = optimize(std::move(module));

    const auto& entry = module.functions.back();
    if (entry.blocks.size() != 1 ||
        entry.blocks.front().instructions.size() != 4) {
        std::cerr
            << "package specialization did not clone borrowed argument\n";
        std::exit(1);
    }
    const auto* clone =
        std::get_if<Clone>(&entry.blocks.front().instructions[1]);
    const auto* selected =
        std::get_if<Call>(&entry.blocks.front().instructions[2]);
    const auto* release =
        std::get_if<Release>(&entry.blocks.front().instructions[3]);
    if (!clone || clone->value != 1 ||
        !selected || selected->callee != "alias.training" ||
        selected->args.size() != 1 ||
        selected->args.front().value != clone->out ||
        !release || release->value != 1 || release->type != tensor) {
        std::cerr
            << "package specialization mishandled borrowed-to-owned replacement\n";
        std::exit(1);
    }
}


static void package_extension_backend_selection_tracks_transfer_layout() {
    using namespace quidra;
    using namespace quidra::ir;

    const auto tensor =
        Type::tensor(Type::simple(TypeKind::Float32), 1);

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
    user.result = Type::simple(TypeKind::Int);
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
    module = optimize(std::move(module));

    const auto& entry = module.functions.back();
    const auto* selected =
        std::get_if<Call>(&entry.blocks.front().instructions.back());
    if (!selected || selected->callee != "alias.gpu") {
        std::cerr
            << "package backend selection lost contiguous transfer facts\n";
        std::exit(1);
    }
}

int main(){
    package_extension_calls_form_tensor_regions();
    package_extension_replacement_rewrites_pure_chain();
    package_extension_replacement_crosses_eliminated_release();
    package_extension_memory_reuse_respects_tensor_storage_aliases();
    package_extension_conditional_replacement_preserves_owned_argument_lifetime();
    package_extension_conditional_replacement_clones_borrowed_argument();
    package_extension_backend_selection_tracks_transfer_layout();
 good(R"(bigint huge = 12345678901234567890123456789012345678901234567890
bigint one = 1
bigint sum = huge + one
print(sum)
print(NL)
)");
 good(R"(bigreal ratio = bigreal(1) / bigreal(3)
print(ratio)
print(NL)
)");
 good(R"(T identity<T>(T value)
    return value

bigint large = identity<bigint>(123456789012345678901234567890)
bigreal exact_value = identity<bigreal>(bigreal(1) / bigreal(3))
print(large)
print(NL)
print(exact_value)
print(NL)
)");
 good(R"(tensor<T> preserve<T: floating>(tensor<T> value)
    return value

tensor<float32> left = tensor.ones<float32>([2])
tensor<float32> right = tensor.ones<float32>([2])
tensor<float32> product = preserve(left * right)
tensor<float32> scaled = preserve(product * float32(2))
print(scaled[0].item())
print(NL)
)");
 good(R"(tensor<T> preserve_grad<T: floating>(tensor<T> value)
    return value

tensor<float32> root = tensor.ones<float32>([1]).track()
(root * root).backward(&root)
tensor<float32> gradient = preserve_grad(root.grad)
print(gradient[0].item())
print(NL)
)");
 good(R"(bigint[] values = [
    123456789012345678901234567890,
    2,
]
bigreal[] reals = bigreal(values)
print(values[0])
print(NL)
print(reals[1])
print(NL)
)");
 llvm_contains(
     "bigint x = 123456789012345678901234567890\n",
     "call ptr @quidra_bigint_literal");
 bad_code("tensor<bigint> x = tensor<bigint>([1])\n", "INVALID_TYPE");
 bad_code("map.Map<bigreal, int> values = map.Map<bigreal, int>()\n", "STANDARD_KEY_TYPE");
 bad_code("set.Set<bigreal> values = set.Set<bigreal>()\n", "STANDARD_KEY_TYPE");
 llvm_not_contains(
     "map.Map<int, int> values = map.Map<int, int>()\nvalues.set(1, 2)\nauto value = values.get(1)\n",
     "call ptr @quidra_format_signed");
 llvm_not_contains(
     "set.Set<uint64> values = set.Set<uint64>()\nuint64 key = uint64(7)\nvalues.add(key)\nbool present = values.has(key)\n",
     "call ptr @quidra_format_unsigned");
 llvm_not_contains(
     "map.Map<bigint, int> values = map.Map<bigint, int>()\nbigint key = bigint(7)\nvalues.set(key, 2)\nauto value = values.get(key)\n",
     "call ptr @quidra_bigint_text");
 // Standard map/set slot capacities are compiler-owned powers of two, so hot
 // probing uses masking rather than signed remainder and branchy wraparound.
 llvm_function_not_contains(
     "map.Map<int, int> values = map.Map<int, int>()\nvalues.set(1, 2)\nauto value = values.get(1)\n",
     "___slot_of(", "srem i64");
 llvm_function_not_contains(
     "set.Set<int> values = set.Set<int>()\nvalues.add(1)\nbool present = values.has(1)\n",
     "___slot_of(", "srem i64");
 // Map/Set scalar fields have compiler-owned initializers. Array-copy paths
 // inside these methods may still need element initialization checks, so scope
 // this regression to the typed-IR address path used by scalar compound updates.
 ir_function_not_contains(
     "map.Map<int, int> values = map.Map<int, int>()\nvalues.set(1, 2)\n",
     ".set(", "address.load");
 ir_function_not_contains(
     "map.Map<int, int> values = map.Map<int, int>()\nvalues.set(1, 2)\n",
     ".set(", "address.store");

 // Compiler-owned +1 induction variables are bounded by their loop
 // conditions, so they do not need a checked-overflow branch on every iteration.
 llvm_function_not_contains(R"(void walk_range()
    for i in range(0, 8)
        print(i)
        print(NL)

walk_range()
)", "@n_walk_range(", "@llvm.sadd.with.overflow.i64");
 llvm_function_not_contains(R"(void walk_array(const int[] &values)
    for value in values
        print(value)
        print(NL)

int[] values = array(8, fill = 1)
walk_array(&values)
)", "@n_walk_array(", "@llvm.sadd.with.overflow.i64");

 // Empty-string checks do not need a Unicode code-point count. Quidra text
 // has a NUL-terminated, no-embedded-NUL representation, so len(text) == 0
 // lowers to one byte load while all non-empty length queries keep full
 // code-point semantics.
 ir_contains(R"(bool is_empty(string text)
    return len(text) == 0
)", "string.empty");
 ir_contains(R"(bool is_nonempty(string text)
    return 0 != len(text)
)", "string.nonempty");
 llvm_function_not_contains(R"(bool is_empty(string text)
    return len(text) == 0
)", "@n_is_empty(", "@quidra_string_length");
 llvm_function_not_contains(R"(bool is_nonempty(string text)
    return 0 != len(text)
)", "@n_is_nonempty(", "@quidra_string_length");

 // Hot string/parse patterns keep their source semantics while lowering to
 // allocation-light native operations.
 ir_contains(R"(for i in range(0, 2)
    string[] fields = [i.string(), " ", NL]
    string line = fields.join("")
    print(line)
    print(NL)
)", "string.build");
 llvm_contains(R"(string content = ""
int written = 0
for i in range(0, 2)
    string[] fields = [i.string(), " ", i.string(), NL]
    string line = fields.join("")
    content = content + line
    written += len(line)
print(written)
print(NL)
)", "@quidra_string_build_append_move");
 ir_contains(R"(string content = ""
int written = 0
for i in range(0, 2)
    string[] fields = [i.string(), " ", i.string(), NL]
    string line = fields.join("")
    content = content + line
    written += len(line)
print(written)
print(NL)
)", "string.build_append");
 llvm_contains(R"(string content = ""
int written = 0
for i in range(0, 2)
    string[] fields = [i.string(), " ", i.string(), NL]
    string line = fields.join("")
    content = content + line
    written += len(line)
print(written)
print(NL)
)", "call ptr @quidra_string_build_append_move_unique_direct");
 llvm_contains(R"(string content = ""
int written = 0
for i in range(0, 2)
    string[] fields = [i.string(), " ", i.string(), NL]
    string line = fields.join("")
    content = content + line
    written += len(line)
print(written)
print(NL)
)", "store i8 4");
 llvm_not_contains(R"(string content = ""
int written = 0
for i in range(0, 2)
    string[] fields = [i.string(), " ", i.string(), NL]
    string line = fields.join("")
    content = content + line
    written += len(line)
print(written)
print(NL)
)", "call i64 @quidra_string_build_append_last_length");
 llvm_contains(R"(for i in range(0, 2)
    string[] fields = [i.string(), " ", NL]
    string line = fields.join("")
    print(line)
    print(NL)
)", "@quidra_string_build");
 ir_contains(R"(string text = "a b"
for i in range(0, len(text))
    if text[i] == " "
        print(i)
        print(NL)
)", "string.index_ascii_compare");
 ir_contains(R"(string text = "a b a"
int spaces = 0
for i in range(0, len(text))
    if text[i] == " "
        spaces += 1
print(spaces)
print(NL)
)", "string.ascii_count_prefix");
 llvm_contains(R"(string text = "a b a"
int spaces = 0
for i in range(0, len(text))
    if text[i] == " "
        spaces += 1
print(spaces)
print(NL)
)", "call i64 @quidra_string_count_ascii_prefix");
 ir_contains(R"(string source = "ab"
uint8[] data = uint8[](source.utf8())
match string.from_utf8(bin(data))
    string decoded
        print(decoded)
        print(NL)
    error problem
        print(problem)
        print(NL)
)", "string.from_utf8_array_direct");
 ir_contains(R"(match int.parse("42")
    int value
        print(value)
        print(NL)
    error problem
        print(problem)
        print(NL)
)", "parse.direct");
 ir_contains(R"(int parse_decimal(string text)
    match int.parse(text)
        int value
            return value
        error problem
            process.exit(1)

string line = "12 34"
for i in range(0, 1)
    string[] fields = line.split(" ")
    int left = parse_decimal(fields[0])
    int right = parse_decimal(fields[1])
    print(left + right)
    print(NL)
)", "string.parse_two_signed");
 llvm_contains(R"(int parse_decimal(string text)
    match int.parse(text)
        int value
            return value
        error problem
            process.exit(1)

string line = "12 34"
for i in range(0, 1)
    string[] fields = line.split(" ")
    int left = parse_decimal(fields[0])
    int right = parse_decimal(fields[1])
    print(left + right)
    print(NL)
)", "call i1 @__quidra_string_parse_two_signed_fast");
 ir_contains(R"(string line = "12 34"
for i in range(0, 1)
    string[] fields = line.split(" ")
    int left = int.parse(fields[0])
    int right = int.parse(fields[1])
    print(left + right)
    print(NL)
)", "string.parse_two_signed");
 llvm_contains(R"(string line = "12 34"
for i in range(0, 1)
    string[] fields = line.split(" ")
    int left = int.parse(fields[0])
    int right = int.parse(fields[1])
    print(left + right)
    print(NL)
)", "call i1 @__quidra_string_parse_two_signed_fast");
 llvm_not_contains(R"(int parse_decimal(string text)
    match int.parse(text)
        int value
            return value
        error problem
            process.exit(1)

string line = "12 34"
for i in range(0, 1)
    string[] fields = line.split(" ")
    int left = parse_decimal(fields[0])
    int right = parse_decimal(fields[1])
    print(left + right)
    print(NL)
)", "call i1 @quidra_string_parse_two_signed(");
 ir_contains(R"(string text = "a b"
for part in text.split(" ")
    print(part)
    print(NL)
)", "string.split_iter.begin");
 ir_contains(R"(string text = "a b"
for outer in range(0, 1)
    string[] parts = text.split(" ")
    int index = 0
    for part in parts
        print(part)
        print(NL)
        index += 1
)", "string.split_iter.begin");
 llvm_contains(R"(string text = "a b"
string[] parts = text.split(" ")
for part in parts
    print(part)
    print(NL)
)", "call ptr @quidra_string_split_iter_begin_move");
 llvm_not_contains(R"(string text = "a b"
string[] parts = text.split(" ")
for part in parts
    print(part)
    print(NL)
print(text)
print(NL)
)", "call ptr @quidra_string_split_iter_begin_move");

 // Nonnegative modulo invariants prove hot integer arithmetic safe without
 // weakening the default overflow semantics.
 ir_contains(R"(int bounded_hash(const uint8[] &data, int size)
    int h = 0
    for i in range(0, size)
        h = (h * 131 + int(data[i])) % 1000000007
    return h
)", "no-overflow");
 llvm_contains(R"(int bounded_hash(const uint8[] &data, int size)
    int h = 0
    for i in range(0, size)
        h = (h * 131 + int(data[i])) % 1000000007
    return h
)", "mul nsw i64");
 llvm_contains(R"(int checked_add(int value)
    return value + 1
)", "llvm.sadd.with.overflow.i64");

 // Proven dynamic-array loop bounds remove runtime slot checks only when the
 // array length relation is statically preserved.
 llvm_not_contains(R"(int[] data = array(8, fill = 0)
int size = len(data)
int total = 0
for i in range(0, size)
    total += data[i]
for i in range(0, size - 1)
    total += data[i + 1]
for i in range(0, size)
    total += data[size - 1 - i]
print(total)
print(NL)
)", "call ptr @quidra_array_slot(ptr");
 llvm_contains(R"(int[] data = array(8, fill = 0)
int size = len(data)
size = 4
print(data[size])
print(NL)
)", "call ptr @quidra_array_slot(ptr");
 llvm_contains(R"(int[] data = array(8, fill = 0)
int size = len(data)
for i in range(0, size)
    data = array(0, fill = 0)
    print(data[i])
    print(NL)
)", "call ptr @quidra_array_slot(ptr");
 // dynamic array loop bounds lower to proven slots only while the length relation
 // is loop invariant; replacing the array inside the loop keeps the checked path.
 ir_contains(R"(string[] values = ["a", "b", "c"]
for value in values
    print(value)
    print(NL)
)", "store.borrow $local.value.");
 // Immutable array elements are borrowed for a non-mutating loop; the backing
 // array keeps their shared storage alive for the whole iteration region.

 // Release lowering keeps the public function ABI but threads recursion depth
 // through an internal implementation instead of touching TLS on every direct
 // self-recursive call. Function values retain the ordinary ABI and guard path.
 llvm_contains(R"(int fib(int n)
    if n < 2
        return n
    return fib(n - 1) + fib(n - 2)
print(fib(10))
print(NL)
)", "define i64 @n_fib.depth(i64 %arg.n, i64 %quidra.depth)");
 llvm_contains(R"(int fib(int n)
    if n < 2
        return n
    return fib(n - 1) + fib(n - 2)
print(fib(10))
print(NL)
)", "call i64 @n_fib.depth(");
 llvm_not_contains(R"(int fib(int n)
    if n < 2
        return n
    return fib(n - 1) + fib(n - 2)
fn<int>(int) callback = fib
print(callback(10))
print(NL)
)", "@n_fib.depth");
 // depth-parameter self recursion

 good(R"(uint8 a = 240
uint8 b = 204
uint8 both = a AND b
uint8 either = a OR b
uint8 different = a XOR b
uint8 inverted = NOT a
uint8 left = a << 1
uint8 right = a >> 2
int8 signed_value = -8
int8 signed_right = signed_value >> 2
print(both)
print(NL)
print(either)
print(NL)
print(different)
print(NL)
print(inverted)
print(NL)
print(left)
print(NL)
print(right)
print(NL)
print(signed_right)
print(NL)
)");
 llvm_contains("uint8 a = 3\nuint8 b = a << 2\n", "shl i8");
 llvm_contains("uint8 a = 3\nuint8 b = NOT a\n", "xor i8");
 good("uint8 inverted = NOT 1\n");
 good("uint8 flags = 12\nuint8 mask = 10\nbool selected = flags AND mask == 8\n");
 good("uint8 flags = 1\nuint8 mask = 2\nbool selected = flags OR mask == 3\n");
 bad_code("bool a = true\nbool b = false\nbool c = a AND b\n", "TYPE_MISMATCH");
 bad_code("float a = 1.0\nfloat b = 2.0\nfloat c = a OR b\n", "TYPE_MISMATCH");
 bad_code("bigint a = 1\nbigint b = 2\nbigint c = a XOR b\n", "TYPE_MISMATCH");
 bad_code("uint8 a = 1\nuint8 b = a << 8\n", "SHIFT_COUNT");
 good(R"(class TensorState
    tensor<float32> value

class NestedState
    TensorState value
)");

 good("extern void scalar_abi(int8 a, int16 b, int32 c, int d, uint8 e, uint16 f, uint32 g, uint64 h, float32 i, float j, bool k) = \"scalar_abi\"\n");
 llvm_contains("extern bool c_bool(bool value) = \"c_bool\"\n", "declare zeroext i1 @c_bool(i1 zeroext)");
 llvm_contains("extern int8 c_i8(int8 value) = \"c_i8\"\n", "declare signext i8 @c_i8(i8 signext)");
 llvm_contains("extern int16 c_i16(int16 value) = \"c_i16\"\n", "declare signext i16 @c_i16(i16 signext)");
 llvm_contains("extern uint8 c_u8(uint8 value) = \"c_u8\"\n", "declare zeroext i8 @c_u8(i8 zeroext)");
 llvm_contains("extern uint16 c_u16(uint16 value) = \"c_u16\"\n", "declare zeroext i16 @c_u16(i16 zeroext)");
 llvm_contains("extern int8 c_i8(int8 value) = \"c_i8\"\nint8 x = 1\nint8 y = c_i8(x)\n", "call signext i8 @c_i8(i8 signext");
 good("extern int32 c_text(const string &text) = \"c_text\"\n");
 good("extern int32 c_bin(const bin &data) = \"c_bin\"\n");
 ir_contains("extern int32 c_text(const string &text) = \"c_text\"\n", "function c_text(const string &text) -> int32 = \"c_text\"");
 llvm_contains("extern int32 c_text(const string &text) = \"c_text\"\n", "declare i32 @c_text(ptr nocapture nonnull readonly, i64)");
 llvm_contains("extern int32 c_bin(const bin &data) = \"c_bin\"\n", "declare i32 @c_bin(ptr nocapture nonnull readonly, i64)");
 llvm_contains("extern int32 c_bin_mut(bin &data) = \"c_bin_mut\"\n", "declare i32 @c_bin_mut(ptr nocapture nonnull, i64)");
 good("extern int32 c_tensor(const tensor<float32> &input, tensor<float32> &output) = \"c_tensor\"\n");
 llvm_contains(
     "extern int32 c_tensor(const tensor<float32> &input, tensor<float32> &output) = \"c_tensor\"\n"
     "tensor<float32> input = tensor.ones<float32>([1])\n"
     "tensor<float32> output = tensor.zeros<float32>([1])\n"
     "int32 status = c_tensor(&input, &output)\n",
     "call i32 @c_tensor(ptr");
 bad_code("extern int32 c_tensor_value(tensor<float32> input) = \"c_tensor_value\"\n", "FFI_REFERENCE");
 llvm_contains("extern int32 c_text(const string &text) = \"c_text\"\nstring value = \"abc\"\nint32 result = c_text(&value)\n", "ffi.borrowed.value");
 llvm_contains("extern int32 c_text(const string &text) = \"c_text\"\nstring value = \"abc\"\nint32 result = c_text(&value)\n", "call i64 @strlen(ptr");
 llvm_contains("extern int32 c_bin(const bin &data) = \"c_bin\"\nbin value = bin.fill(24, 1)\nint32 result = c_bin(&value)\n", "ffi.bin.length");
 llvm_contains("extern int32 c_bin(const bin &data) = \"c_bin\"\nbin value = bin.fill(24, 1)\nint32 result = c_bin(&value)\n", "call i32 @c_bin(ptr nocapture nonnull readonly");
 llvm_contains("extern int32 c_bin_mut(bin &data) = \"c_bin_mut\"\nbin value = bin.fill(24, 1)\nint32 result = c_bin_mut(&value)\n", "call i32 @c_bin_mut(ptr nocapture nonnull");
 good("bin data = \"ok\".utf8()\nauto text = string.from_utf8(data)\n");
 llvm_contains("bin data = \"ok\".utf8()\nauto text = string.from_utf8(data)\n", "@quidra_bin_try_utf8");
 bad_code("auto text = string.from_utf8(\"not bin\")\n", "TYPE_MISMATCH");
 llvm_file_contains("tensor<float> source = tensor.ones<float>([1])\ntensor<float> value = source.track()\ntensor<float> next = value + 1.0\n", "@quidra_tensor_binary");
 llvm_file_contains("tensor<float32> source = tensor.ones<float32>([1])\ntensor<float32> value = source.track()\nuint8 scalar = 255\ntensor<float32> next = value + float32(scalar)\n", "uitofp i8");
 llvm_file_contains("tensor<float32> source = tensor.ones<float32>([1])\ntensor<float32> value = source.track()\nint8 scalar = -1\ntensor<float32> next = value + float32(scalar)\n", "sitofp i8");

 bad_code("extern int32 implicit_text(string text) = \"implicit_text\"\n", "FFI_REFERENCE");
 bad_code("extern int32 implicit_bytes(bin data) = \"implicit_bytes\"\n", "FFI_REFERENCE");
 good("extern int32 mutable_bytes(bin &data) = \"mutable_bytes\"\n");
 good("extern int c_apply(fn<int>(int) callback, int value) = \"c_apply\"\n");
 llvm_contains("extern int c_apply(fn<int>(int) callback, int value) = \"c_apply\"\n", "declare i64 @c_apply(ptr, i64)");
 llvm_contains(R"(int twice(int value)
    return value * 2
extern int c_apply(fn<int>(int) callback, int value) = "c_apply"
int result = c_apply(twice, 21)
)", "call i64 @c_apply(ptr");
 bad_code("extern int unsafe_callback(fn<int8>(int8) callback) = \"unsafe_callback\"\n", "FFI_CALLBACK_TYPE");
 bad_code("extern int unsafe_callback(fn<string>(int) callback) = \"unsafe_callback\"\n", "FFI_CALLBACK_TYPE");
 bad_code("extern int32 mutable_text(string &text) = \"mutable_text\"\n", "FFI_REFERENCE");
 bad_code("extern int32 const_value(const string text) = \"const_value\"\n", "FFI_REFERENCE");
 bad_code("extern int32 c_puts(const string &text) = \"puts\"\n", "FFI_SYMBOL_CONFLICT");
 bad_code("extern int32 c_main(int32 value) = \"main\"\n", "FFI_SYMBOL_CONFLICT");
 bad_code("extern int32 c_mangled(int32 value) = \"n_user_function\"\n", "FFI_SYMBOL_CONFLICT");
 bad_code("extern int32 c_runtime(int32 value) = \"quidra_future_runtime_symbol\"\n", "FFI_SYMBOL_CONFLICT");
 bad_code("extern int32 c_internal(int32 value) = \"__quidra_internal_symbol\"\n", "FFI_SYMBOL_CONFLICT");
 bad_code("extern int32 first(int32 value) = \"shared_symbol\"\nextern int32 second(int32 value) = \"shared_symbol\"\n", "FFI_SYMBOL_CONFLICT");
 good(R"(extern int32 generic_tensor_bridge<T: numeric>(tensor<T> &value) = "generic_tensor_bridge"
tensor<uint8> a = tensor.zeros<uint8>([1])
tensor<float32> b = tensor.zeros<float32>([1])
int32 first_result = generic_tensor_bridge(&a)
int32 second_result = generic_tensor_bridge(&b)
)");
 bad_code("extern string unsafe(int value) = \"unsafe_symbol\"\n", "FFI_TYPE");
 bad_code("extern bin unsafe_bin(int value) = \"unsafe_symbol\"\n", "FFI_TYPE");
 bad_code("extern int unsafe(int &value) = \"unsafe_symbol\"\n", "FFI_REFERENCE");
 bad_code("extern int unsafe(int value = 1) = \"unsafe_symbol\"\n", "FFI_DEFAULT");
 bad_code("extern int unsafe(int value) = \"bad-symbol\"\n", "FFI_SYMBOL");

 good(R"(int twice(int value)
    return value * 2

int apply(fn<int>(int) operation, int value)
    return operation(value)

fn<int>(int) operation = twice
int a = operation(3)
int b = apply(operation, 4)
print(a)
print(NL)
print(b)
print(NL)
)");
 ir_contains(R"(int twice(int value)
    return value * 2
fn<int>(int) operation = twice
int result = operation(3)
)", "fn.ref twice");
 ir_contains(R"(int twice(int value)
    return value * 2
fn<int>(int) operation = twice
int result = operation(3)
)", "call.indirect");
 llvm_contains(R"(int twice(int value)
    return value * 2
fn<int>(int) operation = twice
int result = operation(3)
)", "select i1 true, ptr @n_twice, ptr null");
 bad_code(R"(int twice(int value)
    return value * 2
fn<float>(int) operation = twice
)", "FUNCTION_REFERENCE_SIGNATURE");
 bad_code(R"(int mutate(int &value)
    value = value + 1
    return value
fn<int>(int) operation = mutate
)", "FUNCTION_REFERENCE_SIGNATURE");
 bad_code(R"(extern int32 foreign(int32 value) = "foreign"
fn<int32>(int32) operation = foreign
)", "FUNCTION_REFERENCE_EXTERN");
 bad_code(R"(int twice(int value)
    return value * 2
auto operation = twice
)", "FUNCTION_REFERENCE_CONTEXT");
 bad_code(R"(int twice(int value)
    return value * 2
fn<int>(int) left = twice
fn<int>(int) right = twice
bool same = left == right
)", "TYPE_MISMATCH");
 good(R"(void first()
    print("first")
    print(NL)

void second()
    print("second")
    print(NL)

task.all([first, second])
task.all([])
)");
 llvm_contains(R"(void first()
    print("first")
    print(NL)
task.all([first])
)", "call void @quidra_task_all");
 llvm_contains(R"(void first()
    print("first")
    print(NL)
task.all([first])
)", "@.quidra.stack.depth = internal thread_local global i64 0");
 bad_code(R"(int wrong()
    return 1
task.all([wrong])
)", "FUNCTION_REFERENCE_SIGNATURE");
 good(R"(int first_value()
    return 20
int second_value()
    return 22
int[] results = task.all([first_value, second_value])
int sum = results[0] + results[1]
)");
 llvm_contains(R"(int first_value()
    return 20
int second_value()
    return 22
int[] results = task.all([first_value, second_value])
)", "call void @quidra_task_all_i64");
 good(R"(float first_value()
    return float(20.0)
float second_value()
    return float(22.0)
float[] results = task.all([first_value, second_value])
)");
 good(R"(void increment(atomic.Counter counter)
    counter.add(1)
atomic.Counter counter = atomic.counter(0)
task.all([increment, increment], counter)
int value = counter.load()
)");
 good(R"(ref.Cell<int32> first = ref.Cell<int32>(value = int32(5))
ref.Cell<int32>[] cells = [first]
ref.Cell<int32> second = cells[0]
bool same = first.same(second)
second.value = int32(9)
int32 observed = first.value
)");
 llvm_contains(R"(void increment(atomic.Counter counter)
    counter.add(1)
atomic.Counter counter = atomic.counter(0)
task.all([increment, increment], counter)
)", "call void @quidra_task_all_atomic_counter");
 good(R"(const atomic.Counter counter = atomic.counter(0)
int value = counter.load()
)");
 bad_code(R"(const atomic.Counter counter = atomic.counter(0)
counter.add(1)
)", "WRITE_CAPABILITY");
 bad_code(R"(void mutate(const atomic.Counter &counter)
    counter.add(1)
)", "WRITE_CAPABILITY");
 bad_code(R"(fn<int>(int) operation
int result = operation(1)
)", "UNINITIALIZED");
 good(R"(int twice(int value)
    return value * 2
int apply_ref(const fn<int>(int) &operation, int value)
    return operation(value)
fn<int>(int) operation = twice
int result = apply_ref(&operation, 4)
print(result)
print(NL)
)");

 root_source_override_with_import();
 string_input_ignores_package_lock();
 {
  const auto compile_chain=[](std::string source,const std::string& term){
   for(int index=1;index<64;++index) source+=" + "+term;
   source+="\n";
   const auto root=std::filesystem::temp_directory_path()/"quidra-compiler-tests";
   const auto path=root/"deep-binary.qui";
   try {
    std::filesystem::create_directories(root);
    {
     std::ofstream out(path,std::ios::binary);
     if(!out) throw std::runtime_error("cannot create deep binary test source");
     out<<source;
    }
    (void)quidra::compile_file(path,{},root);
    std::filesystem::remove(path);
   } catch(const std::exception& e) {
    std::error_code ignored;
    std::filesystem::remove(path,ignored);
    std::cerr<<"unexpected deep binary rejection: "<<e.what()<<"\n"<<source;
    std::exit(1);
   }
  };
  compile_chain("string scalar_chain = \"x\"","\"x\"");
  compile_chain(
      "tensor<float32> tensor_value = tensor<float32>([1])\n"
      "tensor_value[0] = 1.0\n"
      "tensor<float32> tensor_chain = tensor_value",
      "tensor_value");
  compile_chain(
      "tensor<float32> tracked_source = tensor<float32>([1])\n"
      "tracked_source[0] = 1.0\n"
      "tensor<float32> tracked_value = tracked_source.track()\n"
      "tensor<float32> tracked_chain = tracked_value",
      "tracked_value");
 }
 for(const auto& s:std::vector<std::string>{
 R"(void replace(int[] &x, int count = 3)
    x = array(count, fill = 7)
int[] y = []
replace(&x = &y)
print(y[2])
print(NL)
)",
 R"(int | error read(bool ok)
    if ok
        return 1
    return error("bad")
string | int | error use(bool ok)
    auto x = try read(ok)
    return x + 1
int | error value = read(true)
match value
    int
        print(value + 1)
        print(NL)
    error e
        print(e)
        print(NL)
)",
 R"(void | error f()
    return
void | error g()
    try f()
    return
int | none x = none
match x
    none
        print("none")
        print(NL)
    int value
        print(value)
        print(NL)
)",
 R"(int x
if true
    x = 1
else
    x = 2
print(x)
print(NL)
int[2][2] matrix = [[1, 2], [3, 4]]
int[][] copy = matrix
copy[0][0] = 9
)",
 R"(int f(bool yes)
    int x
    if yes
        return 1
    else
        x = 2
    return x
int[] a = [
  1,
  2,
]
print(f(yes = true,))
print(NL)
)",
 R"(int[] values = array(2, fill = 0)
for &value in values
    value = 7
for i in range(0, 3, step = 1)
    print(i)
    print(NL)
)",
 R"(int f(int[] a = [1])
    a[0] = a[0] + 1
    return a[0]
print(f())
print(NL)
print(f())
print(NL)
)",
 R"(auto path = "C:\Users\data\image.png"
auto raw = "\n\t\r\b\f\v\a\u3042"
auto controls = NL + HT + CR + DQ + BS + FF + VT + BL
string separator = HT
print("A{separator}B")
print(NL)
print("nested {error("ok")}")
print(NL)
)",
 R"(class PrivateCounter
    private int value = 0

    private void increment_raw()
        value = value + 1

    void increment()
        increment_raw()

    int get()
        return value

PrivateCounter counter
counter.increment()
print(counter.get())
print(NL)
)",
 R"(class Point
    int x
    int y

    construct(int x_value, int y_value)
        x = x_value
        y = y_value

    int sum()
        return x + y

class LabeledPoint
    Point point
    string label

    construct(Point point_value, string label_value)
        point = point_value
        label = label_value

    int sum()
        return point.sum()

class OffsetPoint
    Point point
    int offset

    construct(Point point_value, int offset_value)
        point = point_value
        offset = offset_value

    int sum()
        return point.sum() + offset

class Box
    Point point

    construct(Point point_value)
        point = point_value

Point a = Point(1, 2)
Point b = a
b.x = 9
Point &alias = &a
alias.y = 5
LabeledPoint labeled = LabeledPoint(Point(3, 4), "p")
OffsetPoint offset = OffsetPoint(Point(3, 4), 10)
Box box = Box(a)
Box copy = box
copy.point.x = 99
print(a.sum())
print(NL)
print(labeled.sum())
print(NL)
print(offset.sum())
print(NL)
print(box.point.x)
print(NL)
)",
 R"(int x
int &y = &x
y = 10
print(x)
print(NL)
)",
 R"(void initialize(int &x)
    x = 7
int x
initialize(&x)
print(x)
print(NL)
)",
 R"(void accept_address(int &x)
    return
int x
accept_address(&x)
)",
 R"(int a = 1
int c = 2
int &b = &a
b = 5
&b = &c
b = 8
print(a)
print(NL)
print(c)
print(NL)
)",
 R"(class PartialPoint
    int x
    int y

PartialPoint p
p.x = 1
print(p.x)
print(NL)
PartialPoint q = p
q.y = 5
print(q.x)
print(NL)
print(q.y)
print(NL)
)",
 R"(int local_scope_value()
    int x = 5
    return x

int x = 7
print(local_scope_value())
print(NL)
print(x)
print(NL)
)",
 R"(class ResetCounter
    int value

    void reset()
        value = 0

    void increment()
        value = value + 1

ResetCounter counter
counter.reset()
counter.increment()
print(counter.value)
print(NL)
)",
 R"(class RefPoint
    int x
    int y

    construct(int x_value)
        x = x_value

RefPoint p = RefPoint(1)
int &field = &p.y
field = 5
print(p.y)
print(NL)
)",
 R"(int[] values = [1, 2, 3]
int &first = &values[0]
first = 9
values = [4, 5]
print(first)
print(NL)
print(values[0])
print(NL)
)",
 R"(class DefaultPoint
    int x = 1
    int y = 2

    construct(int x_value = 1)
        x = x_value

    int sum()
        return x + y

class DefaultOffset
    DefaultPoint point = DefaultPoint()
    int offset = 1

    int sum()
        return point.sum() + offset

DefaultPoint a
DefaultPoint b = DefaultPoint(1)
bool same = a == b
int[] left = [1, 2, 3]
int[] right = [1, 2, 3]
bool arrays_same = left == right
DefaultOffset shifted
print(a.sum())
print(NL)
print(shifted.sum())
print(NL)
print(same)
print(NL)
print(arrays_same)
print(NL)
)",
 R"(class ArrayEqualityPoint
    int x
    int y

    construct(int x_value, int y_value)
        x = x_value
        y = y_value

ArrayEqualityPoint[] left = [
    ArrayEqualityPoint(1, 2),
    ArrayEqualityPoint(3, 4),
]
ArrayEqualityPoint[] right = left
bool same = left == right
right[1] = ArrayEqualityPoint(3, 5)
bool different = left != right
print(same)
print(NL)
print(different)
print(NL)
)",
 R"(class DefaultBag
    int[] values = [1]

DefaultBag a
DefaultBag b
a.values[0] = 9
print(b.values[0])
print(NL)
)",
 R"(class Box<T>
    T value

    construct(T value_value)
        value = value_value

    T get()
        return value

T first<T>(T[] values)
    return values[0]

Box<int> box = Box<int>(7)
print(box.get())
print(NL)
print(first<int>([4, 5]))
print(NL)
)",
 R"(class GenericBase<T>
    T value

    construct(T value_value)
        value = value_value

    T read()
        return value

class GenericHolder<T>
    GenericBase<T> base

    construct(GenericBase<T> base_value)
        base = base_value

    T read()
        return base.read()

GenericHolder<int> child = GenericHolder<int>(GenericBase<int>(9))
print(child.read())
print(NL)
)",
 R"(class GenericMethod
    T identity<T>(T value)
        return value

GenericMethod g
print(g.identity<int>(8))
print(NL)
)",
 R"(class GenericParent

    construct()
        return
    T echo<T>(T value)
        return value

class GenericChild
    GenericParent parent

    construct(GenericParent parent_value)
        parent = parent_value

    T echo<T>(T value)
        return parent.echo<T>(value)

GenericChild child = GenericChild(GenericParent())
print(child.echo<int>(11))
print(NL)
)",
 R"(class NestedBox<T>
    T value

    construct(T value_value)
        value = value_value

NestedBox<NestedBox<int>> outer = NestedBox<NestedBox<int>>(NestedBox<int>(12))
print(outer.value.value)
print(NL)
)",
 R"(class OneArgMethod
    T choose<T>(T value)
        return value

class TwoArgMethod
    T choose<T, U>(T value, U ignored)
        return value

OneArgMethod one
print(one.choose<int>(13))
print(NL)
)",
 R"(class GoodGenericMethod
    T route<T>(T value)
        return value

class UnusedGenericMethod
    T route<T>(T value)
        return value + 1

GoodGenericMethod good
print(good.route<string>("ok"))
print(NL)
)",
 R"(class GenericRouter

    construct()
        return
    T pass<T>(T value)
        return value

class RouterFactory

    construct()
        return
    GenericRouter make()
        return GenericRouter()

    string run()
        return make().pass<string>("method")

GenericRouter make_router()
    return GenericRouter()

string use_router()
    return make_router().pass<string>("function")

auto router = GenericRouter()
GenericRouter[] routers = [GenericRouter()]
print(router.pass<string>("auto"))
print(NL)
print(routers[0].pass<int>(14))
print(NL)
print(RouterFactory().run())
print(NL)
print(use_router())
print(NL)
)",
 R"(class ReturnModel
    float bb

    construct(float bb_value)
        bb = bb_value

ReturnModel build_model()
    return ReturnModel(2.0)

ReturnModel model = build_model()
print(model.bb)
print(NL)
)",
 R"(class ReturnPoint
    int x
    int y

    construct(int x_value, int y_value = 0)
        x = x_value
        y = y_value

ReturnPoint choose_point(bool full)
    if full
        return ReturnPoint(1, 2)
    return ReturnPoint(3)

ReturnPoint point = choose_point(true)
print(point.x)
print(NL)
)",
 R"(class ForwardProduct
    int value

    construct(int value_value)
        value = value_value

ForwardProduct inner_build();

ForwardProduct outer_build()
    return inner_build()

ForwardProduct inner_build()
    return ForwardProduct(8)

ForwardProduct forward = outer_build()
print(forward.value)
print(NL)
)",
 R"(class MethodProduct
    int value

    construct(int value_value)
        value = value_value

class MethodFactory
    MethodProduct outer()
        return inner()

    MethodProduct inner()
        return MethodProduct(9)

MethodFactory factory
MethodProduct product = factory.outer()
print(product.value)
print(NL)
)",
 R"(class NestedData
    float[] ys

class NestedOwner
    NestedData data

    float first()
        data.ys = [3.0]
        return data.ys[0]

    void initialize()
        data.ys = [4.0]

NestedData empty
NestedOwner owner
owner.data = empty
print(owner.first())
print(NL)
owner.initialize()
print(owner.data.ys[0])
print(NL)
)",
 R"(class ReplaceInner
    int x
    int y

    construct(int x_value, int y_value)
        x = x_value
        y = y_value

class ReplaceOuter
    ReplaceInner inner

    construct(ReplaceInner inner_value)
        inner = inner_value

    void reset()
        ReplaceInner replacement
        replacement.x = 5
        replacement.y = 6
        inner = replacement

ReplaceOuter outer = ReplaceOuter(ReplaceInner(1, 2))
outer.reset()
print(outer.inner.x)
print(NL)
)",
 R"(class ConditionalInner
    int x
    int y

    construct(int x_value, int y_value)
        x = x_value
        y = y_value

class ConditionalOuter
    ConditionalInner inner

    construct(ConditionalInner inner_value)
        inner = inner_value

    void maybe_reset(bool replace)
        if replace
            ConditionalInner replacement
            replacement.x = 5
            replacement.y = 6
            inner = replacement

ConditionalOuter outer = ConditionalOuter(ConditionalInner(1, 2))
outer.maybe_reset(false)
print(outer.inner.x)
print(NL)
)",
 R"(class RepairInner
    int x
    int y

    construct(int x_value, int y_value)
        x = x_value
        y = y_value

class RepairOuter
    RepairInner inner

    construct(RepairInner inner_value)
        inner = inner_value

    void reset()
        RepairInner replacement
        replacement.x = 5
        replacement.y = 6
        inner = replacement

RepairOuter outer = RepairOuter(RepairInner(1, 2))
outer.reset()
print(outer.inner.y)
print(NL)
)",
 R"(int denominator = 0
int result = 10 / denominator
)",
 R"(int8 small = 127
int16 wider = int16(small)
uint8 byte_value = 255
uint32 count = 100
int total = int(count)
float exact_float = 1.0
float32 exact_small_float = 1.5
int8 casted = int8(100)
int exact_from_float = 3
string small_text = small.string()
string flag_text = true.string()
int | error parsed = int.parse("123")
float32 | error parsed_float = float32.parse("1.5")
bin allocated = bin.fill(8, 0)
string repeated = string.repeat("a", 3)
bin data = bin.fill(16, 0)
bin first = data[0]
bin slice = data[0:8]
uint8[] decoded = uint8[](data)
bin copy = data
copy[0] = bin.fill(1, 1)
bool same = data == copy
for value in data
    bin x = value
for &value in copy
    value = value
print(small_text)
)",
 R"(int local_name()
    int x = 5
    return x

int x = 7
print(local_name())
print(NL)
print(x)
print(NL)
)",
 "int x\nx = 4\nprint(x)\n", "auto x = int(41)\nprint(x)\n", "int end = 7\nprint(end)\n", "// comment only\nint x = 1 // trailing comment\nprint(x)\n"}) good(s);
 good("int exit = 7\nprint(exit)\n");
 good(R"(int wide = 300
auto | error narrowed = int8(wide)
match narrowed
    int8 value
        print(value)
        print(NL)
    error problem
        print(problem)
        print(NL)
)");
 good(R"(int wide = 100
int8 narrowed = int8(wide)
print(narrowed)
print(NL)
)");
 llvm_contains(R"(int wide = 100
int8 narrowed = int8(wide)
)", "@quidra_managed_release");
 llvm_contains(R"(int wide = 300
auto | error narrowed = int8(wide)
match narrowed
    int8 value
        print(value)
        print(NL)
    error problem
        print(problem)
        print(NL)
)", "cast.error");
 good(R"(bigint wide = 300
auto | error narrowed = int8(wide)
match narrowed
    int8 value
        print(value)
        print(NL)
    error problem
        print(problem)
        print(NL)
)");
 good(R"(bigreal fraction = 1.5
auto | error exact_value = bigint(fraction)
match exact_value
    bigint value
        print(value)
        print(NL)
    error problem
        print(problem)
        print(NL)
)");
 llvm_contains(R"(bigint wide = 300
auto | error narrowed = int8(wide)
match narrowed
    int8 value
        print(value)
        print(NL)
    error problem
        print(problem)
        print(NL)
)", "@quidra_bigint_try_i64");
 llvm_contains(R"(bigreal fraction = 1.5
auto | error exact_value = bigint(fraction)
match exact_value
    bigint value
        print(value)
        print(NL)
    error problem
        print(problem)
        print(NL)
)", "@quidra_bigreal_try_bigint");
 good(R"(bigint huge = 10000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
auto | error rounded = float(huge)
match rounded
    float value
        print(value)
        print(NL)
    error problem
        print(problem)
        print(NL)
)");
 llvm_contains(R"(bigint huge = 10000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
auto | error rounded = float(huge)
match rounded
    float value
        print(value)
        print(NL)
    error problem
        print(problem)
        print(NL)
)", "@quidra_bigint_try_float64");

 ir_contains(R"(bin | error parse_bits()
    return bin.parse("01")
auto | error parsed = parse_bits()
match parsed
    bin bits
        print(bits[0])
        print(NL)
    error problem
        print(problem)
        print(NL)
)", "release %");
 ir_contains(R"(bool | error parse_flag()
    bin bits = try bin.parse("1")
    return bool(bits)
auto | error parsed = parse_flag()
match parsed
    bool flag
        print(flag)
        print(NL)
    error problem
        print(problem)
        print(NL)
)", "release %");
 ir_contains(R"(bin | error update_bits()
    bin bits = bin.fill(1, 0)
    bin one = try bin.parse("1")
    bits[0] = one
    return bits
auto | error parsed = update_bits()
match parsed
    bin bits
        print(bits)
        print(NL)
    error problem
        print(problem)
        print(NL)
)", "release %");
 ir_contains(R"(bin | error parse_bits()
    return bin.parse("01")
auto | error parsed = parse_bits()
match parsed
    bin bits
        for bit in bits
            print(bit)
            print(NL)
    error problem
        print(problem)
        print(NL)
)", "bin.get");

 good(R"(bin | error literal = bin.parse("0101")
string text = "0101"
bin | error dynamic = bin.parse(text)
)");
 good("bin direct = bin.parse(\"0101\")\n");
 good(R"(string text = "0101"
bin direct = bin.parse(text)
)");
 bad_code("auto invalid = bin.parse(\"0102\")\n", "BIN_PARSE");
 llvm_not_contains(R"(auto | error parsed = bin.parse("0101")
match parsed
    bin bits
        print(bits)
        print(NL)
    error problem
        print(problem)
        print(NL)
)", "bin.parse.fail");
 llvm_contains(R"(string text = "0101"
auto | error parsed = bin.parse(text)
match parsed
    bin bits
        print(bits)
        print(NL)
    error problem
        print(problem)
        print(NL)
)", "bin.parse.fail");

 good(R"(float scalar = 3.0
float32 scalar32 = 2.0
float[] values = [1.0, 2.0, 3.0]

float half(float value)
    return value / 2.0

float negative = -3.0
float product = 2.0 * 3.0
float argument = half(3.0)
int8 small = 5
int8 sum = small + 100

print(scalar)
print(NL)
print(scalar32)
print(NL)
print(values[2])
print(NL)
print(negative)
print(NL)
print(product)
print(NL)
print(argument)
print(NL)
print(sum)
print(NL)
)");

 good(R"(class Pair
    int a
    int b

    construct(int a_value, int b_value)
        a = a_value
        b = b_value

Pair | error make_pair(bool ok)
    if not ok
        return error("bad")
    return Pair(2, 3)

int | error pair_sum(bool ok)
    Pair pair = try make_pair(ok)
    return pair.a + pair.b

auto | error result = pair_sum(true)
match result
    int value
        print(value)
        print(NL)
    error problem
        print(problem)
        print(NL)
)");

 const std::string repl_replay_surface = "print(\"A\")\nprint(\"B\")\n";
 repl_ir_contains(
     repl_replay_surface, std::string("print(\"A\")\n").size(),
     "repl.replay on");
 repl_ir_contains(
     repl_replay_surface, std::string("print(\"A\")\n").size(),
     "repl.replay off");

 const std::string const_ir_surface = R"(void inspect(const int &value)
    print(value)
    print(NL)

void modify(int &value)
    value = 1

int data = 0
inspect(&data)
modify(&data)
)";
 ir_contains(const_ir_surface, "function inspect(const int &value)");
 ir_contains(const_ir_surface, "function modify(int &value)");
 llvm_contains(const_ir_surface, "ptr nocapture nonnull readonly %arg.value");
 llvm_contains(const_ir_surface, "define void @n_modify(ptr nocapture nonnull %arg.value)");

 const std::string local_const_reference_ir = R"(int data = 1
const int &view = &data
int &writer = &data
const int frozen = 3
print(view)
print(NL)
writer = 2
print(frozen)
print(NL)
)";
 ir_contains(local_const_reference_ir, "const reference ");
 ir_contains(local_const_reference_ir, "reference.bind");
 inspect_no_source_contains(
     local_const_reference_ir,
     "\"inferred_type\":\"int\",\"authority\":\"read_only_reference\"");
 inspect_no_source_contains(
     local_const_reference_ir,
     "\"inferred_type\":\"int\",\"authority\":\"read_write_reference\"");
 inspect_no_source_contains(
     local_const_reference_ir,
     "\"inferred_type\":\"int\",\"authority\":\"const_value\"");
 inspect_no_source_contains(
     const_ir_surface,
     "\"parameter_authority\":{\"value\":\"read_only_reference\"}");
 inspect_no_source_contains(
     const_ir_surface,
     "\"parameter_authority\":{\"value\":\"read_write_reference\"}");
 bad_code(R"(int data = 1
const int &view = &data
int &writer = &view
)", "REFERENCE_BINDING");
 bad_code(R"(int data = 1
const int &view = &data
view = 2
)", "WRITE_CAPABILITY");
 bad_code(R"(int data = 1
int other = 2
const int &view = &data
&view = &other
)", "WRITE_CAPABILITY");

 tensor_region_at_least(R"(tensor<float32> x = tensor.ones<float32>([]).track()
((x * x + x) * x).backward(&x)
)", 3, true);

 const std::string ir_surface = R"(int source = 1
int &alias = &source
alias = 2
tensor<float32> a = tensor.zeros<float32>([2, 2])
tensor<float32> b = tensor.ones<float32>([2, 2])
tensor<float32> c = a + b
auto row = c[0, :]
)";
 for (const auto& fragment : std::vector<std::string>{
          "reference ", "reference.bind", "reference.store",
          "tensor.create", "tensor.binary", "tensor.index"}) {
     ir_contains(ir_surface, fragment);
 }
 for(const auto& s:std::vector<std::string>{
 "break\n", "continue\n",
 "int x\nprint(x)\n",
 "int x = 10 / 0\n",
 "int x = 10 % (3 - 3)\n",
 "uint8 x = 10\nprint(x / uint8(0))\n",
 "float x = .5\n",
 R"(class PartialReturn
    int x
    int y

    construct(int x_value)
        x = x_value

PartialReturn make_partial()
    return PartialReturn(1)

PartialReturn p = make_partial()
print(p.y)
print(NL)
)",
 R"(class ReplaceInnerBad
    int x
    int y

    construct(int x_value, int y_value)
        x = x_value
        y = y_value

class ReplaceOuterBad
    ReplaceInnerBad inner

    construct(ReplaceInnerBad inner_value)
        inner = inner_value

    void reset()
        ReplaceInnerBad replacement
        replacement.x = 5
        inner = replacement

ReplaceOuterBad outer = ReplaceOuterBad(ReplaceInnerBad(1, 2))
outer.reset()
print(outer.inner.y)
print(NL)
)",
 R"(class ConditionalInnerBad
    int x
    int y

    construct(int x_value, int y_value)
        x = x_value
        y = y_value

class ConditionalOuterBad
    ConditionalInnerBad inner

    construct(ConditionalInnerBad inner_value)
        inner = inner_value

    void maybe_reset(bool replace)
        if replace
            ConditionalInnerBad replacement
            replacement.x = 5
            inner = replacement

ConditionalOuterBad outer = ConditionalOuterBad(ConditionalInnerBad(1, 2))
outer.maybe_reset(false)
print(outer.inner.y)
print(NL)
)",
 "int x\nif true\n    x = 1\nprint(x)\n", "int x\nwhile false\n    x = 1\nprint(x)\n",
 "auto x\n", "none x = none\n", "auto x = none\n", "none f()\n    return none\n", "never x\n", "void[] x = []\n", "unit f()\n    return\n", "auto x = unit\n",
 "int[2] x = [1]\n", "int[] x = [1]\nint[2] y = x\n", "int[-1] x\n", "int x[2]\n",
 "int f()\n    return\n", "int f(auto x)\n    return 1\n", "int main()\n    return 0\n",
 "void f()\n  return\n", "void f()\n\treturn\n", "void f()\n        return\n",
 "auto x = some(1)\n", "result<int, error> x = success(1)\n", "auto x = array(3)\n",
 "void f(int &x)\n    x = 2\nint x = 1\nf(x)\n", "void f(int x)\n    return\nint x = 1\nf(&x)\n",
 "int f(int x = 1, int y)\n    return y\n", "void f(int &x = 1)\n    return\n", "void f(const int &x = 1)\n    return\n", "int f(int x, int y = x)\n    return y\n",
 "int f(int x)\n    return x\nprint(f(x: 1))\n", "int f(int x, int y)\n    return x\nprint(f(x = 1, 2))\n",
 "int | none x = 1\nmatch x\n    int\n        print(x)\n", "int | none x = 1\nmatch x\n    int\n        print(x)\n    int y\n        print(y)\n    none\n        print(int(0))\n",
 "int | none x = 1\nmatch x\n    int\n        x = none\n    none\n        print(int(0))\n",
 "auto x = 9223372036854775808\n", "auto x = []\n", "auto x = range(3)\n",
 "int8 x = 128\n", "uint8 x = -1\n", "int8 x = int8(300)\n",
 "bin x = bin(2, fill = 0)\n", "bin x = bin.fill(-1, 0)\n", "bin x = bin.fill(2, 2)\n", "string x = string(2, fill = \"a\")\n", "string x = string.repeat(\"a\", -1)\n",
 "string HT = \"x\"\n", "void f(string NL)\n    return\n",
 "int x = 1\nif true\n    int x = 2\n", "int x = 1\nint x = 2\n",
 "class A\n    int x\n    int x\n",
 "class A\n    int x\n    void set(int x)\n        return\n",
 "class A\n    int HT\n",
 "class A\n    int x\n    construct(int start)\n        x = start\nA a = A(1)\nA &b = a\n",
 "class A\n    int x\n    int y\n    construct(int start)\n        x = start\nA a = A(1)\nprint(a.y)\n",
 "class Counter\n    int value\n    void increment()\n        value = value + 1\nCounter c = Counter()\nc.increment()\n",
 "int x\nint &y = &x\nprint(y)\n",
 "int a = 1\nint b = 2\nint &r = &a\nr = &b\n",
 "int a = 1\nint b = 2\nint &r = &a\n&r = b\n",
 "int &x = &5\n",
 R"(class EqPartial
    int x
    int y

    construct(int x_value)
        x = x_value

EqPartial a = EqPartial(1)
EqPartial b = EqPartial(1)
bool same = a == b
)",
 R"(import x = "./x.qui"
)",
 R"(T generic<T>(T value)
    return value
print(generic<int, int>(1))
print(NL)
)",
 R"(int plain(int value)
    return value
print(plain<int>(1))
print(NL)
)",
 R"(class GenericOnly<T>
    T value
GenericOnly value
)",
 "#if DEBUG\nprint(int(1))\n", "# comment\nprint(int(1))\n"}) bad(s);
 good(R"(int super = 1
int override = 2
print(super + override)
print(NL)
)");
 good(R"(class Linear
    int unused = 0
class Rbf
    float gamma = 1.0

    construct()
        return
float apply(Linear | Rbf kernel)
    match kernel
        Linear
            return 0.0
        Rbf r
            return r.gamma
print(apply(Rbf()))
print(NL)
)");
 bad_code(R"(class Partial
    int x
    int y

    construct(int x_value)
        x = x_value
Partial value = Partial(1)
Partial | none maybe = value
)", "UNINITIALIZED_UNION_PAYLOAD");
 good(R"(class LocalReset
    int value
    void reset()
        value = 0
void run()
    LocalReset item
    item.reset()
    print(item.value)
    print(NL)
run()
)");
 bad_code(R"(class EarlyReceiver
    float[] values
    void | error initialize(int count)
        if count < 0
            return error("bad")
        values = array(count, fill = 0.0)
        return
EarlyReceiver item
auto result = item.initialize(-1)
print(item.values[0])
print(NL)
)", "UNINITIALIZED");
 bad_code(R"(class EarlyUnit
    int value
    void reset(bool ok)
        if not ok
            return
        value = 1
EarlyUnit item
item.reset(false)
print(item.value)
print(NL)
)", "UNINITIALIZED");
 bad_code(R"(void maybe_initialize(int &value, bool ok)
    if not ok
        return
    value = 1
int value
maybe_initialize(&value, false)
print(value)
print(NL)
)", "UNINITIALIZED");
 inspect_contains(R"(class SummaryReceiver
    int value
    void reset(bool ok)
        if not ok
            return
        value = 1
)", "\"function\":\"$method.SummaryReceiver.reset\",\"receiver\":{\"requires\":[],\"writes\":[\"value\"],\"initializes\":[],\"invalidates\":[]}");

 // Aliased reference preconditions are all checked against call-entry state.
 // A later write through another alias must not retroactively make an earlier
 // read requirement safe.
 bad_code(R"(void initialize_then_read(int &destination, const int &observation)
    destination = 1
    print(observation)
    print(NL)

int value
initialize_then_read(&value, &value)
)", "UNINITIALIZED");

 bad_code(R"(void initialize_then_read_writable(int &destination, int &observation)
    destination = 1
    print(observation)
    print(NL)

int value
initialize_then_read_writable(&value, &value)
)", "UNINITIALIZED");

 // Receiver/reference overlap follows the same rule. The method body writes the
 // receiver field first, but the aliased argument still has a call-entry read
 // requirement and cannot consume uninitialized storage.
 bad_code(R"(class AliasReceiver
    int value

    void initialize_then_read(int &observation)
        value = 1
        print(observation)
        print(NL)

AliasReceiver item
item.initialize_then_read(&item.value)
)", "UNINITIALIZED");

 // Aliasing itself is legal once every read precondition is satisfied.
 good(R"(void observe_and_update(const int &observation, int &destination)
    print(observation)
    print(NL)
    destination = observation + 1

int value = 4
observe_and_update(&value, &value)
print(value)
print(NL)
)");

 // Const authority is path-local and one-way: a readonly path may observe a
 // writable alias, but it cannot be promoted back into write authority.
 bad_code(R"(int value = 1
const int &view = &value
int &writer = &view
)", "REFERENCE_BINDING");

 bad_code(R"(class ConstNested
    int[] values

    construct(int[] values_value)
        values = values_value

const ConstNested item = ConstNested([1, 2])
item.values[0] = 3
)", "WRITE_CAPABILITY");

 bad_code(R"(class ConstMethod
    int value

    construct(int value_value)
        value = value_value

    void change_value()
        value = 2

const ConstMethod item = ConstMethod(1)
item.change_value()
)", "WRITE_CAPABILITY");

 good(R"(T observe_generic<T>(const T &value)
    return value

int source = 7
print(observe_generic(&source))
print(NL)
)");

 // A control-flow-dependent rebind must never let a later write be credited to
 // the pre-branch target.
 bad_code(R"(int left
int right = 2
int &slot = &left
bool choose = true
if choose
    &slot = &right
slot = 9
print(left)
print(NL)
)", "UNINITIALIZED");

 // Rebinding to possibly-uninitialized storage also invalidates the reference's
 // own read proof after the join.
 bad_code(R"(int left = 1
int right
int &slot = &left
bool choose = true
if choose
    &slot = &right
print(slot)
print(NL)
)", "UNINITIALIZED");

 // When every continuing path resolves to the same storage, precision is kept.
 good(R"(int left = 1
int right = 2
int &slot = &left
bool choose = true
if choose
    &slot = &right
else
    &slot = &right
slot = 9
print(right)
print(NL)
)");

 // A write through an ambiguous target initializes the reference path itself,
 // but still must not initialize either possible concrete root.
 good(R"(int left = 1
int right = 2
int &slot = &left
bool choose = true
if choose
    &slot = &right
slot = 9
print(slot)
print(NL)
)");

 // A loop may execute zero or many times. Any escaping rebind therefore loses
 // target identity and previous read proofs until an explicit write/rebind.
 bad_code(R"(int left = 1
int right
int &slot = &left
for i in range(0, 1)
    &slot = &right
print(slot)
print(NL)
)", "UNINITIALIZED");

 bad_code(R"(int left = 1
int right
int &slot = &left
bool choose = true
while choose
    &slot = &right
    break
print(slot)
print(NL)
)", "UNINITIALIZED");

 bad_code(R"(int | none choice = 1
int left
int right = 2
int &slot = &left
match choice
    int value
        &slot = &right
    none
        print("none")
        print(NL)
slot = 9
print(left)
print(NL)
)", "UNINITIALIZED");

 good(R"(int left = 1
int right = 2
int &slot = &left
bool choose = true
if choose
    &slot = &right
&slot = &right
print(slot)
print(NL)
)");
 bad_code(R"(int square(int x)
    return x * x
auto f = square
)", "FUNCTION_REFERENCE_CONTEXT");
 bad_code("int[0] xs = []\nprint(xs[0])\n", "INDEX_BOUNDS");
 bad_code("int[2] xs = [1, 2]\nxs[2] = 3\n", "INDEX_BOUNDS");
 llvm_contains(
     "int[2] xs = [1, 2]\nprint(xs[1])\n",
     "call ptr @quidra_fixed_array_slot_proven");
 llvm_contains(R"(int sum_values(const int[] &values, int n)
    int total = 0
    for i in range(0, n)
        total += values[i]
    return total

int[] values = array(8, fill = 1)
print(sum_values(&values, len(values)))
print(NL)
)", "call i1 @quidra_array_initialization_complete");
 llvm_contains(R"(int sum_values(const int[] &values, int n)
    int total = 0
    for i in range(0, n)
        total += values[i]
    return total

int[] values = array(8, fill = 1)
print(sum_values(&values, len(values)))
print(NL)
)", "call ptr @quidra_array_slot_proven");
 llvm_contains(R"(int sum_values(const int[] &values, int n)
    int total = 0
    for i in range(0, n)
        total += values[i]
    return total

int[] values = array(8, fill = 1)
print(sum_values(&values, len(values)))
print(NL)
)", "phi ptr [ %array.bounds.proven.slot.");
 bad_code("int | none x = 1\nmatch x\n    int\n        print(x)\n", "MATCH_EXHAUSTIVE");
 bad_code("auto values = []\n", "AMBIGUOUS_TYPE");
 bad_code("int f(int x)\n    return x\nprint(f())\n", "ARGUMENT_MISMATCH");
 bad_message(R"(int combine(int first, int second = 2)
    return first + second
print(combine(first = 1, 2))
print(NL)
)", "ARGUMENT_MISMATCH", "Positional argument cannot follow named arguments");
 bad_message(R"(int combine(int first, int second = 2)
    return first + second
print(combine(first = 1, first = 2))
print(NL)
)", "ARGUMENT_MISMATCH", "Argument 'first' is supplied more than once");
 bad_message(R"(int combine(int first, int second = 2)
    return first + second
print(combine(third = 1))
print(NL)
)", "ARGUMENT_MISMATCH", "Unknown argument 'third'");
 bad_message(R"(int combine(int first, int second = 2)
    return first + second
print(combine())
print(NL)
)", "ARGUMENT_MISMATCH", "Missing required argument 'first'");
 bad_message(R"(int combine(int first, int second = 2)
    return first + second
print(combine(1, 2, 3))
print(NL)
)", "ARGUMENT_MISMATCH", "Too many positional arguments");
 bad_message(R"(int combine(int first, int second = 2)
    return first + second
print(combine(1, first = 2))
print(NL)
)", "ARGUMENT_MISMATCH", "Argument 'first' is supplied more than once");
 bad_code("int[2] values = [1]\n", "ARRAY_SHAPE");
 bad_code(R"(class Secret
    private int value = 1
Secret secret
print(secret.value)
print(NL)
)", "PRIVATE_MEMBER");
 good(R"(class Secret
    private int value

    construct(int value_value)
        value = value_value
Secret secret = Secret(1)
)");
 bad_code(R"(class Secret
    private void hidden()
        return
Secret secret
secret.hidden()
)", "PRIVATE_MEMBER");
 bad_code(R"(class Secret
    private int value = 1
Secret secret
secret.value = 2
)", "PRIVATE_MEMBER");
 bad_code(R"(class Secret
    private int value = 1
Secret secret
int &alias = &secret.value
)", "PRIVATE_MEMBER");
 good(R"(class Secret
    private int value = 1

    void copy_from(Secret other)
        value = other.value
)");
 good(R"(class Secret
    private int value = 1

    void update_other(Secret other)
        int &alias = &other.value
        alias = 2
)");
 good(R"(class Secret
    private void hidden()
        return

    void call_other(Secret other)
        other.hidden()
)");
 bad_code(R"(class Box<T>
    private T value

    construct(T value_value)
        value = value_value
Box<int> box = Box<int>(1)
print(box.value)
print(NL)
)", "PRIVATE_MEMBER");
 bad_code(R"(class Box<T>
    private void hidden()
        return
Box<int> box
box.hidden()
)", "PRIVATE_MEMBER");
 bad_code(R"(class Secret
    private T echo<T>(T value)
        return value
Secret secret
print(secret.echo<int>(1))
print(NL)
)", "PRIVATE_MEMBER");
 bad_code("class Secret\n    private private int value\n", "PARSE_ERROR");
 bad_code("private int value = 1\n", "PARSE_ERROR");
 good(R"(class SecretRef
    private int value = 1
    int read_other(SecretRef other)
        const int &alias = &other.value
        return alias
SecretRef a
SecretRef b
print(a.read_other(b))
print(NL)
)");
 good(R"(class PrivateInit
    private int value

    construct(int value_value)
        value = value_value
    int read()
        return value
PrivateInit item = PrivateInit(9)
print(item.read())
print(NL)
)");
 bad_code(R"(class PrivateInit
    private int value

    construct(int start)
        value = start
PrivateInit item = PrivateInit(other = 9)
)", "ARGUMENT_MISMATCH");
 bad_code("class A\n    int x\nclass A\n    int y\n", "DUPLICATE_NAME");
 bad("write(\"legacy console output\")\n");
 bad("io.flush()\n");
 good("flush()\n");
 good("int io = 1\nprint(io)\n");
 good(R"(int write(int value)
    return value
print(write(7))
print(NL)
)");
 bad_code(R"(int parse(int value)
    return value
int parse(int value, int base)
    return value + base
)", "DUPLICATE_NAME");
 bad_code(R"(int choose(int value)
    return value
float choose(int value)
    return float(value)
)", "DUPLICATE_NAME");
 good(R"(string classify(uint8 value)
    return "byte"
string classify<T: floating>(T value)
    return "floating"
print(classify(uint8(1)))
print(NL)
print(classify(float32(1.0)))
print(NL)
)");
 good(R"(int domain<T: numeric>(T value)
    return 1
int domain<T: floating>(T value)
    return 2
print(domain<int>(int(1)))
print(NL)
print(domain<float32>(float32(1.0)))
print(NL)
)");
 bad_code(R"(T ambiguous<T: ordered>(T value)
    return value
T ambiguous<T: equatable>(T value)
    return value
)", "AMBIGUOUS_SPECIALIZATION");
 good(R"(T exact_cast_family<T: numeric>(T value)
    return value
bigint exact_integer = exact_cast_family(bigint(2))
bigreal exact_real = exact_cast_family(bigreal(2))
print(exact_integer)
print(NL)
print(exact_real)
print(NL)
)");
 good(R"(T1 pair_domain<T1: numeric, T2: integer>(T1 left, T2 right)
    return left
T1 pair_domain<T1: floating, T2: integer>(T1 left, T2 right)
    return left
float32 narrow = pair_domain(float32(2.0), int(1))
int broad = pair_domain(int(2), int(1))
print(narrow)
print(NL)
print(broad)
print(NL)
)");
 bad_code(R"(T default_shape<T: numeric>(T value, int mode = 0)
    return value
T default_shape<T: floating>(T value, int mode)
    return value
)", "DUPLICATE_NAME");
 bad_code(R"(T relation<T>(T left, T right)
    return left
T relation<T, U>(T left, U right)
    return left
)", "DUPLICATE_NAME");
 bad_code(R"(int unrelated_pattern(int value)
    return value
int unrelated_pattern<T: floating>(tensor<T> value)
    return 1
)", "DUPLICATE_NAME");
 bad_code(R"(int concrete_relation(int left, float right)
    return left
T concrete_relation<T: numeric>(T left, T right)
    return left
)", "DUPLICATE_NAME");
 good(R"(int radius_mode(tensor<uint8> value, int radius = 1)
    return radius
T radius_mode<T: floating>(tensor<T> value, int radius = 1)
    return value[0].item()
tensor<uint8> pixels = tensor.zeros<uint8>([1])
print(radius_mode(pixels, radius = 1))
print(NL)
)");

 good(R"(int signed_radius(tensor<uint8> value, int radius = 1)
    return radius
T signed_radius<T: floating>(tensor<T> value, int radius = 1)
    return value[0].item()
tensor<uint8> pixels = tensor.zeros<uint8>([1])
print(signed_radius(pixels, radius = -1))
print(NL)
)");

 good(R"(int kernel_device(tensor<uint8> value, tensor<int> kernel)
    return 1
T kernel_device<T: floating, K: floating>(tensor<T> value, tensor<K> kernel)
    return value[0].item()
tensor<uint8> pixels = tensor.zeros<uint8>([1])
tensor<int> kernel = tensor.ones<int>([1])
print(kernel_device(pixels, kernel.gpu(0)))
print(NL)
)");

 good(R"(string shaped_specialization(tensor<uint8> value)
    return "byte"
string shaped_specialization<T: floating>(tensor<T> value)
    return "floating"
tensor<uint8><1, 2, 2> pixels = tensor.ones<uint8>([1, 2, 2])
print(shaped_specialization(pixels))
print(NL)
)");

 good(R"(tensor<T> transformed_specialization<T: numeric>(
    tensor<T> left,
    tensor<T> right
)
    return right % right
tensor<float32> transformed_specialization(
    tensor<float32> left,
    tensor<float32> right
)
    return right
tensor<float32> specialization_left = tensor.ones<float32>([1, 2])
tensor<float32> specialization_right = tensor.ones<float32>([1, 2])
tensor<float32> specialization_result = transformed_specialization(
    specialization_left,
    specialization_right.transpose(0, 1)
)
print(specialization_result.shape()[0])
print(NL)
)");

 good(R"(T classify_proto<T: numeric>(T value);
T classify_proto<T: floating>(T value);

T classify_proto<T: numeric>(T value)
    return value
T classify_proto<T: floating>(T value)
    return value

print(classify_proto<int>(int(1)))
print(NL)
print(classify_proto<float32>(float32(2.0)))
print(NL)
)");
 good(R"(class MethodDomain
    string classify(uint8 value)
        return "byte"
    string classify<T: floating>(T value)
        return "floating"

MethodDomain domain
print(domain.classify(uint8(1)))
print(NL)
print(domain.classify(float32(1.0)))
print(NL)
)");

 good(R"(class MethodPriority
    T choose<T: numeric>(T value)
        return value
    T choose<T: floating>(T value)
        return value

MethodPriority priority
print(priority.choose(int(2)))
print(NL)
print(priority.choose(float32(3.0)))
print(NL)
)");

 bad_code(R"(class MethodArityOverload
    int parse(int value)
        return value
    int parse(int value, int base)
        return value + base
)", "DUPLICATE_NAME");


 good(R"(class MethodPairDomain
    T1 combine<T1: numeric, T2: integer>(T1 left, T2 right)
        return left
    T1 combine<T1: floating, T2: integer>(T1 left, T2 right)
        return left

MethodPairDomain domain
float32 narrow = domain.combine(float32(2.0), int(1))
int broad = domain.combine(int(2), int(1))
print(narrow)
print(NL)
print(broad)
print(NL)
)");

 bad_code(R"(class MethodAmbiguous
    T choose<T: ordered>(T value)
        return value
    T choose<T: equatable>(T value)
        return value
)", "AMBIGUOUS_SPECIALIZATION");

 bad_code(R"(class MethodDefaultShape
    T choose<T: numeric>(T value, int mode = 0)
        return value
    T choose<T: floating>(T value, int mode)
        return value
)", "DUPLICATE_NAME");

 bad_code(R"(class MethodRelation
    T choose<T>(T left, T right)
        return left
    T choose<T, U>(T left, U right)
        return left
)", "DUPLICATE_NAME");

 bad_code(R"(class TooManyConstructors
    int value
    construct()
        value = 0
    construct(int value_value)
        value = value_value
)", "DUPLICATE_NAME");
 good(R"(class DefaultConstructorParameter
    int value
    construct(int value_value = 7)
        value = value_value
DefaultConstructorParameter item = DefaultConstructorParameter()
print(item.value)
print(NL)
)");
 bad_code(R"(T identity<T>(T value)
    return value
auto result = identity(1)
)", "GENERIC_INFERENCE");
 good(R"(T identity<T>(T value)
    return value
auto result = identity(int32(1))
print(result)
print(NL)
)");
 bad_code(R"(T identity<T>(T value)
    return value
print(identity<int, int>(1))
print(NL)
)", "GENERIC_ARITY");
 good(R"(void | error read_open_file(string path)
    file.Handle handle = try file.open(path)
    string text = try handle.read()
    handle.close()
    return void
)");
 good(R"(void | error read_open_bin(string path)
    file.Handle handle = try file.open(path)
    bin data = try handle.read_bin()
    return void
)");
 good(R"(int | error fallible_value(bool ok)
    if ok
        return 7
    return error("bad")
int value = fallible_value(true)
print(value)
print(NL)
)");
 good(R"(int | error fallible_value(bool ok)
    if ok
        return 7
    return error("bad")
auto value = fallible_value(true)
print(value)
print(NL)
)");
 good(R"(int | none | error maybe_value(int mode)
    if mode < 0
        return error("bad")
    if mode == 0
        return none
    return 7
auto value = maybe_value(0)
match value
    int number
        print(number)
        print(NL)
    none
        print("none")
        print(NL)
)");
 bad_code(R"(int value = 7
auto | error preserved = value
)", "INVALID_AUTO");
 bad_code(R"(int value = 7
auto | error &preserved = &value
)", "INVALID_AUTO");
 bad_code(R"(int | error fallible_value()
    return 7
auto | none invalid = fallible_value()
)", "INVALID_AUTO");
 good(R"(int | error fallible_value(bool ok)
    if ok
        return 7
    return error("bad")
auto | error result = fallible_value(true)
match result
    int value
        print(value)
        print(NL)
    error problem
        print(problem)
        print(NL)
)");
 ir_contains(R"(int | error fallible_value(bool ok)
    if ok
        return 7
    return error("bad")
int value = fallible_value(true)
print(value)
print(NL)
)", "fail.error");
 llvm_contains(R"(int | error fallible_value(bool ok)
    if ok
        return 7
    return error("bad")
int value = fallible_value(true)
print(value)
print(NL)
)", "UNHANDLED_ERROR");
 bad_code(R"(int | float value = 5
float narrowed = value
)", "TYPE_MISMATCH");
 bad("never stop()\n    process.exit(1)\n");
 good(R"(void stop()
    process.exit(1)
int no_return()
    stop()
)");
 bad_code(R"(void maybe_stop(bool stop_now)
    if stop_now
        process.exit(1)
int still_returns()
    maybe_stop(true)
)", "MISSING_RETURN");
 bad_code(R"(int loop_only()
    while true
        print("loop")
        print(NL)
)", "MISSING_RETURN");
 llvm_contains(R"(void | error read_open_file(string path)
    file.Handle handle = try file.open(path)
    string text = try handle.read()
    return void
)", "ptr @quidra_file_handle_drop)");
 llvm_contains(R"(void | error copy_handle(string path)
    file.Handle first = try file.open(path)
    file.Handle second = first
    return void
)", "call ptr @quidra_file_handle_clone");
 bad_code(R"(void close_readonly(const file.Handle &handle)
    handle.close()
)", "WRITE_CAPABILITY");
 bad_code(R"(void read_readonly(const file.Handle &handle)
    auto text = handle.read()
)", "WRITE_CAPABILITY");
 bad_code(R"(void read_bin_readonly(const file.Handle &handle)
    auto data = handle.read_bin()
)", "WRITE_CAPABILITY");
 good(R"(enum Token
    Number(float)
    Name(string)
    Plus
    End

Token token = Token.Number(3.0)
match token
    Token.Number(value)
        print(value)
        print(NL)
    Token.Name(name)
        print(name)
        print(NL)
    Token.Plus
        void
    Token.End
        void
)");
 good(R"(enum State
    First(int)
    Second(int)
State state = State.Second(2)
match state
    State.First(value)
        print(value)
        print(NL)
    State.Second(value)
        print(value)
        print(NL)
)");
 bad_code(R"(enum Token
    Number(float)
    End
Token token = Token.End
match token
    Token.Number(value)
        print(value)
        print(NL)
)", "MATCH_EXHAUSTIVE");
 bad_code(R"(enum Token
    Number(float)
Token token = Token.Number
)", "ARGUMENT_MISMATCH");
 good(R"(T maximum<T: ordered>(T a, T b)
    if a > b
        return a
    return b
print(maximum<int>(3, 7))
print(NL)
)");
 good(R"(T square<T: numeric>(T value)
    return value * value
print(square<float32>(float32(3.0)))
print(NL)
)");
 good(R"(class Box<T: equatable>
    T value

    construct(T value_value)
        value = value_value
Box<string> box = Box<string>("ok")
print(box.value)
print(NL)
)");
 bad_code(R"(class CallbackHolder
    fn<void>() callback
class Box<T: equatable>
    T value
Box<CallbackHolder> box
)", "GENERIC_CONSTRAINT");
 bad_code(R"(class ResourceHolder
    file.Handle handle
T same<T: equatable>(T value)
    return value
ResourceHolder holder
auto copy = same<ResourceHolder>(holder)
)", "GENERIC_CONSTRAINT");
 bad_code(R"(T square<T: numeric>(T value)
    return value
print(square<string>("no"))
print(NL)
)", "GENERIC_CONSTRAINT");
 bad_code(R"(T bad<T: mystery>(T value)
    return value
print(bad<int>(1))
print(NL)
)", "GENERIC_CONSTRAINT");
 bad_code(R"(int plain(int value)
    return value
print(plain<int>(1))
print(NL)
)", "GENERIC_TARGET");
 bad_code("void f()\n\treturn\n", "INDENTATION");
 bad_code("auto x\n", "INVALID_AUTO");
 bad_code("break\n", "LOOP_CONTROL_CONTEXT");
 bad_code("int | none x = 1\nmatch x\n    int\n        print(int(1))\n    int y\n        print(y)\n    none\n        print(int(0))\n", "MATCH_CASE");
 bad_code("int f(bool yes)\n    if yes\n        return 1\n", "MISSING_RETURN");
 bad_code("int8 x = int8(300)\n", "NUMERIC_CAST");
 bad_code("int x = int(3.5)\n", "NUMERIC_CAST");
 good("int8 minimum = -128\nint minimum64 = -9223372036854775808\n");
 bad_code("int8 too_small = -129\n", "INTEGER_RANGE");
 good("int8 minimum = int8(-128)\n");
 bad_code("int8 too_small = int8(-129)\n", "NUMERIC_CAST");

 // Numeric literals carry families, not default concrete types.
 good("int32 x = 3\nfloat32 y = 3.0\nfloat32 z = 0.1\n");
 bad_code("auto x = 3\n", "AMBIGUOUS_NUMERIC_LITERAL");
 bad_code("auto x = 1.5\n", "AMBIGUOUS_NUMERIC_LITERAL");
 bad_code("print(1)\n", "AMBIGUOUS_NUMERIC_LITERAL");
 bad_code("float x = 3\n", "NUMERIC_FAMILY");
 bad_code("int x = 3.0\n", "NUMERIC_FAMILY");
 bad_code("auto x = [1, 2, 3]\n", "AMBIGUOUS_NUMERIC_LITERAL");
 good(R"(int32 seed = 1
auto values = [2, seed, 3]
print(values[0])
print(NL)
)");
 good(R"(int32 fixed_identity(int32 value)
    return value
auto result = fixed_identity(3)
print(result)
print(NL)
)");
 bad_code("bin raw = bin(3567446)\n", "AMBIGUOUS_NUMERIC_LITERAL");
 good("bin raw = bin(int32(3567446))\nprint(raw)\n");
 bad_code("bigint value = 1\nbin raw = bin(value)\n", "TYPE_MISMATCH");
 bad_code("bigreal value = 1.0\nbin raw = bin(value)\n", "TYPE_MISMATCH");
 good("float32 rounded = float32(16777217)\n");
 bad_code("float32 too_large = 1.0e100\n", "FLOAT_RANGE");
 bad_code("float32 too_large = float32(1.0e100)\n", "NUMERIC_CAST");

 // Numeric spelling has one integer radix; exponent notation is visibly floating.
 bad_code("float x = 1e8\n", "LEX_ERROR");
 good("float x = 1.0e8\n");

 // '^' is a Core basic operator. Scalar power preserves the concrete
 // numeric family, and tensor power is deliberately one-way: tensor ^ scalar.
 // Matrix multiplication has no '@' syntax.
 good("int base = 2\nint powered = base ^ 10\nprint(powered)\n");
 good("float base = 4.0\nfloat powered = base ^ 0.5\nprint(powered)\n");
 good(R"(tensor<int> base = tensor.ones<int>([2]) * 2
tensor<int> powered = base ^ 3
print(powered[0].item())
print(NL)
)");
 good(R"(tensor<float> base = tensor.ones<float>([2]) * 2.0
tensor<float> powered = base ^ 0.5
print(powered[0].item())
print(NL)
)");
 bad_code(R"(tensor<int> base = tensor.ones<int>([1])
tensor<int> exponent = tensor.ones<int>([1])
tensor<int> powered = base ^ exponent
)", "TYPE_MISMATCH");
 bad_code(R"(tensor<int> base = tensor.ones<int>([1])
tensor<int> powered = 2 ^ base
)", "TYPE_MISMATCH");
 bad_code(R"(tensor<int> base = tensor.ones<int>([1])
tensor<int> powered = base ^ -1
)", "POWER_DOMAIN");
 bad_code("int a = 2\nint b = 3\nint c = a @ b\n", "LEX_ERROR");
 bad_code("int x = 0x10\n", "LEX_ERROR");
 bad_code("int array = 1\n", "SHADOWING");
 good("int math = 1\n");
 bad_code("class ReservedBuiltinField\n    int array\n", "SHADOWING");
 bad_code("class ReservedBuiltinMethod\n    int array()\n        return 1\n", "SHADOWING");
 bad_code("class ReservedNamespaceMethod\n    int tensor()\n        return 1\n", "SHADOWING");
 
 bad_code("int scan = 1\n", "SHADOWING");
 bad_code("int len = 1\n", "SHADOWING");
 bad_code("void use(int print)\n    return\n", "SHADOWING");
  bad_code("range(3)\n", "RANGE_CONTEXT");
 bad_code("int main()\n    return 0\n", "RESERVED_MAIN");
 bad_code("return\n", "RETURN_OUTSIDE_FUNCTION");
 bad_code(R"(int | error read()
    return 1
int use()
    return try read()
)", "TRY_CONTEXT");
 bad_code(R"(class P
    int x
    int y

    construct(int x_value)
        x = x_value
int read_y(P value)
    return value.y
P value = P(1)
print(read_y(value))
print(NL)
)", "UNINITIALIZED_ARGUMENT");
 bad_code(R"(class P
    int x
    int y

    construct(int x_value)
        x = x_value
P a = P(1)
P b = P(1)
bool same = a == b
)", "UNINITIALIZED_FIELD_EQUALITY");
 bad_code(R"(class ArrayPartialLiteral
    int x
    int y

    construct(int x_value)
        x = x_value
ArrayPartialLiteral partial = ArrayPartialLiteral(1)
ArrayPartialLiteral[] values = [partial]
)", "UNINITIALIZED_ARGUMENT");
 bad_code(R"(class ArrayPartialFill
    int x
    int y

    construct(int x_value)
        x = x_value
ArrayPartialFill partial = ArrayPartialFill(1)
ArrayPartialFill[] values = array(2, fill = partial)
)", "UNINITIALIZED_ARGUMENT");
 bad_code(R"(class ArrayPartialAppend
    int x
    int y

    construct(int x_value)
        x = x_value
ArrayPartialAppend partial = ArrayPartialAppend(1)
ArrayPartialAppend[] values = []
values = values.append(partial)
)", "UNINITIALIZED_ARGUMENT");
 bad_code(R"(class ArrayPartialAssign
    int x
    int y

    construct(int x_value)
        x = x_value
ArrayPartialAssign partial = ArrayPartialAssign(1)
ArrayPartialAssign[] values = array(1)
values[0] = partial
)", "UNINITIALIZED_ARGUMENT");
 good(R"(class IndexedPoint
    int x
    int y

    construct(int x_value, int y_value)
        x = x_value
        y = y_value

    int sum()
        return x + y

IndexedPoint[] dynamic_points = [IndexedPoint(1, 2), IndexedPoint(3, 4)]
int dynamic_index = 1
print(dynamic_points[dynamic_index].x)
print(NL)
print(dynamic_points[0].sum())
print(NL)

IndexedPoint[2] fixed_points
fixed_points[0] = IndexedPoint(5, 6)
fixed_points[1] = IndexedPoint(7, 8)
print(fixed_points[1].y)
print(NL)
print(fixed_points[0].sum())
print(NL)
)");
 bad_code(R"(class IndexedPartialPoint
    int x
    int y

    construct(int x_value)
        x = x_value

IndexedPartialPoint partial = IndexedPartialPoint(1)
IndexedPartialPoint[] values = array(1)
values[0] = partial
)", "UNINITIALIZED_ARGUMENT");
 bad_code("class A\n    int x\n    construct(int start)\n        x = start\nA a = A(1)\nprint(a.y)\n", "UNKNOWN_MEMBER");
 bad_code("print(missing)\n", "UNKNOWN_NAME");
 // The text constants are two-letter capitals only; lowercase spellings are
 // ordinary identifiers, with the diagnostic naming the constant meant.
 bad_code("string s = ht\n", "UNKNOWN_NAME");
 (void)quidra::compile("string ht = \"x\"\nstring nl = ht\nprint(nl)\n");
 bad_code("string s = \"A{nl}B\"\n", "UNKNOWN_NAME");
 {
     bool hinted = false;
     try { (void)quidra::compile("string s = bs\n"); }
     catch (const quidra::CompileErrors& errors) {
         for (const auto& d : errors.diagnostics())
             if (d.code == "UNKNOWN_NAME" && d.message.find("spelled 'BS'") != std::string::npos) hinted = true;
     }
     if (!hinted) { std::cerr << "lowercase text constant did not name BS\n"; std::exit(1); }
 }
 bad_code("Missing value\n", "UNKNOWN_TYPE");
 bad_code("float value = 1.0e9999\n", "FLOAT_RANGE");
 bad_code("import math\n", "IMPORT_CONTEXT");
 bad_code("auto loaded = vision.read<uint8>(\"input.png\")\n", "GENERIC_RECEIVER");
 bad_code("auto loaded = vision.read(\"input.png\")\n", "UNKNOWN_NAME");
 bad_code("print(math.sqrt(float(16.0)))\n", "UNKNOWN_NAME");
 good("tensor<float32> grid = tensor.zeros<float32>([2, 2])\nprint(grid.shape()[0])\n");
 bad_code("tensor<float> x = tensor.ones<float>([1])\nauto y = x.abs()\n", "UNKNOWN_MEMBER");
 bad_code("tensor<float> x = tensor.ones<float>([1])\nauto y = x.exp()\n", "UNKNOWN_MEMBER");
 bad_code("tensor<float> x = tensor.ones<float>([1])\nauto y = x.log()\n", "UNKNOWN_MEMBER");
 bad_code("tensor<float> x = tensor.ones<float>([1])\nauto y = x.sqrt()\n", "UNKNOWN_MEMBER");

 // exact.* is a package-neutral Core mechanism. Providers and opcodes are
 // opaque to Core; packages own the mathematical meaning.
 good(R"(bigreal input = bigreal(2)
bigreal result = exact.unary("test-provider", 7, input)
print(result.string())
print(NL)
)");
 llvm_contains(R"(bigreal input = bigreal(2)
bigreal result = exact.unary("test-provider", 7, input)
)", "@qcore_exact_real_unary");
 bad_code(R"(string provider = "test-provider"
bigreal input = bigreal(2)
bigreal result = exact.unary(provider, 7, input)
)", "EXACT_PROVIDER");
 bad_code(R"(int opcode = 7
bigreal input = bigreal(2)
bigreal result = exact.unary("test-provider", opcode, input)
)", "EXACT_PROVIDER");
 good(R"(tensor<int><2> a = tensor.ones<int>([2])
tensor<int><2> b = tensor.ones<int>([2])
tensor<bool><2> eq = a == b
tensor<bool><2> ne = a != b
tensor<bool><2> lt = a < b
tensor<bool><2> le = a <= b
tensor<bool><2> gt = a > b
tensor<bool><2> ge = a >= b
bool same = eq.all()
bool different = ne.any()
tensor<bool><2> scalar_left = 1 < a
tensor<bool><2> scalar_right = a >= 1
)");
 llvm_contains(R"(tensor<int><2> a = tensor.ones<int>([2])
tensor<int><2> b = tensor.ones<int>([2])
tensor<bool><2> mask = a == b
bool same = mask.all()
)", "@quidra_tensor_compare");
 llvm_contains(R"(tensor<int><2> a = tensor.ones<int>([2])
bool any = (a != 1).any()
)", "@quidra_tensor_bool_reduce");
 bad_code(R"(tensor<int><2> a = tensor.ones<int>([2])
tensor<int><3> b = tensor.ones<int>([3])
tensor<bool> same = a == b
)", "TENSOR_SHAPE");
 good(R"(tensor<int><2> a = tensor.ones<int>([2])
tensor<bool><2> same = a == 1
tensor<bool><2> reverse = 1 == a
)");
 bad_code(R"(tensor<int><2> a = tensor.ones<int>([2])
tensor<bool> bad = a == int32(1)
)", "TYPE_MISMATCH");
 good(R"(tensor<bool> empty = tensor.zeros<bool>([0])
bool all_empty = empty.all()
bool any_empty = empty.any()
)");
 bad_code("1 = 2\n", "INVALID_ASSIGNMENT");
 bad_code("void[] values = []\n", "INVALID_TYPE");
 bad_code("auto value = 1e\n", "LEX_ERROR");
 bad_code("Missing<int> value\n", "UNKNOWN_GENERIC");
 bad_code(R"(class A
    int x = 0
A value
print(value.missing<int>(1))
print(NL)
)", "UNKNOWN_GENERIC_METHOD");
 bad_code("int[] values = [1]\nvalues.missing<int>()\n", "GENERIC_RECEIVER");

 // Tensor shape patterns fix rank exactly; '_' keeps only that extent unconstrained.
 good(R"(tensor<float32><2, 3> matrix = tensor.zeros<float32>([2, 3])
int[2] dimensions = matrix.shape()
tensor<float32> erased = matrix
int[2] inferred_dimensions = erased.shape()
tensor<float32><3> row = matrix[0]
tensor<float32><2> column = matrix[:, 0]
tensor<float32> cell = matrix[0, 0]
float32 value = cell.item()
tensor<float32><6> reshaped = matrix.reshape([6])
tensor<float32><2, 3> contiguous = matrix.contiguous()
tensor<float><2, 3> converted = float(matrix)
)");
 good(R"(tensor<T><3, _> first_three<T>(tensor<T><3, _> value)
    return value
tensor<float32> source = tensor.ones<float32>([3, 2])
tensor<float32><3, _> constrained = first_three(source)
)");
 // Generic dtype inference may proceed through an unknown shape, but the
 // specialized call still enforces the exact-rank shape pattern.
 bad_code(R"(tensor<T><3, _> first_three<T>(tensor<T><3, _> value)
    return value
tensor<float32> source = tensor.ones<float32>([2, 2])
tensor<float32><3, _> constrained = first_three(source)
)", "TYPE_MISMATCH");
 good(R"(tensor<float32> | error direct = tensor.zeros<float32>([2, 2])
tensor<float32><2, _> | error constrained = tensor.ones<float32>([2, 2])
tensor<float32> | error widened = constrained
match widened
    tensor<float32> pixels
        print(pixels.shape()[0])
        print(NL)
    error problem
        print(problem)
        print(NL)
)");
 bad_code("tensor<float32><3, _> wrong = tensor.zeros<float32>([2, 2])\n", "TYPE_MISMATCH");
 bad_code("tensor<float32><2, 4> wrong = tensor.zeros<float32>([2, 3])\n", "TYPE_MISMATCH");
 bad_code("tensor<float32><3, _> wrong_rank = tensor.zeros<float32>([3, 4, 5])\n", "TYPE_MISMATCH");
 good("tensor<float32><3, _, _> exact_rank = tensor.zeros<float32>([3, 4, 5])\n");
 good(R"(tensor<float32> contextual_dtype = tensor.zeros([2, 3])
int[2] contextual_shape = contextual_dtype.shape()
tensor<float32><2, 3> contextual_exact = tensor.ones([2, 3])
)");
 bad_code("auto missing_dtype = tensor.zeros([2, 3])\n", "GENERIC_ARITY");
 good(R"(auto generated = tensor.zeros<float32>([2, 3])
int[2] generated_shape = generated.shape()
)");
 bad_code(R"(auto generated = tensor.zeros<float32>([2, 3])
tensor<float32><2, 4> impossible = generated
)", "TYPE_MISMATCH");
 bad_code(R"(tensor<float32><2, 3> source = tensor.zeros<float32>([2, 3])
auto transposed = source.transpose(0, 1)
tensor<float32><2, 3> impossible = transposed
)", "TYPE_MISMATCH");
 good(R"(tensor<float32><2, 3> source = tensor.zeros<float32>([2, 3])
auto transposed = source.transpose(0, 1)
tensor<float32><3, 2> exact_shape = transposed
auto reshaped = source.reshape([3, 2])
tensor<float32><3, 2> reshaped_exact = reshaped
)");
 bad_code(R"(tensor<float32><2, 3> source = tensor.zeros<float32>([2, 3])
auto reshaped = source.reshape([3, 2])
tensor<float32><2, 3> impossible = reshaped
)", "TYPE_MISMATCH");
 ir_contains(R"(tensor<float32> erase_shape(tensor<float32> value)
    return value
auto unknown = erase_shape(tensor.zeros<float32>([2, 3]))
tensor<float32><2, 4> checked_at_runtime = unknown
)", "shape.constraint");
 bad_code(R"(tensor<float32><2, 3> source = tensor.zeros<float32>([2, 3])
auto invalid = source.transpose(0, 2)
)", "ARGUMENT_MISMATCH");
 good(R"(T identity<T>(T value)
    return value
int integer_value = 1
float float_value = 1.0
int integer_result = identity(integer_value)
float float_result = identity(float_value)
)");
 bad_code("tensor<float32><2, 3> value = tensor.ones<float32>([2, 3])\nint[3] wrong_shape = value.shape()\n", "TYPE_MISMATCH");
 good(R"(tensor<float32> erase(tensor<float32> value)
    return value
tensor<float32><2, _> known = erase(tensor.zeros<float32>([2, 2]))
)");
 bad_code("tensor<float32> value = tensor.ones<float32>([1])\nfloat32 scalar = value.item()\n", "TYPE_MISMATCH");
 bad_code("tensor<float32><2, 2> value = tensor.ones<float32>([2, 2])\nauto bad = value[0, 0, 0]\n", "INDEX_ARITY");

 // Shape-pattern match cases select only exact-rank compatible tensor alternatives.
 good(R"(void classify(tensor<float32><3, 4> | tensor<float32><1, 4> value)
    match value
        tensor<float32><3, _> rgb
            print(rgb.shape()[0])
            print(NL)
        tensor<float32><1, _> gray
            print(gray.shape()[0])
            print(NL)
)");
 bad_code(R"(void invalid_case(tensor<float32><1, 4> | tensor<float32><4, 4> value)
    match value
        tensor<float32><3, _> impossible
            print(impossible.shape()[0])
            print(NL)
        tensor<float32><1, _> gray
            print(gray.shape()[0])
            print(NL)
        tensor<float32><4, _> rgba
            print(rgba.shape()[0])
            print(NL)
)", "MATCH_CASE");

 // Tracking preserves the tensor's exact shape contract.
 good(R"(tensor<float32> source = tensor.ones<float32>([3, 8, 8])
tensor<float32><3, _, _> value = source.track()
tensor<float32><3, _, _> restored = value.untrack()
)");
 good(R"(tensor<float> source = tensor.ones<float>([2, 4])
tensor<float><2, _> value = source.track()
tensor<float><2, _> restored = value.untrack()
)");
 bad_code(R"(tensor<float32> source = tensor.ones<float32>([3, 8, 8])
tensor<float32><3, _> wrong = source.track()
)", "TYPE_MISMATCH");

 // Dependent shape expressions in function signatures are checked at the boundary.
 good(R"(tensor<float><n, 2> keep_shape(int n, tensor<float><n, 2> value)
    return value
tensor<float> unknown = tensor.ones<float>([3, 2])
tensor<float><3, 2> checked = keep_shape(3, unknown)
)");
 good(R"(int[][n] keep_rows(int n, int[][n] rows)
    return rows
int[][] dynamic_rows = [[1, 2], [3, 4]]
int[][] checked_rows = keep_rows(2, dynamic_rows)
)");

 // Runtime extent expressions are captured per binding; mutable sources remain legal.
 good(R"(int n = 3
int m = 4
tensor<float><n * 2 + 1, 224> captured = tensor.ones<float>([7, 224])
n = 10
tensor<float><n, 224> later = tensor.ones<float>([10, 224])
float[n * m] dynamic_fixed
dynamic_fixed[0] = 1.0
tensor<float><3, 224> contextual = tensor.zeros()
tensor<float><_, 224> explicit_shape = tensor.zeros([3, 224])
)");
 bad_code(R"(int n = 3
tensor<float><n, 224> wrong = tensor.ones<float>([3, 224, 1])
)", "TYPE_MISMATCH");

 // Numeric container casts preserve array structure and tensor shape facts.
 good(R"(int[][] values = [[1, 2], [3, 4]]
float[][] converted = float(values)
int[2][2] fixed = [[1, 2], [3, 4]]
float[2][2] fixed_converted = float(fixed)
tensor<int><2, 2> matrix = tensor.ones<int>([2, 2])
tensor<float><2, 2> tensor_converted = float(matrix)
tensor<float32><2, 2> tracked_source = tensor.ones<float32>([2, 2])
tensor<float32><2, 2> tracked = tracked_source.track()
tensor<float><2, 2> tracked_converted = float(tracked.untrack())
)");
 good(R"(int[] values = [1, 300]
auto | error narrowed = int8(values)
match narrowed
    int8[] converted
        print(converted[0])
        print(NL)
    error problem
        print(problem)
        print(NL)
tensor<int><2, 2> matrix = tensor.ones<int>([2, 2])
auto | error tensor_narrowed = int8(matrix)
match tensor_narrowed
    tensor<int8><2, 2> converted
        print(converted[0, 0].item())
        print(NL)
    error problem
        print(problem)
        print(NL)
)");
 llvm_contains(R"(int[] values = [1, 300]
auto | error narrowed = int8(values)
match narrowed
    int8[] converted
        print(converted[0])
        print(NL)
    error problem
        print(problem)
        print(NL)
)", "@quidra_array_cast_validate_");
 llvm_contains(R"(tensor<int> values = tensor.ones<int>([2])
auto | error narrowed = int8(values)
match narrowed
    tensor<int8> converted
        print(converted[0].item())
        print(NL)
    error problem
        print(problem)
        print(NL)
)", "@quidra_tensor_try_cast");
 bad_code("int[] values = [1, 2]\nfloat[] converted = values\n", "TYPE_MISMATCH");
 bad_code("float[] values = [1.0, 2.0]\nint[] converted = int(values)\n", "NUMERIC_CAST");
 bad_code("tensor<float> values = tensor.ones<float>([2])\nauto converted = int(values)\n", "NUMERIC_CAST");
 bad_code("tensor<int> values = tensor.ones<int>([2])\nauto converted = values.cast<float>()\n", "UNKNOWN_MEMBER");

 // Tensor flow facts weaken at joins. A later exact-shape binding accepts the
 // unknown fact set and emits a runtime constraint check.
 good(R"(void branch_shape(bool flag)
    tensor<float32> value = tensor.zeros<float32>([3, 4])
    if flag
        value = tensor.zeros<float32>([3, 5])
    tensor<float32><3, 4> exact_shape = value
)");
 bad_code(R"(void branch_rank(bool flag)
    tensor<float32> value = tensor.zeros<float32>([2, 2])
    if flag
        value = tensor.zeros<float32>([2, 2, 2])
    int[2] dimensions = value.shape()
)", "TYPE_MISMATCH");
 bad_code(R"(void while_rank(bool flag)
    tensor<float32> value = tensor.zeros<float32>([2, 2])
    while flag
        value = tensor.zeros<float32>([2, 2, 2])
        flag = false
    int[2] dimensions = value.shape()
)", "TYPE_MISMATCH");
 bad_code(R"(void for_rank(int[] items)
    tensor<float32> value = tensor.zeros<float32>([2, 2])
    for item in items
        value = tensor.zeros<float32>([2, 2, 2])
    int[2] dimensions = value.shape()
)", "TYPE_MISMATCH");
 bad_code(R"(void match_rank(int | string choice)
    tensor<float32> value = tensor.zeros<float32>([2, 2])
    match choice
        int
            value = tensor.zeros<float32>([2, 2, 2])
        string
            value = tensor.zeros<float32>([2, 2])
    int[2] dimensions = value.shape()
)", "TYPE_MISMATCH");

 // Device placement is runtime/compiler metadata, not part of the nominal tensor type.
 good(R"(tensor<float32> cpu = tensor.zeros<float32>([2])
tensor<float32> direct_gpu = tensor.zeros<float32>([2], gpu = 0)
tensor<float32><2> contextual_gpu = tensor.ones(gpu = 1)
tensor<float32> copied = cpu.gpu(0)
tensor<float32> returned = copied.cpu()
)");
 good("gpu.sync(0)\ngpu.sync(index = 1)\n");
 bad_message("gpu.sync(-1)\n",
             "ARGUMENT_MISMATCH", "requires a non-negative GPU index");
 bad_code("gpu.sync()\n", "ARGUMENT_MISMATCH");
 bad_code("gpu.sync(0, 1)\n", "ARGUMENT_MISMATCH");
 bad_code("gpu.sync(1.0)\n", "NUMERIC_FAMILY");
 llvm_contains("gpu.sync(0)\n", "@quidra_gpu_sync");
 bad_message("auto value = tensor.zeros<float32>([1], gpu = -1)\n",
             "ARGUMENT_MISMATCH", "gpu index must be non-negative");
 bad_message("auto value = tensor.zeros<float32>([1], gpu = 1.5)\n",
             "NUMERIC_FAMILY", "Real-family literal cannot materialize as an integer-family type");
 bad_code("auto value = tensor.zeros<float32>([1], device = 0)\n",
          "ARGUMENT_MISMATCH");
 bad_message("auto value = tensor.zeros<float32>([1])\nauto moved = value.gpu()\n",
             "ARGUMENT_MISMATCH", "requires exactly one positional integer GPU index");
 bad_message("auto value = tensor.zeros<float32>([1])\nauto moved = value.gpu(-1)\n",
             "ARGUMENT_MISMATCH", "non-negative GPU index");
 bad_message("auto value = tensor.zeros<float32>([1])\nauto moved = value.cpu(0)\n",
             "ARGUMENT_MISMATCH", "takes no arguments");

 // Explicit transfer remains an observable IR boundary; it must not be folded
 // into the preceding allocation or relocate earlier arithmetic.
 const std::string explicit_gpu_transfer = R"(tensor<float32> source = tensor.zeros<float32>([2])
tensor<float32> moved = source.gpu(0)
)";
 ir_contains(explicit_gpu_transfer, "tensor.create");
 ir_contains(explicit_gpu_transfer, " cpu");
 ir_contains(explicit_gpu_transfer, "tensor.gpu");
 const std::string post_compute_transfer = R"(tensor<float32> left = tensor.ones<float32>([2])
tensor<float32> right = tensor.ones<float32>([2])
tensor<float32> sum = left + right
tensor<float32> moved = sum.gpu(0)
)";
 ir_contains(post_compute_transfer, "tensor.binary");
 ir_contains(post_compute_transfer, "tensor.gpu");
 llvm_contains(
     "auto value = tensor.zeros<float32>([1], gpu = 0)\n",
     "@quidra_tensor_create");
 llvm_contains(
     "auto value = tensor.zeros<float32>([1])\nauto moved = value.gpu(0)\n",
     "@quidra_tensor_to_gpu");
 llvm_contains(
     "auto value = tensor.zeros<float32>([1])\nauto moved = value.cpu()\n",
     "@quidra_tensor_to_cpu");

 // Loop-carried tensor facts must be weakened before checking the loop body.
 good(R"(void loop_backedge(bool flag)
    tensor<float32> value = tensor.zeros<float32>([2, 2])
    while flag
        auto product = value + value
        value = tensor.zeros<float32>([2, 2, 2])
        flag = false
)");
 good(R"(void for_backedge(int[] items)
    tensor<float32> value = tensor.zeros<float32>([2, 2])
    for item in items
        auto product = value + value
        value = tensor.zeros<float32>([2, 2, 2])
)");

 // Storage addresses are observable only through print and identity equality.
 good(R"(int x = 1
int &alias = &x
int other = 1
print(&x)
print(NL)
print(&x)
print(NL)
print(&alias)
print(NL)
bool same = &x == &alias
bool different = &x != &other
print(same)
print(NL)
print(different)
print(NL)
string text = "hello"
print(&text)
print(NL)
)");
 llvm_contains("int x = 1\nprint(&x)\n", "@.fmt.address");
 llvm_contains(R"(int x = 1
int &alias = &x
bool same = &x == &alias
print(same)
print(NL)
)", "icmp eq ptr");
 bad_code("int x = 1\nint y = 2\nbool ordered = &x < &y\n", "TYPE_MISMATCH");
 bad_code("int x = 1\nint y = 2\nprint(&x + &y)\n", "TYPE_MISMATCH");
 bad_code("int x = 1\nauto saved = &x\n", "INVALID_TYPE");

 // Existing call-site '&' remains reference-argument syntax outside print.
 good(R"(void touch(int &value)
    value = 2
int x = 1
touch(&x)
print(x)
print(NL)
)");

 std::string deep = "print(";
 deep.append(5000, '(');
 deep += "1";
 deep.append(5000, ')');
 deep += ")\n";
 bad_code(deep, "PARSE_DEPTH");

 // Nesting budgets. Each of these segfaulted the compiler before the budgets
 // existed; the source stays small because the cost is depth, not size.
 std::string eager = "int x = 1";
 for (int i = 0; i < 200; ++i) eager += " + 1";
 eager += "\n";
 bad_code(eager, "NESTING_DEPTH");

 // Short-circuit operands are excluded from the iterative eager-binary
 // worklist, so this shape reaches IR lowering's own expression recursion.
 std::string shortcircuit = "bool b = true\nbool c = b";
 for (int i = 0; i < 200; ++i) shortcircuit += " and b";
 shortcircuit += "\n";
 bad_code(shortcircuit, "NESTING_DEPTH");

 // The cheapest crash shape: 600 links segfaulted in 4,284 bytes.
 std::string postfix = "class P\n    int v\n\n    construct(int start)\n        v = start\n\n    P grow()\n        return P(v + 1)\n\n"
                       "P p = P(1)\nP q = p";
 for (int i = 0; i < 100; ++i) postfix += ".grow()";
 postfix += "\n";
 bad_code(postfix, "NESTING_DEPTH");

 std::string nested = "void f()\n";
 for (int i = 0; i < 300; ++i) nested += std::string(4 * (i + 1), ' ') + "if true\n";
 nested += std::string(4 * 301, ' ') + "print(\"x\")\n";
 bad_code(nested, "NESTING_DEPTH");

 // elif recurses without passing through Parser::statement().
 std::string elifs = "void f()\n    int x = 0\n    if x == 0\n        print(\"a\")\n";
 for (int i = 0; i < 300; ++i)
     elifs += "    elif x == " + std::to_string(i + 1) + "\n        print(\"b\")\n";
 bad_code(elifs, "NESTING_DEPTH");

 std::cout<<"all compiler tests passed\n";
}
