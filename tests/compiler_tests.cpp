#include "quidra/abi/symbols.hpp"
#include "quidra/compile_inputs.hpp"
#include "quidra/compiler.hpp"
#include "quidra/formatter.hpp"
#include "quidra/frontend.hpp"
#include "quidra/ir.hpp"
#include "quidra/ir/dtype.hpp"
#include "quidra/package_lock.hpp"
#include "quidra/source_tools.hpp"
#include "quidra/standard_classes.hpp"
#include "llvm_backend/runtime_abi.hpp"
#include "llvm_backend/runtime_prelude.hpp"
#include "semantics/effect_summary.hpp"
#include "optimizer_fixtures.hpp"
#include "platform/environment.hpp"
#include "platform/sha256.hpp"
#include <algorithm>
#include <iostream>
#include <sstream>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>
#ifndef _WIN32
#include <unistd.h>
#endif

// Golden corpus hook (tests/golden/README.md, Inputs). With
// QUIDRA_CAPTURE_SOURCES set, every inline program a helper compiles is also
// written there as <helper>-<sha256 prefix>.qui, with a sidecar .expect that
// names the helper, its expected diagnostic code and the REPL replay prefix,
// so that the refactoring gates compile it too. Unset, nothing changes.
static void capture_source(const char* helper, const std::string& source,
                           const std::string& code = {},
                           std::size_t replay_prefix_bytes = 0) {
    static const auto directory =
        quidra::platform::environment_value("QUIDRA_CAPTURE_SOURCES");
    if (!directory || directory->empty()) return;
    const auto key = quidra::sha256_hex(
        source + '\0' + std::to_string(replay_prefix_bytes));
    const auto base =
        (std::filesystem::path(*directory) / (std::string(helper) + "-" + key.substr(0, 16)))
            .string();
    std::filesystem::create_directories(*directory);
    std::ofstream(base + ".qui", std::ios::binary) << source;
    std::ofstream expect(base + ".expect", std::ios::binary);
    expect << "helper " << helper << "\n";
    if (!code.empty()) expect << "code " << code << "\n";
    if (replay_prefix_bytes) expect << "replay_prefix_bytes " << replay_prefix_bytes << "\n";
}
static void good(const std::string& s) {
    capture_source("good", s);
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
static void bad(const std::string& s) { capture_source("bad", s); try {(void)quidra::compile(s);}catch(const quidra::CompileErrors&){return;}catch(const quidra::CompileError&){return;}std::cerr<<"unexpected acceptance:\n"<<s;std::exit(1); }
static void inspect_contains(const std::string& s, const std::string& expected) {
    capture_source("inspect_contains", s);
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
    capture_source("inspect_no_source_contains", s);
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
    capture_source("repl_ir_contains", source, {}, replay_prefix_bytes);
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
    capture_source("ir_contains", s);
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
    capture_source("tensor_region_at_least", source);
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
    capture_source("ir_function_not_contains", s);
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
static void ir_function_contains(
    const std::string& s, const std::string& function_marker,
    const std::string& expected) {
    capture_source("ir_function_contains", s);
    try {
        const auto c = quidra::compile(s);
        const auto dumped = quidra::ir::dump(c.ir);
        const auto marker = dumped.find(function_marker);
        const auto begin = marker == std::string::npos ? marker : dumped.rfind("function ", marker);
        const auto end = marker == std::string::npos ? marker : dumped.find("\nend\n", marker);
        if (begin == std::string::npos || end == std::string::npos) {
            std::cerr << "cannot isolate typed IR function for marker: " << function_marker << "\n"
                      << dumped << "\n";
            std::exit(1);
        }
        const auto body = dumped.substr(begin, end + 5 - begin);
        if (body.find(expected) != std::string::npos) return;
        std::cerr << "typed IR function missing fragment: " << expected << "\n" << body << "\n";
    } catch (const std::exception& e) {
        std::cerr << "unexpected typed IR generation rejection: " << e.what() << "\n" << s;
    }
    std::exit(1);
}
static void llvm_contains(const std::string& s, const std::string& expected) {
    capture_source("llvm_contains", s);
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
    capture_source("llvm_not_contains", s);
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
    capture_source("llvm_function_not_contains", s);
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
    capture_source("llvm_file_contains", s);
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
    capture_source("bad_code", s, expected);
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
    capture_source("bad_message", s, code);
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


// True when compiling `path` reports a SHADOWING diagnostic.
static bool compile_reports_shadowing(const std::filesystem::path& path,
                                      const std::filesystem::path& root) {
    try {
        (void)quidra::compile_file(path, {}, root);
    } catch (const quidra::CompileErrors& errors) {
        bool shadowing = false;
        for (const auto& d : errors.diagnostics()) shadowing |= d.code == "SHADOWING";
        return shadowing;
    } catch (const quidra::CompileError& error) {
        return error.diagnostic().code == "SHADOWING";
    }
    return false;
}

// A local inside imported module code is checked against that module's
// own declarations, never against the importing program's globals. Generic
// instances keep the namespace of their template, and an imported module's
// `if main` guard (merged into the root statements, inactive) is checked
// against its own module too.
static void imported_locals_use_module_scope() {
    const auto root = std::filesystem::temp_directory_path() / "quidra-imported-local-scope";
    const auto write = [&](const char* name, const char* text) {
        std::ofstream out(root / name, std::ios::binary);
        if (!out) throw std::runtime_error("cannot create temporary Quidra source");
        out << text;
    };
    try {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        write("lib.qui",
              "int generic_extent<T: numeric>(T value, int top)\n"
              "    int input_height = 4\n"
              "    int input_width = 5\n"
              "    int Box = 0\n"
              "    return input_height * input_width + top + Box\n"
              "\n"
              "int plain_extent(int top)\n"
              "    int input_height = 2\n"
              "    int total = 0\n"
              "    for input_width in range(3)\n"
              "        total = total + input_width\n"
              "    match int.parse(\"7\")\n"
              "        int parsed\n"
              "            total = total + parsed\n"
              "        error problem\n"
              "            return 0\n"
              "    return input_height + total + top\n"
              "\n"
              "if main\n"
              "    int input_height = 6\n"
              "    for input_width in range(2)\n"
              "        print(\"{input_height + input_width}{NL}\")\n");
        write("main.qui",
              "import lib = \"./lib.qui\"\n"
              "\n"
              "int input_height()\n"
              "    return 1\n"
              "\n"
              "int input_width()\n"
              "    return 1\n"
              "\n"
              "int parsed()\n"
              "    return 1\n"
              "\n"
              "class Box\n"
              "    int value\n"
              "\n"
              "int total = lib.generic_extent<int>(1, 2) + lib.plain_extent(3)\n");
        (void)quidra::compile_file(root / "main.qui", {}, root);

        // The module's own declarations stay visible to its locals, in plain
        // functions and in generic instances alike.
        write("own.qui",
              "int helper()\n"
              "    return 1\n"
              "\n"
              "int generic_shadows_own<T: numeric>(T value)\n"
              "    int helper = 3\n"
              "    return helper\n");
        write("own_main.qui",
              "import own = \"./own.qui\"\n"
              "\n"
              "int value = own.generic_shadows_own<int>(1)\n");
        if (!compile_reports_shadowing(root / "own_main.qui", root))
            throw std::runtime_error("own-module local shadowing was not reported");

        // The same holds for an imported module's `if main` guard: rejected
        // when imported, exactly as when the module is compiled on its own.
        write("own_guard.qui",
              "int helper()\n"
              "    return 1\n"
              "\n"
              "if main\n"
              "    int helper = 3\n"
              "    print(\"{helper}{NL}\")\n");
        write("own_guard_main.qui",
              "import own_guard = \"./own_guard.qui\"\n"
              "\n"
              "int value = own_guard.helper()\n");
        if (!compile_reports_shadowing(root / "own_guard.qui", root))
            throw std::runtime_error("standalone guard shadowing was not reported");
        if (!compile_reports_shadowing(root / "own_guard_main.qui", root))
            throw std::runtime_error("imported guard shadowing was not reported");
        std::filesystem::remove_all(root);
    } catch (const quidra::CompileErrors& errors) {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        std::cerr << "imported local scope failed:";
        for (const auto& d : errors.diagnostics()) {
            std::cerr << " " << d.span.start.line << ":" << d.span.start.column
                      << " [" << d.code << "] " << d.message;
        }
        std::cerr << "\n";
        std::exit(1);
    } catch (const std::exception& e) {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        std::cerr << "imported local scope failed: " << e.what() << "\n";
        std::exit(1);
    }
}

// Parameters, fields, and method names of an imported declaration are
// checked against the class and enum names of their own module, never against
// the importing program's (a package generic `save<M>(M model)` must accept an
// importer's `class model`). Standard-library classes are language-owned and
// never conflict with root user classes.
static void imported_signatures_use_module_scope() {
    const auto root = std::filesystem::temp_directory_path() / "quidra-imported-signature-scope";
    const auto write = [&](const char* name, const char* text) {
        std::ofstream out(root / name, std::ios::binary);
        if (!out) throw std::runtime_error("cannot create temporary Quidra source");
        out << text;
    };
    try {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        write("lib.qui",
              "int scale(int height, int width)\n"
              "    return height * width\n"
              "\n"
              "int generic_scale<M>(M model, int height)\n"
              "    return height\n"
              "\n"
              "class Frame\n"
              "    int height\n"
              "\n"
              "    construct(int h)\n"
              "        this.height = h\n"
              "\n"
              "    int area(int width)\n"
              "        return this.height * width\n"
              "\n"
              "    int depth()\n"
              "        return 1\n");
        write("main.qui",
              "import lib = \"./lib.qui\"\n"
              "\n"
              "class height\n"
              "    int v\n"
              "\n"
              "class model\n"
              "    int v\n"
              "\n"
              "class depth\n"
              "    int v\n"
              "\n"
              "enum width\n"
              "    narrow\n"
              "    wide\n"
              "\n"
              "model m\n"
              "m.v = 1\n"
              "lib.Frame f = lib.Frame(2)\n"
              "int total = lib.scale(2, 3) + lib.generic_scale<model>(m, 4) + f.area(4) + f.depth()\n");
        (void)quidra::compile_file(root / "main.qui", {}, root);

        // Language-owned members (time.Duration.seconds(), ref.Cell.value)
        // never conflict with root user classes.
        write("std.qui",
              "class seconds\n"
              "    int v\n"
              "\n"
              "class value\n"
              "    int v\n"
              "\n"
              "time.Duration pause = time.seconds(0.5)\n"
              "ref.Cell<int64> cell = ref.Cell<int64>(value = 3)\n"
              "real64 total = pause.seconds() + real64(cell.value)\n");
        (void)quidra::compile_file(root / "std.qui", {}, root);

        // A module's own classes stay reserved for its parameters, method
        // parameters, fields, and method names, whether it is compiled on its
        // own or imported.
        write("own_parameter.qui",
              "class shape\n"
              "    int v\n"
              "\n"
              "int rank_of(int[] shape)\n"
              "    return 2\n");
        write("own_method_parameter.qui",
              "enum shape\n"
              "    flat\n"
              "\n"
              "class Frame\n"
              "    int height\n"
              "\n"
              "    int rank_of(int[] shape)\n"
              "        return 2\n");
        write("own_field.qui",
              "class shape\n"
              "    int v\n"
              "\n"
              "class Frame\n"
              "    int shape\n");
        write("own_method.qui",
              "class shape\n"
              "    int v\n"
              "\n"
              "class Frame\n"
              "    int height\n"
              "\n"
              "    int shape()\n"
              "        return 1\n");
        for (const char* module : {"own_parameter", "own_method_parameter", "own_field", "own_method"}) {
            const auto importer = std::string(module) + "_main.qui";
            write(importer.c_str(),
                  (std::string("import ") + module + " = \"./" + module + ".qui\"\n\nint value = 1\n").c_str());
            bool standalone = false;
            bool imported = false;
            for (const auto& [path, rejected] :
                 {std::pair{root / (std::string(module) + ".qui"), &standalone},
                  std::pair{root / importer, &imported}}) {
                try {
                    (void)quidra::compile_file(path, {}, root);
                } catch (const quidra::CompileErrors& errors) {
                    for (const auto& d : errors.diagnostics())
                        *rejected |= d.code == "SHADOWING" || d.code == "DUPLICATE_NAME";
                } catch (const quidra::CompileError& error) {
                    *rejected = error.diagnostic().code == "SHADOWING" ||
                                error.diagnostic().code == "DUPLICATE_NAME";
                }
            }
            if (!standalone || !imported)
                throw std::runtime_error(std::string("own-module declaration conflict in ") + module +
                                         " was not reported (standalone " + (standalone ? "yes" : "no") +
                                         ", imported " + (imported ? "yes" : "no") + ")");
        }
        std::filesystem::remove_all(root);
    } catch (const quidra::CompileErrors& errors) {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        std::cerr << "imported signature scope failed:";
        for (const auto& d : errors.diagnostics()) {
            std::cerr << " " << d.span.start.line << ":" << d.span.start.column
                      << " [" << d.code << "] " << d.message;
        }
        std::cerr << "\n";
        std::exit(1);
    } catch (const std::exception& e) {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        std::cerr << "imported signature scope failed: " << e.what() << "\n";
        std::exit(1);
    }
}

// Generic type parameter names of imported functions and classes are
// checked against the functions, classes, and enums of their own module,
// never against the importing program's (`import nn` with a root `int M()`
// must accept nn's `save<M>`). Standard-library generics such as
// map.Map<K, V> never conflict with root declarations. A module whose type
// parameter reuses its own declaration name is rejected whether it is compiled
// on its own or imported.
static void imported_type_parameters_use_module_scope() {
    const auto root = std::filesystem::temp_directory_path() / "quidra-imported-type-parameter-scope";
    const auto write = [&](const char* name, const char* text) {
        std::ofstream out(root / name, std::ios::binary);
        if (!out) throw std::runtime_error("cannot create temporary Quidra source");
        out << text;
    };
    try {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        write("lib.qui",
              "class Holder<M>\n"
              "    M value\n"
              "\n"
              "int first<T>(T[] values, int n)\n"
              "    return n\n"
              "\n"
              "int pick<K, V>(K key, V value)\n"
              "    return 1\n");
        // A nested importer whose own class T does not belong to lib's scope.
        write("outer.qui",
              "import lib = \"./lib.qui\"\n"
              "\n"
              "class T\n"
              "    int v\n"
              "\n"
              "int through(int n)\n"
              "    int[] values = [1, 2]\n"
              "    return lib.first(values, n)\n");
        write("main.qui",
              "import lib = \"./lib.qui\"\n"
              "import outer = \"./outer.qui\"\n"
              "\n"
              "int M()\n"
              "    return 3\n"
              "\n"
              "class T\n"
              "    int v\n"
              "\n"
              "class K\n"
              "    int v\n"
              "\n"
              "enum V\n"
              "    low\n"
              "    high\n"
              "\n"
              "lib.Holder<int> holder\n"
              "holder.value = 4\n"
              "int[] values = [1, 2]\n"
              "string key = \"k\"\n"
              "real64 scale = 2.0\n"
              "map.Map<string, int> counts = map.Map<string, int>()\n"
              "int total = lib.first(values, 2) + lib.pick(key, scale) + outer.through(5) + holder.value + M()\n");
        (void)quidra::compile_file(root / "main.qui", {}, root);

        write("own_function.qui",
              "class T\n"
              "    int v\n"
              "\n"
              "int first<T>(T value)\n"
              "    return 1\n");
        write("own_class.qui",
              "int M()\n"
              "    return 1\n"
              "\n"
              "class Holder<M>\n"
              "    M value\n");
        write("own_enum.qui",
              "enum K\n"
              "    low\n"
              "\n"
              "int pick<K>(K key)\n"
              "    return 1\n");
        for (const char* module : {"own_function", "own_class", "own_enum"}) {
            const auto importer = std::string(module) + "_main.qui";
            write(importer.c_str(),
                  (std::string("import ") + module + " = \"./" + module + ".qui\"\n\nint value = 1\n").c_str());
            const bool standalone =
                compile_reports_shadowing(root / (std::string(module) + ".qui"), root);
            const bool imported = compile_reports_shadowing(root / importer, root);
            if (!standalone || !imported)
                throw std::runtime_error(std::string("own-module type parameter conflict in ") + module +
                                         " was not reported (standalone " + (standalone ? "yes" : "no") +
                                         ", imported " + (imported ? "yes" : "no") + ")");
        }
        std::filesystem::remove_all(root);
    } catch (const quidra::CompileErrors& errors) {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        std::cerr << "imported type parameter scope failed:";
        for (const auto& d : errors.diagnostics()) {
            std::cerr << " " << d.span.start.line << ":" << d.span.start.column
                      << " [" << d.code << "] " << d.message;
        }
        std::cerr << "\n";
        std::exit(1);
    } catch (const quidra::CompileError& error) {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        const auto& d = error.diagnostic();
        std::cerr << "imported type parameter scope failed: " << d.span.start.line << ":"
                  << d.span.start.column << " [" << d.code << "] " << d.message << "\n";
        std::exit(1);
    } catch (const std::exception& e) {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        std::cerr << "imported type parameter scope failed: " << e.what() << "\n";
        std::exit(1);
    }
}

static bool compile_reports_code(const std::filesystem::path& path,
                                 const std::filesystem::path& root, const std::string& code) {
    try {
        (void)quidra::compile_file(path, {}, root);
    } catch (const quidra::CompileErrors& errors) {
        bool found = false;
        for (const auto& d : errors.diagnostics()) found |= d.code == code;
        return found;
    } catch (const quidra::CompileError& error) {
        return error.diagnostic().code == code;
    }
    return false;
}

// A bare name in imported module code resolves in that module.
// A function value or a call denotes the module's own function, and the
// importing program's top-level functions are not visible: they neither
// replace the module's function values nor hide its classes' fields.
static void imported_names_resolve_in_module_scope() {
    const auto root = std::filesystem::temp_directory_path() / "quidra-imported-name-resolution";
    const auto write = [&](const char* name, const char* text) {
        std::ofstream out(root / name, std::ios::binary);
        if (!out) throw std::runtime_error("cannot create temporary Quidra source");
        out << text;
    };
    try {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        write("lib.qui",
              "int helper()\n"
              "    return 7\n"
              "\n"
              "int use()\n"
              "    fn<int>() f = helper\n"
              "    return f()\n"
              "\n"
              "int use_generic<T>(T value)\n"
              "    fn<int>() f = helper\n"
              "    return f()\n"
              "\n"
              "class Box\n"
              "    int storage\n"
              "\n"
              "    int twice()\n"
              "        return this.storage * 2\n");
        write("main.qui",
              "import lib = \"./lib.qui\"\n"
              "\n"
              "int helper()\n"
              "    return 99\n"
              "\n"
              "int storage()\n"
              "    return 5\n"
              "\n"
              "lib.Box b\n"
              "b.storage = 4\n"
              "int total = lib.use() + lib.use_generic<int>(1) + b.twice() + helper() + storage()\n");
        const auto compiled = quidra::compile_file(root / "main.qui", {}, root);
        const auto dumped = quidra::ir::dump(compiled.ir);
        if (dumped.find("fn.ref lib.helper") == std::string::npos ||
            dumped.find("fn.ref helper ") != std::string::npos)
            throw std::runtime_error("lib's function value did not resolve to lib.helper:\n" + dumped);

        // A call from module code to a function only the importer declares is
        // unknown, exactly as when the module is compiled on its own.
        write("calls.qui",
              "int use()\n"
              "    return missing() + 1\n");
        write("calls_main.qui",
              "import calls = \"./calls.qui\"\n"
              "\n"
              "int missing()\n"
              "    return 1\n"
              "\n"
              "int value = calls.use()\n");
        const bool standalone = compile_reports_code(root / "calls.qui", root, "UNKNOWN_NAME");
        const bool imported = compile_reports_code(root / "calls_main.qui", root, "UNKNOWN_NAME");
        if (!standalone || !imported)
            throw std::runtime_error(std::string("call to an importer-only function was not rejected (standalone ") +
                                     (standalone ? "yes" : "no") + ", imported " +
                                     (imported ? "yes" : "no") + ")");
        std::filesystem::remove_all(root);
    } catch (const quidra::CompileErrors& errors) {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        std::cerr << "imported name resolution failed:";
        for (const auto& d : errors.diagnostics()) {
            std::cerr << " " << d.span.start.line << ":" << d.span.start.column
                      << " [" << d.code << "] " << d.message;
        }
        std::cerr << "\n";
        std::exit(1);
    } catch (const quidra::CompileError& error) {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        const auto& d = error.diagnostic();
        std::cerr << "imported name resolution failed: " << d.span.start.line << ":"
                  << d.span.start.column << " [" << d.code << "] " << d.message << "\n";
        std::exit(1);
    } catch (const std::exception& e) {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        std::cerr << "imported name resolution failed: " << e.what() << "\n";
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


// compile_file reports every package the program imports, directly or through
// another package, with the main module the loader resolved: the map a native
// build reads the packages' native inputs from. It equals what a separate
// resolution pass finds.
static void compile_file_reports_imported_packages() {
    const auto root = std::filesystem::temp_directory_path() / "quidra-compiled-packages";
    const auto write = [&](const std::filesystem::path& name, const char* text) {
        std::filesystem::create_directories((root / name).parent_path());
        std::ofstream out(root / name, std::ios::binary);
        if (!out) throw std::runtime_error("cannot create temporary Quidra source");
        out << text;
    };
    const auto set_package_path = [](const std::string& value) {
#ifdef _WIN32
        return _putenv_s("QUIDRA_PACKAGE_PATH", value.c_str()) == 0;
#else
        return value.empty() ? unsetenv("QUIDRA_PACKAGE_PATH") == 0
                             : setenv("QUIDRA_PACKAGE_PATH", value.c_str(), 1) == 0;
#endif
    };
    const auto previous = quidra::platform::environment_value("QUIDRA_PACKAGE_PATH");
    try {
        std::filesystem::remove_all(root);
        write("packages/outer_pkg/quidra.package", "name = outer_pkg\nversion = 0.1.0\n");
        write("packages/outer_pkg/main.qui",
              "import inner = inner_pkg\n"
              "\n"
              "int answer()\n"
              "    return inner.base() + 1\n");
        write("packages/inner_pkg/quidra.package", "name = inner_pkg\nversion = 0.1.0\n");
        write("packages/inner_pkg/main.qui",
              "int base()\n"
              "    return 41\n");
        write("lib.qui", "int twice(int value)\n    return value * 2\n");
        write("main.qui",
              "import outer = outer_pkg\n"
              "import lib = \"./lib.qui\"\n"
              "print(lib.twice(outer.answer()))\n");
        if (!set_package_path((root / "packages").string()))
            throw std::runtime_error("cannot set QUIDRA_PACKAGE_PATH");
        const auto compiled = quidra::compile_file(root / "main.qui", {}, root);
        const std::map<std::string, std::filesystem::path> expected{
            {"inner_pkg", (root / "packages" / "inner_pkg" / "main.qui").lexically_normal()},
            {"outer_pkg", (root / "packages" / "outer_pkg" / "main.qui").lexically_normal()},
        };
        if (compiled.packages != expected) {
            throw std::runtime_error("compile_file reported the wrong packages");
        }
        if (quidra::resolve_package_dependencies(root / "main.qui", root) != compiled.packages) {
            throw std::runtime_error("compile_file and package resolution disagree");
        }
        if (!quidra::compile_file(root / "lib.qui", {}, root).packages.empty()) {
            throw std::runtime_error("a program without package imports reported packages");
        }
        set_package_path(previous.value_or(""));
        std::filesystem::remove_all(root);
    } catch (const std::exception& e) {
        set_package_path(previous.value_or(""));
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        std::cerr << "compiled package map failed: " << e.what() << "\n";
        std::exit(1);
    }
}

// compile_file with a recorder records every file the compilation read,
// with its digest, every path it looked at in vain, and every resolution, in
// the order it made them; the compilation itself is the same.
static void compile_file_records_its_inputs() {
    namespace fs = std::filesystem;
    const auto root = (fs::temp_directory_path() / "quidra-compile-inputs").lexically_normal();
    const auto write = [&](const fs::path& name, const std::string& text) {
        fs::create_directories((root / name).parent_path());
        std::ofstream out(root / name, std::ios::binary);
        if (!out) throw std::runtime_error("cannot create temporary Quidra source");
        out << text;
    };
    const auto set_package_path = [](const std::string& value) {
#ifdef _WIN32
        return _putenv_s("QUIDRA_PACKAGE_PATH", value.c_str()) == 0;
#else
        return value.empty() ? unsetenv("QUIDRA_PACKAGE_PATH") == 0
                             : setenv("QUIDRA_PACKAGE_PATH", value.c_str(), 1) == 0;
#endif
    };
    const auto previous = quidra::platform::environment_value("QUIDRA_PACKAGE_PATH");
    const auto sha = [](const std::string& text) { return quidra::platform::sha256_hex(text); };
    try {
        fs::remove_all(root);
        const std::string manifest = "name = pkg_a\nversion = 0.1.0\n";
        const std::string package = "int base()\n    return 41\n";
        const std::string library = "int twice(int value)\n    return value * 2\n";
        const std::string entry =
            "import pkg = pkg_a\n"
            "import lib = \"./lib.qui\"\n"
            "print(lib.twice(pkg.base()))\n";
        write("second/pkg_a/quidra.package", manifest);
        write("second/pkg_a/main.qui", package);
        write("lib.qui", library);
        write("main.qui", entry);
        fs::create_directories(root / "first");
#ifdef _WIN32
        const char separator = ';';
#else
        const char separator = ':';
#endif
        if (!set_package_path((root / "first").string() + separator + (root / "second").string()))
            throw std::runtime_error("cannot set QUIDRA_PACKAGE_PATH");

        quidra::CompileInputs inputs;
        const auto recorded = quidra::compile_file(root / "main.qui", {}, root, &inputs);
        const auto plain = quidra::compile_file(root / "main.qui", {}, root);
        if (recorded.llvm != plain.llvm) throw std::runtime_error("recording changed the compilation");
        if (!inputs.complete()) throw std::runtime_error("inputs incomplete: " + inputs.incomplete_reason());

        using quidra::InputFileKind;
        using quidra::InputPathState;
        const auto package_main = root / "second" / "pkg_a" / "main.qui";
        const std::vector<quidra::CompileInput> expected{
            quidra::AbsentInput{root / "quidra.lock", InputPathState::missing},
            quidra::InputFile{InputFileKind::source, root / "main.qui", entry.size(), sha(entry)},
            quidra::AbsentInput{root / "first" / "pkg_a" / "main.qui", InputPathState::missing},
            quidra::PackageInput{"pkg_a", package_main},
            quidra::InputFile{InputFileKind::manifest, root / "second" / "pkg_a" / "quidra.package",
                              manifest.size(), sha(manifest)},
            quidra::AbsentInput{root / "second" / "pkg_a" / "project.toml", InputPathState::missing},
            quidra::InputFile{InputFileKind::source, package_main, package.size(), sha(package)},
            quidra::LocalImportInput{root / "main.qui", "./lib.qui", root / "lib.qui"},
            quidra::InputFile{InputFileKind::source, root / "lib.qui", library.size(), sha(library)},
        };
        if (inputs.records() != expected) {
            throw std::runtime_error(
                "recorded " + std::to_string(inputs.records().size()) +
                " inputs that differ from the expected " + std::to_string(expected.size()));
        }

        // A lockfile is recorded with its content, and each locked package
        // with its tree digest.
        const auto lock = quidra::package_lock_text({{"pkg_a", package_main}});
        write("quidra.lock", lock);
        quidra::CompileInputs locked;
        (void)quidra::compile_file(root / "main.qui", {}, root, &locked);
        const auto& records = locked.records();
        const quidra::CompileInput lock_record =
            quidra::InputFile{InputFileKind::lock, root / "quidra.lock", lock.size(), sha(lock)};
        const quidra::CompileInput tree_record =
            quidra::PackageTreeInput{package_main, quidra::package_tree_sha256(package_main)};
        if (records.empty() || records.front() != lock_record ||
            std::find(records.begin(), records.end(), tree_record) == records.end()) {
            throw std::runtime_error("the lockfile or the package tree was not recorded");
        }

        // A file that exists but cannot be read makes the inputs incomplete.
#ifndef _WIN32
        if (::geteuid() != 0) {
            fs::remove(root / "quidra.lock");
            write("second/pkg_a/project.toml", "[package]\n");
            fs::permissions(root / "second" / "pkg_a" / "project.toml", fs::perms::none);
            quidra::CompileInputs unreadable;
            (void)quidra::compile_file(root / "main.qui", {}, root, &unreadable);
            fs::permissions(root / "second" / "pkg_a" / "project.toml", fs::perms::owner_all);
            if (unreadable.complete()) throw std::runtime_error("an unreadable input went unnoticed");
        }
#endif
        set_package_path(previous.value_or(""));
        fs::remove_all(root);
    } catch (const std::exception& e) {
        set_package_path(previous.value_or(""));
        std::error_code ignored;
        fs::permissions(root / "second" / "pkg_a" / "project.toml", fs::perms::owner_all, ignored);
        fs::remove_all(root, ignored);
        std::cerr << "compile input recording failed: " << e.what() << "\n";
        std::exit(1);
    }
}

static void package_extension_calls_form_tensor_regions() {
    using namespace quidra;
    using namespace quidra::ir;

    Module module = optimizer_fixtures::calls_form_tensor_regions();
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
        Type::tensor(Type::simple(TypeKind::Real32), 1);

    Module module = optimizer_fixtures::replacement_rewrites_pure_chain();
    Module unsafe = module;
    optimizer_fixtures::drop_fused_safety_traits(unsafe);
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

    Module module = optimizer_fixtures::replacement_crosses_eliminated_release();
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

    Module module = optimizer_fixtures::memory_reuse_respects_tensor_storage_aliases();
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
        Type::tensor(Type::simple(TypeKind::Real32), 1);

    Module module = optimizer_fixtures::conditional_replacement_preserves_owned_argument_lifetime();
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
        Type::tensor(Type::simple(TypeKind::Real32), 1);

    Module module = optimizer_fixtures::conditional_replacement_clones_borrowed_argument();
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

    Module module = optimizer_fixtures::backend_selection_tracks_transfer_layout();
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

// Each generic standard class's instance prefix (quidra/standard_classes.hpp)
// is the start of the names the frontend gives its instances.
static void standard_class_instance_prefixes_follow_the_frontend() {
    namespace standard_class = quidra::standard_class;
    const std::pair<const char*, const char*> classes[] = {
        {standard_class::map, standard_class::map_instance_prefix},
        {standard_class::set, standard_class::set_instance_prefix},
        {standard_class::ref_cell, standard_class::ref_cell_instance_prefix},
    };
    for (const auto& [id, prefix] : classes) {
        const auto expected = quidra::abi::generic_instance_prefix(
            quidra::abi::generic_class_kind, id);
        if (expected != prefix) {
            std::cerr << "instance prefix of " << id << ": expected " << expected
                      << ", found " << prefix << "\n";
            std::exit(1);
        }
    }
}

// The element size the compiler stores (runtime_storage_bytes) is the size
// the runtime stores for the element's dtype (abi::dtype_info), for every
// tensor element type.
static void tensor_element_sizes_match_their_dtypes() {
    const quidra::TypeKind kinds[] = {
        quidra::TypeKind::Int64,    quidra::TypeKind::Int8,    quidra::TypeKind::Int16,
        quidra::TypeKind::Int32,  quidra::TypeKind::Nat8,   quidra::TypeKind::Nat16,
        quidra::TypeKind::Nat32, quidra::TypeKind::Nat64,  quidra::TypeKind::Real64,
        quidra::TypeKind::Real32, quidra::TypeKind::Bool,
    };
    for (const auto kind : kinds) {
        const auto type = quidra::Type::simple(kind);
        const auto dtype = quidra::ir::dtype_of(type);
        if (quidra::runtime_storage_bytes(type) !=
            static_cast<std::size_t>(quidra::abi::dtype_info(dtype).bytes)) {
            std::cerr << "element size of dtype " << quidra::abi::dtype_code(dtype)
                      << " differs between the compiler and the runtime\n";
            std::exit(1);
        }
    }
}

// The checker reserves every C library function the runtime prelude
// declares: an extern declaration that names one is FFI_SYMBOL_CONFLICT
// (checker.cpp, runtime_reserved_c_symbol). The programs are built from the
// prelude, so they are not captured for the golden corpus.
static void prelude_c_library_symbols_are_reserved() {
    const std::string_view prelude = quidra::llvm_backend::runtime_prelude_text();
    std::size_t checked = 0;
    for (auto at = prelude.find("\ndeclare "); at != std::string_view::npos;
         at = prelude.find("\ndeclare ", at + 1)) {
        const auto name_start = prelude.find('@', at) + 1;
        const auto name = prelude.substr(name_start, prelude.find('(', name_start) - name_start);
        if (name.starts_with("quidra_") || name.starts_with("qcore_") || name.starts_with("llvm.")) {
            continue;
        }
        bool reserved = false;
        try {
            (void)quidra::compile("extern int32 c_reserved(int32 value) = \"" + std::string(name) + "\"\n");
        } catch (const quidra::CompileErrors& errors) {
            for (const auto& d : errors.diagnostics()) reserved = reserved || d.code == "FFI_SYMBOL_CONFLICT";
        } catch (const quidra::CompileError& error) {
            reserved = error.diagnostic().code == "FFI_SYMBOL_CONFLICT";
        }
        if (!reserved) {
            std::cerr << "the runtime prelude declares " << name
                      << ", which an extern declaration may name\n";
            std::exit(1);
        }
        ++checked;
    }
    if (checked == 0) {
        std::cerr << "the runtime prelude declares no C library function\n";
        std::exit(1);
    }
}

// The backend's runtime handle classes (runtime_abi::runtime_handle_classes,
// with their drop and clone functions) are exactly the standard classes
// whose values are runtime handles (standard_class::is_runtime_handle).
static void runtime_handle_classes_match_the_standard_classes() {
    namespace standard_class = quidra::standard_class;
    const char* const ids[] = {
        standard_class::json_value,     standard_class::http_response,
        standard_class::file_handle,    standard_class::atomic_counter,
        standard_class::autograd_target, standard_class::time_instant,
        standard_class::time_duration,  standard_class::process_result,
        standard_class::random_generator, standard_class::map,
        standard_class::set,            standard_class::ref_cell,
    };
    for (const char* id : ids) {
        const bool handle = quidra::llvm_backend::runtime_abi::runtime_handle_class(id) != nullptr;
        if (handle != standard_class::is_runtime_handle(id)) {
            std::cerr << "runtime handle class mismatch for " << id << "\n";
            std::exit(1);
        }
    }
}

// The code of the first diagnostic a file-aware check of `main` reports, with
// `lib` written beside it as ./lib.qui; empty when it checks.
static std::string imported_check_code(const std::string& main, const std::string& lib) {
    const auto root = std::filesystem::temp_directory_path() / "quidra-declared-names";
    std::string code;
    try {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        std::ofstream(root / "lib.qui", std::ios::binary) << lib;
        (void)quidra::check_file_source(root / "main.qui", main, {}, root);
    } catch (const quidra::CompileErrors& errors) {
        if (!errors.diagnostics().empty()) code = errors.diagnostics().front().code;
    } catch (const quidra::CompileError& error) {
        code = error.diagnostic().code;
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    return code;
}

// `_` is the discard name: no declaration may use it and no expression may
// read it, in every position where a name is declared or read.
static void discard_name_is_never_declared_or_read() {
    const std::string typed = "'_' takes no type";
    const std::string declared = "'_' is the discard name";
    const std::string read = "'_' discards a value";
    bad_message("int _ = 1\n", "DISCARD", typed);
    bad_message("int f()\n    return 1\n\nauto _ = f()\n", "DISCARD", typed);
    bad_message("const int _ = 1\n", "DISCARD", typed);
    bad_message("void f()\n    int _ = 1\n", "DISCARD", typed);
    bad_message("int x = 1\nint &_ = &x\n", "DISCARD", typed);
    bad_message("int f(int _)\n    return 1\n", "DISCARD", declared);
    bad_message("extern int32 c_abs(int32 _) = \"abs\"\n", "DISCARD", declared);
    bad_message("class C\n    int x\n\n    construct(int _)\n        x = 1\n", "DISCARD", declared);
    bad_message("class C\n    int _\n", "DISCARD", declared);
    bad_message("class C\n    int x\n\n    int _()\n        return 1\n", "DISCARD", declared);
    bad_message("class C\n    int x\n\n    int get(int _)\n        return x\n", "DISCARD", declared);
    bad_message("int _()\n    return 1\n", "DISCARD", declared);
    bad_message("class _\n    int x\n", "DISCARD", declared);
    bad_message("class Box<_>\n    int x\n", "DISCARD", declared);
    bad_message("int f<_>(int v)\n    return v\n", "DISCARD", declared);
    bad_message("enum _\n    A\n", "DISCARD", declared);
    bad_message("enum E\n    A\n    _\n", "DISCARD", declared);
    // Imports need a file, and an imported module follows the same rule.
    const std::string lib = "int value()\n    return 1\n";
    if (imported_check_code("import _ = \"./lib.qui\"\n", lib) != "DISCARD" ||
        imported_check_code("import lib = \"./lib.qui\"\nprint(lib.value())\n",
                            "int _()\n    return 1\n\n" + lib) != "DISCARD" ||
        imported_check_code("import lib = \"./lib.qui\"\nprint(lib.value())\n", lib) != "") {
        std::cerr << "'_' as an import alias or in an imported module is not DISCARD\n";
        std::exit(1);
    }
    bad_message("int[] values = [1, 2]\nfor _ in values\n    print(1)\n", "DISCARD", declared);
    bad_message("int | none value = 1\nmatch value\n    int _\n        print(1)\n    none\n        print(2)\n",
                "DISCARD", declared);
    bad_message("int x = 1\nint y = _ + x\n", "DISCARD", read);
    bad_message("print(_)\n", "DISCARD", read);
    bad_message("void f(int v)\n    print(v)\n\nf(_)\n", "DISCARD", read);
    bad_message("void f(int &v)\n    v = 1\n\nf(&_)\n", "DISCARD", read);
    bad_message("print(\"{_}\")\n", "DISCARD", read);
    bad_message("_ = 1\n", "DISCARD", read);
    bad_message("_ += 1\n", "DISCARD", read);
    bad_message("int x = 1\n&_ = &x\n", "DISCARD", read);
    bad_message("class C\n    int x\n\n    int get()\n        return _\n", "DISCARD", read);
    // The tensor shape wildcard keeps its meaning.
    good("tensor<real32><2, _> grid = tensor.zeros<real32>([2, 3])\nprint(grid.shape()[1])\nprint(NL)\n");
    // A name that only begins with '_' is an ordinary name.
    good("int _count = 1\nint __total = _count + 1\nprint(__total)\nprint(NL)\n");
}

// A class belongs to the standard library by where the compiler created it,
// never by its name: a user class named like a standard class's instance is
// an ordinary class, and the names the compiler makes ("__quidra_...") cannot
// be declared at all.
static void standard_library_classes_are_marked_by_origin() {
    const std::string reserved = "starts with '__quidra_'";
    // A user class named like the instance of a generic standard class.
    bad_message("class __quidra_gc__std_box\n    int value\n\n"
                "__quidra_gc__std_box b = __quidra_gc__std_box()\n",
                "SHADOWING", reserved);
    // A user generic class whose instance is mangled like a standard one is
    // still a user class: without a constructor it is not constructible,
    // exactly as Box<T> is not.
    bad_code("class Box<T>\n    T value\n\nBox<int> b = Box<int>()\n", "NO_CONSTRUCTOR");
    bad_code("class _std_box<T>\n    T value\n\n_std_box<int> b = _std_box<int>()\n",
             "NO_CONSTRUCTOR");
    // Fields of a user class named like map.Map's instances are user fields,
    // readable like any other.
    good("class _std_map_Map<K, V>\n    K __key\n    V __value\n\n"
         "    construct(K key, V value)\n        this.__key = key\n        this.__value = value\n\n"
         "_std_map_Map<int, string> entry = _std_map_Map<int, string>(1, \"one\")\n"
         "print(entry.__value)\nprint(NL)\n");
    // A standard collection's internals stay hidden.
    bad_code("map.Map<int, int> values = map.Map<int, int>()\nprint(len(values.__keys))\n",
             "UNKNOWN_MEMBER");
    // Every kind of declaration rejects the compiler's names.
    bad_message("int __quidra_value = 1\n", "SHADOWING", reserved);
    bad_message("void f()\n    int __quidra_value = 1\n", "SHADOWING", reserved);
    bad_message("int __quidra_f()\n    return 1\n", "SHADOWING", reserved);
    bad_message("int f(int __quidra_v)\n    return 1\n", "SHADOWING", reserved);
    bad_message("class C\n    int __quidra_x\n", "SHADOWING", reserved);
    bad_message("class C\n    int x\n\n    int __quidra_get()\n        return x\n",
                "SHADOWING", reserved);
    bad_message("T f<__quidra_T>(int v)\n    return v\n", "SHADOWING", reserved);
    bad_message("enum __quidra_E\n    A\n", "SHADOWING", reserved);
    bad_message("int[] values = [1, 2]\nfor __quidra_v in values\n    print(1)\n",
                "SHADOWING", reserved);
    const std::string lib = "int value()\n    return 1\n";
    if (imported_check_code("import __quidra_lib = \"./lib.qui\"\n", lib) != "SHADOWING" ||
        imported_check_code("import lib = \"./lib.qui\"\nprint(lib.value())\n",
                            "class __quidra_box\n    int v\n\n" + lib) != "SHADOWING") {
        std::cerr << "a '__quidra_' import alias or imported declaration is not SHADOWING\n";
        std::exit(1);
    }
    // A name that merely contains the text is an ordinary name.
    good("int my__quidra_value = 1\nprint(my__quidra_value)\nprint(NL)\n");
}

// If expressions: typing with and without a contextual type, the
// parenthesis rules the parser enforces, references, and flow.
static void if_expressions_are_typed_like_initializers() {
    const std::string ready = "bool ready = true\n";
    // With a contextual type every branch is checked as an initializer is.
    good(ready + "real32 x = if ready then 1.0 else 2.0\nprint(x)\nprint(NL)\n");
    good(ready + "int | error result = if ready then 1 else error(\"bad\")\n"
                 "match result\n    int value\n        print(value)\n    error problem\n        print(problem)\n");
    good(ready + "int x = 1 + (if ready then 2 else 3)\nint y = x * (if ready then 2 else 3)\nprint(y)\nprint(NL)\n");
    good(ready + "int64 n = 5\nint64 x = n + (if ready then 1 else 2)\nprint(x)\nprint(NL)\n");
    // Without one the typed branches have one type, and a literal branch
    // materializes into it.
    good(ready + "int64 n = 5\nauto x = if ready then n else 0\nint64 y = x\nprint(y)\nprint(NL)\n");
    good(ready + "int64 n = 5\nauto x = if ready then n else -1\nint64 y = x\nprint(y)\nprint(NL)\n");
    good(ready + "string a = \"a\"\nauto s = if ready then a else \"b\"\nprint(s)\nprint(NL)\n");
    bad_code(ready + "int64 n = 5\nauto x = if ready then n else 2.5\n", "NUMERIC_FAMILY");
    bad(ready + "auto x = if ready then 1 else 2.0\n");
    bad_code(ready + "auto x = if ready then 1 else 2\n", "AMBIGUOUS_NUMERIC_LITERAL");
    bad_message(ready + "int64 a = 1\nreal64 b = 2.0\nauto x = if ready then a else b\n",
                "TYPE_MISMATCH", "different types: 'int64' and 'real64'");
    bad_message("int k = 1\nint x = if k then 1 else 2\n", "TYPE_MISMATCH",
                "An if-expression condition must be bool.");
    // An if-expression as the condition of an if-expression, parenthesized.
    good(ready + "int x = if (if ready then false else true) then 1 else 2\nprint(x)\nprint(NL)\n");
    // Strings, arrays, classes, tensors and unions as branches.
    good(ready + "int[] a = [1, 2]\nint[] b = [3]\nint[] c = if ready then a else b\nprint(len(c))\nprint(NL)\n");
    good("class P\n    int v\n\n    construct(int value)\n        this.v = value\n\n" + ready +
         "P p = P(1)\nP q = P(2)\nP r = if ready then p else q\nprint(r.v)\nprint(NL)\n"
         "bool same = (if ready then p else q) == p\nprint(same)\nprint(NL)\n");
    good(ready + "tensor<real32> t = tensor.zeros<real32>([2])\n"
                 "tensor<real32> u = tensor.ones<real32>([2])\n"
                 "tensor<real32> w = if ready then t else u\nprint(w[0].item())\nprint(NL)\n");
    good(ready + "int | none v = if ready then 1 else none\n"
                 "match v\n    int value\n        print(value)\n    none\n        print(\"none\")\n");
    // Whole-expression positions.
    good("int pick(bool c, int a, int b = if true then 1 else 2)\n    return if c then a elif a > b then b else a + b\n\n" +
         ready + "print(pick(ready, if ready then 3 else 4))\nprint(NL)\n"
         "int[] v = [if ready then 1 else 2, 3]\nprint(v[if ready then 0 else 1])\nprint(NL)\n"
         "int total = 0\ntotal += if ready then 1 else 2\nprint(\"{if ready then total else 0}\")\nprint(NL)\n"
         "class Box\n    int v = if true then 1 else 2\n");
    // Generic inference sees the branches' type.
    good("T identity<T>(T value)\n    return value\n\n" + ready +
         "int64 n = 1\nint64 m = identity(if ready then n else 0)\nprint(m)\nprint(NL)\n");
    // Void branches give no value.
    bad_code("void g()\n    return\n\n" + ready + "(if ready then g() else g())\n", "TYPE_MISMATCH");
    // An if-expression gives a value, never a reference.
    const std::string storage = ready + "int a = 1\nint b = 2\n";
    const std::string writer = "void assign(int &target)\n    target = 1\n\n";
    bad_code(writer + storage + "assign(&if ready then a else b)\n", "IF_EXPRESSION");
    bad_code(writer + storage + "assign(&(if ready then a else b))\n", "IF_EXPRESSION");
    bad_code(storage + "int &r = if ready then &a else &b\n", "IF_EXPRESSION");
    bad_code(storage + "bool same = (if ready then &a else &b) == &a\n", "IF_EXPRESSION");
    bad_code(storage + "int &r = &a\n&r = &(if ready then a else b)\n", "IF_EXPRESSION");
    // Parse errors keep their code through compile().
    bad_code(ready + "int x = if ready then 1\n", "IF_EXPRESSION");
    bad_code(ready + "int x = 1 + if ready then 2 else 3\n", "IF_EXPRESSION");
    // Initialization flows as through an if statement: storage a branch
    // initializes is initialized afterwards on every path only if every
    // branch does, otherwise a read of it is checked at run time, and a
    // condition always runs.
    const std::string init = "bool fill(int &target)\n    target = 1\n    return true\n\n";
    good(init + ready + "int x\nbool done = if ready then fill(&x) else false\nprint(x)\nprint(NL)\n");
    ir_contains(init + ready + "int x\nbool done = if ready then fill(&x) else false\nprint(x)\n",
                "init.check %");
    good(init + ready + "int x\nbool done = if fill(&x) then ready else false\nprint(x)\nprint(NL)\n");
    good(init + ready + "int x\nbool done = if ready then fill(&x) else fill(&x)\nprint(x)\nprint(NL)\n");
    // Lowering and tools.
    ir_contains(ready + "int x = if ready then 1 else 2\nprint(x)\nprint(NL)\n", "if.result");
    inspect_contains(ready + "int x = if ready then 1 else 2\n", "\"if_expression\"");
}

// Initialization checking (L13): a read that no path initializes is a
// compile error, a read that some paths initialize is checked at run time,
// and a read that every path initializes has no check.
static void initialization_is_checked_by_path() {
    const std::string ready = "bool ready = len(\"ab\") > 1\n";
    // Every path: no flag, no check.
    good("int x\nif true\n    x = 1\nelse\n    x = 2\nprint(x)\nprint(NL)\n");
    ir_function_not_contains("int both(bool c)\n    int x\n    if c\n        x = 1\n    else\n        x = 2\n"
                             "    return x\n", "both", "init.check");
    // No path: a compile error naming the binding.
    bad_message("int x\nprint(x)\n", "UNINITIALIZED",
                "'x' is read before it is initialized on every path.");
    bad_message("int one(bool c)\n    int x\n    if c\n        return 1\n    return x\n", "UNINITIALIZED",
                "'x' is read before it is initialized on every path.");
    // Some paths: a run-time check on the read. Conditions are not folded:
    // `if true` without else and `while false` leave a read to the check.
    good("int x\nif true\n    x = 1\nprint(x)\nprint(NL)\n");
    good("int x\nwhile false\n    x = 1\nprint(x)\nprint(NL)\n");
    ir_function_contains("int maybe(bool c)\n    int x\n    if c\n        x = 1\n    return x\n",
                         "maybe", "init.check %");
    ir_function_contains("int zero(int n)\n    int x\n    for i in range(n)\n        x = i\n    return x\n",
                         "zero", "init.check %");
    ir_function_contains("int repeat(bool c)\n    int x\n    while c\n        x = 1\n        return x\n"
                         "    return x\n", "repeat", "init.check %");
    ir_function_contains("int pick(int | none value)\n    int x\n    match value\n        int present\n"
                         "            x = present\n        none\n            void\n    return x\n",
                         "pick", "init.check %");
    // A read later in a loop body than the write it depends on, on a later
    // iteration.
    ir_function_contains("int carried(int n)\n    int x\n    int total = 0\n    for i in range(n)\n"
                         "        if i > 0\n            total += x\n        x = i\n    return total\n",
                         "carried", "init.check %");
    // A loop that runs at least once and has no break or continue
    // initializes what its body does: a constant range, a fixed array, a
    // literal.
    ir_function_not_contains("int counted()\n    int x\n    for i in range(3)\n        x = i\n    return x\n",
                             "counted", "init.check");
    ir_function_not_contains("int fixed(int[3] values)\n    int x\n    for v in values\n        x = v\n"
                             "    return x\n", "fixed", "init.check");
    ir_function_not_contains("string listed()\n    string x\n    for v in [\"a\", \"b\"]\n        x = v\n"
                             "    return x\n", "listed", "init.check");
    ir_function_contains("int stopped()\n    int x\n    for i in range(3)\n        if i == 1\n            break\n"
                         "        x = i\n    return x\n", "stopped", "init.check %");
    ir_function_contains("int empty_range()\n    int x\n    for i in range(3, 3)\n        x = i\n    return x\n",
                         "empty_range", "init.check %");
    // Past a checked read the binding is initialized: one check.
    good(ready + "int x\nif ready\n    x = 1\nprint(x)\nprint(x + 1)\nprint(NL)\n");
    // A reference whose target is known reads its target's flag.
    ir_function_contains("int through(bool c)\n    int x\n    if c\n        x = 1\n    int &r = &x\n"
                         "    return r\n", "through", "init.check %");
    // A reference whose target is not known after a join stays an error.
    bad_message("int left = 1\nint right\nint &slot = &left\nbool choose = true\nif choose\n"
                "    &slot = &right\nprint(slot)\nprint(NL)\n", "UNINITIALIZED",
                "may be uninitialized through a reference whose target is not known");
    // A callee that writes on some paths only leaves the binding untracked:
    // still an error, also in a loop.
    const std::string partial = "void partial(int &value, bool ok)\n    if ok\n        value = 1\n\n";
    bad_message(partial + "int value\npartial(&value, false)\nprint(value)\n", "UNINITIALIZED",
                "may be uninitialized");
    bad_message(partial + ready + "int value\nwhile ready\n    partial(&value, true)\n    print(value)\n",
                "UNINITIALIZED", "may be uninitialized");
    // A read early in a loop body that a later untracked write could reach
    // on the next iteration.
    bad_message(partial + ready + "int value\nwhile ready\n    if len(\"abc\") > 2\n        print(value)\n"
                "    partial(&value, true)\n", "UNINITIALIZED", "may be uninitialized");
    // A callee that reads the whole binding checks it at the call.
    ir_function_contains("void show(int &value)\n    print(value)\n\nvoid caller(bool c)\n    int x\n"
                         "    if c\n        x = 1\n    show(&x)\n", "caller", "init.check %");
}

// Array element reads that no path initializes are compile errors (L13);
// every other element read keeps the run-time element check.
static void array_element_reads_need_an_initializing_path() {
    bad_message("int[3] a\nprint(a[1])\n", "UNINITIALIZED",
                "'a[1]' is read before it is initialized on every path.");
    bad_message("int[] b = array(4)\nprint(b[0])\n", "UNINITIALIZED",
                "'b[0]' is read before it is initialized on every path.");
    bad_message("int[3] a\nint i = int(len(a) - 1)\nprint(a[i])\n", "UNINITIALIZED",
                "'a[i]' is read before it is initialized on every path.");
    bad_message("int[2] a\nint[2] b = [1, 2]\na[0] = 1\nprint(a == b)\n", "UNINITIALIZED",
                "'a[1]' is read before it is initialized on every path.");
    bad_message("int[3] a\na[0] = 5\nprint(a[1])\n", "UNINITIALIZED", "'a[1]'");
    good("int[3] a\na[0] = 5\nprint(a[0])\nprint(NL)\n");
    // A store under a branch, through a reference, at another index or in a
    // loop leaves the element to the run-time check.
    good("int[3] a\nif len(a) > 1\n    a[1] = 5\nprint(a[1])\nprint(NL)\n");
    good("void fill(int[3] &values)\n    values[2] = 1\n\nint[3] a\nfill(&a)\nprint(a[2])\nprint(NL)\n");
    good("int[3] a\nint i = int(len(a) - 1)\na[i] = 1\nprint(a[0])\nprint(NL)\n");
    good("int[] b = array(3)\nfor i in range(len(b))\n    b[i] = i\nprint(b[2])\nprint(NL)\n");
    good("int[] b = array(len(\"ab\"))\nb[0] = 1\nprint(b[1])\nprint(NL)\n");
    good("int[3] a\nint &slot = &a[1]\nslot = 4\nprint(a[1])\nprint(NL)\n");
    // Whole assignment initializes every element; tensors stay run-time.
    good("int[3] a\na = [1, 2, 3]\nprint(a[2])\nprint(NL)\n");
    good("tensor<real32> t = tensor<real32>([2])\nt[0] = 1.0\nprint(t[1].item())\nprint(NL)\n");
}

// Initialization checking of class fields (L13): a field of a local value or
// of the receiver in its constructor that some paths store is checked at run
// time through the bit its class keeps per field; fields written where no
// bit records it stay compile errors.
static void field_initialization_is_checked_by_path() {
    const std::string point = "class Point\n    int x\n    int y = 0\n\n";
    ir_function_contains(point + "int pick(bool c)\n    Point p\n    if c\n        p.x = 1\n    return p.x\n",
                         "pick", "init.check %");
    ir_contains(point + "bool c = len(\"ab\") > 1\nPoint p\nif c\n    p.x = 1\nprint(p.x)\n", "field 'p.x'");
    ir_function_not_contains(point + "int both(bool c)\n    Point p\n    if c\n        p.x = 1\n    else\n"
                             "        p.x = 2\n    return p.x\n", "both", "init.check");
    bad_message(point + "Point p\nprint(p.x)\n", "UNINITIALIZED",
                "'p.x' is read before it is initialized on every path.");
    // A value from elsewhere keeps today's rule: its bits are not known.
    bad_message(point + "Point make()\n    Point p\n    p.x = 1\n    return p\n\nbool c = len(\"ab\") > 1\n"
                "Point q = make()\nPoint r\nif c\n    r = q\nprint(r.y)\nprint(q.y)\nPoint s\nif c\n    s.y = 1\n"
                "Point t = s\nprint(t.x)\n", "UNINITIALIZED", "may be uninitialized");
    // A field written through a reference or by a callee is not tracked.
    bad_message(point + "void fill(int &value, bool ok)\n    if ok\n        value = 1\n\nPoint p\n"
                "fill(&p.x, true)\nprint(p.x)\n", "UNINITIALIZED", "may be uninitialized");
    // The receiver in its constructor.
    ir_function_contains("class Gate\n    int value\n    int seen = 0\n\n    construct(bool open)\n"
                         "        if open\n            this.value = 1\n        this.seen = this.value\n",
                         "$construct.Gate", "init.check %");
    bad_message("class Shut\n    int value\n\n    construct()\n        print(this.value)\n", "UNINITIALIZED",
                "is read before the constructor initializes it");
    // A local that shares a field's name keeps its own flag: `this.value`
    // stores and reads the receiver's field, the bare name the local.
    const std::string gate = "class Gate\n    int value\n    int seen = 0\n\n";
    const std::string local_maybe = gate + "    construct(bool open)\n        int value\n        if open\n"
        "            value = 1\n        this.value = 2\n        this.seen = value\n";
    ir_function_contains(local_maybe, "$construct.Gate", "binding 'value'");
    ir_function_not_contains(local_maybe, "$construct.Gate", "field 'this.value'");
    const std::string field_maybe = gate + "    construct(bool open)\n        int value = 1\n        if open\n"
        "            this.value = value\n        this.seen = this.value\n";
    ir_function_contains(field_maybe, "$construct.Gate", "field 'this.value'");
    ir_function_not_contains(field_maybe, "$construct.Gate", "binding 'value'");
    ir_function_not_contains(gate + "    construct(bool open)\n        int value\n        if open\n"
                             "            value = 1\n            this.value = 2\n        else\n"
                             "            value = 3\n            this.value = 4\n"
                             "        this.seen = value + this.value\n",
                             "$construct.Gate", "init.check");
    bad_message(gate + "    construct()\n        int value\n        this.value = 1\n        this.seen = value\n",
                "UNINITIALIZED", "'value' is read before it is initialized on every path.");
    bad_message(gate + "    construct()\n        int value = 1\n        this.seen = this.value\n",
                "UNINITIALIZED", "is read before the constructor initializes it");
    // Equality and display skip the mask; copies keep it.
    good(point + "bool c = len(\"ab\") > 1\nPoint p\np.x = 1\nPoint q\nif c\n    q.x = 1\nprint(q.x)\n"
         "Point r\nr.x = 1\nprint(\"{p == r}\")\nprint(NL)\n");
    // Core gap G-d: an element of an initialized array of class values is a
    // whole value inside a method too.
    good(R"(class Slot
    int storage

    construct(int storage)
        this.storage = storage

    int value()
        return this.storage

class Shelf
    Slot[] items

    construct()
        this.items = [Slot(1), Slot(2)]

    int second()
        return this.items[1].value() + this.items[0].storage

Shelf shelf = Shelf()
print(shelf.second())
print(NL)
)");
    bad_code(R"(class Slot
    int storage

class Shelf
    Slot[] items

    int first()
        return this.items[0].storage

Shelf shelf
print(shelf.first())
)", "UNINITIALIZED");
}

// Effect summaries (src/semantics/effect_summary.hpp): what a function may
// write and read of its caller's storage, compose through calls; hidden
// mutations, resources, address observations and result provenance.
static void effect_summaries_describe_functions() {
    namespace semantics = quidra::semantics;
    const auto source = R"(class Counter
    int count = 0
    int[] items = []

    void bump()
        this.count += 1

    int total()
        return this.count

void set_first(int[] &values)
    values[0] = 1

void forward(int[] &values)
    set_first(&values)

void through_alias(int[] &values)
    int &slot = &values[2]
    slot = 4

void local_copy(int[] values)
    values[0] = 2

void say(string text)
    print(text)
    print(NL)

void put(ref.Cell<int> cell)
    cell.value = 3

void put_fresh()
    ref.Cell<int> cell = ref.Cell<int>(value = 1)
    cell.value = 2

void put_copy(ref.Cell<int> cell)
    ref.Cell<int> copy = cell
    copy.value = 5

void bump_counter(Counter &counter)
    counter.bump()

bool same(const int[] b, int[] &a)
    return &b[0] == &a[0]

int[] make()
    return [1, 2]

int[] pass(int[] values)
    return values

int depth(int[] &values, int n)
    if n == 0
        values[1] = 0
        return 0
    return depth(&values, n - 1)

int bump_first(int[] &values)
    values[0] = 5
    return 1

int choose(int[] &values, bool flag)
    return if flag then bump_first(&values) else 0

int[] v = [1, 2, 3]
forward(&v)
through_alias(&v)
local_copy(v)
say("x")
put(ref.Cell<int>(value = 1))
put_fresh()
put_copy(ref.Cell<int>(value = 1))
Counter c
bump_counter(&c)
print(c.total())
print(same(v, &v))
print(make()[0] + pass(v)[0] + depth(&v, 3) + choose(&v, true))
print(NL)
)";
    const auto compiled = quidra::compile(source);
    if (!compiled.checked.effects) {
        std::cerr << "the checked program has no effect summaries\n";
        std::exit(1);
    }
    const auto& summaries = *compiled.checked.effects;
    const auto summary = [&](const std::string& name) -> const semantics::EffectSummary& {
        const auto* found = summaries.find(name);
        if (!found) {
            std::cerr << "no effect summary for " << name << "\n";
            std::exit(1);
        }
        return *found;
    };
    const auto fail = [](const std::string& what) {
        std::cerr << "effect summary: " << what << "\n";
        std::exit(1);
    };
    const auto parameter_path = [](std::size_t index, std::vector<semantics::PathStep> steps) {
        return semantics::StoragePath{semantics::StorageRoot::parameter(index), std::move(steps)};
    };
    const auto element = [](std::int64_t index) { return semantics::PathStep::element(index); };
    const auto field = [](const char* name) { return semantics::PathStep::field_named(name); };

    if (!summary("set_first").exposed_writes.at(0).contains(parameter_path(0, {element(0)})))
        fail("set_first writes values[0]");
    if (summary("set_first").first_hidden_mutation) fail("an & write is not hidden");
    if (!summary("forward").exposed_writes.at(0).contains(parameter_path(0, {element(0)})))
        fail("forward carries set_first's write through its & argument");
    if (!summary("through_alias").exposed_writes.at(0).contains(parameter_path(0, {element(2)})))
        fail("a write through a reference local reaches its target");
    if (!summary("local_copy").exposed_writes.at(0).empty() || summary("local_copy").mutates_ambient)
        fail("a write to a by-value parameter is local");

    const auto& bump = summary("$method.Counter.bump");
    if (!bump.mutates_receiver || !bump.exposed_writes.at(0).contains(parameter_path(0, {field("count")})))
        fail("bump writes its receiver's count");
    if (!bump.first_hidden_mutation || bump.first_hidden_mutation->description != "field 'count'")
        fail("bump's hidden mutation names the field");
    if (summary("$method.Counter.total").mutates_receiver ||
        !summary("$method.Counter.total").exposed_reads.at(0).contains(parameter_path(0, {field("count")})))
        fail("total only reads its receiver");
    if (!summary("bump_counter").exposed_writes.at(0).contains(parameter_path(0, {field("count")})) ||
        summary("bump_counter").first_hidden_mutation)
        fail("a method call on an & parameter writes its target, exposed");

    const semantics::ResourceAccess stdout_access{
        semantics::ResourceKind::Stdout, semantics::ResourceUse::Exclusive,
        semantics::ResourceOrigin::Direct, {}};
    if (!summary("say").resources.contains(stdout_access)) fail("print uses stdout exclusively");
    if (summary("say").mutates_ambient || summary("say").first_hidden_mutation)
        fail("output is not a hidden mutation");

    if (!summary("put").mutates_shared_of_parameter.at(0) || summary("put").mutates_ambient ||
        !summary("put").first_hidden_mutation)
        fail("put changes the cell its argument shares");
    if (summary("put_fresh").first_hidden_mutation || summary("put_fresh").mutates_ambient)
        fail("a cell the function created is not existing state");
    if (!summary("put_copy").mutates_shared_of_parameter.at(0))
        fail("a copy of a parameter shares its cell");

    if (!summary("same").const_parameter_address_observed.at(0) ||
        summary("same").const_parameter_address_observed.at(1))
        fail("same observes the address of its const parameter only");

    using Provenance = semantics::ResultProvenance;
    if (summary("make").result.kind != Provenance::Kind::Fresh) fail("make returns a fresh value");
    if (summary("pass").result.kind != Provenance::Kind::Parameter || summary("pass").result.parameter != 0)
        fail("pass returns its parameter");
    if (!summary("depth").exposed_writes.at(0).contains(parameter_path(0, {element(1)})))
        fail("a recursive function reaches its fixed point");
    if (!summary("choose").exposed_writes.at(0).contains(parameter_path(0, {element(0)})))
        fail("a call inside an if expression is applied");

    const auto cell_place = semantics::StoragePath{semantics::StorageRoot::shared("c"), {field("value")}};
    if (!semantics::overlaps(parameter_path(0, {element(1)}),
                             parameter_path(0, {semantics::PathStep::any_element(), field("x")})) ||
        semantics::overlaps(parameter_path(0, {element(1)}), parameter_path(0, {element(2)})) ||
        semantics::overlaps(parameter_path(0, {field("a")}), parameter_path(0, {field("b")})) ||
        !semantics::overlaps(parameter_path(0, {}), parameter_path(0, {field("b")})) ||
        semantics::overlaps(parameter_path(0, {}), parameter_path(1, {})) ||
        semantics::overlaps(cell_place, parameter_path(0, {field("value")})))
        fail("the overlap rule");
}

// D12: a borrowed argument is copied only for a call that may write its
// storage while it runs (semantics/argument_isolation.hpp).
static void borrowed_arguments_copy_only_where_the_call_writes() {
    const auto calls = std::string(R"(void show(const int[] values)
    print(values[0])
    print(NL)

void keep(const int[] values, int[] &other)
    other[0] = values[0]

class Box
    int[] items = [1, 2]
    int count = 0

    void note(const int[] values)
        this.count = int(len(values))

    void reset(const int[] values)
        this.items = values

)");
    ir_function_not_contains(calls + R"(void separate()
    int[] a = [1, 2, 3]
    int[] b = [4]
    show(a)
    keep(a, &b)
    Box box
    box.note(box.items)

separate()
)", "function separate", "= clone %");
    ir_function_contains(calls + R"(void aliased()
    int[] a = [1, 2, 3]
    keep(a, &a)

aliased()
)", "function aliased", "= clone %");
    ir_function_contains(calls + R"(void receiver()
    Box box
    box.reset(box.items)

receiver()
)", "function receiver", "= clone %");
}

// Package code reports its failures at the user's statement: its statements
// call the package setter, its failure sites pass no location (0, 0), and it
// saves and restores the user statement around calls that may enter user
// code. A user statement refreshes the user statement before a call that
// may read it when a call into user code ran before it in the statement, and
// the initialization check of a load through a reference passes its
// statement's position.
static void package_code_reports_at_the_user_statement() {
    const auto root = std::filesystem::temp_directory_path() / "quidra-user-statement";
    const auto write = [&](const std::filesystem::path& name, const char* text) {
        std::filesystem::create_directories((root / name).parent_path());
        std::ofstream out(root / name, std::ios::binary);
        if (!out) throw std::runtime_error("cannot create temporary Quidra source");
        out << text;
    };
    const auto set_package_path = [](const std::string& value) {
#ifdef _WIN32
        return _putenv_s("QUIDRA_PACKAGE_PATH", value.c_str()) == 0;
#else
        return value.empty() ? unsetenv("QUIDRA_PACKAGE_PATH") == 0
                             : setenv("QUIDRA_PACKAGE_PATH", value.c_str(), 1) == 0;
#endif
    };
    // The text of the function whose symbol is `symbol`.
    const auto body = [](const std::string& llvm, const std::string& symbol) {
        const auto start = llvm.find("@" + symbol + "(");
        if (start == std::string::npos) throw std::runtime_error("no function " + symbol);
        const auto define = llvm.rfind("define ", start);
        const auto end = llvm.find("\n}\n", start);
        return llvm.substr(define, end - define);
    };
    const auto require = [](bool condition, const std::string& what, const std::string& text) {
        if (!condition) throw std::runtime_error(what + "\n" + text);
    };
    const auto previous = quidra::platform::environment_value("QUIDRA_PACKAGE_PATH");
    try {
        std::filesystem::remove_all(root);
        write("packages/loc_pkg/quidra.package", "name = loc_pkg\nversion = 0.1.0\n");
        write("packages/loc_pkg/main.qui",
              "import internal = \"./internal.qui\"\n"
              "\n"
              "extern int64 c_apply(fn<int64>(int64) callback, int64 value) = \"loc_apply\"\n"
              "\n"
              "int pick(int[] values, int i)\n"
              "    return values[i]\n"
              "\n"
              "int apply(fn<int>(int) f, int x)\n"
              "    return f(x)\n"
              "\n"
              "int64 apply_native(fn<int64>(int64) f, int64 x)\n"
              "    return c_apply(f, x)\n"
              "\n"
              "int deep(int[] values)\n"
              "    return internal.pick(values, 1)\n");
        write("packages/loc_pkg/internal.qui",
              "int pick(int[] values, int i)\n"
              "    return values[i]\n");
        write("lib.qui", "int lib_pick(int[] values, int i)\n    return values[i]\n");
        write("main.qui",
              "import loc = loc_pkg\n"
              "import lib = \"./lib.qui\"\n"
              "\n"
              "extern int64 c_twice(fn<int64>(int64) callback, int64 value) = \"user_apply\"\n"
              "\n"
              "int64 scale(int64 x)\n"
              "    return x * 3\n"
              "\n"
              "int[] data = [1, 2, 3]\n"
              "int a = loc.pick(data, int(scale(1)))\n"
              "int b = loc.pick(data, int(c_twice(scale, 1)))\n"
              "int c = lib.lib_pick(data, 1)\n"
              "int d = loc.deep(data)\n"
              "int[] slots = array(2)\n"
              "int &r = &slots[1]\n"
              "int e = int(scale(1)) + r\n");
        write("quiet.qui",
              "import loc = loc_pkg\n"
              "\n"
              "int f(int x)\n"
              "    return x + 1\n"
              "\n"
              "int g(int x)\n"
              "    return x * 2\n"
              "\n"
              "int[] data = [1, 2, 3]\n"
              "int y = loc.pick(data, 1)\n"
              "int total = 1\n"
              "total = total + f(2)\n"
              "int z = g(f(3))\n");
        if (!set_package_path((root / "packages").string()))
            throw std::runtime_error("cannot set QUIDRA_PACKAGE_PATH");
        const auto llvm = quidra::compile_file(root / "main.qui", {}, root).llvm;
        const auto pick = body(llvm, "n_loc_pick");
        require(pick.find("call void @quidra_runtime_set_package_source_provenance(ptr @.quidra.source.") != std::string::npos &&
                    pick.find("@quidra_runtime_set_source_provenance(") == std::string::npos,
                "package statements call the package setter", pick);
        require(pick.find("@quidra_array_slot(") != std::string::npos &&
                    pick.find("i64 0, i64 0)") != std::string::npos,
                "package failure sites pass no location", pick);
        const auto internal = body(llvm, "n_loc_internal_pick");
        require(internal.find("@quidra_runtime_set_package_source_provenance(") != std::string::npos,
                "a package's own quoted import is package code", internal);
        const auto lib = body(llvm, "n_lib_lib_pick");
        require(lib.find("call void @quidra_runtime_set_source_provenance(ptr @.quidra.source.") != std::string::npos,
                "a local import of the root is user code", lib);
        for (const auto* symbol : {"n_loc_apply", "n_loc_apply_native"}) {
            const auto text = body(llvm, symbol);
            const auto save = text.find("= call ptr @quidra_runtime_user_statement()");
            const auto call = text.find(std::string(symbol) == "n_loc_apply" ? "call i64 %" : "call i64 @loc_apply(");
            const auto restore = text.find("call void @quidra_runtime_restore_user_statement(ptr %user.statement.");
            require(save != std::string::npos && call != std::string::npos && restore != std::string::npos &&
                        save < call && call < restore,
                    std::string("package code saves and restores the user statement around ") + symbol, text);
        }
        const auto entry = body(llvm, "main");
        const auto first_scale = entry.find("call i64 @n_scale(");
        const auto refresh = entry.find("call void @quidra_runtime_restore_user_statement(ptr @.quidra.source.", first_scale);
        const auto first_pick = entry.find("call i64 @n_loc_pick(", first_scale);
        require(first_scale != std::string::npos && refresh != std::string::npos && refresh < first_pick,
                "a user call before a package call refreshes the user statement", entry);
        const auto native = entry.find("call i64 @user_apply(");
        const auto native_refresh = entry.find("call void @quidra_runtime_restore_user_statement(ptr @.quidra.source.", native);
        const auto second_pick = entry.find("call i64 @n_loc_pick(", native);
        require(native != std::string::npos && native_refresh != std::string::npos && native_refresh < second_pick,
                "a C call given a callback refreshes the user statement", entry);
        require(entry.find("call void @quidra_init_check(ptr %ref.load.addr.") != std::string::npos &&
                    entry.find(", i64 16, i64 1)") != std::string::npos,
                "a load through a reference checks initialization at its statement", entry);
        const auto quiet = quidra::compile_file(root / "quiet.qui", {}, root).llvm;
        require(body(quiet, "main").find("@quidra_runtime_restore_user_statement(") == std::string::npos,
                "statements without a call into user code before a reader need no refresh", body(quiet, "main"));
        require(quiet.find("declare void @quidra_runtime_set_package_source_provenance(ptr)") != std::string::npos &&
                    quiet.find("declare ptr @quidra_runtime_user_statement()") != std::string::npos &&
                    quiet.find("declare void @quidra_runtime_restore_user_statement(ptr)") != std::string::npos,
                "the prelude declares the user statement entries", "");
        set_package_path(previous.value_or(""));
        std::filesystem::remove_all(root);
    } catch (const std::exception& e) {
        set_package_path(previous.value_or(""));
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        std::cerr << "user statement attribution failed: " << e.what() << "\n";
        std::exit(1);
    }
}

// The entry registers the module's source table before its first statement:
// one entry per user file, the root first, with its display path, absolute
// path, revision, line count and size. The failure immediates of a
// non-root user file carry its index in their upper 32 bits; the root's are
// its lines.
static void source_table_names_every_user_file() {
    const auto root = std::filesystem::temp_directory_path() / "quidra-source-table";
    const auto write = [&](const std::filesystem::path& name, const char* text) {
        std::filesystem::create_directories((root / name).parent_path());
        std::ofstream out(root / name, std::ios::binary);
        if (!out) throw std::runtime_error("cannot create temporary Quidra source");
        out << text;
    };
    const auto require = [](bool condition, const std::string& what, const std::string& text) {
        if (!condition) throw std::runtime_error(what + "\n" + text);
    };
    try {
        std::filesystem::remove_all(root);
        const char* lib = "int pick(int[] values, int i)\n    return values[i]\n";
        const char* main = "import lib = \"./lib/pick.qui\"\n"
                           "\n"
                           "int pick_root(int[] values, int i)\n"
                           "    return values[i]\n"
                           "\n"
                           "int[] data = [1, 2, 3]\n"
                           "print(lib.pick(data, 1) + pick_root(data, 2))\n";
        write("lib/pick.qui", lib);
        write("main.qui", main);
        quidra::CompileOptions options;
        options.source_display_path = "app/main.qui";
        const auto llvm = quidra::compile_file(root / "main.qui", options, root).llvm;
        const auto entry = llvm.find("define i64 @main(");
        require(entry != std::string::npos &&
                    llvm.find("call void @quidra_runtime_register_sources(ptr @.quidra.sources)", entry) ==
                        llvm.find("\n  call ", entry) + 3,
                "the entry registers the source table first", llvm.substr(entry, 400));
        require(llvm.find("declare void @quidra_runtime_register_sources(ptr)") != std::string::npos,
                "the prelude declares the registration", "");
        require(llvm.find("@.quidra.sources = private constant { i64, [2 x { ptr, ptr, ptr, i64, i64 }] } { i64 2, ") !=
                    std::string::npos,
                "the table has the root and its import", llvm);
        const auto revision = quidra::platform::sha256_hex(main);
        for (const auto& text : {std::string("app/main.qui"), std::string("app/lib/pick.qui"),
                                 (root / "main.qui").lexically_normal().string(), revision}) {
            require(llvm.find("c\"" + text + "\\00\"") != std::string::npos,
                    "the table names " + text, "");
        }
        require(llvm.find(", i64 8, i64 " + std::to_string(std::string(main).size()) + " }") !=
                    std::string::npos,
                "the root's line count and size", "");
        const auto pick = llvm.find("@n_lib_pick(");
        const auto pick_end = llvm.find("\n}\n", pick);
        require(llvm.substr(pick, pick_end - pick).find("i64 4294967298, i64 19)") != std::string::npos,
                "an import's immediates carry its index", llvm.substr(pick, pick_end - pick));
        const auto root_pick = llvm.find("@n_pick_root(");
        const auto root_pick_end = llvm.find("\n}\n", root_pick);
        require(llvm.substr(root_pick, root_pick_end - root_pick).find("i64 4, i64 19)") != std::string::npos,
                "the root's immediates are its lines", llvm.substr(root_pick, root_pick_end - root_pick));
        const auto memory = quidra::compile("int[] values = [1]\nprint(values[0])\n").llvm;
        require(memory.find("c\"<memory>\\00\"") != std::string::npos &&
                    memory.find("@.quidra.sources = private constant { i64, [1 x") != std::string::npos,
                "string input is <memory>", memory);
        std::filesystem::remove_all(root);
    } catch (const std::exception& e) {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        std::cerr << "source table failed: " << e.what() << "\n";
        std::exit(1);
    }
}

// `export "C"` declarations: the targets, types, forms and symbols the
// frontend and the checker accept and reject (docs/spec/language.md, C ABI
// export).
static void c_export_declarations_are_checked() {
    const std::string add2 = "export \"C\" int32 add2(int32 a, int32 b)\n    return a + b\n";
    good(add2);
    good("export \"C\" void touch()\n    print(\"x\")\n");
    good("export \"C\" int8 f8(int8 a, nat8 b)\n    return a\n"
         "export \"C\" int16 f16(int16 a, nat16 b)\n    return a\n"
         "export \"C\" int64 f64(int64 a, nat64 b, nat32 c, int32 d)\n    return a\n"
         "export \"C\" nat8 g8(nat8 a)\n    return a\n"
         "export \"C\" nat16 g16(nat16 a)\n    return a\n"
         "export \"C\" nat32 g32(nat32 a)\n    return a\n"
         "export \"C\" nat64 g64(nat64 a)\n    return a\n"
         "export \"C\" real32 r32(real32 a)\n    return a\n"
         "export \"C\" real64 r64(real64 a)\n    return a\n"
         "export \"C\" int32 zero()\n    return 0\n");
    good("export \"C\" int32 frozen(const int32 a)\n    return a\n");
    good("export \"C\" int32 later(int32 a);\n\nexport \"C\" int32 later(int32 a)\n    return a + 1\n");
    good("export \"C\" int64 fact(int64 n)\n    if n <= 1\n        return 1\n    return n * fact(n - 1)\n");
    good("extern int32 c_abs(int32 value) = \"abs\"\n"
         "export \"C\" int32 magnitude(int32 value)\n    return c_abs(value)\n");
    good(add2 + "int32 inside = add2(1, 2)\nfn<int32>(int32, int32) f = add2\nint32 through = f(3, 4)\n");
    good("int export = 3\nexport = export + 1\nprint(export)\n");
    // Targets and the ABI string.
    bad_message("export \"C\" T same<T>(T value)\n    return value\n", "FFI_EXPORT",
                "Generic functions cannot be exported");
    bad_message("class Counter\n    int32 value\n\n    export \"C\" int32 get()\n        return this.value\n",
                "FFI_EXPORT", "methods cannot be exported");
    bad_message("class Counter\n    int32 value\n\n    export \"C\" construct()\n        this.value = 0\n",
                "FFI_EXPORT", "constructors cannot be exported");
    bad_code("class Counter\n    export \"C\" int32 value\n", "PARSE_ERROR");
    bad_message("int32 outer()\n    export \"C\" int32 inner()\n        return 1\n    return 0\n",
                "PARSE_ERROR", "export declarations are only valid at top level");
    bad_message("export \"Rust\" int32 add2(int32 a, int32 b)\n    return a + b\n", "FFI_EXPORT",
                "Only the \"C\" ABI can be exported");
    bad_message("int32 later(int32 a);\n\nexport \"C\" int32 later(int32 a)\n    return a\n",
                "FFI_EXPORT", "must both carry export");
    bad_message("export \"C\" int32 later(int32 a);\n\nint32 later(int32 a)\n    return a\n",
                "FFI_EXPORT", "must both carry export");
    // Types, checked as spelled.
    bad_message("export \"C\" int bare(int32 a)\n    return 1\n", "FFI_TYPE", "write int64");
    bad_message("export \"C\" int32 bare(int a)\n    return 1\n", "FFI_TYPE", "write int64");
    bad_message("export \"C\" int32 bare(nat a)\n    return 1\n", "FFI_TYPE", "write nat64");
    const std::vector<std::string> rejected = {
        "nat", "real", "bool", "string", "bin", "int32[]", "int32[4]", "tensor<real32>",
        "Box", "Color", "int32 | error", "int32 | none", "fn<int32>(int32)"};
    const std::string declarations =
        "class Box\n    int32 value\n\nenum Color\n    red\n    green\n\n";
    for (const auto& type : rejected) {
        bad_code(declarations + "export \"C\" int32 take(" + type + " value)\n    return 0\n", "FFI_TYPE");
        bad_code(declarations + "export \"C\" " + type + " give()\n    return give()\n", "FFI_TYPE");
    }
    bad_message("export \"C\" int32 take(string value)\n    return 0\n", "FFI_TYPE",
                "Exported C parameter 'value' has type string");
    bad_message("export \"C\" string give()\n    return \"x\"\n", "FFI_TYPE",
                "Exported C result must be void or a fixed-width scalar");
    // Forms.
    bad_code("export \"C\" int32 take(int32 &value)\n    return value\n", "FFI_REFERENCE");
    bad_code("export \"C\" int32 take(const string &value)\n    return 0\n", "FFI_REFERENCE");
    bad_code("export \"C\" int32 take(int32 value = 1)\n    return value\n", "FFI_DEFAULT");
    // Symbols.
    bad_code(add2 + add2, "DUPLICATE_NAME");
    bad_message(add2 + "extern int32 other(int32 a, int32 b) = \"add2\"\n", "FFI_SYMBOL_CONFLICT",
                "both exported and bound by an extern declaration");
    bad_code("export \"C\" int32 main()\n    return 0\n", "RESERVED_MAIN");
    bad_code("export \"C\" int32 n_x()\n    return 0\n", "FFI_SYMBOL_CONFLICT");
    bad_code("export \"C\" int32 quidra_x()\n    return 0\n", "FFI_SYMBOL_CONFLICT");
    bad_code("export \"C\" int32 __quidra_x()\n    return 0\n", "SHADOWING");
    bad_message("export \"C\" int32 printf()\n    return 0\n", "FFI_SYMBOL_CONFLICT",
                "C symbol 'printf' is reserved by the compiler/runtime implementation");
    bad_message("export \"C\" int32 static()\n    return 0\n", "FFI_SYMBOL", "is a C keyword");
    bad_message("export \"C\" int32 _Bool()\n    return 0\n", "FFI_SYMBOL", "is a C keyword");
    bad_message("export \"C\" int32 __x()\n    return 0\n", "FFI_SYMBOL", "reserved C identifier");
    bad_message("export \"C\" int32 _Upper()\n    return 0\n", "FFI_SYMBOL", "reserved C identifier");
    // The REPL cannot export.
    {
        const auto root = std::filesystem::temp_directory_path() / "quidra-compiler-tests";
        std::filesystem::create_directories(root);
        bool rejected_in_repl = false;
        try {
            (void)quidra::compile_repl_file_source(root / "repl_export.qui", add2, {}, root);
        } catch (const quidra::CompileErrors& errors) {
            for (const auto& d : errors.diagnostics()) rejected_in_repl = rejected_in_repl || d.code == "FFI_EXPORT";
        } catch (const quidra::CompileError& error) {
            rejected_in_repl = error.diagnostic().code == "FFI_EXPORT";
        }
        if (!rejected_in_repl) {
            std::cerr << "an export in a REPL submission was not rejected with FFI_EXPORT\n";
            std::exit(1);
        }
    }
    // The formatter keeps the prefix.
    const auto formatted = quidra::format_source(add2);
    if (formatted.find("export \"C\" int32 add2(int32 a, int32 b)") == std::string::npos) {
        std::cerr << "quidra fmt dropped the export prefix:\n" << formatted;
        std::exit(1);
    }
    // Exports in imported modules use their declared name as the symbol, so
    // two modules cannot export the same name.
    const auto root = std::filesystem::temp_directory_path() / "quidra-c-export-modules";
    const auto write = [&](const char* name, const std::string& text) {
        std::ofstream out(root / name, std::ios::binary);
        if (!out) throw std::runtime_error("cannot create temporary Quidra source");
        out << text;
    };
    try {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        write("lib.qui", add2);
        write("other.qui", add2);
        write("main.qui", "import lib = \"./lib.qui\"\nprint(lib.add2(1, 2))\n");
        write("both.qui", "import lib = \"./lib.qui\"\nimport other = \"./other.qui\"\n");
        (void)quidra::compile_file(root / "main.qui", {}, root);
        bool conflict = false;
        try {
            (void)quidra::compile_file(root / "both.qui", {}, root);
        } catch (const quidra::CompileErrors& errors) {
            for (const auto& d : errors.diagnostics())
                conflict = conflict || (d.code == "FFI_SYMBOL_CONFLICT" &&
                                        d.message.find("exported more than once") != std::string::npos);
        } catch (const quidra::CompileError& error) {
            conflict = error.diagnostic().code == "FFI_SYMBOL_CONFLICT";
        }
        if (!conflict) {
            std::cerr << "two modules exported add2 without FFI_SYMBOL_CONFLICT\n";
            std::exit(1);
        }
        std::filesystem::remove_all(root);
    } catch (const std::exception& error) {
        std::cerr << "exports in imported modules: " << error.what() << "\n";
        std::exit(1);
    }
}

// The text of the LLVM function that `marker` (its define line's start)
// begins, through its closing brace and line break; empty when absent.
static std::string llvm_function_text(const std::string& llvm, const std::string& marker) {
    const auto begin = llvm.find(marker);
    if (begin == std::string::npos) return {};
    const auto end = llvm.find("\n}\n", begin);
    if (end == std::string::npos) return {};
    return llvm.substr(begin, end + 3 - begin);
}

static void require_llvm_function(const std::string& source, const std::string& marker,
                                  const std::vector<std::string>& present,
                                  const std::vector<std::string>& absent) {
    capture_source("llvm_contains", source);
    const auto compiled = quidra::compile(source);
    const auto text = llvm_function_text(compiled.llvm, marker);
    bool ok = !text.empty();
    for (const auto& fragment : present) ok = ok && text.find(fragment) != std::string::npos;
    for (const auto& fragment : absent) ok = ok && text.find(fragment) == std::string::npos;
    if (ok) return;
    std::cerr << "C entry point " << marker << " does not read as expected:\n" << compiled.llvm;
    std::exit(1);
}

// The C entry points of exported functions (export "C"): their signatures,
// the call into the function's own symbol, the boundary hook only where
// device work may be left behind, and nothing else changed.
static void c_export_entry_points_are_emitted() {
    const std::string add2 = "export \"C\" int32 add2(int32 a, int32 b)\n    return a + b\n";
    require_llvm_function(add2, "define i32 @add2(i32 %a, i32 %b)",
                          {"call void @quidra_runtime_register_sources(ptr @.quidra.sources)",
                           "call i32 @n_add2(i32 %a, i32 %b)", "ret i32 %export.result"},
                          {"@quidra_runtime_export_leave"});
    require_llvm_function("export \"C\" int8 neg8(int8 x)\n    return x\n",
                          "define signext i8 @neg8(i8 %x)", {"call i8 @n_neg8(i8 %x)"}, {});
    require_llvm_function("export \"C\" nat16 widen(nat16 x)\n    return x\n",
                          "define zeroext i16 @widen(i16 %x)", {}, {});
    require_llvm_function("export \"C\" real32 r32(real32 x, real64 y)\n    return x\n",
                          "define float @r32(float %x, double %y)", {}, {});
    require_llvm_function("export \"C\" void touch()\n    print(\"x\")\n", "define void @touch()",
                          {"call void @n_touch()", "ret void"}, {});
    // A self-recursive export calls the public symbol, never the depth body.
    require_llvm_function(
        "export \"C\" int64 fact(int64 n)\n    if n <= 1\n        return 1\n    return n * fact(n - 1)\n",
        "define i64 @fact(i64 %n)", {"call i64 @n_fact(i64 %n)"}, {".depth"});
    // Device work, or a call to C, gets the boundary hook.
    require_llvm_function(
        "export \"C\" void make()\n    tensor<real32> t = tensor.zeros<real32>([1])\n",
        "define void @make()", {"call void @quidra_runtime_export_leave()"}, {});
    require_llvm_function(
        "extern int32 c_abs(int32 value) = \"abs\"\n"
        "export \"C\" int32 magnitude(int32 value)\n    return c_abs(value)\n",
        "define i32 @magnitude(i32 %value)", {"call void @quidra_runtime_export_leave()"}, {});
    // Through a Quidra helper too.
    require_llvm_function(
        "void work()\n    tensor<real32> t = tensor.zeros<real32>([1])\n"
        "export \"C\" void outer()\n    work()\n",
        "define void @outer()", {"call void @quidra_runtime_export_leave()"}, {});
    // Quidra callers call the function itself.
    llvm_contains(add2 + "int32 inside = add2(1, 2)\n", "call i32 @n_add2(");
    ir_contains(add2, "function add2(int32 a, int32 b) -> int32 export \"C\" add2");

    // Marker invariance: with and without the prefix, the program's LLVM
    // differs only by the entry point and by what describes the source text
    // itself (its revision, and the size its source table records), and its
    // typed IR only by the marker.
    const std::string body = "int32 twice(int32 value)\n    return value * 2\n";
    const std::string tail = "print(add2(1, twice(2)))\nprint(NL)\n";
    const auto marked_source = body + add2 + tail;
    const auto plain_source = body + add2.substr(std::string("export \"C\" ").size()) + tail;
    const auto marked = quidra::compile(marked_source);
    const auto plain = quidra::compile(plain_source);
    const auto normalized = [](std::string text, const std::string& source) {
        const auto at = text.find("\n@.quidra.sources = ");
        if (at != std::string::npos) text.erase(at, text.find('\n', at + 1) - at);
        const auto revision = quidra::platform::sha256_hex(source);
        for (auto found = text.find(revision); found != std::string::npos;
             found = text.find(revision, found))
            text.replace(found, revision.size(), "REVISION");
        return text;
    };
    auto llvm = marked.llvm;
    const auto entry = llvm_function_text(llvm, "define i32 @add2(");
    if (!entry.empty()) {
        // The entry point and the blank line that follows it.
        const auto at = llvm.find(entry);
        const bool blank = llvm.compare(at + entry.size(), 1, "\n") == 0;
        llvm.erase(at, entry.size() + (blank ? 1 : 0));
    }
    llvm = normalized(llvm, marked_source);
    auto ir = quidra::ir::dump(marked.ir);
    const std::string marker = " export \"C\" add2";
    if (const auto at = ir.find(marker); at != std::string::npos) ir.erase(at, marker.size());
    if (entry.empty() || llvm != normalized(plain.llvm, plain_source) ||
        ir != quidra::ir::dump(plain.ir)) {
        std::cerr << "export \"C\" changed more than its C entry point:\n--- marked\n"
                  << marked.llvm << "\n--- plain\n" << plain.llvm;
        std::exit(1);
    }
}

// A library build (CompileOptions::artifact = Library): the root holds
// declarations only, exports something, and has no entry point.
static void library_builds_check_their_root() {
    quidra::CompileOptions library;
    library.artifact = quidra::CompileArtifact::Library;
    const auto codes = [&](const std::string& source) {
        std::vector<std::string> out;
        try {
            (void)quidra::compile(source, library);
        } catch (const quidra::CompileErrors& errors) {
            for (const auto& d : errors.diagnostics()) out.push_back(d.code);
        } catch (const quidra::CompileError& error) {
            out.push_back(error.diagnostic().code);
        }
        return out;
    };
    const auto expect_code = [&](const std::string& source, const std::string& code) {
        const auto found = codes(source);
        if (std::find(found.begin(), found.end(), code) != found.end()) return;
        std::cerr << "library build: expected " << code << " for:\n" << source;
        std::exit(1);
    };
    const std::string accepted =
        "const int64 limit = 3\n"
        "export \"C\" int32 one()\n    return 1\n\n"
        "if main\n    print(one())\n";
    const auto compiled = quidra::compile(accepted, library);
    for (const auto& function : compiled.ir.functions) {
        if (function.entrypoint) {
            std::cerr << "a library build lowered an entry point\n";
            std::exit(1);
        }
    }
    if (compiled.llvm.find("define i32 @main(") != std::string::npos ||
        compiled.llvm.find("define i32 @one(") == std::string::npos) {
        std::cerr << "a library module defines main or lacks the export:\n" << compiled.llvm;
        std::exit(1);
    }
    expect_code("export \"C\" int32 one()\n    return 1\n\nprint(one())\n", "LIBRARY_TOP_LEVEL");
    expect_code("export \"C\" int32 one()\n    return 1\n\nint64 counter = 0\n", "LIBRARY_TOP_LEVEL");
    expect_code("cli Options\n    int64 count = option(default = 1)\n\n"
                "export \"C\" int32 one()\n    return 1\n", "LIBRARY_TOP_LEVEL");
    expect_code("int32 one()\n    return 1\n", "FFI_EXPORT");
    // The same programs are ordinary executables.
    good("export \"C\" int32 one()\n    return 1\n\nprint(one())\n");
    good("int32 one()\n    return 1\n");
}

// Element index failures report the index operand's first token (arrays,
// strings and bin elements, written and read); slices and tensor accesses
// keep the indexing expression's start. bin elements and slices and
// text.slice pass their positions to the runtime; the static bound uses the
// runtime's words.
static void index_failures_report_the_index_operand() {
    const auto require = [](bool condition, const std::string& what, const std::string& text) {
        if (!condition) {
            std::cerr << "index positions failed: " << what << "\n" << text << "\n";
            std::exit(1);
        }
    };
    // Whether one line of `llvm` holds both fragments.
    const auto line_with = [](const std::string& llvm, const std::string& first,
                              const std::string& second) {
        std::istringstream lines(llvm);
        for (std::string line; std::getline(lines, line);)
            if (line.find(first) != std::string::npos && line.find(second) != std::string::npos)
                return true;
        return false;
    };
    const auto bin = quidra::compile("bin b = bin.fill(8, 0)\n"
                                     "nat at = len(b) + 2\n"
                                     "print(b[at])\n"
                                     "b[ at ] = bin.fill(1, 1)\n"
                                     "print(b[1:at])\n").llvm;
    require(line_with(bin, "@quidra_bin_index(ptr ", ", i64 3, i64 9)"), "bin element read", bin);
    require(line_with(bin, "@quidra_bin_set(ptr ", ", i64 4, i64 4)"), "bin element write", bin);
    require(line_with(bin, "@quidra_bin_slice(ptr ", ", i64 5, i64 7)"), "bin slice", bin);
    require(bin.find("declare ptr @quidra_bin_index(ptr, i64, i64, i64)") != std::string::npos,
            "the bin entries take a position", bin);
    const auto text = quidra::compile("string word = \"quidra\"\n"
                                      "nat at = len(word)\n"
                                      "print(word.slice(1, int(at) + 3))\n"
                                      "print(word[at])\n").llvm;
    require(line_with(text, "@quidra_string_slice(ptr ", ", i64 3, i64 7)"), "text.slice", text);
    require(line_with(text, "@quidra_string_index(ptr ", ", i64 4, i64 12)"), "string index", text);
    const auto array = quidra::compile("int[] values = [1, 2, 3]\n"
                                       "nat at = len(values)\n"
                                       "print(values[ at ])\n").llvm;
    require(line_with(array, "@quidra_array_slot(", ", i64 3, i64 15)"), "array element", array);
    bad_message("int[2] xs = [1, 2]\nxs[2] = 3\n", "INDEX_BOUNDS",
                "Index 2 out of bounds for length 2.");
}

// The recursion limit's message names the limit the guards compare against.
static void recursion_limit_message_names_the_limit() {
    const auto llvm = quidra::compile("int down(int n)\n    return down(n + 1)\n\nprint(down(0))\n").llvm;
    const auto at = llvm.find("@.msg.stack = ");
    const auto line = at == std::string::npos ? std::string() : llvm.substr(at, llvm.find('\n', at) - at);
    if (line.find("[46 x i8] c\"maximum recursion depth exceeded (limit 4096)\\00\"") ==
        std::string::npos) {
        std::cerr << "recursion limit message missing\n" << llvm << "\n";
        std::exit(1);
    }
}

int main(){
    // Direction markers and a statically known step must agree, even while
    // the directional slice runtime remains guarded by SLICE_MARKER.
    bad_message(
        "string text = \"abcdef\"\n"
        "auto section = text[0:<5:-1]\n",
        "SLICE_STEP", "conflicts with the step sign");
    bad_message(
        "string text = \"abcdef\"\n"
        "auto section = text[5>:0:1]\n",
        "SLICE_STEP", "conflicts with the step sign");
    bad_message(
        "string text = \"abcdef\"\n"
        "auto section = text[0:<5:0]\n",
        "SLICE_STEP", "cannot be zero");
    bad_message(
        "string text = \"abcdef\"\n"
        "auto section = text[0<:>5]\n",
        "SLICE_STEP", "must agree on direction");
    library_builds_check_their_root();
    c_export_entry_points_are_emitted();
    c_export_declarations_are_checked();
    index_failures_report_the_index_operand();
    recursion_limit_message_names_the_limit();
    source_table_names_every_user_file();
    package_code_reports_at_the_user_statement();
    standard_class_instance_prefixes_follow_the_frontend();
    borrowed_arguments_copy_only_where_the_call_writes();
    if_expressions_are_typed_like_initializers();
    initialization_is_checked_by_path();
    array_element_reads_need_an_initializing_path();
    field_initialization_is_checked_by_path();
    standard_library_classes_are_marked_by_origin();
    discard_name_is_never_declared_or_read();
    effect_summaries_describe_functions();
    runtime_handle_classes_match_the_standard_classes();
    prelude_c_library_symbols_are_reserved();
    tensor_element_sizes_match_their_dtypes();
    package_extension_calls_form_tensor_regions();
    package_extension_replacement_rewrites_pure_chain();
    package_extension_replacement_crosses_eliminated_release();
    package_extension_memory_reuse_respects_tensor_storage_aliases();
    package_extension_conditional_replacement_preserves_owned_argument_lifetime();
    package_extension_conditional_replacement_clones_borrowed_argument();
    package_extension_backend_selection_tracks_transfer_layout();
 good(R"(int huge = 12345678901234567890123456789012345678901234567890
int one = 1
int sum = huge + one
print(sum)
print(NL)
)");
 good(R"(real ratio = real(1) / real(3)
print(ratio)
print(NL)
)");
 good(R"(T identity<T>(T value)
    return value

int large = identity<int>(123456789012345678901234567890)
real exact_value = identity<real>(real(1) / real(3))
print(large)
print(NL)
print(exact_value)
print(NL)
)");
 good(R"(tensor<T> preserve<T: floating>(tensor<T> value)
    return value

tensor<real32> left = tensor.ones<real32>([2])
tensor<real32> right = tensor.ones<real32>([2])
tensor<real32> product = preserve(left * right)
tensor<real32> scaled = preserve(product * real32(2))
print(scaled[0].item())
print(NL)
)");
 good(R"(tensor<T> preserve_grad<T: floating>(tensor<T> value)
    return value

tensor<real32> root = tensor.ones<real32>([1]).track()
(root * root).backward(&root)
tensor<real32> gradient = preserve_grad(root.grad)
print(gradient[0].item())
print(NL)
)");
 good(R"(int[] values = [
    123456789012345678901234567890,
    2,
]
real[] reals = real(values)
print(values[0])
print(NL)
print(reals[1])
print(NL)
)");
 llvm_contains(
     "int x = 123456789012345678901234567890\n",
     "call i64 @quidra.int.literal");
 bad_code("tensor<int> x = tensor<int>([1])\n", "INVALID_TYPE");
 bad_code("map.Map<real, int> values = map.Map<real, int>()\n", "STANDARD_KEY_TYPE");
 bad_code("set.Set<real> values = set.Set<real>()\n", "STANDARD_KEY_TYPE");
 llvm_not_contains(
     "map.Map<int, int> values = map.Map<int, int>()\nvalues.set(1, 2)\nauto value = values.get(1)\n",
     "call ptr @quidra_format_signed");
 llvm_not_contains(
     "set.Set<nat64> values = set.Set<nat64>()\nnat64 key = nat64(7)\nvalues.add(key)\nbool present = values.has(key)\n",
     "call ptr @quidra_format_unsigned");
 llvm_not_contains(
     "map.Map<int, int> values = map.Map<int, int>()\nint key = int(7)\nvalues.set(key, 2)\nauto value = values.get(key)\n",
     "call ptr @quidra.int.text");
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
    written += int(len(line))
print(written)
print(NL)
)", "@quidra_string_build_append_move");
 ir_contains(R"(string content = ""
int written = 0
for i in range(0, 2)
    string[] fields = [i.string(), " ", i.string(), NL]
    string line = fields.join("")
    content = content + line
    written += int(len(line))
print(written)
print(NL)
)", "string.build_append");
 llvm_contains(R"(string content = ""
int written = 0
for i in range(0, 2)
    string[] fields = [i.string(), " ", i.string(), NL]
    string line = fields.join("")
    content = content + line
    written += int(len(line))
print(written)
print(NL)
)", "call ptr @quidra_string_build_append_move_unique_direct");
 llvm_contains(R"(string content = ""
int written = 0
for i in range(0, 2)
    string[] fields = [i.string(), " ", i.string(), NL]
    string line = fields.join("")
    content = content + line
    written += int(len(line))
print(written)
print(NL)
)", "store i8 4");
 llvm_not_contains(R"(string content = ""
int written = 0
for i in range(0, 2)
    string[] fields = [i.string(), " ", i.string(), NL]
    string line = fields.join("")
    content = content + line
    written += int(len(line))
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
int64 spaces = 0
for i in range(0, len(text))
    if text[i] == " "
        spaces += 1
print(spaces)
print(NL)
)", "string.ascii_count_prefix");
 llvm_contains(R"(string text = "a b a"
int64 spaces = 0
for i in range(0, len(text))
    if text[i] == " "
        spaces += 1
print(spaces)
print(NL)
)", "call i64 @quidra_string_count_ascii_prefix");
 ir_contains(R"(string source = "ab"
nat8[] data = nat8[](source.utf8())
match string.from_utf8(bin(data))
    string decoded
        print(decoded)
        print(NL)
    error problem
        print(problem)
        print(NL)
)", "string.from_utf8_array_direct");
 ir_contains(R"(match int64.parse("42")
    int64 value
        print(value)
        print(NL)
    error problem
        print(problem)
        print(NL)
)", "parse.direct");
 ir_contains(R"(int64 parse_decimal(string text)
    match int64.parse(text)
        int64 value
            return value
        error problem
            process.exit(1)

string line = "12 34"
for i in range(0, 1)
    string[] fields = line.split(" ")
    int64 left = parse_decimal(fields[0])
    int64 right = parse_decimal(fields[1])
    print(left + right)
    print(NL)
)", "string.parse_two_signed");
 llvm_contains(R"(int64 parse_decimal(string text)
    match int64.parse(text)
        int64 value
            return value
        error problem
            process.exit(1)

string line = "12 34"
for i in range(0, 1)
    string[] fields = line.split(" ")
    int64 left = parse_decimal(fields[0])
    int64 right = parse_decimal(fields[1])
    print(left + right)
    print(NL)
)", "call i1 @__quidra_string_parse_two_signed_fast");
 ir_contains(R"(string line = "12 34"
for i in range(0, 1)
    string[] fields = line.split(" ")
    int64 left = int64.parse(fields[0])
    int64 right = int64.parse(fields[1])
    print(left + right)
    print(NL)
)", "string.parse_two_signed");
 llvm_contains(R"(string line = "12 34"
for i in range(0, 1)
    string[] fields = line.split(" ")
    int64 left = int64.parse(fields[0])
    int64 right = int64.parse(fields[1])
    print(left + right)
    print(NL)
)", "call i1 @__quidra_string_parse_two_signed_fast");
 llvm_not_contains(R"(int64 parse_decimal(string text)
    match int64.parse(text)
        int64 value
            return value
        error problem
            process.exit(1)

string line = "12 34"
for i in range(0, 1)
    string[] fields = line.split(" ")
    int64 left = parse_decimal(fields[0])
    int64 right = parse_decimal(fields[1])
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
 ir_contains(R"(int bounded_hash(const nat8[] &data, int size)
    int h = 0
    for i in range(0, size)
        h = (h * 131 + int(data[i])) % 1000000007
    return h
)", "no-overflow");
 llvm_contains(R"(int bounded_hash(const nat8[] &data, int size)
    int h = 0
    for i in range(0, size)
        h = (h * 131 + int(data[i])) % 1000000007
    return h
)", "mul nsw i64");
 llvm_contains(R"(int checked_add(int value)
    return value + 1
)", "llvm.sadd.with.overflow.i64");
 // Bare integers whose operands and results are proven inline take plain
 // word arithmetic: no tag test and no promotion call.
 ir_contains(R"(int widen(nat8 small)
    return int(small) * 3 + 1
)", " inline");
 llvm_contains(R"(int widen(nat8 small)
    return int(small) * 3 + 1
)", "add nsw i64");
 llvm_function_not_contains(R"(int widen(nat8 small)
    return int(small) * 3 + 1
)", "@n_widen(", "call i64 @quidra.int.add");
 llvm_function_not_contains(R"(bool below(int32 a, int32 b)
    return int(a) < int(b)
)", "@n_below(", "call i1 @quidra.int.lt");
 llvm_function_not_contains(R"(int64 narrow(nat8 small)
    return int64(int(small))
)", "@n_narrow(", "call i64 @quidra.int.to_i64");
 // Unproven operands keep the tag test and the promotion path.
 llvm_contains(R"(int grow(int value)
    return value + 1
)", "call i64 @quidra.int.add");
 ir_contains("int negative = -12345678901234567890123\n", "-12345678901234567890123");
 // `int` and `nat` are arbitrary-precision integers: bit operations, shifts,
 // bin conversions, tensor elements and C signatures need a fixed width, a
 // negative literal does not materialize as nat, and the generic constraint
 // `integer` covers both.
 bad_message("int a = 6\nint b = a AND 3\n", "TYPE_MISMATCH",
             "Bitwise operations require nat8..nat64 or int8..int64.");
 bad_code("nat a = 6\nnat b = a << 1\n", "TYPE_MISMATCH");
 bad_code("int a = 6\nbin bits = bin(a)\n", "TYPE_MISMATCH");
 bad_message("tensor<int> t = tensor.zeros<int>([2])\n", "INVALID_TYPE", "use tensor<int64>");
 bad_code("tensor<nat> t = tensor.zeros<nat>([2])\n", "INVALID_TYPE");
 bad_code("extern int c_abs(int value) = \"llabs\"\n", "FFI_TYPE");
 bad_code("extern int64 c_abs(nat value) = \"llabs\"\n", "FFI_TYPE");
 bad_code("nat n = -1\n", "NUMERIC_FAMILY");
 bad_code("int a = 3\nnat n = a\n", "TYPE_MISMATCH");
 good(R"(T twice<T: integer>(T value)
    return value * 2

int a = twice(int(3))
nat b = twice(nat(4))
)");
 good("int big = 123456789012345678901234567890 * 10\nnat count = 18446744073709551616\n");
 // `bigint` and json.Value.bigint() are released: bigint is an ordinary
 // identifier again.
 bad_code("bigint value = 1\n", "UNKNOWN_TYPE");
 good("int bigint = 1\nint copy = bigint\n");
 // nat subtraction checks its sign; int64 keeps the overflow branch.
 llvm_contains("nat f(nat a, nat b)\n    return a - b\n", "call i64 @quidra.nat.sub");
 llvm_contains("int64 g(int64 a, int64 b)\n    return a + b\n", "@llvm.sadd.with.overflow.i64");
 // Literal categories against every numeric destination: a non-negative
 // integer literal materializes as nat* and int*, a negative one as int*
 // only, a real literal as real*, and an imaginary literal as no type yet.
 {
     const char* categories[] = {"10", "-10", "-0", "10.0", "2.0i"};
     const char* kinds[] = {"int8", "int16", "int32", "int64", "int", "nat8", "nat16",
                            "nat32", "nat64", "nat", "real32", "real64", "real"};
     for (const std::string literal : categories) {
         for (const std::string kind : kinds) {
             const bool integer = kind.starts_with("int") || kind.starts_with("nat");
             const bool natural = kind.starts_with("nat");
             bool accepted = false;
             if (literal == "10") accepted = integer;
             else if (literal == "-10" || literal == "-0") accepted = integer && !natural;
             else if (literal == "10.0") accepted = !integer;
             const auto source = kind + " value = " + literal + "\n";
             if (accepted) good(source);
             else bad_code(source, "NUMERIC_FAMILY");
             // The same rows inside an if-expression of literals.
             const auto conditional =
                 "bool ready = true\n" + kind + " value = (if ready then " + literal + " else " +
                 literal + ")\n";
             if (accepted) good(conditional);
             else bad_code(conditional, "NUMERIC_FAMILY");
         }
     }
 }
 bad_message("real32 x = 7\n", "NUMERIC_FAMILY",
             "An integer literal cannot materialize as real32: write 7.0 or real32(7).");
 bad_message("int32 x = 1.5\n", "NUMERIC_FAMILY", "A real literal cannot materialize as int32.");
 bad_message("nat8 x = -1\n", "NUMERIC_FAMILY", "A negative integer literal cannot materialize as nat8.");
 bad_message("nat8 x = 5 + -3\n", "NUMERIC_FAMILY", "A negative integer literal cannot materialize as nat8.");
 bad_message("real64 z = 2i\n", "NUMERIC_FAMILY", "An imaginary literal requires real form: write 2.0i.");
 bad_message("real64 z = 1.0 + 2.0i\n", "NUMERIC_FAMILY", "A complex literal cannot materialize as real64.");
 bad_message("real64 z = 1 + 2.0i\n", "NUMERIC_FAMILY", "Complex literal parts require real form: write 1.0 + 2.0i.");
 bad_code("auto z = 2.0i\n", "AMBIGUOUS_NUMERIC_LITERAL");
 bad_code("auto x = 10\n", "AMBIGUOUS_NUMERIC_LITERAL");
 good("real32 x = real32(10)\nint8 y = -128\nint8 z = 100 - 101\n");
 // A compound literal expression computes in its context type, with checked
 // arithmetic at every step.
 bad_code("nat8 x = 3 - 5\n", "INTEGER_RANGE");
 bad_code("nat8 x = 300 - 100\n", "INTEGER_RANGE");
 bad_code("int8 x = 100 + 100 - 100\n", "INTEGER_RANGE");
 bad_code("nat x = 3 - 5\n", "INTEGER_RANGE");
 bad_code("int64 x = 9223372036854775807 + 1\n", "INTEGER_RANGE");
 good("int x = 9223372036854775807 + 1\nnat y = 18446744073709551615 + 1\n");
 // Literal powers without a value are compile-time errors.
 bad_message("int64 x = 0 ^ 0\n", "POWER_DOMAIN", "0 ^ 0 is undefined.");
 bad_message("real64 x = 0.0 ^ 0.0\n", "POWER_DOMAIN", "0 ^ 0 is undefined.");
 bad_message("real64 x = 0.0 ^ -1.0\n", "POWER_DOMAIN", "0 ^ a negative exponent is undefined.");
 bad_message("real64 x = (-2.0) ^ 0.5\n", "POWER_DOMAIN", "A negative base requires an integer exponent.");
 good("real64 x = (-2.0) ^ 3.0\nint64 y = 0 ^ 2\n");
 llvm_contains("real64 f(real64 a, real64 b)\n    return a ^ b\n", "call double @quidra.power.real64(");

 // Proven dynamic-array loop bounds remove runtime slot checks only when the
 // array length relation is statically preserved.
 llvm_not_contains(R"(int[] data = array(8, fill = 0)
int size = int(len(data))
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
int size = int(len(data))
size = 4
print(data[size])
print(NL)
)", "call ptr @quidra_array_slot(ptr");
 llvm_contains(R"(int[] data = array(8, fill = 0)
int size = int(len(data))
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

 good(R"(nat8 a = 240
nat8 b = 204
nat8 both = a AND b
nat8 either = a OR b
nat8 different = a XOR b
nat8 inverted = NOT a
nat8 left = a << 1
nat8 right = a >> 2
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
 llvm_contains("nat8 a = 3\nnat8 b = a << 2\n", "shl i8");
 llvm_contains("nat8 a = 3\nnat8 b = NOT a\n", "xor i8");
 good("nat8 inverted = NOT 1\n");
 good("nat8 flags = 12\nnat8 mask = 10\nbool selected = flags AND mask == 8\n");
 good("nat8 flags = 1\nnat8 mask = 2\nbool selected = flags OR mask == 3\n");
 bad_code("bool a = true\nbool b = false\nbool c = a AND b\n", "TYPE_MISMATCH");
 bad_code("real64 a = 1.0\nreal64 b = 2.0\nreal64 c = a OR b\n", "TYPE_MISMATCH");
 bad_code("int a = 1\nint b = 2\nint c = a XOR b\n", "TYPE_MISMATCH");
 bad_code("nat8 a = 1\nnat8 b = a << 8\n", "SHIFT_COUNT");
 good(R"(class TensorState
    tensor<real32> value

class NestedState
    TensorState value
)");

 good("extern void scalar_abi(int8 a, int16 b, int32 c, int64 d, nat8 e, nat16 f, nat32 g, nat64 h, real32 i, real64 j, bool k) = \"scalar_abi\"\n");
 llvm_contains("extern bool c_bool(bool value) = \"c_bool\"\n", "declare zeroext i1 @c_bool(i1 zeroext)");
 llvm_contains("extern int8 c_i8(int8 value) = \"c_i8\"\n", "declare signext i8 @c_i8(i8 signext)");
 llvm_contains("extern int16 c_i16(int16 value) = \"c_i16\"\n", "declare signext i16 @c_i16(i16 signext)");
 llvm_contains("extern nat8 c_u8(nat8 value) = \"c_u8\"\n", "declare zeroext i8 @c_u8(i8 zeroext)");
 llvm_contains("extern nat16 c_u16(nat16 value) = \"c_u16\"\n", "declare zeroext i16 @c_u16(i16 zeroext)");
 llvm_contains("extern int8 c_i8(int8 value) = \"c_i8\"\nint8 x = 1\nint8 y = c_i8(x)\n", "call signext i8 @c_i8(i8 signext");
 good("extern int32 c_text(const string &text) = \"c_text\"\n");
 good("extern int32 c_bin(const bin &data) = \"c_bin\"\n");
 ir_contains("extern int32 c_text(const string &text) = \"c_text\"\n", "function c_text(const string &text) -> int32 = \"c_text\"");
 llvm_contains("extern int32 c_text(const string &text) = \"c_text\"\n", "declare i32 @c_text(ptr nocapture nonnull readonly, i64)");
 llvm_contains("extern int32 c_bin(const bin &data) = \"c_bin\"\n", "declare i32 @c_bin(ptr nocapture nonnull readonly, i64)");
 llvm_contains("extern int32 c_bin_mut(bin &data) = \"c_bin_mut\"\n", "declare i32 @c_bin_mut(ptr nocapture nonnull, i64)");
 good("extern int32 c_tensor(const tensor<real32> &input, tensor<real32> &output) = \"c_tensor\"\n");
 llvm_contains(
     "extern int32 c_tensor(const tensor<real32> &input, tensor<real32> &output) = \"c_tensor\"\n"
     "tensor<real32> input = tensor.ones<real32>([1])\n"
     "tensor<real32> output = tensor.zeros<real32>([1])\n"
     "int32 status = c_tensor(&input, &output)\n",
     "call i32 @c_tensor(ptr");
 bad_code("extern int32 c_tensor_value(tensor<real32> input) = \"c_tensor_value\"\n", "FFI_REFERENCE");
 llvm_contains("extern int32 c_text(const string &text) = \"c_text\"\nstring value = \"abc\"\nint32 result = c_text(&value)\n", "ffi.borrowed.value");
 llvm_contains("extern int32 c_text(const string &text) = \"c_text\"\nstring value = \"abc\"\nint32 result = c_text(&value)\n", "call i64 @strlen(ptr");
 llvm_contains("extern int32 c_bin(const bin &data) = \"c_bin\"\nbin value = bin.fill(24, 1)\nint32 result = c_bin(&value)\n", "ffi.bin.length");
 llvm_contains("extern int32 c_bin(const bin &data) = \"c_bin\"\nbin value = bin.fill(24, 1)\nint32 result = c_bin(&value)\n", "call i32 @c_bin(ptr nocapture nonnull readonly");
 llvm_contains("extern int32 c_bin_mut(bin &data) = \"c_bin_mut\"\nbin value = bin.fill(24, 1)\nint32 result = c_bin_mut(&value)\n", "call i32 @c_bin_mut(ptr nocapture nonnull");
 good("bin data = \"ok\".utf8()\nauto text = string.from_utf8(data)\n");
 llvm_contains("bin data = \"ok\".utf8()\nauto text = string.from_utf8(data)\n", "@quidra_bin_try_utf8");
 bad_code("auto text = string.from_utf8(\"not bin\")\n", "TYPE_MISMATCH");
 llvm_file_contains("tensor<real64> source = tensor.ones<real64>([1])\ntensor<real64> value = source.track()\ntensor<real64> next = value + 1.0\n", "@quidra_tensor_binary");
 llvm_file_contains("tensor<real32> source = tensor.ones<real32>([1])\ntensor<real32> value = source.track()\nnat8 scalar = 255\ntensor<real32> next = value + real32(scalar)\n", "uitofp i8");
 llvm_file_contains("tensor<real32> source = tensor.ones<real32>([1])\ntensor<real32> value = source.track()\nint8 scalar = -1\ntensor<real32> next = value + real32(scalar)\n", "sitofp i8");

 bad_code("extern int32 implicit_text(string text) = \"implicit_text\"\n", "FFI_REFERENCE");
 bad_code("extern int32 implicit_bytes(bin data) = \"implicit_bytes\"\n", "FFI_REFERENCE");
 good("extern int32 mutable_bytes(bin &data) = \"mutable_bytes\"\n");
 good("extern int64 c_apply(fn<int64>(int64) callback, int64 value) = \"c_apply\"\n");
 llvm_contains("extern int64 c_apply(fn<int64>(int64) callback, int64 value) = \"c_apply\"\n", "declare i64 @c_apply(ptr, i64)");
 llvm_contains(R"(int64 twice(int64 value)
    return value * 2
extern int64 c_apply(fn<int64>(int64) callback, int64 value) = "c_apply"
int result = int(c_apply(twice, 21))
)", "call i64 @c_apply(ptr");
 bad_code("extern int64 unsafe_callback(fn<int8>(int8) callback) = \"unsafe_callback\"\n", "FFI_CALLBACK_TYPE");
 bad_code("extern int64 unsafe_callback(fn<string>(int64) callback) = \"unsafe_callback\"\n", "FFI_CALLBACK_TYPE");
 bad_code("extern int32 mutable_text(string &text) = \"mutable_text\"\n", "FFI_REFERENCE");
 bad_code("extern int32 const_value(const string text) = \"const_value\"\n", "FFI_REFERENCE");
 bad_code("extern int32 c_puts(const string &text) = \"puts\"\n", "FFI_SYMBOL_CONFLICT");
 bad_code("extern int32 c_main(int32 value) = \"main\"\n", "FFI_SYMBOL_CONFLICT");
 bad_code("extern int32 c_mangled(int32 value) = \"n_user_function\"\n", "FFI_SYMBOL_CONFLICT");
 bad_code("extern int32 c_runtime(int32 value) = \"quidra_future_runtime_symbol\"\n", "FFI_SYMBOL_CONFLICT");
 bad_code("extern int32 c_internal(int32 value) = \"__quidra_internal_symbol\"\n", "FFI_SYMBOL_CONFLICT");
 bad_code("extern int32 first(int32 value) = \"shared_symbol\"\nextern int32 second(int32 value) = \"shared_symbol\"\n", "FFI_SYMBOL_CONFLICT");
 good(R"(extern int32 generic_tensor_bridge<T: numeric>(tensor<T> &value) = "generic_tensor_bridge"
tensor<nat8> a = tensor.zeros<nat8>([1])
tensor<real32> b = tensor.zeros<real32>([1])
int32 first_result = generic_tensor_bridge(&a)
int32 second_result = generic_tensor_bridge(&b)
)");
 bad_code("extern string unsafe(int64 value) = \"unsafe_symbol\"\n", "FFI_TYPE");
 bad_code("extern bin unsafe_bin(int64 value) = \"unsafe_symbol\"\n", "FFI_TYPE");
 bad_code("extern int64 unsafe(int64 &value) = \"unsafe_symbol\"\n", "FFI_REFERENCE");
 bad_code("extern int64 unsafe(int64 value = 1) = \"unsafe_symbol\"\n", "FFI_DEFAULT");
 bad_code("extern int64 unsafe(int64 value) = \"bad-symbol\"\n", "FFI_SYMBOL");

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
fn<real64>(int) operation = twice
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
)", "call void @quidra.int.task_all");
 llvm_contains(R"(int64 first_value()
    return 20
int64 second_value()
    return 22
int64[] results = task.all([first_value, second_value])
)", "call void @quidra_task_all_i64");
 good(R"(real64 first_value()
    return real64(20.0)
real64 second_value()
    return real64(22.0)
real64[] results = task.all([first_value, second_value])
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
 imported_locals_use_module_scope();
 imported_signatures_use_module_scope();
 imported_type_parameters_use_module_scope();
 imported_names_resolve_in_module_scope();
 string_input_ignores_package_lock();
 compile_file_reports_imported_packages();
 compile_file_records_its_inputs();
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
      "tensor<real32> tensor_value = tensor<real32>([1])\n"
      "tensor_value[0] = 1.0\n"
      "tensor<real32> tensor_chain = tensor_value",
      "tensor_value");
  compile_chain(
      "tensor<real32> tracked_source = tensor<real32>([1])\n"
      "tracked_source[0] = 1.0\n"
      "tensor<real32> tracked_value = tracked_source.track()\n"
      "tensor<real32> tracked_chain = tracked_value",
      "tracked_value");
 }
 for(const auto& s:std::vector<std::string>{
 R"(void replace(int[] &x, int count = 3)
    x = array(nat(count), fill = 7)
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
        this.value = this.value + 1

    void increment()
        increment_raw()

    int get()
        return this.value

PrivateCounter counter
counter.increment()
print(counter.get())
print(NL)
)",
 R"(class Point
    int x
    int y

    construct(int x_value, int y_value)
        this.x = x_value
        this.y = y_value

    int sum()
        return this.x + this.y

class LabeledPoint
    Point point
    string label

    construct(Point point_value, string label_value)
        this.point = point_value
        this.label = label_value

    int sum()
        return this.point.sum()

class OffsetPoint
    Point point
    int offset

    construct(Point point_value, int offset_value)
        this.point = point_value
        this.offset = offset_value

    int sum()
        return this.point.sum() + this.offset

class Box
    Point point

    construct(Point point_value)
        this.point = point_value

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
        this.value = 0

    void increment()
        this.value = this.value + 1

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
        this.x = x_value

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
        this.x = x_value

    int sum()
        return this.x + this.y

class DefaultOffset
    DefaultPoint point = DefaultPoint()
    int offset = 1

    int sum()
        return this.point.sum() + this.offset

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
        this.x = x_value
        this.y = y_value

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
        this.value = value_value

    T get()
        return this.value

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
        this.value = value_value

    T read()
        return this.value

class GenericHolder<T>
    GenericBase<T> base

    construct(GenericBase<T> base_value)
        this.base = base_value

    T read()
        return this.base.read()

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
        this.parent = parent_value

    T echo<T>(T value)
        return this.parent.echo<T>(value)

GenericChild child = GenericChild(GenericParent())
print(child.echo<int>(11))
print(NL)
)",
 R"(class NestedBox<T>
    T value

    construct(T value_value)
        this.value = value_value

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
    real64 bb

    construct(real64 bb_value)
        this.bb = bb_value

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
        this.x = x_value
        this.y = y_value

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
        this.value = value_value

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
        this.value = value_value

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
    real64[] ys

class NestedOwner
    NestedData data

    real64 first()
        this.data.ys = [3.0]
        return this.data.ys[0]

    void initialize()
        this.data.ys = [4.0]

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
        this.x = x_value
        this.y = y_value

class ReplaceOuter
    ReplaceInner inner

    construct(ReplaceInner inner_value)
        this.inner = inner_value

    void reset()
        ReplaceInner replacement
        replacement.x = 5
        replacement.y = 6
        this.inner = replacement

ReplaceOuter outer = ReplaceOuter(ReplaceInner(1, 2))
outer.reset()
print(outer.inner.x)
print(NL)
)",
 R"(class ConditionalInner
    int x
    int y

    construct(int x_value, int y_value)
        this.x = x_value
        this.y = y_value

class ConditionalOuter
    ConditionalInner inner

    construct(ConditionalInner inner_value)
        this.inner = inner_value

    void maybe_reset(bool replace)
        if replace
            ConditionalInner replacement
            replacement.x = 5
            replacement.y = 6
            this.inner = replacement

ConditionalOuter outer = ConditionalOuter(ConditionalInner(1, 2))
outer.maybe_reset(false)
print(outer.inner.x)
print(NL)
)",
 R"(class RepairInner
    int x
    int y

    construct(int x_value, int y_value)
        this.x = x_value
        this.y = y_value

class RepairOuter
    RepairInner inner

    construct(RepairInner inner_value)
        this.inner = inner_value

    void reset()
        RepairInner replacement
        replacement.x = 5
        replacement.y = 6
        this.inner = replacement

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
nat8 byte_value = 255
nat32 count = 100
int total = int(count)
real64 exact_float = 1.0
real32 exact_small_float = 1.5
int8 casted = int8(100)
int exact_from_float = 3
string small_text = small.string()
string flag_text = true.string()
int | error parsed = int.parse("123")
real32 | error parsed_float = real32.parse("1.5")
bin allocated = bin.fill(8, 0)
string repeated = string.repeat("a", 3)
bin data = bin.fill(16, 0)
bin first = data[0]
bin slice = data[0:8]
nat8[] decoded = nat8[](data)
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
 good(R"(real fraction = 1.5
auto | error exact_value = int(fraction)
match exact_value
    int value
        print(value)
        print(NL)
    error problem
        print(problem)
        print(NL)
)");
 llvm_contains(R"(int wide = 300
auto | error narrowed = int8(wide)
match narrowed
    int8 value
        print(value)
        print(NL)
    error problem
        print(problem)
        print(NL)
)", "@quidra.int.try_i64");
 llvm_contains(R"(real fraction = 1.5
auto | error exact_value = int(fraction)
match exact_value
    int value
        print(value)
        print(NL)
    error problem
        print(problem)
        print(NL)
)", "@quidra_bigreal_try_bigint");
 good(R"(int huge = 10000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
auto | error rounded = real64(huge)
match rounded
    real64 value
        print(value)
        print(NL)
    error problem
        print(problem)
        print(NL)
)");
 llvm_contains(R"(int huge = 10000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
auto | error rounded = real64(huge)
match rounded
    real64 value
        print(value)
        print(NL)
    error problem
        print(problem)
        print(NL)
)", "@quidra.int.try_float64");

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

 good(R"(real64 scalar = 3.0
real32 scalar32 = 2.0
real64[] values = [1.0, 2.0, 3.0]

real64 half(real64 value)
    return value / 2.0

real64 negative = -3.0
real64 product = 2.0 * 3.0
real64 argument = half(3.0)
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
        this.a = a_value
        this.b = b_value

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

 tensor_region_at_least(R"(tensor<real32> x = tensor.ones<real32>([]).track()
((x * x + x) * x).backward(&x)
)", 3, true);

 const std::string ir_surface = R"(int source = 1
int &alias = &source
alias = 2
tensor<real32> a = tensor.zeros<real32>([2, 2])
tensor<real32> b = tensor.ones<real32>([2, 2])
tensor<real32> c = a + b
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
 "nat8 x = 10\nprint(x / nat8(0))\n",
 "real64 x = .5\n",
 R"(class PartialReturn
    int x
    int y

    construct(int x_value)
        this.x = x_value

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
        this.x = x_value
        this.y = y_value

class ReplaceOuterBad
    ReplaceInnerBad inner

    construct(ReplaceInnerBad inner_value)
        this.inner = inner_value

    void reset()
        ReplaceInnerBad replacement
        replacement.x = 5
        this.inner = replacement

ReplaceOuterBad outer = ReplaceOuterBad(ReplaceInnerBad(1, 2))
outer.reset()
print(outer.inner.y)
print(NL)
)",
 R"(class ConditionalInnerBad
    int x
    int y

    construct(int x_value, int y_value)
        this.x = x_value
        this.y = y_value

class ConditionalOuterBad
    ConditionalInnerBad inner

    construct(ConditionalInnerBad inner_value)
        this.inner = inner_value

    void maybe_reset(bool replace)
        if replace
            ConditionalInnerBad replacement
            replacement.x = 5
            this.inner = replacement

ConditionalOuterBad outer = ConditionalOuterBad(ConditionalInnerBad(1, 2))
outer.maybe_reset(false)
print(outer.inner.y)
print(NL)
)",
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
 "int8 x = 128\n", "nat8 x = -1\n", "int8 x = int8(300)\n",
 "bin x = bin(2, fill = 0)\n", "bin x = bin.fill(-1, 0)\n", "bin x = bin.fill(2, 2)\n", "string x = string(2, fill = \"a\")\n", "string x = string.repeat(\"a\", -1)\n",
 "string HT = \"x\"\n", "void f(string NL)\n    return\n",
 "int x = 1\nif true\n    int x = 2\n", "int x = 1\nint x = 2\n",
 "class A\n    int x\n    int x\n",
 "class A\n    int HT\n",
 "class A\n    int x\n    construct(int start)\n        this.x = start\nA a = A(1)\nA &b = a\n",
 "class A\n    int x\n    int y\n    construct(int start)\n        this.x = start\nA a = A(1)\nprint(a.y)\n",
 "class Counter\n    int value\n    void increment()\n        this.value = this.value + 1\nCounter c = Counter()\nc.increment()\n",
 "int x\nint &y = &x\nprint(y)\n",
 "int a = 1\nint b = 2\nint &r = &a\nr = &b\n",
 "int a = 1\nint b = 2\nint &r = &a\n&r = b\n",
 "int &x = &5\n",
 R"(class EqPartial
    int x
    int y

    construct(int x_value)
        this.x = x_value

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
    real64 gamma = 1.0

    construct()
        return
real64 apply(Linear | Rbf kernel)
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
        this.x = x_value
Partial value = Partial(1)
Partial | none maybe = value
)", "UNINITIALIZED_UNION_PAYLOAD");
 good(R"(class LocalReset
    int value
    void reset()
        this.value = 0
void run()
    LocalReset item
    item.reset()
    print(item.value)
    print(NL)
run()
)");
 bad_code(R"(class EarlyReceiver
    real64[] values
    void | error initialize(int count)
        if count < 0
            return error("bad")
        this.values = array(count, fill = 0.0)
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
        this.value = 1
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
        this.value = 1
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
        this.value = 1
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
        this.values = values_value

const ConstNested item = ConstNested([1, 2])
item.values[0] = 3
)", "WRITE_CAPABILITY");

 bad_code(R"(class ConstMethod
    int value

    construct(int value_value)
        this.value = value_value

    void change_value()
        this.value = 2

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
print(sum_values(&values, int(len(values))))
print(NL)
)", "call i1 @quidra_array_initialization_complete");
 llvm_contains(R"(int sum_values(const int[] &values, int n)
    int total = 0
    for i in range(0, n)
        total += values[i]
    return total

int[] values = array(8, fill = 1)
print(sum_values(&values, int(len(values))))
print(NL)
)", "call ptr @quidra_array_slot_proven");
 llvm_contains(R"(int sum_values(const int[] &values, int n)
    int total = 0
    for i in range(0, n)
        total += values[i]
    return total

int[] values = array(8, fill = 1)
print(sum_values(&values, int(len(values))))
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
        this.value = value_value
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
        this.value = other.value
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
        this.value = value_value
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
        this.value = value_value
    int read()
        return this.value
PrivateInit item = PrivateInit(9)
print(item.read())
print(NL)
)");
 bad_code(R"(class PrivateInit
    private int value

    construct(int start)
        this.value = start
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
real64 choose(int value)
    return real64(value)
)", "DUPLICATE_NAME");
 good(R"(string classify(nat8 value)
    return "byte"
string classify<T: floating>(T value)
    return "floating"
print(classify(nat8(1)))
print(NL)
print(classify(real32(1.0)))
print(NL)
)");
 good(R"(int domain<T: numeric>(T value)
    return 1
int domain<T: floating>(T value)
    return 2
print(domain<int>(int(1)))
print(NL)
print(domain<real32>(real32(1.0)))
print(NL)
)");
 bad_code(R"(T ambiguous<T: ordered>(T value)
    return value
T ambiguous<T: equatable>(T value)
    return value
)", "AMBIGUOUS_SPECIALIZATION");
 good(R"(T exact_cast_family<T: numeric>(T value)
    return value
int exact_integer = exact_cast_family(int(2))
real exact_real = exact_cast_family(real(2))
print(exact_integer)
print(NL)
print(exact_real)
print(NL)
)");
 good(R"(T1 pair_domain<T1: numeric, T2: integer>(T1 left, T2 right)
    return left
T1 pair_domain<T1: floating, T2: integer>(T1 left, T2 right)
    return left
real32 narrow = pair_domain(real32(2.0), int(1))
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
 bad_code(R"(int concrete_relation(int left, real64 right)
    return left
T concrete_relation<T: numeric>(T left, T right)
    return left
)", "DUPLICATE_NAME");
 good(R"(int radius_mode(tensor<nat8> value, int radius = 1)
    return radius
T radius_mode<T: floating>(tensor<T> value, int radius = 1)
    return value[0].item()
tensor<nat8> pixels = tensor.zeros<nat8>([1])
print(radius_mode(pixels, radius = 1))
print(NL)
)");

 good(R"(int signed_radius(tensor<nat8> value, int radius = 1)
    return radius
T signed_radius<T: floating>(tensor<T> value, int radius = 1)
    return value[0].item()
tensor<nat8> pixels = tensor.zeros<nat8>([1])
print(signed_radius(pixels, radius = -1))
print(NL)
)");

 good(R"(int kernel_device(tensor<nat8> value, tensor<int64> kernel)
    return 1
T kernel_device<T: floating, K: floating>(tensor<T> value, tensor<K> kernel)
    return value[0].item()
tensor<nat8> pixels = tensor.zeros<nat8>([1])
tensor<int64> kernel = tensor.ones<int64>([1])
print(kernel_device(pixels, kernel.gpu(0)))
print(NL)
)");

 good(R"(string shaped_specialization(tensor<nat8> value)
    return "byte"
string shaped_specialization<T: floating>(tensor<T> value)
    return "floating"
tensor<nat8><1, 2, 2> pixels = tensor.ones<nat8>([1, 2, 2])
print(shaped_specialization(pixels))
print(NL)
)");

 good(R"(tensor<T> transformed_specialization<T: numeric>(
    tensor<T> left,
    tensor<T> right
)
    return right % right
tensor<real32> transformed_specialization(
    tensor<real32> left,
    tensor<real32> right
)
    return right
tensor<real32> specialization_left = tensor.ones<real32>([1, 2])
tensor<real32> specialization_right = tensor.ones<real32>([1, 2])
tensor<real32> specialization_result = transformed_specialization(
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
print(classify_proto<real32>(real32(2.0)))
print(NL)
)");
 good(R"(class MethodDomain
    string classify(nat8 value)
        return "byte"
    string classify<T: floating>(T value)
        return "floating"

MethodDomain domain
print(domain.classify(nat8(1)))
print(NL)
print(domain.classify(real32(1.0)))
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
print(priority.choose(real32(3.0)))
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
real32 narrow = domain.combine(real32(2.0), int(1))
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
        this.value = 0
    construct(int value_value)
        this.value = value_value
)", "DUPLICATE_NAME");
 good(R"(class DefaultConstructorParameter
    int value
    construct(int value_value = 7)
        this.value = value_value
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
 bad_code(R"(int | real64 value = 5
real64 narrowed = value
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
    Number(real64)
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
    Number(real64)
    End
Token token = Token.End
match token
    Token.Number(value)
        print(value)
        print(NL)
)", "MATCH_EXHAUSTIVE");
 bad_code(R"(enum Token
    Number(real64)
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
print(square<real32>(real32(3.0)))
print(NL)
)");
 good(R"(class Box<T: equatable>
    T value

    construct(T value_value)
        this.value = value_value
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
 good("int32 x = 3\nreal32 y = 3.0\nreal32 z = 0.1\n");
 bad_code("auto x = 3\n", "AMBIGUOUS_NUMERIC_LITERAL");
 bad_code("auto x = 1.5\n", "AMBIGUOUS_NUMERIC_LITERAL");
 bad_code("print(1)\n", "AMBIGUOUS_NUMERIC_LITERAL");
 bad_code("real64 x = 3\n", "NUMERIC_FAMILY");
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
 bad_code("int value = 1\nbin raw = bin(value)\n", "TYPE_MISMATCH");
 bad_code("real value = 1.0\nbin raw = bin(value)\n", "TYPE_MISMATCH");
 good("real32 rounded = real32(16777217)\n");
 bad_code("real32 too_large = 1.0e100\n", "REAL_RANGE");
 bad_code("real32 too_large = real32(1.0e100)\n", "NUMERIC_CAST");
 // A literal is rounded once, from its decimal value, to the target
 // format. 1.00000005960464477550 lies just above the binary32 midpoint
 // 1 + 2^-24 and its binary64 value is that midpoint, so rounding through
 // binary64 would give 1.0; 2^60 + 2^36 + 1 likewise lies just above a
 // binary32 midpoint. Just below the binary32 overflow threshold a literal
 // rounds to the largest finite value, although its binary64 value is the
 // threshold.
 llvm_contains("real32 x = 1.00000005960464477550\nprint(x)\n", "0x3FF0000020000000");
 llvm_contains("real32 x = real32(1152921573326323713)\nprint(x)\n", "0x43B0000020000000");
 llvm_contains("real32 x = 3.4028235677973366e38\nprint(x)\n", "0x47EFFFFFE0000000");
 bad_code("real32 too_large = 3.4028235677973367e38\n", "REAL_RANGE");
 // An integer literal beyond 64 bits converts from its digits.
 llvm_contains("real32 x = real32(100000000000000000000000)\nprint(x)\n", "0x44B52D02C0000000");
 bad_code("real32 too_large = real32(1000000000000000000000000000000000000000)\n", "NUMERIC_CAST");

 // Numeric spelling has one integer radix; exponent notation is visibly floating.
 bad_code("real64 x = 1e8\n", "LEX_ERROR");
 good("real64 x = 1.0e8\n");

 // '^' is a Core basic operator. Scalar power preserves the concrete
 // numeric family, and tensor power is deliberately one-way: tensor ^ scalar.
 // Matrix multiplication has no '@' syntax.
 good("int base = 2\nint powered = base ^ 10\nprint(powered)\n");
 good("real64 base = 4.0\nreal64 powered = base ^ 0.5\nprint(powered)\n");
 good(R"(tensor<int64> base = tensor.ones<int64>([2]) * 2
tensor<int64> powered = base ^ 3
print(powered[0].item())
print(NL)
)");
 good(R"(tensor<real64> base = tensor.ones<real64>([2]) * 2.0
tensor<real64> powered = base ^ 0.5
print(powered[0].item())
print(NL)
)");
 bad_code(R"(tensor<int64> base = tensor.ones<int64>([1])
tensor<int64> exponent = tensor.ones<int64>([1])
tensor<int64> powered = base ^ exponent
)", "TYPE_MISMATCH");
 bad_code(R"(tensor<int64> base = tensor.ones<int64>([1])
tensor<int64> powered = 2 ^ base
)", "TYPE_MISMATCH");
 bad_code(R"(tensor<int64> base = tensor.ones<int64>([1])
tensor<int64> powered = base ^ -1
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
        this.x = x_value
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
        this.x = x_value
P a = P(1)
P b = P(1)
bool same = a == b
)", "UNINITIALIZED_FIELD_EQUALITY");
 bad_code(R"(class ArrayPartialLiteral
    int x
    int y

    construct(int x_value)
        this.x = x_value
ArrayPartialLiteral partial = ArrayPartialLiteral(1)
ArrayPartialLiteral[] values = [partial]
)", "UNINITIALIZED_ARGUMENT");
 bad_code(R"(class ArrayPartialFill
    int x
    int y

    construct(int x_value)
        this.x = x_value
ArrayPartialFill partial = ArrayPartialFill(1)
ArrayPartialFill[] values = array(2, fill = partial)
)", "UNINITIALIZED_ARGUMENT");
 bad_code(R"(class ArrayPartialAppend
    int x
    int y

    construct(int x_value)
        this.x = x_value
ArrayPartialAppend partial = ArrayPartialAppend(1)
ArrayPartialAppend[] values = []
values = values.append(partial)
)", "UNINITIALIZED_ARGUMENT");
 bad_code(R"(class ArrayPartialAssign
    int x
    int y

    construct(int x_value)
        this.x = x_value
ArrayPartialAssign partial = ArrayPartialAssign(1)
ArrayPartialAssign[] values = array(1)
values[0] = partial
)", "UNINITIALIZED_ARGUMENT");
 good(R"(class IndexedPoint
    int x
    int y

    construct(int x_value, int y_value)
        this.x = x_value
        this.y = y_value

    int sum()
        return this.x + this.y

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
        this.x = x_value

IndexedPartialPoint partial = IndexedPartialPoint(1)
IndexedPartialPoint[] values = array(1)
values[0] = partial
)", "UNINITIALIZED_ARGUMENT");
 bad_code("class A\n    int x\n    construct(int start)\n        this.x = start\nA a = A(1)\nprint(a.y)\n", "UNKNOWN_MEMBER");
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
 // The spellings that the numeric families replaced are released: they name
 // no type or namespace any more and are ordinary identifiers again.
 bad_code("uint8 value = 1\n", "UNKNOWN_TYPE");
 bad_code("uint64 value = 1\n", "UNKNOWN_TYPE");
 bad_code("float value = 1.0\n", "UNKNOWN_TYPE");
 bad_code("float32 value = 1.0\n", "UNKNOWN_TYPE");
 bad_code("float64 value = 1.0\n", "UNKNOWN_TYPE");
 bad_code("bigreal value = 1.0\n", "UNKNOWN_TYPE");
 bad_code("real value = exact.atom(\"test-provider\", 1)\n", "UNKNOWN_NAME");
 bad_code("auto generator = random.generator(1)\nreal64 value = generator.float()\n", "UNKNOWN_MEMBER");
 good("int float = 3\nint uint8 = 4\nint bigreal = float + uint8\nprint(bigreal)\n");
 good("int exact = 1\nprint(exact)\n");
 bad_code("real64 value = 1.0e9999\n", "REAL_RANGE");
 bad_code("import math\n", "IMPORT_CONTEXT");
 bad_code("auto loaded = vision.read<nat8>(\"input.png\")\n", "GENERIC_RECEIVER");
 bad_code("auto loaded = vision.read(\"input.png\")\n", "UNKNOWN_NAME");
 bad_code("print(math.sqrt(real64(16.0)))\n", "UNKNOWN_NAME");
 good("tensor<real32> grid = tensor.zeros<real32>([2, 2])\nprint(grid.shape()[0])\n");
 bad_code("tensor<real64> x = tensor.ones<real64>([1])\nauto y = x.abs()\n", "UNKNOWN_MEMBER");
 bad_code("tensor<real64> x = tensor.ones<real64>([1])\nauto y = x.exp()\n", "UNKNOWN_MEMBER");
 bad_code("tensor<real64> x = tensor.ones<real64>([1])\nauto y = x.log()\n", "UNKNOWN_MEMBER");
 bad_code("tensor<real64> x = tensor.ones<real64>([1])\nauto y = x.sqrt()\n", "UNKNOWN_MEMBER");

 // exact.* is a package-neutral Core mechanism. Providers and opcodes are
 // opaque to Core; packages own the mathematical meaning.
 good(R"(real input = real(2)
real result = real.unary("test-provider", 7, input)
print(result.string())
print(NL)
)");
 llvm_contains(R"(real input = real(2)
real result = real.unary("test-provider", 7, input)
)", "@qcore_exact_real_unary");
 bad_code(R"(string provider = "test-provider"
real input = real(2)
real result = real.unary(provider, 7, input)
)", "EXACT_PROVIDER");
 bad_code(R"(int opcode = 7
real input = real(2)
real result = real.unary("test-provider", opcode, input)
)", "EXACT_PROVIDER");
 good(R"(tensor<int64><2> a = tensor.ones<int64>([2])
tensor<int64><2> b = tensor.ones<int64>([2])
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
 llvm_contains(R"(tensor<int64><2> a = tensor.ones<int64>([2])
tensor<int64><2> b = tensor.ones<int64>([2])
tensor<bool><2> mask = a == b
bool same = mask.all()
)", "@quidra_tensor_compare");
 llvm_contains(R"(tensor<int64><2> a = tensor.ones<int64>([2])
bool any = (a != 1).any()
)", "@quidra_tensor_bool_reduce");
 bad_code(R"(tensor<int64><2> a = tensor.ones<int64>([2])
tensor<int64><3> b = tensor.ones<int64>([3])
tensor<bool> same = a == b
)", "TENSOR_SHAPE");
 good(R"(tensor<int64><2> a = tensor.ones<int64>([2])
tensor<bool><2> same = a == 1
tensor<bool><2> reverse = 1 == a
)");
 bad_code(R"(tensor<int64><2> a = tensor.ones<int64>([2])
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
 good(R"(tensor<real32><2, 3> matrix = tensor.zeros<real32>([2, 3])
nat[2] dimensions = matrix.shape()
tensor<real32> erased = matrix
nat[2] inferred_dimensions = erased.shape()
tensor<real32><3> row = matrix[0]
tensor<real32><2> column = matrix[:, 0]
tensor<real32> cell = matrix[0, 0]
real32 value = cell.item()
tensor<real32><6> reshaped = matrix.reshape([6])
tensor<real32><2, 3> contiguous = matrix.contiguous()
tensor<real64><2, 3> converted = real64(matrix)
)");
 good(R"(tensor<T><3, _> first_three<T>(tensor<T><3, _> value)
    return value
tensor<real32> source = tensor.ones<real32>([3, 2])
tensor<real32><3, _> constrained = first_three(source)
)");
 // Generic dtype inference may proceed through an unknown shape, but the
 // specialized call still enforces the exact-rank shape pattern.
 bad_code(R"(tensor<T><3, _> first_three<T>(tensor<T><3, _> value)
    return value
tensor<real32> source = tensor.ones<real32>([2, 2])
tensor<real32><3, _> constrained = first_three(source)
)", "TYPE_MISMATCH");
 good(R"(tensor<real32> | error direct = tensor.zeros<real32>([2, 2])
tensor<real32><2, _> | error constrained = tensor.ones<real32>([2, 2])
tensor<real32> | error widened = constrained
match widened
    tensor<real32> pixels
        print(pixels.shape()[0])
        print(NL)
    error problem
        print(problem)
        print(NL)
)");
 bad_code("tensor<real32><3, _> wrong = tensor.zeros<real32>([2, 2])\n", "TYPE_MISMATCH");
 bad_code("tensor<real32><2, 4> wrong = tensor.zeros<real32>([2, 3])\n", "TYPE_MISMATCH");
 bad_code("tensor<real32><3, _> wrong_rank = tensor.zeros<real32>([3, 4, 5])\n", "TYPE_MISMATCH");
 good("tensor<real32><3, _, _> exact_rank = tensor.zeros<real32>([3, 4, 5])\n");
 good(R"(tensor<real32> contextual_dtype = tensor.zeros([2, 3])
nat[2] contextual_shape = contextual_dtype.shape()
tensor<real32><2, 3> contextual_exact = tensor.ones([2, 3])
)");
 bad_code("auto missing_dtype = tensor.zeros([2, 3])\n", "GENERIC_ARITY");
 good(R"(auto generated = tensor.zeros<real32>([2, 3])
nat[2] generated_shape = generated.shape()
)");
 bad_code(R"(auto generated = tensor.zeros<real32>([2, 3])
tensor<real32><2, 4> impossible = generated
)", "TYPE_MISMATCH");
 bad_code(R"(tensor<real32><2, 3> source = tensor.zeros<real32>([2, 3])
auto transposed = source.transpose(0, 1)
tensor<real32><2, 3> impossible = transposed
)", "TYPE_MISMATCH");
 good(R"(tensor<real32><2, 3> source = tensor.zeros<real32>([2, 3])
auto transposed = source.transpose(0, 1)
tensor<real32><3, 2> exact_shape = transposed
auto reshaped = source.reshape([3, 2])
tensor<real32><3, 2> reshaped_exact = reshaped
)");
 bad_code(R"(tensor<real32><2, 3> source = tensor.zeros<real32>([2, 3])
auto reshaped = source.reshape([3, 2])
tensor<real32><2, 3> impossible = reshaped
)", "TYPE_MISMATCH");
 ir_contains(R"(tensor<real32> erase_shape(tensor<real32> value)
    return value
auto unknown = erase_shape(tensor.zeros<real32>([2, 3]))
tensor<real32><2, 4> checked_at_runtime = unknown
)", "shape.constraint");
 bad_code(R"(tensor<real32><2, 3> source = tensor.zeros<real32>([2, 3])
auto invalid = source.transpose(0, 2)
)", "ARGUMENT_MISMATCH");
 // The static shape of tensor arithmetic follows the broadcast rule: a 1
 // takes the other side's extent, 0 included, in either operand order, so
 // these results are known exactly (and shapes cannot be misreported as the
 // left operand's).
 good(R"(tensor<real32><1, 3> one = tensor.ones<real32>([1, 3])
tensor<real32><0, 3> empty = tensor.zeros<real32>([0, 3])
tensor<real32><2, 3> two = tensor.ones<real32>([2, 3])
tensor<real32><0, 3> one_empty = one + empty
tensor<real32><0, 3> empty_one = empty * one
tensor<real32><2, 3> one_two = one - two
tensor<real32><2, 3> two_one = two / one
tensor<real32><1, 3> one_one = one + one
tensor<real32><0, 3> scalar = empty + real32(2)
)");
 good(R"(tensor<real32> one = tensor.ones<real32>([1, 3])
tensor<real32> empty = tensor.zeros<real32>([0, 3])
tensor<real32> flow = one + empty
tensor<real32><0, 3> refined = flow
)");
 bad_code(R"(tensor<real32><1, 3> one = tensor.ones<real32>([1, 3])
tensor<real32><0, 3> empty = tensor.zeros<real32>([0, 3])
tensor<real32><1, 3> stale = one + empty
)", "TYPE_MISMATCH");
 bad_code(R"(tensor<real32><1, 3> one = tensor.ones<real32>([1, 3])
tensor<real32><2, 3> two = tensor.ones<real32>([2, 3])
tensor<real32><1, 3> stale = one * two
)", "TYPE_MISMATCH");
 // A 1 against an unknown extent leaves the result extent open: checked at
 // runtime, not rejected.
 ir_contains(R"(tensor<real32> erase_shape(tensor<real32> value)
    return value
tensor<real32><1, 3> one = tensor.ones<real32>([1, 3])
auto unknown = erase_shape(tensor.zeros<real32>([4, 3]))
tensor<real32><4, 3> checked_at_runtime = one + unknown
)", "shape.constraint");
 // A 1 against an extent known to be 1 keeps the 1 in the result's source
 // pattern, whether the right operand's 1 comes from its pattern, a `_`
 // pattern plus flow facts, flow facts alone or a constructor, so the static
 // type stays identical to the left operand's wherever reference arguments,
 // generic inference or array literals require it.
 good(R"(void bump(tensor<real32><1, 3> &x)
    x = x * real32(2)
real32 first(const tensor<real32><1, 3> &x)
    return x[0, 0].item()
T pick<T>(T first_value, T second_value)
    return first_value
tensor<real32><1, 3> a = tensor.ones<real32>([1, 3])
tensor<real32><1, 3> patterned = tensor.ones<real32>([1, 3])
tensor<real32><_, 3> wildcard = tensor.ones<real32>([1, 3])
tensor<real32> flow = tensor.ones<real32>([1, 3])
auto from_pattern = a + patterned
bump(&from_pattern)
auto from_wildcard = a - wildcard
bump(&from_wildcard)
auto from_flow = a * flow
bump(&from_flow)
auto from_constructor = a / tensor.ones<real32>([1, 3])
bump(&from_constructor)
real32 value = first(&from_flow)
auto picked = pick(a + flow, a)
auto items = [a + flow, a]
)");
 // Against an unknown extent the result axis is a wildcard, so its type is
 // no longer the left operand's: a reference argument needs a binding
 // declared with the pattern, which checks the extent at runtime.
 bad_message(R"(tensor<real32> erase_shape(tensor<real32> value)
    return value
void bump(tensor<real32><1, 3> &x)
    x = x * real32(2)
tensor<real32><1, 3> one = tensor.ones<real32>([1, 3])
auto unknown = erase_shape(tensor.ones<real32>([1, 3]))
auto result = one + unknown
bump(&result)
)", "TYPE_MISMATCH", "Reference arguments require identical declared types");
 good(R"(tensor<real32> erase_shape(tensor<real32> value)
    return value
void bump(tensor<real32><1, 3> &x)
    x = x * real32(2)
tensor<real32><1, 3> one = tensor.ones<real32>([1, 3])
auto unknown = erase_shape(tensor.ones<real32>([1, 3]))
tensor<real32><1, 3> result = one + unknown
bump(&result)
)");
 // The wildcard also keeps facts derived from the result honest: keeping
 // the left operand's 1 there would make the transposed result [3, 1] and
 // reject these bindings, which hold for a [2, 3] or [0, 3] right operand.
 // Each one is checked at runtime instead.
 good(R"(tensor<real32> erase_shape(tensor<real32> value)
    return value
tensor<real32><1, 3> one = tensor.ones<real32>([1, 3])
auto unknown = erase_shape(tensor.ones<real32>([2, 3]))
auto turned = (one + unknown).transpose(0, 1)
tensor<real32><3, 2> wide = turned
tensor<real32><3, 0> empty = turned
tensor<real32><3, 1> narrow = turned
)");
 ir_contains(R"(tensor<real32> erase_shape(tensor<real32> value)
    return value
tensor<real32><1, 3> one = tensor.ones<real32>([1, 3])
auto unknown = erase_shape(tensor.ones<real32>([0, 3]))
tensor<real32><3, 0> empty = (one + unknown).transpose(0, 1)
)", "shape.constraint");
 good(R"(T identity<T>(T value)
    return value
int integer_value = 1
real64 float_value = 1.0
int integer_result = identity(integer_value)
real64 float_result = identity(float_value)
)");
 bad_code("tensor<real32><2, 3> value = tensor.ones<real32>([2, 3])\nint[3] wrong_shape = value.shape()\n", "TYPE_MISMATCH");
 good(R"(tensor<real32> erase(tensor<real32> value)
    return value
tensor<real32><2, _> known = erase(tensor.zeros<real32>([2, 2]))
)");
 bad_code("tensor<real32> value = tensor.ones<real32>([1])\nreal32 scalar = value.item()\n", "TYPE_MISMATCH");
 bad_code("tensor<real32><2, 2> value = tensor.ones<real32>([2, 2])\nauto bad = value[0, 0, 0]\n", "INDEX_ARITY");

 // Shape-pattern match cases select only exact-rank compatible tensor alternatives.
 good(R"(void classify(tensor<real32><3, 4> | tensor<real32><1, 4> value)
    match value
        tensor<real32><3, _> rgb
            print(rgb.shape()[0])
            print(NL)
        tensor<real32><1, _> gray
            print(gray.shape()[0])
            print(NL)
)");
 bad_code(R"(void invalid_case(tensor<real32><1, 4> | tensor<real32><4, 4> value)
    match value
        tensor<real32><3, _> impossible
            print(impossible.shape()[0])
            print(NL)
        tensor<real32><1, _> gray
            print(gray.shape()[0])
            print(NL)
        tensor<real32><4, _> rgba
            print(rgba.shape()[0])
            print(NL)
)", "MATCH_CASE");

 // Tracking preserves the tensor's exact shape contract.
 good(R"(tensor<real32> source = tensor.ones<real32>([3, 8, 8])
tensor<real32><3, _, _> value = source.track()
tensor<real32><3, _, _> restored = value.untrack()
)");
 good(R"(tensor<real64> source = tensor.ones<real64>([2, 4])
tensor<real64><2, _> value = source.track()
tensor<real64><2, _> restored = value.untrack()
)");
 bad_code(R"(tensor<real32> source = tensor.ones<real32>([3, 8, 8])
tensor<real32><3, _> wrong = source.track()
)", "TYPE_MISMATCH");

 // Dependent shape expressions in function signatures are checked at the boundary.
 good(R"(tensor<real64><n, 2> keep_shape(int n, tensor<real64><n, 2> value)
    return value
tensor<real64> unknown = tensor.ones<real64>([3, 2])
tensor<real64><3, 2> checked = keep_shape(3, unknown)
)");
 good(R"(int[][n] keep_rows(int n, int[][n] rows)
    return rows
int[][] dynamic_rows = [[1, 2], [3, 4]]
int[][] checked_rows = keep_rows(2, dynamic_rows)
)");

 // Runtime extent expressions are captured per binding; mutable sources remain legal.
 good(R"(int n = 3
int m = 4
tensor<real64><n * 2 + 1, 224> captured = tensor.ones<real64>([7, 224])
n = 10
tensor<real64><n, 224> later = tensor.ones<real64>([10, 224])
real64[n * m] dynamic_fixed
dynamic_fixed[0] = 1.0
tensor<real64><3, 224> contextual = tensor.zeros()
tensor<real64><_, 224> explicit_shape = tensor.zeros([3, 224])
)");
 bad_code(R"(int n = 3
tensor<real64><n, 224> wrong = tensor.ones<real64>([3, 224, 1])
)", "TYPE_MISMATCH");

 // Numeric container casts preserve array structure and tensor shape facts.
 good(R"(int[][] values = [[1, 2], [3, 4]]
real64[][] converted = real64(values)
int[2][2] fixed = [[1, 2], [3, 4]]
real64[2][2] fixed_converted = real64(fixed)
tensor<int64><2, 2> matrix = tensor.ones<int64>([2, 2])
tensor<real64><2, 2> tensor_converted = real64(matrix)
tensor<real32><2, 2> tracked_source = tensor.ones<real32>([2, 2])
tensor<real32><2, 2> tracked = tracked_source.track()
tensor<real64><2, 2> tracked_converted = real64(tracked.untrack())
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
tensor<int64><2, 2> matrix = tensor.ones<int64>([2, 2])
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
 llvm_contains(R"(tensor<int64> values = tensor.ones<int64>([2])
auto | error narrowed = int8(values)
match narrowed
    tensor<int8> converted
        print(converted[0].item())
        print(NL)
    error problem
        print(problem)
        print(NL)
)", "@quidra_tensor_try_cast");
 bad_code("int[] values = [1, 2]\nreal64[] converted = values\n", "TYPE_MISMATCH");
 bad_code("real64[] values = [1.0, 2.0]\nint[] converted = int(values)\n", "NUMERIC_CAST");
 bad_code("tensor<real64> values = tensor.ones<real64>([2])\nauto converted = int(values)\n", "NUMERIC_CAST");
 bad_code("tensor<int64> values = tensor.ones<int64>([2])\nauto converted = values.cast<real64>()\n", "UNKNOWN_MEMBER");

 // Tensor flow facts weaken at joins. A later exact-shape binding accepts the
 // unknown fact set and emits a runtime constraint check.
 good(R"(void branch_shape(bool flag)
    tensor<real32> value = tensor.zeros<real32>([3, 4])
    if flag
        value = tensor.zeros<real32>([3, 5])
    tensor<real32><3, 4> exact_shape = value
)");
 bad_code(R"(void branch_rank(bool flag)
    tensor<real32> value = tensor.zeros<real32>([2, 2])
    if flag
        value = tensor.zeros<real32>([2, 2, 2])
    int[2] dimensions = value.shape()
)", "TYPE_MISMATCH");
 bad_code(R"(void while_rank(bool flag)
    tensor<real32> value = tensor.zeros<real32>([2, 2])
    while flag
        value = tensor.zeros<real32>([2, 2, 2])
        flag = false
    int[2] dimensions = value.shape()
)", "TYPE_MISMATCH");
 bad_code(R"(void for_rank(int[] items)
    tensor<real32> value = tensor.zeros<real32>([2, 2])
    for item in items
        value = tensor.zeros<real32>([2, 2, 2])
    int[2] dimensions = value.shape()
)", "TYPE_MISMATCH");
 bad_code(R"(void match_rank(int | string choice)
    tensor<real32> value = tensor.zeros<real32>([2, 2])
    match choice
        int
            value = tensor.zeros<real32>([2, 2, 2])
        string
            value = tensor.zeros<real32>([2, 2])
    int[2] dimensions = value.shape()
)", "TYPE_MISMATCH");

 // Device placement is runtime/compiler metadata, not part of the nominal tensor type.
 good(R"(tensor<real32> cpu = tensor.zeros<real32>([2])
tensor<real32> direct_gpu = tensor.zeros<real32>([2], gpu = 0)
tensor<real32><2> contextual_gpu = tensor.ones(gpu = 1)
tensor<real32> copied = cpu.gpu(0)
tensor<real32> returned = copied.cpu()
)");
 good("gpu.sync(0)\ngpu.sync(index = 1)\n");
 bad_message("gpu.sync(-1)\n",
             "NUMERIC_FAMILY", "A negative integer literal cannot materialize as nat.");
 bad_code("gpu.sync()\n", "ARGUMENT_MISMATCH");
 bad_code("gpu.sync(0, 1)\n", "ARGUMENT_MISMATCH");
 bad_code("gpu.sync(1.0)\n", "NUMERIC_FAMILY");
 llvm_contains("gpu.sync(0)\n", "@quidra_gpu_sync");
 bad_message("auto value = tensor.zeros<real32>([1], gpu = -1)\n",
             "NUMERIC_FAMILY", "A negative integer literal cannot materialize as nat.");
 bad_message("auto value = tensor.zeros<real32>([1], gpu = 1.5)\n",
             "NUMERIC_FAMILY", "A real literal cannot materialize as nat.");
 bad_code("auto value = tensor.zeros<real32>([1], device = 0)\n",
          "ARGUMENT_MISMATCH");
 bad_message("auto value = tensor.zeros<real32>([1])\nauto moved = value.gpu()\n",
             "ARGUMENT_MISMATCH", "requires exactly one positional integer GPU index");
 bad_message("auto value = tensor.zeros<real32>([1])\nauto moved = value.gpu(-1)\n",
             "NUMERIC_FAMILY", "A negative integer literal cannot materialize as nat.");
 bad_message("auto value = tensor.zeros<real32>([1])\nauto moved = value.cpu(0)\n",
             "ARGUMENT_MISMATCH", "takes no arguments");

 // Explicit transfer remains an observable IR boundary; it must not be folded
 // into the preceding allocation or relocate earlier arithmetic.
 const std::string explicit_gpu_transfer = R"(tensor<real32> source = tensor.zeros<real32>([2])
tensor<real32> moved = source.gpu(0)
)";
 ir_contains(explicit_gpu_transfer, "tensor.create");
 ir_contains(explicit_gpu_transfer, " cpu");
 ir_contains(explicit_gpu_transfer, "tensor.gpu");
 const std::string post_compute_transfer = R"(tensor<real32> left = tensor.ones<real32>([2])
tensor<real32> right = tensor.ones<real32>([2])
tensor<real32> sum = left + right
tensor<real32> moved = sum.gpu(0)
)";
 ir_contains(post_compute_transfer, "tensor.binary");
 ir_contains(post_compute_transfer, "tensor.gpu");
 llvm_contains(
     "auto value = tensor.zeros<real32>([1], gpu = 0)\n",
     "@quidra_tensor_create");
 llvm_contains(
     "auto value = tensor.zeros<real32>([1])\nauto moved = value.gpu(0)\n",
     "@quidra_tensor_to_gpu");
 llvm_contains(
     "auto value = tensor.zeros<real32>([1])\nauto moved = value.cpu()\n",
     "@quidra_tensor_to_cpu");

 // Loop-carried tensor facts must be weakened before checking the loop body.
 good(R"(void loop_backedge(bool flag)
    tensor<real32> value = tensor.zeros<real32>([2, 2])
    while flag
        auto product = value + value
        value = tensor.zeros<real32>([2, 2, 2])
        flag = false
)");
 good(R"(void for_backedge(int[] items)
    tensor<real32> value = tensor.zeros<real32>([2, 2])
    for item in items
        auto product = value + value
        value = tensor.zeros<real32>([2, 2, 2])
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
 std::string postfix = "class P\n    int v\n\n    construct(int start)\n        this.v = start\n\n    P grow()\n        return P(this.v + 1)\n\n"
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
