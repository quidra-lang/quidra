#include "quidra/llvm_backend.hpp"
#include "quidra/abi/symbols.hpp"
#include "quidra/compile_inputs.hpp"
#include "quidra/compiler.hpp"
#include "quidra/formatter.hpp"
#include "quidra/frontend.hpp"
#include "quidra/ir.hpp"
#include "quidra/ir/dtype.hpp"
#include "quidra/lowering.hpp"
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
static void string_templates_release_owned_inputs() {
    using namespace quidra;
    struct Case {
        const char* expression;
        TypeKind kind;
        bool formatted;
        bool owned;
    };
    const Case cases[] = {
        {"x ^ half", TypeKind::Real, false, true},
        {"x ^ half:frac=2", TypeKind::Real, true, true},
        {"z - z", TypeKind::Com, false, true},
        {"real32(x)", TypeKind::Union, false, true},
        {"x", TypeKind::Real, false, false},
        {"x:frac=2", TypeKind::Real, true, false},
        {"z", TypeKind::Com, false, false},
    };
    for (const auto& test : cases) {
        const std::string source =
            "real x = 2.0\nreal half = 0.5\ncom z = 0.5 + 0.25i\n"
            "string text = \"{" + std::string(test.expression) + "}\"\n";
        capture_source("string_template_ownership", source);
        const auto module = ir::lower(quidra::check(source));
        bool found = false;
        for (const auto& function : module.functions) {
            if (!function.entrypoint) continue;
            ir::ValueId input = 0;
            std::size_t converted_at = 0;
            std::size_t position = 0;
            for (const auto& block : function.blocks) {
                for (const auto& instruction : block.instructions) {
                    ++position;
                    if (const auto* text = std::get_if<ir::ToString>(&instruction);
                        text && !test.formatted && text->source_type.kind == test.kind) {
                        input = text->value;
                        converted_at = position;
                    } else if (const auto* format = std::get_if<ir::FormatNumber>(&instruction);
                               format && test.formatted && format->source_type.kind == test.kind) {
                        input = format->value;
                        converted_at = position;
                    } else if (const auto* tag = std::get_if<ir::VariantTag>(&instruction);
                               tag && test.kind == TypeKind::Union) {
                        input = tag->container;
                        converted_at = position;
                    }
                }
            }
            if (!input) continue;
            found = true;
            position = 0;
            std::size_t releases = 0;
            for (const auto& block : function.blocks) {
                for (const auto& instruction : block.instructions) {
                    ++position;
                    if (const auto* release = std::get_if<ir::Release>(&instruction);
                        release && release->value == input && position > converted_at) {
                        ++releases;
                    }
                }
            }
            if (releases == static_cast<std::size_t>(test.owned)) continue;
            std::cerr << "string template input has " << releases << " releases: "
                      << test.expression << "\n" << ir::dump(module);
            std::exit(1);
        }
        if (!found) {
            std::cerr << "string template conversion missing: " << test.expression << "\n";
            std::exit(1);
        }
    }
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
              "    for 0 <= nat input_width < 3\n"
              "        total = total + int(input_width)\n"
              "    match int.parse(\"7\")\n"
              "        int parsed\n"
              "            total = total + parsed\n"
              "        error problem\n"
              "            return 0\n"
              "    return input_height + total + top\n"
              "\n"
              "if main\n"
              "    int input_height = 6\n"
              "    for 0 <= nat input_width < 2\n"
              "        print(\"{input_height + int(input_width)}{NL}\")\n");
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
              "class Box()\n"
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
              "class Frame this(local int h)\n"
              "    int height\n"
              "\n"
              "\n"
              "    this.height = h\n"
              "\n"
              "    int area(int width)\n"
              "        return this.height * width\n"
              "\n"
              "    int depth()\n"
              "        return 1\n");
        write("main.qui",
              "import lib = \"./lib.qui\"\n"
              "\n"
              "class height()\n"
              "    int v\n"
              "\n"
              "class model()\n"
              "    int v\n"
              "\n"
              "class depth()\n"
              "    int v\n"
              "\n"
              "enum width\n"
              "    narrow\n"
              "    wide\n"
              "\n"
              "model m = model()\n"
              "m.v = 1\n"
              "lib.Frame f = lib.Frame(2)\n"
              "int total = lib.scale(2, 3) + lib.generic_scale<model>(m, 4) + f.area(4) + f.depth()\n");
        (void)quidra::compile_file(root / "main.qui", {}, root);

        // Language-owned members (time.Duration.seconds())
        // never conflict with root user classes.
        write("std.qui",
              "class seconds()\n"
              "    int v\n"
              "\n"
              "class value()\n"
              "    int v\n"
              "\n"
              "time.Duration pause = time.seconds(0.5)\n"
              "real64 total = pause.seconds() + 3.0\n");
        (void)quidra::compile_file(root / "std.qui", {}, root);

        // A module's own classes stay reserved for its parameters, method
        // parameters, fields, and method names, whether it is compiled on its
        // own or imported.
        write("own_parameter.qui",
              "class shape()\n"
              "    int v\n"
              "\n"
              "int rank_of(int[] shape)\n"
              "    return 2\n");
        write("own_method_parameter.qui",
              "enum shape\n"
              "    flat\n"
              "\n"
              "class Frame()\n"
              "    int height\n"
              "\n"
              "    int rank_of(int[] shape)\n"
              "        return 2\n");
        write("own_field.qui",
              "class shape()\n"
              "    int v\n"
              "\n"
              "class Frame()\n"
              "    int shape\n");
        write("own_method.qui",
              "class shape()\n"
              "    int v\n"
              "\n"
              "class Frame()\n"
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
              "class Holder<M>()\n"
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
              "class T()\n"
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
              "class T()\n"
              "    int v\n"
              "\n"
              "class K()\n"
              "    int v\n"
              "\n"
              "enum V\n"
              "    low\n"
              "    high\n"
              "\n"
              "lib.Holder<int> holder = lib.Holder<int>()\n"
              "holder.value = 4\n"
              "int[] values = [1, 2]\n"
              "string key = \"k\"\n"
              "real64 scale = 2.0\n"
              "map.Map<string, int> counts = map.Map<string, int>()\n"
              "int total = lib.first(values, 2) + lib.pick(key, scale) + outer.through(5) + holder.value + M()\n");
        (void)quidra::compile_file(root / "main.qui", {}, root);

        write("own_function.qui",
              "class T()\n"
              "    int v\n"
              "\n"
              "int first<T>(T value)\n"
              "    return 1\n");
        write("own_class.qui",
              "int M()\n"
              "    return 1\n"
              "\n"
              "class Holder<M>()\n"
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
              "    func<int>() f = helper\n"
              "    return f()\n"
              "\n"
              "int use_generic<T>(T value)\n"
              "    func<int>() f = helper\n"
              "    return f()\n"
              "\n"
              "class Box this()\n"
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
              "lib.Box b = lib.Box()\n"
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
        standard_class::file_handle,
        standard_class::autograd_target, standard_class::time_instant,
        standard_class::time_duration,  standard_class::process_result,
        standard_class::random_generator, standard_class::map,
        standard_class::set,
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
    bad_message("class C(local int _)\n"
                "    int x\n"
                "\n"
                "\n"
                "    x = 1\n", "DISCARD", declared);
    bad_message("class C()\n"
                "    int _\n", "DISCARD", declared);
    bad_message("class C()\n"
                "    int x\n"
                "\n"
                "    int _()\n"
                "        return 1\n", "DISCARD", declared);
    bad_message("class C()\n"
                "    int x\n"
                "\n"
                "    int get(int _)\n"
                "        return x\n", "DISCARD", declared);
    bad_message("int _()\n    return 1\n", "DISCARD", declared);
    bad_message("class _()\n"
                "    int x\n", "DISCARD", declared);
    bad_message("class Box<_>()\n"
                "    int x\n", "DISCARD", declared);
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
    good("int[] values = [1, 2]\nfor _ = values[]\n    print(int(1))\n");
    bad_message("int | none value = 1\nmatch value\n    int _\n        print(int(1))\n    none\n        print(int(2))\n",
                "DISCARD", declared);
    bad_message("int x = 1\nint y = _ + x\n", "DISCARD", read);
    bad_message("print(_)\n", "DISCARD", read);
    bad_message("void f(int v)\n    print(v)\n\nf(_)\n", "DISCARD", read);
    bad_message("void f(int &v)\n    v = 1\n\nf(&_)\n", "DISCARD", read);
    bad_message("print(\"{_}\")\n", "DISCARD", read);
    bad_message("_ = 1\n", "DISCARD", read);
    bad_message("_ += 1\n", "DISCARD", read);
    bad_message("int x = 1\n&_ = &x\n", "DISCARD", read);
    bad_message("class C()\n"
                "    int x\n"
                "\n"
                "    int get()\n"
                "        return _\n", "DISCARD", read);
    // The tensor shape wildcard keeps its meaning.
    good("tensor<real32>{2, _} grid = tensor.zeros<real32>([2, 3])\nprint(grid.shape()[1])\nprint(NL)\n");
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
    bad_message("class __quidra_gc__std_box()\n"
                "    int value\n"
                "\n"
                "__quidra_gc__std_box b = __quidra_gc__std_box()\n",
                "SHADOWING", reserved);
    // A user generic class whose instance is mangled like a standard one is
    // still a user class: its required class header cannot be omitted,
    // exactly as for Box<T>.
    bad_code("class Box<T>\n"
             "    T value\n"
             "\n"
             "Box<int> b = Box<int>()\n", "CONSTRUCTOR_SIGNATURE");
    bad_code("class _std_box<T>\n"
             "    T value\n"
             "\n"
             "_std_box<int> b = _std_box<int>()\n",
             "CONSTRUCTOR_SIGNATURE");
    // Fields of a user class named like map.Map's instances are user fields,
    // readable like any other.
    good("class _std_map_Map<K, V> this(local K key, local V value)\n"
         "    K __key\n"
         "    V __value\n"
         "\n"
         "\n"
         "    this.__key = key\n"
         "    this.__value = value\n"
         "\n"
         "_std_map_Map<int, string> entry = _std_map_Map<int, string>(1, \"one\")\n"
         "print(entry.__value)\n"
         "print(NL)\n");
    // A standard collection's internals stay hidden.
    bad_code("map.Map<int, int> values = map.Map<int, int>()\nprint(len(values.__keys))\n",
             "UNKNOWN_MEMBER");
    // Every kind of declaration rejects the compiler's names.
    bad_message("int __quidra_value = 1\n", "SHADOWING", reserved);
    bad_message("void f()\n    int __quidra_value = 1\n", "SHADOWING", reserved);
    bad_message("int __quidra_f()\n    return 1\n", "SHADOWING", reserved);
    bad_message("int f(int __quidra_v)\n    return 1\n", "SHADOWING", reserved);
    bad_message("class C()\n"
                "    int __quidra_x\n", "SHADOWING", reserved);
    bad_message("class C()\n"
                "    int x\n"
                "\n"
                "    int __quidra_get()\n"
                "        return x\n",
                "SHADOWING", reserved);
    bad_message("T f<__quidra_T>(int v)\n    return v\n", "SHADOWING", reserved);
    bad_message("enum __quidra_E\n    A\n", "SHADOWING", reserved);
    bad_message("int[] values = [1, 2]\nfor auto __quidra_v = values[]\n    print(int(1))\n",
                "SHADOWING", reserved);
    const std::string lib = "int value()\n    return 1\n";
    if (imported_check_code("import __quidra_lib = \"./lib.qui\"\n", lib) != "SHADOWING" ||
        imported_check_code("import lib = \"./lib.qui\"\nprint(lib.value())\n",
                            "class __quidra_box()\n"
                            "    int v\n"
                            "\n" + lib) != "SHADOWING") {
        std::cerr << "a '__quidra_' import alias or imported declaration is not SHADOWING\n";
        std::exit(1);
    }
    // A name that merely contains the text is an ordinary name.
    good("int my__quidra_value = 1\nprint(my__quidra_value)\nprint(NL)\n");
}

// If expressions: typing with and without a contextual type, the
// parenthesis rules the parser enforces, references, and flow.
static void symbolic_tensor_extents() {
    using quidra::ExtentOrigin;
    const auto compiled = quidra::compile(R"(
tensor<real32>{_, 3} preserve(tensor<real32>{_, 3} x)
    return x.contiguous()
tensor<real32>{_, 3} combine(tensor<real32>{_, 3} a, tensor<real32>{_, 3} b)
    auto c = a + b
    return c + (b + a)
tensor<real32>{_, 3} choose_first(tensor<real32>{_, 3} x, tensor<real32>{_, 3} y)
    return x
tensor<real32>{_, 3} named(tensor<real32>{_, 3} a, tensor<real32>{_, 3} b)
    return choose_first(y = a, x = b)
tensor<real32>{_, 3} shape_roundtrip(tensor<real32>{_, 3} x)
    nat n = x.shape()[0]
    return tensor.zeros<real32>([n, 3])
tensor<real32>{_, 3} stable_loop(tensor<real32>{_, 3} x, bool more)
    while more
        x = x.contiguous()
        more = false
    return x
tensor<real32>{_, 3} growing_loop(tensor<real32>{_, 3} acc, tensor<real32>{_, 3} t, bool more)
    while more
        acc = acc + t
        more = false
    return acc
tensor<real32>{_, 3} equal_join(tensor<real32>{_, 3} a, tensor<real32>{_, 3} b, bool first)
    tensor<real32>{_, 3} result = a
    if first
        result = a + b
    else
        result = b + a
    return result
tensor<real32>{_, 3} different_join(tensor<real32>{_, 3} a, tensor<real32>{_, 3} b, bool first)
    tensor<real32>{_, 3} result = a
    if first
        result = a
    else
        result = b
    return result
tensor<real32>{_, 3} local_mix(tensor<real32>{_, 3} x, nat n)
    return x + tensor.zeros<real32>([n, 3])
tensor<real32>{_, 3} capture_version(nat n)
    tensor<real32>{n, 3} first = tensor.zeros<real32>([n, 3])
    n += 1
    tensor<real32>{n, 3} second = tensor.zeros<real32>([n, 3])
    return first + second
tensor<T>{_, 3} generic_preserve<T: numeric>(tensor<T>{_, 3} x)
    return x.contiguous()
tensor<real32>{_, 3} generic_call(tensor<real32>{_, 3} x)
    return generic_preserve<real32>(x)
)");
    const auto require = [](bool condition, const char* message) {
        if (condition) return;
        std::cerr << "symbolic extent failure: " << message << "\n";
        std::exit(1);
    };
    const auto& functions = compiled.checked.functions;
    const auto axis = [&](const char* name) -> const ExtentOrigin& {
        const auto& extents = functions.at(name).result_extents;
        require(extents.size() == 2, "rank-two result summary");
        require(extents[1].kind == ExtentOrigin::Kind::Known && extents[1].value == 3,
                "source pattern preserves known axes");
        return extents[0];
    };
    require(axis("preserve").kind == ExtentOrigin::Kind::ParameterAxis &&
                axis("preserve").parameter == 0, "shape-preserving method retains its input origin");
    require(axis("named").kind == ExtentOrigin::Kind::ParameterAxis &&
                axis("named").parameter == 1, "named arguments substitute parameter coordinates");
    require(axis("shape_roundtrip").kind == ExtentOrigin::Kind::ParameterAxis &&
                axis("shape_roundtrip").parameter == 0, "shape reads survive allocation round trips");
    const std::vector<std::pair<std::size_t, std::size_t>> expected{{0, 0}, {1, 0}};
    require(axis("combine").kind == ExtentOrigin::Kind::Broadcast &&
                axis("combine").axes == expected && !axis("combine").local,
            "nested broadcasts retain canonical parameter-axis origins");
    require(axis("equal_join") == axis("combine"), "equal canonical expressions survive CFG joins");
    require(axis("different_join").kind == ExtentOrigin::Kind::Unknown,
            "different input origins join without assuming inequality");
    require(axis("stable_loop").kind == ExtentOrigin::Kind::ParameterAxis,
            "shape-preserving loop retains its entry symbol");
    require(axis("growing_loop").kind == ExtentOrigin::Kind::Unknown,
            "accumulating broadcast reaches the loop merge fixed point");
    require(axis("local_mix").kind == ExtentOrigin::Kind::Broadcast &&
                axis("local_mix").axes == std::vector<std::pair<std::size_t, std::size_t>>{{0, 0}} &&
                axis("local_mix").local, "callee-local operands remain local in result summaries");
    require(axis("capture_version").kind == ExtentOrigin::Kind::Broadcast &&
                axis("capture_version").axes.empty() && axis("capture_version").local,
            "assignments separate successive scalar captures");
    require(axis("generic_call").kind == ExtentOrigin::Kind::ParameterAxis &&
                axis("generic_call").parameter == 0, "monomorphized callees retain their input origin");
}

// L17: shapes in braces. Literal and `_` axes have their meaning everywhere;
// in bindings and match cases every axis form does: `nat N` and
// `nat N = EXPR` declare read-only nat shape variables, a bare name refers to
// one or to an existing nat value, and any other expression is a nat
// constraint. A shape is one scope unit.
static void brace_shapes_and_shape_variables() {
    bad_message("tensor<real32><2, 3> values = tensor.zeros<real32>([2, 3])\n", "TENSOR_TYPE",
                "Tensor shapes are written in braces");

    good("tensor<real32>{2, 3} m = tensor.zeros<real32>([2, 3])\n");
    good("tensor<real32>{3, _, _} image\n");
    good("tensor<real32>{_, _} grid = tensor.zeros<real32>([2, 3])\n");
    good("tensor<real32>{2, 3, 5} a\n");
    good("tensor<real32> any_rank = tensor.zeros<real32>([4])\n");
    bad_code("tensor<real32>{3, _} wrong = tensor.zeros<real32>([2, 2])\n", "TYPE_MISMATCH");
    // A shape split across lines is one logical line.
    good("tensor<real32>{nat B,\n    nat C} x = tensor.zeros<real32>([2, 3])\nprint(B + C)\n");
    // Braces belong to shapes, and a tensor type names its element type.
    bad_code("tensor{2, 3} x = tensor.zeros<real32>([2, 3])\n", "TENSOR_TYPE");
    bad_message("int x = {3}\n", "PARSE_ERROR", "Braces appear only in tensor shapes.");
    bad_message("int{3} y = 1\n", "PARSE_ERROR", "Braces appear only in tensor shapes.");
    // Braced type arguments inside interpolations stay one expression.
    good("nat count<T>(T[] values)\n"
         "    return len(values)\n"
         "tensor<real32>[] xs = [tensor.zeros<real32>([2, 3])]\n"
         "print(\"{count<tensor<real32>{2, 3}>(xs)}{NL}\")\n"
         "print(\"{count<tensor<real32>{2}>(xs):int=3}{NL}\")\n");
    // Signatures take literal and `_` axes until they take shape variables.
    good("void f(tensor<real32>{2, _} x)\n    print(x.shape()[1])\n");
    good("void f(tensor<real32>{nat N} x)\n    print(N)\n");
    good("tensor<real32>{n} f(nat n)\n    return tensor.zeros<real32>([n])\n");

    // The canonical snippets (L17 REVISION 2 and L17 FINAL in its spelling).
    good("tensor<real32>{nat R, nat W} d = tensor.zeros<real32>([2, 3])\n"
         "print(\"{R} x {W}{NL}\")\n");
    good("nat n = 3\ntensor<real32>{n, _} rows = tensor.zeros<real32>([3, 7])\n");
    good("tensor<real32>{nat B, 3, nat H, nat W} rgb = tensor.zeros<real32>([1, 3, 4, 4])\n"
         "print(B * H * W)\n");
    good("nat N = 3\ntensor<real32>{N} v\n");
    good("tensor<real32>{nat B, nat C, nat H, nat W} x = tensor.zeros<real32>([1, 2, 3, 4])\n"
         "tensor<real32>{B, C, H, W} y = x\n");
    good("tensor<real32>{nat K, K} sq = tensor.zeros<real32>([2, 2])\n");
    good("nat rows = 4\nnat cols = 5\n"
         "tensor<real32>{nat H = rows * cols, nat W} img = tensor.zeros<real32>([20, 3])\n"
         "print(H + W)\n");
    bad_message("tensor<real32>{N, K} a\n", "SHAPE_VARIABLE",
                "'N' is not declared: write 'nat N' to declare a shape variable, or use an "
                "existing nat value.");
    bad_message("tensor<real32>{nat N, C} a = tensor.zeros<real32>([2, 3])\n", "SHAPE_VARIABLE",
                "'C' is not declared");
    bad_message("tensor<real32>{nat B, nat H} t\n", "SHAPE_VARIABLE",
                "'B' has no value: without an initializer, write a literal, an expression, "
                "'nat B = ...' or an existing nat value.");
    bad_message("int n = 3\ntensor<real32>{n} u = tensor.zeros<real32>([3])\n", "SHAPE_VARIABLE",
                "'n' is int; a bare axis name refers only to a nat value: write 'nat(n)'.");
    bad_message("tensor<real32>{nat C, nat H} x = tensor.zeros<real32>([2, 3])\n"
                "tensor<real32>{nat H = 10, nat W} z = tensor.zeros<real32>([10, 2])\n",
                "SHADOWING",
                "'H' already names a nat value; 'nat H' would declare a new shape variable. To "
                "require equality with it, write the bare name 'H'.");
    bad_message("tensor<real32>{nat N, nat N} t = tensor.zeros<real32>([2, 2])\n",
                "SHAPE_VARIABLE",
                "'N' is declared twice; declare it once with 'nat N' and write the bare 'N' for "
                "an equal axis.");
    bad_message("tensor<real32>{nat L} make()\n    return tensor.zeros<real32>([1])\n",
                "SHAPE_VARIABLE",
                "'L' in the result shape cannot be inferred: write 'nat L = EXPR', or "
                "declare it in a parameter with 'nat L'.");

    // A shape is one unit: order-free, `nat N = EXPR` in dependency order.
    good("tensor<real32>{nat M = 2 * N, nat N} x = tensor.zeros<real32>([4, 2])\nprint(M)\n");
    good("tensor<real32>{nat N, N + 1} x = tensor.zeros<real32>([2, 3])\n");
    bad_message("tensor<real32>{nat A = B + 1, nat B = A - 1} t = tensor.zeros<real32>([2, 1])\n",
                "SHAPE_VARIABLE", "'A' and 'B' are defined by each other.");
    bad_message("tensor<real32>{nat A = A + 1} t = tensor.zeros<real32>([2])\n",
                "SHAPE_VARIABLE", "'A' is defined by itself.");
    // Unnamed expressions are nat: `(n)` is an expression, `nat(n)` converts.
    good("nat n = 3\ntensor<real32>{(n)} x = tensor.zeros<real32>([3])\n");
    bad_message("int n = 3\ntensor<real32>{(n)} x = tensor.zeros<real32>([3])\n",
                "TYPE_MISMATCH", "A tensor axis is a nat value: write 'nat(n)'.");
    good("int n = 3\ntensor<real32>{nat(n), 2} x = tensor.zeros<real32>([3, 2])\n");
    good("int[] v = [1, 2]\ntensor<real32>{len(v)} x = tensor.zeros<real32>([2])\n");
    good("bool wide = true\ntensor<real32>{if wide then 4 else 2} x = tensor.zeros<real32>([4])\n");
    bad_message("tensor<real32>{int N} x = tensor.zeros<real32>([2])\n", "SHAPE_VARIABLE",
                "A shape variable is nat: write 'nat N'.");
    bad_message("tensor<real32>{auto N} x = tensor.zeros<real32>([2])\n", "SHAPE_VARIABLE",
                "A shape variable is nat: write 'nat N'.");
    bad_message("tensor<real32>{nat64 N} x = tensor.zeros<real32>([2])\n", "SHAPE_VARIABLE",
                "A shape variable is nat: write 'nat N'.");
    // Bare names refer only to nat values.
    bad_message("real64 r = 2.0\ntensor<real32>{r} x = tensor.zeros<real32>([2])\n",
                "SHAPE_VARIABLE", "'r' is not a nat value.");
    bad_message("nat64 w = 2\ntensor<real32>{w} x = tensor.zeros<real32>([2])\n",
                "SHAPE_VARIABLE", "'w' is nat64; a bare axis name refers only to a nat value");
    bad_message("nat f()\n    return 2\ntensor<real32>{f} x = tensor.zeros<real32>([2])\n",
                "SHAPE_VARIABLE", "'f' is not a nat value.");
    bad_message("tensor<real32>{FF} x = tensor.zeros<real32>([2])\n", "SHAPE_VARIABLE",
                "'FF' is a built-in text constant, not a nat value; choose another axis name.");
    bad_message("void f<T>(tensor<T> x)\n    tensor<T>{nat B, T} y = x\n", "SHAPE_VARIABLE",
                "'T' is a type parameter, not a nat value; choose another axis name.");
    // `nat N` over a visible name is SHADOWING, with the hint that fits.
    bad_message("int H = 3\ntensor<real32>{nat H} x = tensor.zeros<real32>([3])\n",
                "SHADOWING",
                "'H' already names an int binding; 'nat H' would declare a new shape variable. "
                "To require equality with its value, write 'nat(H)'.");
    bad_message("real64 H = 3.0\ntensor<real32>{nat H} x = tensor.zeros<real32>([3])\n",
                "SHADOWING", "'H' already names a real64 binding; 'nat H' would declare a new "
                             "shape variable. Choose another name.");
    bad_message("nat H()\n    return 3\ntensor<real32>{nat H} x = tensor.zeros<real32>([3])\n",
                "SHADOWING", "'H' already names a function");
    // `_` binds nothing and is never read.
    bad_code("tensor<real32>{nat _} x = tensor.zeros<real32>([3])\n", "DISCARD");
    bad_code("tensor<real32>{nat _ = 3} x = tensor.zeros<real32>([3])\n", "DISCARD");
    bad_code("tensor<real32>{_, _ + 1} x = tensor.zeros<real32>([2, 3])\n", "DISCARD");
    // Shape variables are read-only nat values.
    bad_message("tensor<real32>{nat N} x = tensor.zeros<real32>([3])\nN = 4\n",
                "INVALID_ASSIGNMENT", "Shape variable 'N' is read-only.");
    bad_message("tensor<real32>{nat N} x = tensor.zeros<real32>([3])\nN += 1\n",
                "INVALID_ASSIGNMENT", "Shape variable 'N' is read-only.");
    bad_message("void grow(nat &n)\n    n += 1\n"
                "tensor<real32>{nat N} x = tensor.zeros<real32>([3])\ngrow(&N)\n",
                "INVALID_ASSIGNMENT", "Shape variable 'N' is read-only.");
    bad_code("tensor<real32>{nat N} x = tensor.zeros<real32>([3])\nint i = N\n",
             "TYPE_MISMATCH");
    good("tensor<real32>{nat N} x = tensor.zeros<real32>([3])\nint i = int(N)\n");
    // A repeat whose extents are known contradicts at compile time.
    bad_message("tensor<real32>{nat K, K} sq = tensor.zeros<real32>([2, 3])\n", "TENSOR_SHAPE",
                "Shape variable 'K' is 2, but axis 1 of the initializer is 3.");
    // A variable declared in a match case belongs to its body; a case with
    // an equality axis covers nothing.
    good("tensor<real32> make(nat a, nat b)\n"
         "    return tensor.zeros<real32>([a, b])\n"
         "tensor<real32> | none value = make(2, 2)\n"
         "match value\n"
         "    tensor<real32>{nat K, K} square\n"
         "        print(K)\n"
         "    tensor<real32> other\n"
         "        print(int(0))\n"
         "    none\n"
         "        print(int(1))\n");
    bad_code("tensor<real32> | none value = tensor.zeros<real32>([2, 2])\n"
             "match value\n"
             "    tensor<real32>{nat K, K} square\n"
             "        print(K)\n"
             "    none\n"
             "        print(int(1))\n",
             "MATCH_EXHAUSTIVE");
    good("tensor<real32>{3, _} | none value = tensor.zeros<real32>([3, 4])\n"
         "match value\n"
         "    tensor<real32>{3, nat W} rgb\n"
         "        print(W)\n"
         "    none\n"
         "        print(int(1))\n");

    // Array extents are whole expressions; nat(e) captures e.
    good("int n = 3\nint[nat(n)] values\n");
    good("int[] v = [1, 2]\nint[len(v)] copy\n");
    bad_message("int n = 3\nint[n] values\n", "TYPE_MISMATCH",
                "An array extent is a nat value");
}

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
    good("class P this(local int value)\n"
         "    int v\n"
         "\n"
         "\n"
         "    this.v = value\n"
         "\n" + ready +
         "P p = P(1)\nP q = P(2)\nP r = if ready then p else q\nprint(r.v)\nprint(NL)\n"
         "bool same = (if ready then p else q) == p\nprint(same)\nprint(NL)\n");
    good(ready + "tensor<real32> t = tensor.zeros<real32>([2])\n"
                 "tensor<real32> u = tensor.ones<real32>([2])\n"
                 "tensor<real32> w = if ready then t else u\nprint(w[0].item())\nprint(NL)\n");
    good(ready + "int | none v = if ready then 1 else none\n"
                 "match v\n    int value\n        print(value)\n    none\n        print(\"none\")\n");
    // Whole-expression positions.
    good("int pick(bool c, int a, int b = if true then 1 else 2)\n    return if c then a elif a > b then b else a + b\n\n" +
         ready + "print(pick(ready, if ready then 3 else 4))\n"
                 "print(NL)\n"
                 "int[] v = [if ready then 1 else 2, 3]\n"
                 "print(v[if ready then 0 else 1])\n"
                 "print(NL)\n"
                 "int total = 0\n"
                 "total += if ready then 1 else 2\n"
                 "print(\"{if ready then total else 0}\")\n"
                 "print(NL)\n"
                 "class Box()\n"
                 "    int v = if true then 1 else 2\n");
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

// Checks main.qui beside the given module files and returns the first
// diagnostic as "CODE: message", or "" when the program is accepted.
static std::string modules_diagnostic(const std::string& main,
                                      const std::vector<std::pair<std::string, std::string>>& files) {
    const auto root = std::filesystem::temp_directory_path() / "quidra-private-declarations";
    std::string result;
    try {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        for (const auto& [name, text] : files) std::ofstream(root / name, std::ios::binary) << text;
        (void)quidra::check_file_source(root / "main.qui", main, {}, root);
    } catch (const quidra::CompileErrors& errors) {
        if (!errors.diagnostics().empty())
            result = errors.diagnostics().front().code + ": " + errors.diagnostics().front().message;
    } catch (const quidra::CompileError& error) {
        result = error.diagnostic().code + ": " + error.diagnostic().message;
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    return result;
}

// Module-private top-level declarations (L3): usable only inside their
// module, never exported, never exposed by a public declaration.
static void private_declarations_stay_in_their_module() {
    const auto expect = [](const std::string& actual, const std::string& expected) {
        if (actual.rfind(expected, 0) != 0) {
            std::cerr << "private declarations: expected '" << expected << "', got '" << actual
                      << "'\n";
            std::exit(1);
        }
    };
    const std::string lib =
        "private int helper()\n"
        "    return 7\n"
        "\n"
        "int use()\n"
        "    return helper()\n"
        "\n"
        "private class Shape()\n"
        "    int sides\n"
        "\n"
        "private enum Color\n"
        "    Red\n"
        "    Blue\n"
        "\n"
        "private const int LIMIT = 3\n"
        "const int SIZE = 4\n"
        "\n"
        "T pass<T>(T value)\n"
        "    print(helper())\n"
        "    print(NL)\n"
        "    return value\n";
    const std::vector<std::pair<std::string, std::string>> files{{"lib.qui", lib}};
    const std::string head = "import lib = \"./lib.qui\"\n";
    // The module itself and generic instances for an importer use them.
    expect(modules_diagnostic(head + "print(lib.use())\nprint(lib.pass(1))\nprint(lib.SIZE)\nprint(NL)\n",
                              files),
           "");
    // An importer cannot name them.
    expect(modules_diagnostic(head + "print(lib.helper())\n", files),
           "PRIVATE_DECLARATION: 'helper' is private to module 'lib'.");
    expect(modules_diagnostic(head + "func<int>() f = lib.helper\n", files),
           "PRIVATE_DECLARATION: 'helper' is private to module 'lib'.");
    expect(modules_diagnostic(head + "lib.Shape s\n", files),
           "PRIVATE_DECLARATION: 'Shape' is private to module 'lib'.");
    expect(modules_diagnostic(head + "auto s = lib.Shape()\n", files),
           "PRIVATE_DECLARATION: 'Shape' is private to module 'lib'.");
    expect(modules_diagnostic(head + "lib.Color c = lib.Color.Red\n", files),
           "PRIVATE_DECLARATION: 'Color' is private to module 'lib'.");
    expect(modules_diagnostic(head + "print(lib.LIMIT)\n", files),
           "PRIVATE_DECLARATION: 'LIMIT' is private to module 'lib'.");
    // `public import` re-exports only the public declarations.
    const std::vector<std::pair<std::string, std::string>> nested{
        {"lib.qui", lib}, {"outer.qui", "public import inner = \"./lib.qui\"\n"}};
    const std::string outer = "import outer = \"./outer.qui\"\n";
    expect(modules_diagnostic(outer + "print(outer.inner.use())\nprint(NL)\n", nested), "");
    expect(modules_diagnostic(outer + "print(outer.inner.helper())\n", nested),
           "PRIVATE_DECLARATION: 'helper' is private to module 'outer.inner'.");
    // No public declaration exposes a private type.
    const std::string shape = "private class Shape()\n"
                              "    int sides\n"
                              "\n";
    bad_message(shape + "Shape make()\n    Shape s = Shape()\n    s.sides = 3\n    return s\n",
                "PRIVATE_DECLARATION", "Public function 'make' exposes private class 'Shape'.");
    bad_message(shape + "int sides(Shape s)\n    return s.sides\n", "PRIVATE_DECLARATION",
                "Public function 'sides' exposes private class 'Shape'.");
    bad_message(shape + "class Drawing()\n"
                        "    Shape[] shapes\n", "PRIVATE_DECLARATION",
                "Public field 'shapes' of class 'Drawing' exposes private class 'Shape'.");
    bad_message("private enum Color\n    Red\n\nenum Paint\n    Solid(Color)\n", "PRIVATE_DECLARATION",
                "Public enum 'Paint' exposes private enum 'Color'.");
    bad_message("private enum Color\n    Red\n\nconst Color DEFAULT = Color.Red\n", "PRIVATE_DECLARATION",
                "Public constant 'DEFAULT' exposes private enum 'Color'.");
    good(shape + "private Shape make()\n"
                 "    Shape s = Shape()\n"
                 "    s.sides = 3\n"
                 "    return s\n"
                 "\n"
                 "class Drawing()\n"
                 "    private Shape[] shapes\n"
                 "\n"
                 "print(make().sides)\n"
                 "print(NL)\n");
    // Prototypes agree on privacy.
    bad_message("private int f();\n\nint f()\n    return 1\n", "PROTOTYPE_MISMATCH", "agree on 'private'");
    bad_message("private class Box();\n"
                "\n"
                "class Box()\n"
                "    int v\n", "PROTOTYPE_MISMATCH", "agree on 'private'");
    good("private int f();\n\nprint(f())\nprint(NL)\n\nprivate int f()\n    return 1\n");
    // `private` applies to declarations only.
    bad_message("private import lib = \"./lib.qui\"\n", "PARSE_ERROR", "private applies to declarations.");
    bad_message("private int x = 3\n", "PARSE_ERROR", "private applies to declarations.");
    bad_message("private print(1)\n", "PARSE_ERROR", "private applies to declarations.");
    bad_message("private export \"C\" int64 f(int64 x)\n    return x\n", "FFI_EXPORT",
                "Exported C functions are public");
    good("private extern int32 c_abs(int32 value) = \"abs\"\nprint(c_abs(int32(-3)))\nprint(NL)\n");
}

// Mutation markers (L1): `.` before an ordinary function or method name,
// and before a call's postfix chain.
static void mutation_markers_parse() {
    const std::string counter =
        "class Counter this(local int start)\n    int value = 0\n    this.value = start\n\n"
        "    void .increment()\n        this.value += 1\n\n";
    good(counter + "Counter c = Counter(1)\n.c.increment()\nprint(c.value)\nprint(NL)\n");
    good("int64 .twice(int64 v)\n    return v * 2\n\nint64 x = .twice(2)\nprint(x + .twice(3))\nprint(NL)\n");
    good("int64 .twice(int64 v);\n\nprint(.twice(1))\nprint(NL)\n\nint64 .twice(int64 v)\n    return v * 2\n");
    good("extern int32 .c_abs(int32 value) = \"abs\"\nprint(.c_abs(int32(-3)))\nprint(NL)\n");
    good("class Box this(local int initial) | error\n    int v\n    this.v = initial\n\n"
         "auto made = Box(1)\n");
    good("class Inner this()\n"
         "    int v = 0\n"
         "\n"
         "    void .bump()\n"
         "        this.v += 1\n"
         "\n"
         "class Outer this()\n"
         "    Inner inner\n"
         "\n"
         "\n"
         "    local Inner fresh = Inner()\n"
         "    this.inner = fresh\n"
         "\n"
         "    void .touch()\n"
         "        .this.inner.bump()\n"
         "\n"
         "Outer o = Outer()\n"
         ".o.touch()\n"
         "print(o.inner.v)\n"
         "print(NL)\n");
    // The marker covers the chain of a call, and marks only calls.
    bad_message("int x = 1\nint y = .x\n", "PARSE_ERROR", "'.' marks a call.");
    bad_message("class P()\n"
                "    int v\n"
                "\n"
                "P p\n"
                "int y = .p.v\n", "PARSE_ERROR", "'.' marks a call.");
    // A qualified type keeps its touching dots; a dot after a gap is a marker.
    good("class model.Point()\n"
         "    int x = 0\n"
         "\n"
         "model.Point .make()\n"
         "    model.Point p = model.Point()\n"
         "    return p\n"
         "\n"
         "model.Point q = .make()\n"
         "print(q.x)\n"
         "print(NL)\n");
    inspect_contains(counter + "Counter c = Counter(1)\n.c.increment()\n", "\"mutation_marker\":true");
    // A member call written with a space before its '.' stays a statement.
    good("class C this()\n"
         "    int v = 0\n"
         "\n"
         "    void show()\n"
         "        print(this.v)\n"
         "\n"
         "void run(C c)\n"
         "    c .show()\n"
         "    print(NL)\n"
         "\n"
         "C c = C()\n"
         "run(c)\n");
}

// Mutation markers are required (L1, L7): a definition that changes
// existing state its `&` parameters do not expose is `.`, every call of a
// `.` callee is marked, and a marker marks only such a call.
static void mutation_markers_are_required() {
    const std::string counter =
        "class Counter this()\n"
        "    int value = 0\n"
        "\n"
        "    void .increment()\n"
        "        this.value += 1\n"
        "\n";
    // R1: receiver fields, runtime state, shared state of a by-value value.
    bad_message("class Counter this()\n"
                "    int value = 0\n"
                "\n"
                "    void increment()\n"
                "        this.value += 1\n",
                "MUTATION_MARKER",
                "'increment' may change existing state (field 'value'); declare it as '.increment'.");
    bad_message("extern void .tick() = \"tick\"\n\nvoid run()\n    .tick()\n", "MUTATION_MARKER",
                "'run' may change existing state (runtime state through 'tick'); declare it as '.run'.");
    bad_message("void log(file.Handle handle)\n    .handle.write_line(\"x\")\n", "MUTATION_MARKER",
                "declare it as '.log'");
    // R2: a declaration may promise more than its body does.
    good("int64 .zero()\n    return 0\n\nprint(.zero())\nprint(NL)\n");
    // R3: the call of a `.` callee is marked, and a marker needs one.
    good(counter + "Counter c = Counter()\n.c.increment()\nprint(c.value)\nprint(NL)\n");
    bad_message(counter + "Counter c = Counter()\nc.increment()\n", "MUTATION_MARKER",
                "This call may change existing state through '.increment'; write '.c.increment()'.");
    bad_message("int64 zero()\n    return 0\n\nprint(.zero())\n", "MUTATION_MARKER",
                "'.' marks a call that changes no existing state.");
    bad_message("int64 .zero()\n    return 0\n\nprint(zero())\n", "MUTATION_MARKER", "write '.zero()'");
    // One marker covers the calls of its chain, not those in its arguments.
    good(counter + "class Box this()\n"
                   "    Counter counter\n"
                   "\n"
                   "\n"
                   "    local Counter fresh = Counter()\n"
                   "    this.counter = fresh\n"
                   "\n"
                   "    Counter .take()\n"
                   "        .this.counter.increment()\n"
                   "        return this.counter\n"
                   "\n"
                   "Box box = Box()\n"
                   ".box.take().increment()\n"
                   "print(box.counter.value)\n"
                   "print(NL)\n");
    bad_message("int64 .zero()\n    return 0\n\nint64 .twice(int64 v)\n    return v * 2\n\nprint(.twice(zero()))\n",
                "MUTATION_MARKER", "write '.zero()'");
    // R5: transitive unless the state is exposed with & or fresh.
    good(counter + "void bump(Counter &c)\n    .c.increment()\n\n"
                   "int64 count()\n    Counter fresh = Counter()\n    .fresh.increment()\n    return int64(fresh.value)\n\n"
                   "Counter c = Counter()\nbump(&c)\nprint(count())\nprint(NL)\n");
    bad_message(counter + "class Outer this()\n"
                          "    Counter inner\n"
                          "\n"
                          "\n"
                          "    local Counter fresh = Counter()\n"
                          "    this.inner = fresh\n"
                          "\n"
                          "    void touch()\n"
                          "        .this.inner.increment()\n",
                "MUTATION_MARKER", "declare it as '.touch'");
    // Methods of built-in types that change existing state.
    good("map.Map<string, int> counts\n.counts.set(\"a\", 1)\nprint(counts.has(\"a\"))\nprint(NL)\n"
         "print(len(counts.keys()))\nprint(NL)\n");
    bad_message("map.Map<string, int> counts\ncounts.set(\"a\", 1)\n", "MUTATION_MARKER",
                "write '.counts.set(\"a\", 1)'");
    bad_message("set.Set<int> seen\nseen.add(1)\n", "MUTATION_MARKER", "write '.seen.add(1)'");
    good("random.Generator generator = random.generator(7)\nint roll = .generator.int(1, 6)\nprint(roll > 0)\nprint(NL)\n");
    bad_message("random.Generator generator = random.generator(7)\nint roll = generator.int(1, 6)\n",
                "MUTATION_MARKER", "through '.int'");
    bad_message("tensor<real32> t = tensor.ones<real32>([2]).track()\nt.clear_grad()\n", "MUTATION_MARKER",
                "write '.t.clear_grad()'");
    good("tensor<real32> t = tensor.ones<real32>([2])\ntensor<real32> u = t.track()\nprint(t.is_tracked())\nprint(NL)\n");
    // A class whose methods only read its map needs no marker.
    good("class Index this()\n"
         "    map.Map<string, int> entries = map.Map<string, int>()\n"
         "\n"
         "    bool has(string key)\n"
         "        return this.entries.has(key)\n"
         "\n"
         "    nat count()\n"
         "        return len(this.entries.keys())\n"
         "\n"
         "Index index = Index()\n"
         "print(index.has(\"a\"))\n"
         "print(index.count())\n"
         "print(NL)\n");
    // R6: a `.` function is not an fn value.
    bad_message("int64 .twice(int64 v)\n    return v * 2\n\nfn<int64>(int64) f = twice\n",
                "FUNCTION_REFERENCE_SIGNATURE", "fn values cannot refer to '.' functions");
    // Hidden mutation in ordered initialization is inferred at creation calls.
    const std::string clock = "extern void .tick() = \"tick\"\n\nclass Clock()\n    int ticks = 0\n    .tick()\n\n";
    good(clock + "Clock c = .Clock()\nClock plain = .Clock()\nprint(plain.ticks)\nprint(NL)\n");
    bad_message(clock + "Clock c = Clock()\n", "MUTATION_MARKER",
                "This constructor call may change existing state; write '.Clock()'.");
    // construct is an ordinary method name and follows the method marker rule.
    bad_message("class Builder()\n    int value = 0\n    void construct()\n        value += 1\n",
                "MUTATION_MARKER", "declare it as '.construct'");
    good("class Builder()\n    int value = 0\n    void .construct()\n        value += 1\nBuilder builder = Builder()\n.builder.construct()\n");
    good("class Widget(local int &registry)\n"
         "    int n = 0\n"
         "\n"
         "\n"
         "    registry += 1\n"
         "\n"
         "int registry = 0\n"
         "Widget w = Widget(&registry)\n"
         "print(registry)\n"
         "print(NL)\n");
    // Ordinary parameter defaults change no existing state.
    const std::string ids = "extern int64 .next_id() = \"next_id\"\n\n";
    good(ids + "class Item()\n    int64 id = .next_id()\nItem item = .Item()\n");
    bad_message(ids + "class Item()\n    int64 id = .next_id()\nItem item = Item()\n",
                "MUTATION_MARKER", "write '.Item()'");
    bad_message(ids + "void make(int64 id = .next_id())\n    print(id)\n", "MUTATION_MARKER",
                "A parameter default cannot change existing state: 'id' calls '.next_id'");
    // L7: a family agrees on the marker; a prototype agrees with its definition.
    bad_message("T pick<T>(T value)\n    return value\n\nint64 .pick(int64 value)\n    return value\n",
                "MUTATION_MARKER", "Members of a specialization family must agree on the '.' marker");
    bad_message("void .f();\n\nvoid f()\n    return\n", "PROTOTYPE_MISMATCH", "agree on the '.' marker");
    // Option A: a call through a const value may not change it, but may
    // change other hidden state.
    bad_message(counter + "Counter base = Counter()\nconst Counter c = base\n.c.increment()\n", "WRITE_CAPABILITY",
                "'.c.increment()' may change const 'c' (field 'value').");
    good("extern void .tick() = \"tick\"\n"
         "\n"
         "class Logger()\n"
         "    int lines = 0\n"
         "\n"
         "    void .note()\n"
         "        .tick()\n"
         "\n"
         "Logger base = Logger()\n"
         "const Logger logger = base\n"
         ".logger.note()\n"
         "print(logger.lines)\n"
         "print(NL)\n");
}

// D12 at the C boundary: `const string`, `const bin` and `const tensor<T>`
// extern parameters are read-only borrows passed as values.
static void const_values_cross_the_c_boundary_as_borrows() {
    const std::string text = "extern int32 c_text(const string text) = \"c_text\"\n";
    const std::string bytes = "extern int32 c_bin(const bin data) = \"c_bin\"\n";
    const std::string tensors =
        "extern int32 c_tensor(const tensor<real32> input, tensor<real32> &output) = \"c_tensor\"\n";
    good(text + bytes + tensors);
    ir_contains(text, "function c_text(const string &text) -> int32 = \"c_text\"");
    llvm_contains(text, "declare i32 @c_text(ptr nocapture nonnull readonly, i64)");
    llvm_contains(bytes, "declare i32 @c_bin(ptr nocapture nonnull readonly, i64)");
    llvm_contains(tensors, "declare i32 @c_tensor(ptr nocapture nonnull readonly, ptr nocapture nonnull)");
    // A variable, a field and an element pass their storage by address.
    llvm_contains(text + "string value = \"abc\"\nint32 result = c_text(value)\nprint(result)\n",
                  "ffi.borrowed.value");
    llvm_contains(bytes + "bin[] values = [bin.fill(24, 1)]\nint32 result = c_bin(values[0])\nprint(result)\n",
                  "ffi.bin.length");
    // Any other value is held for the call.
    llvm_contains(text + "string value = \"abc\"\nint32 result = c_text(value + \"d\")\nprint(result)\n",
                  "call i64 @strlen(ptr");
    ir_contains(text + "int32 result = c_text(\"literal\")\nprint(result)\n", "ffi.borrow");
    ir_contains(bytes + "int32 result = c_bin(bin.fill(8, 1))\nprint(result)\n", "ffi.borrow");
    // A const parameter takes a value.
    bad_message(text + "string value = \"abc\"\nint32 result = c_text(&value)\n", "WRITE_CAPABILITY",
                "Const parameters take values; remove '&' from '&value'.");
    bad_message("int64 first(const int64[] values)\n    return values[0]\n\n"
                "int64[] data = [1]\nprint(first(&data))\n",
                "WRITE_CAPABILITY", "Const parameters take values; remove '&' from '&data'.");
    // By-value managed parameters without const stay rejected; a string
    // borrow is read-only.
    bad_message("extern int32 c_text(string text) = \"c_text\"\n", "FFI_REFERENCE", "const string");
    bad_message("extern int32 c_text(string &text) = \"c_text\"\n", "FFI_REFERENCE", "const string");
    bad_message("extern int32 c_bin(bin data) = \"c_bin\"\n", "FFI_REFERENCE", "const bin");
    bad_message("extern int32 c_tensor(tensor<real32> input) = \"c_tensor\"\n", "FFI_REFERENCE",
                "const tensor<T>");
}

// D12: there are no const references; `const T` is a value.
static void const_references_are_rejected() {
    const std::string message = "Const references do not exist; write 'const T name' for a read-only value.";
    bad_message("int first(const int[] &values)\n    return values[0]\n", "CONST_REFERENCE", message);
    bad_message("extern int32 c_text(const string &text) = \"c_text\"\n", "CONST_REFERENCE", message);
    bad_message("int value = 1\nconst int &view = &value\n", "CONST_REFERENCE", message);
    bad_message("int value = 1\nconst auto &view = &value\n", "CONST_REFERENCE", message);
    bad_message("class Box this(local const int &v_initial)\n"
                "    int v\n"
                "\n"
                "\n"
                "    this.v = v_initial\n", "CONST_REFERENCE",
                message);
    // A const local is a snapshot with its own identity.
    good("int[] values = [1, 2]\nconst int[] kept = values\nvalues[0] = 5\nprint(kept[0])\nprint(NL)\n"
         "print(&kept == &values)\nprint(NL)\n");
}

// sorted(descending = true) (L4): a named bool, constant or run-time.
static void sorted_takes_a_descending_flag() {
    const std::string values = "int64[] a = [3, 1, 2]\n";
    good(values + "int64[] d = a.sorted(descending = true)\nprint(d[0])\nprint(NL)\n");
    good(values + "bool flag = len(a) > 2\nint64[] d = a.sorted(descending = flag)\nprint(d[0])\nprint(NL)\n");
    good("int64[3] f = [3, 1, 2]\nint64[] d = f.sorted(descending = true)\nprint(d[0])\nprint(NL)\n");
    bad_message(values + "auto d = a.sorted(true)\n", "ARGUMENT_MISMATCH",
                "sorted() accepts only the named argument 'descending'.");
    bad_message(values + "auto d = a.sorted(order = true)\n", "ARGUMENT_MISMATCH",
                "sorted() accepts only the named argument 'descending'.");
    bad_message(values + "auto d = a.sorted(descending = true, descending = false)\n",
                "ARGUMENT_MISMATCH", "sorted() accepts only the named argument 'descending'.");
    bad_message(values + "int64 one = 1\nauto d = a.sorted(descending = one)\n", "TYPE_MISMATCH",
                "descending must be bool.");
    // Ascending (no argument or a constant false) is today's instruction;
    // a constant true calls the descending entry, a flag branches.
    ir_contains(values + "auto d = a.sorted(descending = false)\nprint(d[0])\n", "= array.sorted %");
    ir_contains(values + "auto d = a.sorted(descending = true)\nprint(d[0])\n", " descending : ");
    ir_contains(values + "bool flag = true\nauto d = a.sorted(descending = flag)\nprint(d[0])\n",
                " descending %");
    llvm_contains(values + "auto d = a.sorted(descending = true)\nprint(d[0])\n",
                  "call ptr @quidra_array_sorted_desc(");
    llvm_contains(values + "bool flag = len(a) > 2\nauto d = a.sorted(descending = flag)\nprint(d[0])\n",
                  "array.sorted.descending");
}

// Comparison chains (L14): `a OP1 b OP2 c` is `a OP1 b and b OP2 c` in one
// direction, {<, <=}, {>, >=} or only '=='; '!=' never chains, and a
// relational comparison next to '==' or '!=' must be parenthesized.
static void comparison_chains_keep_one_direction() {
    const std::string values = "int64 a = 1\nint64 b = 2\nint64 c = 3\n";
    good(values + "bool x = 0 <= a < 10\nprint(x)\nprint(NL)\n");
    good(values + "bool x = c >= b > a\nprint(x)\nprint(NL)\n");
    good(values + "bool x = a < b <= c < 10\nprint(x)\nprint(NL)\n");
    good(values + "bool x = a == b == c\nprint(x)\nprint(NL)\n");
    good("bool p = false\nbool q = false\nbool r = true\nprint(p == q == r)\nprint(NL)\n");
    good("string s = \"a\"\nstring t = \"a\"\nstring u = \"a\"\nprint(s == t == u)\nprint(NL)\n");
    bad_message("int[] p = [1]\nint[] q = [1]\nint[] r = [1]\nprint(p == q == r)\n",
                "CHAINED_COMPARISON",
                "Arrays and tensors cannot be compared in a chain; combine the comparisons explicitly.");
    good("class P this(local int value)\n"
         "    int v\n"
         "\n"
         "\n"
         "    this.v = value\n"
         "\n"
         "print(P(1) == P(1) == P(1))\n"
         "print(NL)\n");
    good(values + "int64 &ra = &a\nint64 &rb = &a\nprint(&ra == &rb == &a)\nprint(NL)\n");
    // A chain is a condition like any bool.
    good(values + "if 0 <= a < 10\n    print(a)\nwhile 0 < a < 0\n    print(a)\n"
                  "int64 y = if 0 < a <= 2 then a else b\nprint(y)\nprint(NL)\n");
    // A parenthesized comparison is an operand.
    good(values + "bool x = (a < b) == (b < c)\nbool y = (a < b) == true\nprint(x == y)\nprint(NL)\n");
    // Directions, '!=' and mixed levels.
    bad_message(values + "bool x = a < b > c\n", "CHAINED_COMPARISON",
                "A comparison chain must keep one direction: '<' and '<=', or '>' and '>=', or only '=='.");
    bad_message(values + "bool x = a >= b <= c\n", "CHAINED_COMPARISON", "keep one direction");
    bad_message(values + "bool x = a != b != c\n", "CHAINED_COMPARISON", "'!=' cannot be chained.");
    bad_message(values + "bool x = a == b != c\n", "CHAINED_COMPARISON", "'!=' cannot be chained.");
    bad_message(values + "bool x = a < b == true\n", "CHAINED_COMPARISON",
                "'<' and '==' cannot share a chain; to compare the result of 'a < b', "
                "write '(a < b) == true'.");
    bad_message(values + "bool x = true == a < b\n", "CHAINED_COMPARISON",
                "'==' and '<' cannot share a chain; to compare with the result of 'a < b', "
                "write 'true == (a < b)'.");
    bad_message(values + "bool x = true != a < b\n", "CHAINED_COMPARISON",
                "'!=' and '<' cannot share a chain");
    // Tensors give masks, which a chain cannot join.
    bad_message("tensor<real32> t = tensor.zeros<real32>([2])\nauto m = t < t <= t\n",
                "CHAINED_COMPARISON", "Arrays and tensors cannot be compared in a chain");
    // `a < b < c > (d)` reads as the generic call `b<c>(d)`.
    bad_message(values + "int64 d = 4\nbool x = a < b < c > (d)\n", "CHAINED_COMPARISON",
                "write '(c > d)' if '>' starts a new comparison");
    // Each link follows the single comparison's rules.
    bad_code("int64 a = 1\nint32 b = 2\nbool x = 0 <= a < b\n", "TYPE_MISMATCH");
    bad_code("bool p = true\nbool x = p < p < p\n", "TYPE_MISMATCH");
    bad_code("bool x = 1 < 2 < 3\n", "AMBIGUOUS_NUMERIC_LITERAL");
    // Lowering and tools.
    ir_contains(values + "bool x = a < b < c\nprint(x)\n", "chain.result");
    inspect_contains(values + "bool x = a < b < c\n", "\"comparison_chain\"");
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
    // Range values and index bindings may be passed by value. The callee
    // may change its copy without changing the binding fixed by the header.
    good(R"(int increment(int value)
    value += 1
    return value
class Counter this(local int value)
    int total

    this.total = value
    int increment(int value)
        value += 1
        return value
Counter counter = Counter(0)
for int value < 3
    print(increment(value))
    print(counter.increment(value))
    Counter copy = Counter(value)
    print(copy.total)
nat[] values = [0, 1]
for nat value = values[nat index]
    print(index)
)");
    good("int x\nif true\n    x = 1\nprint(x)\nprint(NL)\n");
    good("int x\nwhile false\n    x = 1\nprint(x)\nprint(NL)\n");
    ir_function_contains("int maybe(bool c)\n    int x\n    if c\n        x = 1\n    return x\n",
                         "maybe", "init.check %");
    ir_function_contains("int zero(int n)\n    int x\n    for 0 <= int i < n\n        x = i\n    return x\n",
                         "zero", "init.check %");
    ir_function_contains("int repeat(bool c)\n    int x\n    while c\n        x = 1\n        return x\n"
                         "    return x\n", "repeat", "init.check %");
    ir_function_contains("int pick(int | none value)\n    int x\n    match value\n        int present\n"
                         "            x = present\n        none\n            void\n    return x\n",
                         "pick", "init.check %");
    // A read later in a loop body than the write it depends on, on a later
    // iteration.
    ir_function_contains("int carried(int n)\n    int x\n    int total = 0\n    for 0 <= int i < n\n"
                         "        if i > 0\n            total += x\n        x = i\n    return total\n",
                         "carried", "init.check %");
    // A loop that runs at least once and has no break or continue
    // initializes what its body does: a constant range, a fixed array, a
    // literal.
    ir_function_not_contains("int counted()\n    int x\n    for 0 <= nat i < 3\n        x = int(i)\n    return x\n",
                             "counted", "init.check");
    ir_function_not_contains("int fixed(int[3] values)\n    int x\n    for auto v = values[]\n        x = v\n"
                             "    return x\n", "fixed", "init.check");
    ir_function_not_contains("string listed()\n    string x\n    for auto v = [\"a\", \"b\"][]\n        x = v\n"
                             "    return x\n", "listed", "init.check");
    ir_function_contains("int stopped()\n    int x\n    for 0 <= nat i < 3\n        if i == 1\n            break\n"
                         "        x = int(i)\n    return x\n", "stopped", "init.check %");
    ir_function_contains("int empty_range()\n    int x\n    for 3 <= nat i < 3\n        x = int(i)\n    return x\n",
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
    good("int[] b = array(3)\nfor 0 <= nat i < len(b)\n    b[i] = int(i)\nprint(b[2])\nprint(NL)\n");
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
    // A conditional direct generation carries possible stores into its fresh mask.
    llvm_contains(R"(class Box this(local int | none value_value)
    int value
    match value_value
        int initial
            this.value = initial
        none
            return

Box box = Box(9)
print(box.value)
)", "init.check.fail");
    // Never-written ordinary fields do not become Maybe merely through generation.
    bad_code("class NeverGenerated()\n    int value\n"
             "NeverGenerated box = NeverGenerated()\nprint(box.value)\n",
             "UNINITIALIZED");
    const std::string point = "class Point()\n"
                              "    int x\n"
                              "    int y = 0\n"
                              "\n";
    ir_function_contains(point + "int pick(bool c)\n    Point p = Point()\n    if c\n        p.x = 1\n    return p.x\n",
                         "pick", "init.check %");
    ir_contains(point + "bool c = len(\"ab\") > 1\nPoint p = Point()\nif c\n    p.x = 1\nprint(p.x)\n", "field 'p.x'");
    ir_function_not_contains(point + "int both(bool c)\n    Point p = Point()\n    if c\n        p.x = 1\n    else\n"
                             "        p.x = 2\n    return p.x\n", "both", "init.check");
    bad_message(point + "Point p = Point()\nprint(p.x)\n", "UNINITIALIZED",
                "'p.x' is read before it is initialized on every path.");
    // Whole-binding generation has the same fresh-field tracking as a declaration.
    ir_function_contains(point + "int assigned(bool c)\n    Point p\n    p = Point()\n    if c\n        p.x = 1\n    return p.x\n",
                         "assigned", "init.check %");
    // A generation body that writes a field through a callee has no reliable bit.
    bad_message("void fill(int &value, bool ok)\n    if ok\n        value = 1\n"
                "class Indirect this(local bool open)\n    int x\n    fill(&this.x, open)\n"
                "int read(bool c)\n    Indirect p = Indirect(c)\n    if c\n        p.x = 2\n    return p.x\n",
                "UNINITIALIZED", "may be uninitialized");
    // A local reference alias must not hide a constructor's untracked field write.
    bad_message("void fill(int &value, bool ok)\n    if ok\n        value = 1\n"
                "class Aliased this(local bool open)\n    int x\n    local int &alias = &this.x\n    fill(&alias, open)\n"
                "int read(bool open, bool c)\n    Aliased p = Aliased(open)\n    if c\n        p.x = 2\n    return p.x\n",
                "UNINITIALIZED", "may be uninitialized");
    // A value from elsewhere keeps today's rule: its bits are not known.
    bad_message(point + "Point make()\n    Point p = Point()\n    p.x = 1\n    return p\n\nbool c = len(\"ab\") > 1\n"
                "Point q = make()\nPoint r\nif c\n    r = q\nprint(r.y)\nprint(q.y)\nPoint s = Point()\nif c\n    s.y = 1\n"
                "Point t = s\nprint(t.x)\n", "UNINITIALIZED", "may be uninitialized");
    // A field written through a reference or by a callee is not tracked.
    bad_message(point + "void fill(int &value, bool ok)\n    if ok\n        value = 1\n\nPoint p = Point()\n"
                "fill(&p.x, true)\nprint(p.x)\n", "UNINITIALIZED", "may be uninitialized");
    // The receiver in its constructor.
    ir_function_contains("class Gate this(local bool open)\n"
                         "    int value\n"
                         "    int seen = 0\n"
                         "\n"
                         "\n"
                         "    if open\n"
                         "        this.value = 1\n"
                         "    this.seen = this.value\n",
                         "$construct.Gate", "init.check %");
    bad_message("class Shut this()\n"
                "    int value\n"
                "\n"
                "\n"
                "    print(this.value)\n", "UNINITIALIZED",
                "is read before the constructor initializes it");
    // Initialization locals remain distinct from the member facts.
    const std::string gate = "class Gate this(local bool open)\n    int value\n    int seen = 0\n\n";
    const std::string local_maybe = gate + "    local int temporary\n    if open\n"
        "        temporary = 1\n    this.value = 2\n    this.seen = temporary\n";
    ir_function_contains(local_maybe, "$construct.Gate", "binding 'temporary'");
    ir_function_not_contains(local_maybe, "$construct.Gate", "field 'this.value'");
    const std::string field_maybe = gate + "    local int temporary = 1\n    if open\n"
        "        this.value = temporary\n    this.seen = this.value\n";
    ir_function_contains(field_maybe, "$construct.Gate", "field 'this.value'");
    ir_function_not_contains(field_maybe, "$construct.Gate", "binding 'temporary'");
    ir_function_not_contains(gate + "    local int temporary\n    if open\n"
                             "        temporary = 1\n        this.value = 2\n    else\n"
                             "        temporary = 3\n        this.value = 4\n"
                             "    this.seen = temporary + this.value\n",
                             "$construct.Gate", "init.check");
    bad_message(gate + "    local int temporary\n    this.value = 1\n    this.seen = temporary\n",
                "UNINITIALIZED", "'temporary' is read before it is initialized on every path.");
    bad_message(gate + "    local int temporary = 1\n    this.seen = this.value\n",
                "UNINITIALIZED", "is read before the constructor initializes it");
    bad_code(gate + "    local int value = 1\n", "SHADOWING");
    // Equality and display skip the mask; copies keep it.
    good(point + "bool c = len(\"ab\") > 1\nPoint p = Point()\np.x = 1\nPoint q = Point()\nif c\n    q.x = 1\nprint(q.x)\n"
         "Point r = Point()\nr.x = 1\nprint(\"{p == r}\")\nprint(NL)\n");
    // Core gap G-d: an element of an initialized array of class values is a
    // whole value inside a method too.
    good(R"(class Slot this(local int storage_initial)
    int storage


    this.storage = storage_initial

    int value()
        return this.storage

class Shelf this()
    Slot[] items


    this.items = [Slot(1), Slot(2)]

    int second()
        return this.items[1].value() + this.items[0].storage

Shelf shelf = Shelf()
print(shelf.second())
print(NL)
)");
    bad_code(R"(class Slot()
    int storage

class Shelf this()
    Slot[] items

    int first()
        return this.items[0].storage

Shelf shelf = Shelf()
print(shelf.first())
)", "UNINITIALIZED");
}

// Effect summaries (src/semantics/effect_summary.hpp): what a function may
// write and read of its caller's storage, compose through calls; hidden
// mutations, resources, address observations and result provenance.
static void effect_summaries_describe_functions() {
    namespace semantics = quidra::semantics;
    bad_code("class UnboundCopy()\n    int &value\n\nUnboundCopy original = UnboundCopy()\nUnboundCopy copy = original\nprint(copy.value)\n",
             "UNBOUND_REFERENCE");
    // Whole assignment and literal-tuple transfer keep known Never descriptors.
    bad_code("class UnboundAssign()\n    int &value\n\nUnboundAssign original = UnboundAssign()\n"
             "UnboundAssign copy = UnboundAssign()\ncopy = original\nprint(copy.value)\n",
             "UNBOUND_REFERENCE");
    bad_code("class UnboundTuple()\n    int &value\n\n(UnboundTuple, int) values = (UnboundTuple(), 1)\n"
             "(UnboundTuple copy, int ignored) = values\nprint(copy.value)\n",
             "UNBOUND_REFERENCE");
    // A may-write in generation retains the runtime descriptor guard after copying.
    ir_contains("class MaybeBound(local int &input, local bool open)\n    int &value\n"
                "    if open\n        &value = &input\n"
                "int read(int &input, bool open)\n    MaybeBound original = MaybeBound(&input, open)\n"
                "    MaybeBound copy = original\n    return copy.value\n",
                "reference.check_bound");
    const auto source = R"(class Counter this()
    int count = 0
    int[] items = []

    void .bump()
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

class SharedInt this(local int &value_initial)
    int &value

    &this.value = &value_initial

void .put(SharedInt holder)
    holder.value = 3

void put_fresh()
    int value = 1
    SharedInt holder = SharedInt(&value)
    holder.value = 2

void .put_copy(SharedInt holder)
    SharedInt copy = holder
    copy.value = 5

void bump_counter(Counter &counter)
    .counter.bump()

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
int shared = 1
.put(SharedInt(&shared))
put_fresh()
.put_copy(SharedInt(&shared))
Counter c = Counter()
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
        fail("put changes the reference target its argument shares");
    if (summary("put_fresh").first_hidden_mutation || summary("put_fresh").mutates_ambient)
        fail("a local target the function created is not existing state");
    if (!summary("put_copy").mutates_shared_of_parameter.at(0))
        fail("a copy of a holder keeps its reference target");

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

    const auto shared_place = semantics::StoragePath{semantics::StorageRoot::shared("c"), {field("value")}};
    if (!semantics::overlaps(parameter_path(0, {element(1)}),
                             parameter_path(0, {semantics::PathStep::any_element(), field("x")})) ||
        semantics::overlaps(parameter_path(0, {element(1)}), parameter_path(0, {element(2)})) ||
        semantics::overlaps(parameter_path(0, {field("a")}), parameter_path(0, {field("b")})) ||
        !semantics::overlaps(parameter_path(0, {}), parameter_path(0, {field("b")})) ||
        semantics::overlaps(parameter_path(0, {}), parameter_path(1, {})) ||
        semantics::overlaps(shared_place, parameter_path(0, {field("value")})))
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

class Box this()
    int[] items = [1, 2]
    int count = 0

    void .note(const int[] values)
        this.count = int(len(values))

    void .reset(const int[] values)
        this.items = values

)");
    ir_function_not_contains(calls + R"(void separate()
    int[] a = [1, 2, 3]
    int[] b = [4]
    show(a)
    keep(a, &b)
    Box box = Box()
    .box.note(box.items)

separate()
)", "function separate", "= clone %");
    ir_function_contains(calls + R"(void aliased()
    int[] a = [1, 2, 3]
    keep(a, &a)

aliased()
)", "function aliased", "= clone %");
    ir_function_contains(calls + R"(void receiver()
    Box box = Box()
    .box.reset(box.items)

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
    // Inspect the source body, including when a shape entry forwards to it.
    const auto body = [](const auto& compilation, const std::string& symbol) {
        const auto function = std::find_if(compilation.ir.functions.begin(),
            compilation.ir.functions.end(), [&](const auto& value) {
                if (value.entrypoint) return symbol == "main";
                const auto& source_name = value.source_function.empty()
                    ? value.name : value.source_function;
                return !value.shape_wrapper && quidra::abi::user_symbol(source_name) == symbol;
            });
        if (function == compilation.ir.functions.end())
            throw std::runtime_error("no source body " + symbol);
        const auto emitted_symbol = function->entrypoint
            ? std::string("main") : quidra::abi::user_symbol(function->name);
        const auto& llvm = compilation.llvm;
        const auto start = llvm.find("define i64 @" + emitted_symbol + "(");
        const auto end = llvm.find("\n}\n", start);
        if (start == std::string::npos || end == std::string::npos)
            throw std::runtime_error("no emitted source body " + symbol);
        return llvm.substr(start, end - start);
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
              "extern int64 c_apply(func<int64>(int64) callback, int64 value) = \"loc_apply\"\n"
              "\n"
              "int pick(int[] values, int i)\n"
              "    return values[i]\n"
              "\n"
              "int apply(func<int>(int) f, int x)\n"
              "    return f(x)\n"
              "\n"
              "int64 apply_native(func<int64>(int64) f, int64 x)\n"
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
              "extern int64 c_twice(func<int64>(int64) callback, int64 value) = \"user_apply\"\n"
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
        const auto compilation = quidra::compile_file(root / "main.qui", {}, root);
        const auto& llvm = compilation.llvm;
        const auto pick = body(compilation, "n_loc_pick");
        require(pick.find("call void @quidra_runtime_set_package_source_provenance(ptr @.quidra.source.") != std::string::npos &&
                    pick.find("@quidra_runtime_set_source_provenance(") == std::string::npos,
                "package statements call the package setter", pick);
        require(pick.find("@quidra_array_slot(") != std::string::npos &&
                    pick.find("i64 0, i64 0)") != std::string::npos,
                "package failure sites pass no location", pick);
        const auto internal = body(compilation, "n_loc_internal_pick");
        require(internal.find("@quidra_runtime_set_package_source_provenance(") != std::string::npos,
                "a package's own quoted import is package code", internal);
        const auto lib = body(compilation, "n_lib_lib_pick");
        require(lib.find("call void @quidra_runtime_set_source_provenance(ptr @.quidra.source.") != std::string::npos,
                "a local import of the root is user code", lib);
        for (const auto* symbol : {"n_loc_apply", "n_loc_apply_native"}) {
            const auto text = body(compilation, symbol);
            const auto save = text.find("= call ptr @quidra_runtime_user_statement()");
            const auto call = text.find(std::string(symbol) == "n_loc_apply" ? "call i64 %" : "call i64 @loc_apply(");
            const auto restore = text.find("call void @quidra_runtime_restore_user_statement(ptr %user.statement.");
            require(save != std::string::npos && call != std::string::npos && restore != std::string::npos &&
                        save < call && call < restore,
                    std::string("package code saves and restores the user statement around ") + symbol, text);
        }
        const auto entry = body(compilation, "main");
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
        const auto quiet_compilation = quidra::compile_file(root / "quiet.qui", {}, root);
        const auto& quiet = quiet_compilation.llvm;
        require(body(quiet_compilation, "main").find("@quidra_runtime_restore_user_statement(") == std::string::npos,
                "statements without a call into user code before a reader need no refresh", body(quiet_compilation, "main"));
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
        const auto compilation = quidra::compile_file(root / "main.qui", options, root);
        const auto& llvm = compilation.llvm;
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
        // LLVM prints nonprintable bytes, including Windows path separators,
        // as two-digit hexadecimal escapes in c"..." constants.
        const auto llvm_c_string = [](const std::string& text) {
            static constexpr char hex[] = "0123456789ABCDEF";
            std::string literal = "c\"";
            for (const unsigned char byte : text) {
                if (byte == '\\' || byte == '"' || byte < 32 || byte >= 127) {
                    literal += '\\';
                    literal += hex[byte >> 4];
                    literal += hex[byte & 15];
                } else {
                    literal += static_cast<char>(byte);
                }
            }
            return literal + "\\00\"";
        };
        for (const auto& text : {std::string("app/main.qui"), std::string("app/lib/pick.qui"),
                                 (root / "main.qui").lexically_normal().string(), revision}) {
            const auto expected = llvm_c_string(text);
            const auto probe = llvm.find("pick.qui");
            const auto nearby = probe == std::string::npos
                ? llvm.substr(0, 1500)
                : llvm.substr(probe > 160 ? probe - 160 : 0, 350);
            require(llvm.find(expected) != std::string::npos,
                    "the table names " + text, nearby);
        }
        require(llvm.find(", i64 8, i64 " + std::to_string(std::string(main).size()) + " }") !=
                    std::string::npos,
                "the root's line count and size", "");
        const auto body = [&](const std::string& source_name) {
            const auto function = std::find_if(compilation.ir.functions.begin(),
                compilation.ir.functions.end(), [&](const auto& value) {
                    return !value.shape_wrapper &&
                        (value.source_function == source_name ||
                         (value.source_function.empty() && value.name == source_name));
                });
            require(function != compilation.ir.functions.end(),
                    "the source function has its body", source_name);
            const auto begin = llvm.find("define i64 @" +
                quidra::abi::user_symbol(function->name) + "(");
            const auto end = llvm.find("\n}\n", begin);
            require(begin != std::string::npos && end != std::string::npos,
                    "the source body is emitted", source_name);
            return llvm.substr(begin, end - begin);
        };
        const auto pick = body("lib.pick");
        require(pick.find("i64 4294967298, i64 19)") != std::string::npos,
                "an import's immediates carry its index", pick);
        const auto root_pick = body("pick_root");
        require(root_pick.find("i64 4, i64 19)") != std::string::npos,
                "the root's immediates are its lines", root_pick);
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
    bad_message("class Counter this()\n"
                "    int32 value\n"
                "\n"
                "    export \"C\" int32 get()\n"
                "        return this.value\n",
                "FFI_EXPORT", "methods cannot be exported");
    bad_message("class Counter()\n"
                "    int32 value\n"
                "    export \"C\" int32 construct()\n"
                "        return 0\n",
                "FFI_EXPORT", "methods cannot be exported");
    bad_code("class Counter()\n"
             "    export \"C\" int32 value\n", "PARSE_ERROR");
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
        "Box", "Color", "int32 | error", "int32 | none", "func<int32>(int32)"};
    const std::string declarations =
        "class Box()\n"
        "    int32 value\n"
        "\n"
        "enum Color\n"
        "    red\n"
        "    green\n"
        "\n";
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
    bad_code("export \"C\" int32 take(const string value)\n    return 0\n", "FFI_TYPE");
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
        // The artificial export subprogram changes metadata identifiers.
        // Compare executable IR while retaining every non-debug operand.
        std::string instructions;
        std::istringstream lines(text);
        for (std::string line; std::getline(lines, line);) {
            if (line.starts_with("!")) continue;
            for (auto found = line.find("!dbg !"); found != std::string::npos;
                 found = line.find("!dbg !")) {
                auto begin = found;
                if (begin && line[begin - 1] == ' ') --begin;
                if (begin && line[begin - 1] == ',') --begin;
                auto end = found + 6;
                while (end < line.size() && line[end] >= '0' && line[end] <= '9') ++end;
                line.erase(begin, end - begin);
            }
            instructions += line + "\n";
        }
        return instructions;
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
// string slices pass their positions to the runtime; the static bound uses
// the runtime's words.
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
                                     "print(b[1:<at])\n").llvm;
    require(line_with(bin, "@quidra_bin_index(ptr ", ", i64 3, i64 9)"), "bin element read", bin);
    require(line_with(bin, "@quidra_bin_set(ptr ", ", i64 4, i64 4)"), "bin element write", bin);
    require(line_with(bin, "@quidra_bin_slice(ptr ", ", i64 5, i64 7)"), "bin slice", bin);
    require(bin.find("declare ptr @quidra_bin_index(ptr, i64, i64, i64)") != std::string::npos,
            "the bin entries take a position", bin);
    const auto text = quidra::compile("string word = \"quidra\"\n"
                                      "nat at = len(word)\n"
                                      "print(word[1:<int(at) + 3])\n"
                                      "print(word[at])\n").llvm;
    require(line_with(text, "@quidra_string_slice(ptr ", ", i64 3, i64 7)"), "string slice", text);
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

// Native call metadata follows the artifact boundary, and a library's
// export wrapper remains a transparent function with its own subprogram.
static void native_call_sites_follow_the_artifact() {
    const std::string source =
        "int32 twice(int32 value)\n    return value * 2\n"
        "int32 value = twice(3)\n";
    const auto require = [](bool condition, const char* message) {
        if (condition) return;
        std::cerr << "native call-site contract failed: " << message << "\n";
        std::exit(1);
    };
    const auto native = quidra::compile(source);
    require(!native.call_sites.empty(), "the executable has no function table");
    require(native.call_sites.sites.size() > 1, "the executable has no call sites");
    require(native.llvm.find("emissionKind: LineTablesOnly") != std::string::npos,
            "release metadata is not line-table metadata");
    require(native.llvm.find("name: \"n_twice\"") != std::string::npos,
            "the function's subprogram does not name its symbol");
    require(native.llvm.find("__quidra_calls_anchor") == std::string::npos,
            "an executable references the library anchor");
    bool found_twice = false;
    for (const auto& site : native.call_sites.sites) {
        if (site.callee_kind != quidra::CallSiteCallee::function) continue;
        require(site.callee < native.call_sites.functions.size(), "an invalid callee id");
        if (native.call_sites.functions[site.callee].display == "twice()") {
            found_twice = true;
            require(site.line == 3 && site.column == 15, "the call's source position");
        }
    }
    require(found_twice, "the user call is missing from the site table");
    quidra::CompileOptions interactive;
    interactive.artifact = quidra::CompileArtifact::Interactive;
    const auto repl = quidra::compile(source, interactive);
    require(repl.call_sites.empty(), "an interactive compile has a table");
    require(repl.llvm.find("!DICompileUnit") == std::string::npos,
            "an interactive compile has call metadata");
    require(repl.llvm.find("__quidra_calls_anchor") == std::string::npos,
            "an interactive compile references the anchor");
    quidra::CompileOptions library;
    library.artifact = quidra::CompileArtifact::Library;
    const auto exported = quidra::compile(
        "export \"C\" int32 twice(int32 value)\n    return value * 2\n", library);
    require(exported.call_sites.library, "the table is not a library table");
    require(exported.llvm.find("@__quidra_calls_anchor = external global i8") != std::string::npos,
            "the library lacks its anchor reference");
    require(exported.llvm.find("flags: DIFlagArtificial") != std::string::npos,
            "the export wrapper lacks an artificial subprogram");
    bool wrapper = false;
    for (const auto& function : exported.call_sites.functions)
        wrapper = wrapper || (function.flags & quidra::call_site_function_flags::export_entry);
    require(wrapper, "the export wrapper has no table identity");
}

static void native_tail_records_are_scoped() {
    const auto require = [](bool condition, const char* message) {
        if (condition) return;
        std::cerr << "native tail-record contract failed: " << message << "\n";
        std::exit(1);
    };
    const std::string source =
        "int64 leaf(int64 value)\n    return 1 / value\n"
        "int64 relay(int64 value)\n    return leaf(value)\n"
        "int64 count(int64 value)\n    if value == 0\n        return leaf(value)\n"
        "    return count(value - 1)\n"
        "int64 stable(int64 value)\n    if value == 0\n        return 1\n"
        "    return stable(value - 1)\n"
        "print(stable(int64.parse(\"7\")))\n"
        "print(count(int64.parse(\"5\")))\n";
    const auto native = quidra::compile(source);
    bool self = false;
    bool unique = false;
    for (const auto& site : native.call_sites.sites) {
        if (!(site.flags & quidra::call_site_flags::tail)) continue;
        const auto& owner = native.call_sites.functions.at(site.owner);
        if (site.flags & quidra::call_site_flags::self_tail) {
            self = true;
            require(site.flags & quidra::call_site_flags::recorded_tail, "a self tail is not recorded");
            require(owner.flags & quidra::call_site_function_flags::self_tail, "the self function lacks cleanup");
        }
        if (owner.display == "relay()") {
            unique = true;
            require(!(site.flags & quidra::call_site_flags::recorded_tail), "a unique acyclic tail was recorded");
        }
    }
    require(self && unique, "the self and unique tail sites are missing");
    const auto relay = llvm_function_text(native.llvm, "define i64 @n_relay(");
    require(relay.find("@quidra_tail_record") == std::string::npos &&
            relay.find("@quidra_tail_return") == std::string::npos,
            "a unique acyclic function touches the tail log");
    const auto stable = llvm_function_text(native.llvm, "define i64 @n_stable.depth(");
    require(stable.find("tail.self.active") != std::string::npos,
            "a self-tail function's normal return lacks its depth compare");
    for (const auto& function : native.call_sites.functions) {
        if (function.kind == quidra::CallSiteFunctionKind::entry) continue;
        const auto body = llvm_function_text(native.llvm, "define i64 @" + function.symbols.back() + "(");
        if (body.find("tail.self.active") != std::string::npos)
            require(function.flags & quidra::call_site_function_flags::self_tail,
                    "a function without self tails has normal-return instrumentation");
    }
    require(native.llvm.find("noinline") == std::string::npos, "tail recording changed inlining policy");
    quidra::CompileOptions interactive;
    interactive.artifact = quidra::CompileArtifact::Interactive;
    const auto repl = quidra::compile(source, interactive);
    require(repl.llvm.find("call void @quidra_tail_record") == std::string::npos &&
            repl.llvm.find("call void @quidra_tail_return") == std::string::npos,
            "an interactive compile records native tails");
    quidra::LlvmEmitOptions disabled;
    disabled.tail_records = false;
    const auto plain = quidra::emit_native_llvm(native.ir, disabled);
    require(plain.text.find("call void @quidra_tail_record") == std::string::npos &&
            plain.text.find("call void @quidra_tail_return") == std::string::npos,
            "the internal scope gate still emits records");
    for (const auto& site : plain.call_sites.sites)
        require(!(site.flags & quidra::call_site_flags::recorded_tail), "the scope gate kept a recorded flag");
}

static void signature_shapes_share_one_scope() {
    good(R"(tensor<real32>{N, M} matmul2(tensor<real32>{nat N, nat K} a, tensor<real32>{K, nat M} b)
    return tensor.zeros<real32>([N, M])
auto product = matmul2(tensor.ones<real32>([2, 3]), tensor.ones<real32>([3, 5]))
)");
    good(R"(tensor<real32>{N + M} join_rows(tensor<real32>{nat N} a, tensor<real32>{nat M} b)
    return tensor.zeros<real32>([N + M])
auto rows = join_rows(tensor.ones<real32>([2]), tensor.ones<real32>([3]))
)");
    good(R"(tensor<real32>{nat L = N + M} join_derived(tensor<real32>{nat N} a, tensor<real32>{nat M} b)
    return tensor.zeros<real32>([L])
auto rows = join_derived(tensor.ones<real32>([2]), tensor.ones<real32>([3]))
)");
    good(R"(void following(tensor<real32>{N + 1} a, tensor<real32>{nat N} b)
    print(N)
following(tensor.ones<real32>([4]), tensor.ones<real32>([3]))
)");
    good(R"(void dependent(tensor<real32>{nat L = N + 1} a, tensor<real32>{nat N} b)
    print(L)
dependent(tensor.ones<real32>([4]), tensor.ones<real32>([3]))
)");
    good(R"(void captured(nat N, tensor<real32>{N} value)
    print(N)
captured(3, tensor.ones<real32>([3]))
)");
    good(R"(tensor<real32>{nat(n), 2} keep_shape(int n, tensor<real32>{nat(n), 2} value)
    return value
auto kept = keep_shape(3, tensor.ones<real32>([3, 2]))
)");
    bad_code(R"(void same_width(tensor<real32>{nat N} a, tensor<real32>{N} b)
    print(N)
same_width(tensor.ones<real32>([2]), tensor.ones<real32>([3]))
)", "TENSOR_SHAPE");
    bad_code(R"(void duplicated(tensor<real32>{nat N} a, tensor<real32>{nat N} b)
    print(N)
)", "SHAPE_VARIABLE");
    bad_code(R"(tensor<real32>{nat L} missing(tensor<real32>{nat N} a)
    return a
)", "SHAPE_VARIABLE");
    bad_code(R"(void cycle(tensor<real32>{nat A = B} a, tensor<real32>{nat B = A} b)
    print(A)
)", "SHAPE_VARIABLE");
    bad_code(R"(void missing(tensor<real32>{C} a)
    print(int(0))
)", "SHAPE_VARIABLE");
    bad_code(R"(void wrong_kind(int n, tensor<real32>{n} a)
    print(n)
)", "SHAPE_VARIABLE");
    bad_code(R"(void shadow(nat N, tensor<real32>{nat N} a)
    print(N)
)", "SHADOWING");
    good(R"(tensor<real32>{N, M} matmul2(tensor<real32>{nat N, nat K} a, tensor<real32>{K, nat M} b)
    return tensor.zeros<real32>([N, M])
func<tensor<real32>{I, J}>(tensor<real32>{nat I, nat K}, tensor<real32>{K, nat J}) operation = matmul2
auto result = operation(tensor.ones<real32>([2, 3]), tensor.ones<real32>([3, 5]))
)");
    bad_code(R"(tensor<real32>{N, N} square(tensor<real32>{nat N, N} value)
    return value
func<tensor<real32>{A, B}>(tensor<real32>{nat A, nat B}) operation = square
)", "FUNCTION_REFERENCE_SIGNATURE");
}

static void function_values_consume_managed_parameters() {
    struct Case { const char* type; const char* value; };
    const Case cases[] = {
        {"tensor<real32>", "tensor.zeros<real32>([2, 3, 4])"},
        {"tensor<real32>{2, ...}", "tensor.zeros<real32>([2, 3, 4])"},
        {"int64[]", "[1, 2]"},
        {"(tensor<real32>, int64[])", "(tensor.zeros<real32>([2, 3, 4]), [1, 2])"},
    };
    const auto require = [](bool condition, const char* message) {
        if (!condition) {
            std::cerr << message << "\n";
            std::exit(1);
        }
    };
    for (const auto& test : cases) {
        const std::string type = test.type;
        const std::string source =
            "void read(" + type + " value)\n    return\n"
            "void direct(" + type + " value)\n    return\n"
            "void reference(" + type + " &value)\n    return\n"
            "func<void>(" + type + ") operation = read\n" +
            type + " source = " + test.value + "\n"
            "operation(source)\nread(source)\ndirect(source)\nreference(&source)\n";
        capture_source("function_value_ownership", source);
        const auto verify = [&](const quidra::Compilation& compilation) {
            bool owned_target = false, borrowed_direct = false, reference_parameter = false;
            bool explicit_target = false, indirect_call = false;
            bool shaped_wrapper = false, checked_body = false, forwarding_move = false;
            for (const auto& function : compilation.ir.functions) {
                if (function.name == "read" || function.name == "read.shape_checked") {
                    require(!function.parameters.at(0).borrowed,
                            "function-value target borrows an owned argument");
                    owned_target = true;
                    shaped_wrapper = shaped_wrapper || function.shape_wrapper;
                    checked_body = checked_body || function.name == "read.shape_checked";
                }
                if (function.name == "direct" || function.name == "direct.shape_checked") {
                    require(function.parameters.at(0).borrowed,
                            "ordinary direct function lost borrow inference");
                    borrowed_direct = true;
                }
                if (function.name == "reference" || function.name == "reference.shape_checked") {
                    const auto& parameter = function.parameters.at(0);
                    require(parameter.writable && !parameter.borrowed,
                            "explicit reference parameter changed ownership form");
                    reference_parameter = true;
                }
                for (const auto& block : function.blocks)
                    for (const auto& instruction : block.instructions) {
                        if (const auto* ref = std::get_if<quidra::ir::FunctionRef>(&instruction))
                            explicit_target = explicit_target || ref->function == "read";
                        indirect_call = indirect_call ||
                            std::holds_alternative<quidra::ir::IndirectCall>(instruction);
                        if (function.name == "read" && function.shape_wrapper)
                            if (const auto* load = std::get_if<quidra::ir::LoadLocal>(&instruction);
                                load && load->name == "value") {
                                // The final forwarding load transfers ownership to the
                                // checked body; earlier shape checks merely inspect it.
                                forwarding_move = forwarding_move || load->move_out;
                            }
                    }
            }
            require(owned_target && borrowed_direct && reference_parameter &&
                        explicit_target && indirect_call,
                    "function-value ownership test did not reach every call form");
            require(!shaped_wrapper || (checked_body && forwarding_move),
                    "shape wrapper did not transfer its owned parameter to the checked body");
        };
        verify(quidra::compile(source));
        quidra::CompileOptions options;
        options.lowering.check_every_shape_constraint = true;
        verify(quidra::compile(source, options));
        // The backend's parallel call receipt also requires a consuming target.
        (void)quidra::compile(source + "parallel\n    operation(" + test.value + ")\n");
    }
}

static void shape_entries_preserve_call_paths() {
    const std::string source = R"(tensor<real32>{N, N} square(tensor<real32>{nat N, N} value)
    return value
tensor<real32> relay(tensor<real32> value)
    return square(value)
func<tensor<real32>{A, A}>(tensor<real32>{nat A, A}) operation = square
auto direct = square(tensor.ones<real32>([2, 2]))
auto indirect = operation(tensor.ones<real32>([2, 2]))
)";
    const auto require = [&](bool condition, const char* message) {
        if (!condition) {
            std::cerr << message << "\n";
            std::exit(1);
        }
    };
    const auto verify = [&](const quidra::Compilation& compilation, bool force) {
        bool checked_body = false, wrapper = false, direct = false;
        bool indirect = false, ordinary_reference = false, relay = false;
        for (const auto& function : compilation.ir.functions) {
            checked_body = checked_body || function.name == "square.shape_checked";
            wrapper = wrapper || (function.name == "square" && function.shape_wrapper);
            for (const auto& block : function.blocks) {
                for (const auto& instruction : block.instructions) {
                    if (const auto* call = std::get_if<quidra::ir::Call>(&instruction)) {
                        if (function.entrypoint)
                            direct = direct || call->callee == (force ? "square" : "square.shape_checked");
                        if (function.name == "relay") relay = relay || call->callee == "square";
                        if (function.name == "square")
                            require(call->callee == "square.shape_checked" && call->shape_forwarder,
                                    "shape wrapper does not transparently forward to checked body");
                    }
                    if (function.entrypoint && std::holds_alternative<quidra::ir::IndirectCall>(instruction))
                        indirect = true;
                    if (const auto* reference = std::get_if<quidra::ir::FunctionRef>(&instruction))
                        ordinary_reference = ordinary_reference || reference->function == "square";
                }
            }
        }
        require(checked_body && wrapper, "shaped function has no shared wrapper and checked body");
        require(direct && relay && indirect && ordinary_reference,
                "direct, unknown and function-value entries differ from their proof contracts");
    };
    verify(quidra::compile(source), false);
    quidra::CompileOptions options;
    options.lowering.check_every_shape_constraint = true;
    verify(quidra::compile(source, options), true);
}

int main(){
    string_templates_release_owned_inputs();
    function_values_consume_managed_parameters();
    inspect_contains("int[] x = [1]\nint[] y = [2]\nfor int a = x[], int b = y[]\n    print(a + b)\n", "\"collection_count\":2");
    inspect_contains("int[] x = [1]\nint[] y = [2]\nfor int a = x[], int b = y[]\n    print(a + b)\n", "ForCollectionBinding");
    good("nat[2] expected_shape = [2, 3]\nparallel\n    tensor<real32>{nat N, N + 1} a = tensor.zeros<real32>([2, 3])\n    tensor<real32>{N, N + 1} b = tensor.ones<real32>()\n    print(b.shape() == expected_shape)\n");
    good("int64 size = 2\nparallel\n    size = 3\n    tensor<real32>{nat N = nat(size)} a = tensor.zeros<real32>()\n    print(N)\n");
    llvm_contains("parallel\n    tensor<real32>{nat N, N + 1} value = tensor.zeros<real32>([2, 3])\n    print(value.shape()[0])\n", "define void @quidra_drop_tensor_real32_");
    bad_code("parallel\n    print(int(1))\n    print(int(2))\n", "PARALLEL_CONFLICT");
    bad_code("void f()\n    parallel\n        return\n", "PARALLEL_CONTROL");
    shape_entries_preserve_call_paths();
    signature_shapes_share_one_scope();
    native_call_sites_follow_the_artifact();
    native_tail_records_are_scoped();
    // Slice markers and a statically known step must agree.
    bad_message(
        "string text = \"abcdef\"\n"
        "auto section = text[0:<5:-1]\n",
        "SLICE_STEP", "Slice step -1 cannot be used with ':<'.");
    bad_message(
        "string text = \"abcdef\"\n"
        "auto section = text[5>:0:1]\n",
        "SLICE_STEP", "Slice step 1 cannot be used with '>:'.");
    bad_message(
        "string text = \"abcdef\"\n"
        "auto section = text[0:<5:0]\n",
        "SLICE_STEP", "Slice step 0 is invalid.");
    bad_message(
        "string text = \"abcdef\"\n"
        "auto section = text[0<:>5]\n",
        "SLICE_STEP",
        "Slice markers point one way: '<:' and ':<' ascend, '>:' and ':>' descend.");
    bad_message(
        "string text = \"abcdef\"\n"
        "auto section = text[5>:>0:2]\n",
        "SLICE_STEP", "Slice step 2 cannot be used with '>:'.");
    bad_message(
        "string text = \"abcdef\"\n"
        "auto section = text[0:>5:3]\n",
        "SLICE_STEP", "Slice step 3 cannot be used with ':>'.");
    bad_message(
        "string text = \"abcdef\"\n"
        "auto section = text[0<:5:-2]\n",
        "SLICE_STEP", "Slice step -2 cannot be used with '<:'.");
    // Array slices: a T[] of copies, T[n] included; constant operands over a
    // fixed array are checked against its length.
    good("int[] values = [1, 2, 3]\nint[] part = values[0<:2]\nint[] whole = values[:]\n"
         "int[] back = values[len(values)>:0]\nint8 a = 0\nint[] small = values[a:<int8(2)]\n");
    good("int[3] fixed = [1, 2, 3]\nint[] part = fixed[1:]\nint[] rest = fixed[3:]\n"
         "int[] reversed = fixed[3>:0]\nint[] empty = fixed[3>:3]\n");
    bad_message("int[3] fixed = [1, 2, 3]\nint[] part = fixed[1:3]\n",
                "INDEX_BOUNDS", "Slice 1:3 out of bounds for length 3.");
    bad_message("int[3] fixed = [1, 2, 3]\nint[] part = fixed[4>:0]\n",
                "INDEX_BOUNDS", "Slice 4>:0 out of bounds for length 3.");
    good("int[3] fixed = [1, 2, 3]\nint[] part = fixed[2:0]\n");
    bad_message("int[3] fixed = [1, 2, 3]\nint[] part = fixed[2:0:1]\n",
                "INDEX_BOUNDS", "Slice 2:0:1 out of bounds for length 3.");
    bad_message("int[3] fixed = [1, 2, 3]\nint[] part = fixed[-1<:-1]\n",
                "INDEX_BOUNDS", "Slice -1<:-1 out of bounds for length 3.");
    bad_message("int[] values = [1, 2, 3]\nint[] part = values[0:1, 1:2]\n",
                "INDEX_ARITY", "Arrays and strings take one index or one slice.");
    good("string text = \"abc\"\nstring a = text[1:]\nstring b = text[::-1]\n"
         "string c = text[len(text)>:0]\nstring d = text[0:<1]\nstring e = \"abc\"[3:]\n");
    bad_message("string part = \"abc\"[1:5]\n", "INDEX_BOUNDS",
                "Slice 1:5 out of bounds for length 3.");
    bad_message("string part = \"a\xc3\xa9\"[0:2]\n", "INDEX_BOUNDS",
                "Slice 0:2 out of bounds for length 2.");
    bad_message("int[] values = [1, 2, 3]\nint part = values[0:1]\n",
                "TYPE_MISMATCH", "");
    // Slice assignment (L6).
    good("int[] values = [1, 2, 3, 4]\nvalues[1:2] = [7, 8]\nvalues[len(values)>:0] = values[:]\n"
         "int[4] fixed = [0, 0, 0, 0]\nfixed[0:<4:2] = [1, 2]\n");
    bad_message("int[] values = [1, 2, 3, 4]\nint[2] two = [5, 6]\nvalues[0:2] = two\n",
                "SLICE_ASSIGNMENT", "Slice assignment needs 3 values, got 2.");
    bad_message("int[] values = [1, 2, 3, 4]\nvalues[0:2] = 5\n",
                "TYPE_MISMATCH", "Slice assignment needs an array.");
    bad_message("string text = \"abc\"\ntext[0:1] = \"xy\"\n",
                "INVALID_ASSIGNMENT", "A string slice cannot be assigned");
    bad_message("const int[] values = [1, 2, 3]\nvalues[0:1] = [4, 5]\n",
                "WRITE_CAPABILITY", "");
    // text.slice is gone: strings are sliced with text[a:<b].
    bad_message("string text = \"abc\"\nstring part = text.slice(0, 1)\n", "UNKNOWN_MEMBER",
                "Type 'string' has no method 'slice'.");
    // Tensor and bin slices take ':<' with the meaning of their ':'.
    good("bin data = bin.fill(8, 0)\nbin head = data[0:<4]\nbin same = data[0:4]\nbin tail = data[:<8]\n");
    good("tensor<real32> t = tensor.zeros<real32>([4, 3])\nauto part = t[0:<2, 1:<3]\n"
         "auto stepped = t[0:<4:2]\n");
    good("bin data = bin.fill(8, 0)\nbin a = data[1<:4]\nbin b = data[len(data)>:0]\n"
         "bin c = data[5:>1]\nbin d = data[1:3]\n");
    good("tensor<real32> t = tensor.zeros<real32>([4])\nauto a = t[0<:3]\nauto b = t[0:3]\n"
         "auto c = t[1<:<3:2]\n");
    bad_message("tensor<real32> t = tensor.zeros<real32>([4])\nauto part = t[3:>0]\n",
                "SLICE_STEP", "Tensor slices ascend");
    bad_message("tensor<real32> t = tensor.zeros<real32>([4])\nauto part = t[3>:0]\n",
                "SLICE_STEP", "Tensor slices ascend");
    bad_message("bin data = bin.fill(8, 0)\nbin part = data[0:4:2]\n",
                "SLICE_STEP", "bin slices do not take a step.");
    bad_message("bin data = bin.fill(8, 0)\nbin part = data[0<:>4]\n",
                "SLICE_STEP", "Slice markers point one way");
    // quidra fmt never separates a slice marker from its colon. (A negative
    // operand gets the space every '-' after a colon or a comparison token
    // gets; the marker stays next to its colon.)
    {
        const std::string marked =
            "int[] values = [1, 2, 3, 4, 5]\n"
            "int n = 5\n"
            "int[] a = values[0<:5]\n"
            "int[] b = values[n>:0]\n"
            "int[] c = values[0:<n]\n"
            "int[] d = values[4:>0: -2]\n"
            "int[] e = values[0<:<n:2]\n"
            "int[] f = values[n>:> -1]\n"
            "int[] g = values[0 <:5]\n"
            "int[] h = values[0<: 5]\n"
            "int[] i = values[if n > 2 then 0 else n>:0]\n"
            "int[] j = values[-1<:2]\n";
        const auto formatted = quidra::format_source(marked);
        if (formatted != marked) {
            std::cerr << "quidra fmt changed slice markers:\n" << formatted;
            std::exit(1);
        }
        const auto respaced = quidra::format_source("int[] d = values[4:>0:-2]\nint[] f = values[n>:>-1]\n");
        if (respaced != "int[] d = values[4:>0: -2]\nint[] f = values[n>:> -1]\n") {
            std::cerr << "quidra fmt moved a slice marker:\n" << respaced;
            std::exit(1);
        }
    }
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
    const_values_cross_the_c_boundary_as_borrows();
    const_references_are_rejected();
    mutation_markers_parse();
    mutation_markers_are_required();
    sorted_takes_a_descending_flag();
    comparison_chains_keep_one_direction();
    private_declarations_stay_in_their_module();
    brace_shapes_and_shape_variables();
    symbolic_tensor_extents();
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
 good(R"(tensor<real32> tracked = tensor.ones<real32>([2]).track()
tensor<real64> converted = real64(tracked)
(converted.gather([0], []) * real64(2)).backward(&tracked)
tensor<real32> gradient = tracked.grad
print(gradient[0].item())
print(NL)
)");
 bad_code(R"(tensor<real32> loss = tensor.ones<real32>([]).track()
tensor<int32> target = tensor.ones<int32>([])
loss.backward(&target)
)", "TYPE_MISMATCH");
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
     "map.Map<int, int> values = map.Map<int, int>()\n.values.set(1, 2)\nauto value = values.get(1)\n",
     "call ptr @quidra_format_signed");
 llvm_not_contains(
     "set.Set<nat64> values = set.Set<nat64>()\nnat64 key = nat64(7)\n.values.add(key)\nbool present = values.has(key)\n",
     "call ptr @quidra_format_unsigned");
 llvm_not_contains(
     "map.Map<int, int> values = map.Map<int, int>()\nint key = int(7)\n.values.set(key, 2)\nauto value = values.get(key)\n",
     "call ptr @quidra.int.text");
 // Standard map/set slot capacities are compiler-owned powers of two, so hot
 // probing uses masking rather than signed remainder and branchy wraparound.
 llvm_function_not_contains(
     "map.Map<int, int> values = map.Map<int, int>()\n.values.set(1, 2)\nauto value = values.get(1)\n",
     "___slot_of(", "srem i64");
 llvm_function_not_contains(
     "set.Set<int> values = set.Set<int>()\n.values.add(1)\nbool present = values.has(1)\n",
     "___slot_of(", "srem i64");
 // Map/Set scalar fields have compiler-owned initializers. Array-copy paths
 // inside these methods may still need element initialization checks, so scope
 // this regression to the typed-IR address path used by scalar compound updates.
 ir_function_not_contains(
     "map.Map<int, int> values = map.Map<int, int>()\n.values.set(1, 2)\n",
     ".set(", "address.load");
 ir_function_not_contains(
     "map.Map<int, int> values = map.Map<int, int>()\n.values.set(1, 2)\n",
     ".set(", "address.store");

 // Compiler-owned +1 induction variables are bounded by their loop
 // conditions, so they do not need a checked-overflow branch on every iteration.
 llvm_function_not_contains(R"(void walk_range()
    for 0 <= nat i < 8
        print(i)
        print(NL)

walk_range()
)", "@n_walk_range(", "@llvm.sadd.with.overflow.i64");
 llvm_function_not_contains(R"(void walk_array(const int[] values)
    for auto value = values[]
        print(value)
        print(NL)

int[] values = array(8, fill = 1)
walk_array(values)
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
 ir_contains(R"(for 0 <= nat i < 2
    string[] fields = [i.string(), " ", NL]
    string line = fields.join("")
    print(line)
    print(NL)
)", "string.build");
 llvm_contains(R"(string content = ""
int written = 0
for 0 <= nat i < 2
    string[] fields = [i.string(), " ", i.string(), NL]
    string line = fields.join("")
    content = content + line
    written += int(len(line))
print(written)
print(NL)
)", "@quidra_string_build_append_move");
 ir_contains(R"(string content = ""
int written = 0
for 0 <= nat i < 2
    string[] fields = [i.string(), " ", i.string(), NL]
    string line = fields.join("")
    content = content + line
    written += int(len(line))
print(written)
print(NL)
)", "string.build_append");
 llvm_contains(R"(string content = ""
int written = 0
for 0 <= nat i < 2
    string[] fields = [i.string(), " ", i.string(), NL]
    string line = fields.join("")
    content = content + line
    written += int(len(line))
print(written)
print(NL)
)", "call ptr @quidra_string_build_append_move_unique_direct");
 llvm_contains(R"(string content = ""
int written = 0
for 0 <= nat i < 2
    string[] fields = [i.string(), " ", i.string(), NL]
    string line = fields.join("")
    content = content + line
    written += int(len(line))
print(written)
print(NL)
)", "store i8 4");
 llvm_not_contains(R"(string content = ""
int written = 0
for 0 <= nat i < 2
    string[] fields = [i.string(), " ", i.string(), NL]
    string line = fields.join("")
    content = content + line
    written += int(len(line))
print(written)
print(NL)
)", "call i64 @quidra_string_build_append_last_length");
 llvm_contains(R"(for 0 <= nat i < 2
    string[] fields = [i.string(), " ", NL]
    string line = fields.join("")
    print(line)
    print(NL)
)", "@quidra_string_build");
 ir_contains(R"(string text = "a b"
for 0 <= nat i < len(text)
    if text[i] == " "
        print(i)
        print(NL)
)", "string.index_ascii_compare");
 ir_contains(R"(string text = "a b a"
int64 spaces = 0
for 0 <= nat i < len(text)
    if text[i] == " "
        spaces += 1
print(spaces)
print(NL)
)", "string.ascii_count_prefix");
 llvm_contains(R"(string text = "a b a"
int64 spaces = 0
for 0 <= nat i < len(text)
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
    error
        print("not a number")
        print(NL)
)", "parse.direct");
 // A direct parse still emits a constant error record. Its type detail must
 // keep its backing string alive until the message and details are interned.
 llvm_contains(R"(match int64.parse("42")
    int64 value
        print(value)
        print(NL)
    error
        print("not a number")
        print(NL)
)", "cannot parse numeric input as int64");
 // A bound error is read, so the match goes through the parse whose error
 // block knows why it failed.
 ir_contains(R"(match int64.parse("42")
    int64 value
        print(value)
        print(NL)
    error problem
        print(problem)
        print(NL)
)", "= parse %1 : int64 -> error | int64");
 ir_contains(R"(int64 parse_decimal(string text)
    match int64.parse(text)
        int64 value
            return value
        error problem
            process.exit(1)

string line = "12 34"
for 0 <= nat i < 1
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
for 0 <= nat i < 1
    string[] fields = line.split(" ")
    int64 left = parse_decimal(fields[0])
    int64 right = parse_decimal(fields[1])
    print(left + right)
    print(NL)
)", "call i1 @__quidra_string_parse_two_signed_fast");
 ir_contains(R"(string line = "12 34"
for 0 <= nat i < 1
    string[] fields = line.split(" ")
    int64 left = int64.parse(fields[0])
    int64 right = int64.parse(fields[1])
    print(left + right)
    print(NL)
)", "string.parse_two_signed");
 llvm_contains(R"(string line = "12 34"
for 0 <= nat i < 1
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
for 0 <= nat i < 1
    string[] fields = line.split(" ")
    int64 left = parse_decimal(fields[0])
    int64 right = parse_decimal(fields[1])
    print(left + right)
    print(NL)
)", "call i1 @quidra_string_parse_two_signed(");
 ir_contains(R"(string text = "a b"
for auto part = text.split(" ")[]
    print(part)
    print(NL)
)", "string.split_iter.begin");
 ir_contains(R"(string text = "a b"
for 0 <= nat outer < 1
    string[] parts = text.split(" ")
    int index = 0
    for auto part = parts[]
        print(part)
        print(NL)
        index += 1
)", "string.split_iter.begin");
 llvm_contains(R"(string text = "a b"
string[] parts = text.split(" ")
for auto part = parts[]
    print(part)
    print(NL)
)", "call ptr @quidra_string_split_iter_begin_move");
 llvm_not_contains(R"(string text = "a b"
string[] parts = text.split(" ")
for auto part = parts[]
    print(part)
    print(NL)
print(text)
print(NL)
)", "call ptr @quidra_string_split_iter_begin_move");

 // Nonnegative modulo invariants prove hot integer arithmetic safe without
 // weakening the default overflow semantics.
 ir_contains(R"(int bounded_hash(const nat8[] data, int size)
    int h = 0
    for 0 <= int i < size
        h = (h * 131 + int(data[i])) % 1000000007
    return h
)", "no-overflow");
 llvm_contains(R"(int bounded_hash(const nat8[] data, int size)
    int h = 0
    for 0 <= int i < size
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
 // Arithmetic produces a fixed-width real before its return is boxed as a
 // success case of T | error. The finite check must retain that real dtype.
 for (const auto& [kind, dtype] :
      std::vector<std::pair<std::string, int>>{{"real16", 12}, {"real16b", 13},
                                            {"real32", 10}, {"real64", 9}}) {
     llvm_contains(kind + " | error root(" + kind + " value)\n"
                   "    return value ^ 0.5\n",
                   "call void @quidra_runtime_real_fail(i32 " + std::to_string(dtype) + ",");
 }

 // Proven dynamic-array loop bounds remove runtime slot checks only when the
 // array length relation is statically preserved.
 llvm_not_contains(R"(int[] data = array(8, fill = 0)
int size = int(len(data))
int total = 0
for 0 <= int i < size
    total += data[i]
for 0 <= int i < size - 1
    total += data[i + 1]
for 0 <= int i < size
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
for 0 <= int i < size
    data = array(0, fill = 0)
    print(data[i])
    print(NL)
)", "call ptr @quidra_array_slot(ptr");
 good("tensor<T> stored<T: floating>(tensor<real32> value)\n    return T(value)\n"
      "tensor<real32> source = tensor.ones<real32>([1])\n"
      "tensor<real16> a = stored<real16>(source)\n"
      "tensor<real16b> b = stored<real16b>(source)\n");
 good("T stored<T: floating>(real32 value)\n    return T(value)\n"
      "real16 a = stored<real16>(real32(1))\n"
      "real16b b = stored<real16b>(real32(1))\n");
 // User-class bindings require generation before any instance access.
 good("class Point(int x)\n    int y = 2\nPoint a(5)\nprint(a.x + a.y)\n");
 good("class Point(int x)\n    int y = 2\nPoint a\na = Point(5)\nprint(a.x + a.y)\n");
 good("class Point(int x)\nPoint original = Point(5)\nPoint copy = original\nprint(copy.x)\n");
 bad_code("class Point(int x)\nPoint a\nprint(a.x)\n", "UNINITIALIZED");
 bad_code("class Point(int x)\nPoint a\na.x = 5\n", "UNINITIALIZED");
 bad_code("class Point(int x)\n    int sum()\n        return x\nPoint a\nprint(a.sum())\n", "UNINITIALIZED");
 bad_code("class Point(int x)\nPoint a\nPoint &alias = &a\n", "UNINITIALIZED");
 good("class Empty()\nEmpty value()\n    return Empty()\nEmpty a()\nEmpty b = value()\n");
 good("class Box<T>(T value)\nBox<int> a(5)\nprint(a.value)\n");
 // Joining parallel work invalidates proofs without unbalancing enclosing loops.
 good(R"(int identity(int value)
    return value
void first()
    for 0 <= int round < 3
        int left
        int right
        parallel
            left = identity(1)
            right = identity(2)
        int sum = left + right
void second()
    int value = 0
first()
second()
)");
 good(R"(void fill()
    int[2] data
    for 0 <= int round < 3
        parallel for index < 2
            data[index] = index
void after()
    int value = 0
fill()
after()
)");
 llvm_contains(R"(int identity(int value)
    return value
int[] data = array(8, fill = 0)
int size = int(len(data))
for 0 <= int index < size
    int value
    parallel
        value = identity(1)
    print(data[index])
)", "call ptr @quidra_array_slot(ptr");
 llvm_not_contains(R"(int identity(int value)
    return value
int value
parallel
    value = identity(1)
int[] data = array(8, fill = 0)
int size = int(len(data))
for 0 <= int index < size
    print(data[index])
)", "call ptr @quidra_array_slot(ptr");
 llvm_contains(R"(int[] data = array(1, fill = 7)
int[] partial = array(1)
while true
    parallel
        data = partial
    break
print(data[0])
)", "call void @quidra_init_check(ptr %arr.get.slot.");
 llvm_contains(R"(int[] data = array(1, fill = 7)
int[] partial = array(1)
if main
    parallel
        data = partial
print(data[0])
)", "call void @quidra_init_check(ptr %arr.get.slot.");
 llvm_contains(R"(int[] data = array(1, fill = 7)
int[] partial = array(1)
while true
    data = partial
    break
print(data[0])
)", "call void @quidra_init_check(ptr %arr.get.slot.");
 llvm_contains(R"(int[] data = array(1, fill = 7)
int[] partial = array(1)
bool stop = true
while stop
    if stop
        data = partial
        break
print(data[0])
)", "call void @quidra_init_check(ptr %arr.get.slot.");
 llvm_not_contains(R"(int[] data = array(1, fill = 7)
while true
    break
print(data[0])
)", "call void @quidra_init_check(ptr %arr.get.slot.");
 // dynamic array loop bounds lower to proven slots only while the length relation
 // is loop invariant; replacing the array inside the loop keeps the checked path.
 ir_contains(R"(string[] values = ["a", "b", "c"]
for auto value = values[]
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
func<int>(int) callback = fib
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
 good(R"(class TensorState()
    tensor<real32> value

class NestedState()
    TensorState value
)");

 good("extern void scalar_abi(int8 a, int16 b, int32 c, int64 d, nat8 e, nat16 f, nat32 g, nat64 h, real32 i, real64 j, bool k) = \"scalar_abi\"\n");
 llvm_contains("extern bool c_bool(bool value) = \"c_bool\"\n", "declare zeroext i1 @c_bool(i1 zeroext)");
 llvm_contains("extern int8 c_i8(int8 value) = \"c_i8\"\n", "declare signext i8 @c_i8(i8 signext)");
 llvm_contains("extern int16 c_i16(int16 value) = \"c_i16\"\n", "declare signext i16 @c_i16(i16 signext)");
 llvm_contains("extern nat8 c_u8(nat8 value) = \"c_u8\"\n", "declare zeroext i8 @c_u8(i8 zeroext)");
 llvm_contains("extern nat16 c_u16(nat16 value) = \"c_u16\"\n", "declare zeroext i16 @c_u16(i16 zeroext)");
 llvm_contains("extern int8 c_i8(int8 value) = \"c_i8\"\nint8 x = 1\nint8 y = c_i8(x)\n", "call signext i8 @c_i8(i8 signext");
 good("extern int32 c_text(const string text) = \"c_text\"\n");
 good("extern int32 c_bin(const bin data) = \"c_bin\"\n");
 ir_contains("extern int32 c_text(const string text) = \"c_text\"\n", "function c_text(const string &text) -> int32 = \"c_text\"");
 llvm_contains("extern int32 c_text(const string text) = \"c_text\"\n", "declare i32 @c_text(ptr nocapture nonnull readonly, i64)");
 llvm_contains("extern int32 c_bin(const bin data) = \"c_bin\"\n", "declare i32 @c_bin(ptr nocapture nonnull readonly, i64)");
 llvm_contains("extern int32 c_bin_mut(bin &data) = \"c_bin_mut\"\n", "declare i32 @c_bin_mut(ptr nocapture nonnull, i64)");
 good("extern int32 c_tensor(const tensor<real32> input, tensor<real32> &output) = \"c_tensor\"\n");
 llvm_contains(
     "extern int32 c_tensor(const tensor<real32> input, tensor<real32> &output) = \"c_tensor\"\n"
     "tensor<real32> input = tensor.ones<real32>([1])\n"
     "tensor<real32> output = tensor.zeros<real32>([1])\n"
     "int32 status = c_tensor(input, &output)\n",
     "call i32 @c_tensor(ptr");
 good("extern int32 c_tensor_array(const tensor<real32>[] input, tensor<real32>[] &output) = \"c_tensor_array\"\n");
 llvm_contains(
     "extern int32 c_tensor_array(const tensor<real32>[] input, tensor<real32>[] &output) = \"c_tensor_array\"\n"
     "tensor<real32>[] input = [tensor.ones<real32>([1])]\n"
     "tensor<real32>[] output = [tensor.zeros<real32>([1])]\n"
     "int32 status = c_tensor_array(input, &output)\n",
     "@quidra_tensor_array_borrow_begin");
 llvm_contains(
     "extern int32 c_tensor_array(tensor<real32>[] &output) = \"c_tensor_array\"\n"
     "tensor<real32>[] output = []\nint32 status = c_tensor_array(&output)\n",
     "@quidra_tensor_array_validate_package");
 llvm_contains("extern int32 c_tensor_array(const tensor<real32>[] input) = \"c_tensor_array\"\n",
               "declare i32 @c_tensor_array(ptr nocapture nonnull readonly)");
 bad_code("extern int32 c_tensor_array(tensor<real32>[] input) = \"c_tensor_array\"\n", "FFI_REFERENCE");
 bad_code("extern int32 c_tensor_array(const tensor<real32>[2] input) = \"c_tensor_array\"\n", "FFI_TYPE");
 bad_code("extern int32 c_tensor_array(const tensor<real32>{1}[] input) = \"c_tensor_array\"\n", "FFI_TYPE");
 bad_code("extern int32 c_tensor_array(const tensor<real32>[][] input) = \"c_tensor_array\"\n", "FFI_TYPE");
 bad_code("extern int32 c_other_array(const int32[] input) = \"c_other_array\"\n", "FFI_TYPE");
 bad_code("extern int32 c_tensor_value(tensor<real32> input) = \"c_tensor_value\"\n", "FFI_REFERENCE");
 llvm_contains("extern int32 c_text(const string text) = \"c_text\"\nstring value = \"abc\"\nint32 result = c_text(value)\n", "ffi.borrowed.value");
 llvm_contains("extern int32 c_text(const string text) = \"c_text\"\nstring value = \"abc\"\nint32 result = c_text(value)\n", "call i64 @strlen(ptr");
 llvm_contains("extern int32 c_bin(const bin data) = \"c_bin\"\nbin value = bin.fill(24, 1)\nint32 result = c_bin(value)\n", "ffi.bin.length");
 llvm_contains("extern int32 c_bin(const bin data) = \"c_bin\"\nbin value = bin.fill(24, 1)\nint32 result = c_bin(value)\n", "call i32 @c_bin(ptr nocapture nonnull readonly");
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
 good("extern int64 c_apply(func<int64>(int64) callback, int64 value) = \"c_apply\"\n");
 llvm_contains("extern int64 c_apply(func<int64>(int64) callback, int64 value) = \"c_apply\"\n", "declare i64 @c_apply(ptr, i64)");
 llvm_contains(R"(int64 twice(int64 value)
    return value * 2
extern int64 c_apply(func<int64>(int64) callback, int64 value) = "c_apply"
int result = int(c_apply(twice, 21))
)", "call i64 @c_apply(ptr");
 bad_code("extern int64 unsafe_callback(func<int8>(int8) callback) = \"unsafe_callback\"\n", "FFI_CALLBACK_TYPE");
 bad_code("extern int64 unsafe_callback(func<string>(int64) callback) = \"unsafe_callback\"\n", "FFI_CALLBACK_TYPE");
 bad_code("extern int32 mutable_text(string &text) = \"mutable_text\"\n", "FFI_REFERENCE");
 good("extern int32 const_value(const string text) = \"const_value\"\n");
 bad_code("extern int32 c_puts(const string text) = \"puts\"\n", "FFI_SYMBOL_CONFLICT");
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

 good("int identity(int value)\n    return value\n"
      "fn<int>(int) legacy = identity\n"
      "func<int>(int) current = legacy\n"
      "int result = current(3)\n");
 good(R"(int twice(int value)
    return value * 2

int apply(func<int>(int) operation, int value)
    return operation(value)

func<int>(int) operation = twice
int a = operation(3)
int b = apply(operation, 4)
print(a)
print(NL)
print(b)
print(NL)
)");
 ir_contains(R"(int twice(int value)
    return value * 2
func<int>(int) operation = twice
int result = operation(3)
)", "fn.ref twice");
 ir_contains(R"(int twice(int value)
    return value * 2
func<int>(int) operation = twice
int result = operation(3)
)", "call.indirect");
 llvm_contains(R"(int twice(int value)
    return value * 2
func<int>(int) operation = twice
int result = operation(3)
)", "select i1 true, ptr @n_twice, ptr null");
 bad_code(R"(int twice(int value)
    return value * 2
func<real64>(int) operation = twice
)", "FUNCTION_REFERENCE_SIGNATURE");
 bad_code(R"(int mutate(int &value)
    value = value + 1
    return value
func<int>(int) operation = mutate
)", "FUNCTION_REFERENCE_SIGNATURE");
 bad_code(R"(extern int32 foreign(int32 value) = "foreign"
func<int32>(int32) operation = foreign
)", "FUNCTION_REFERENCE_EXTERN");
 bad_code(R"(int twice(int value)
    return value * 2
auto operation = twice
)", "FUNCTION_REFERENCE_CONTEXT");
 bad_code(R"(int twice(int value)
    return value * 2
func<int>(int) left = twice
func<int>(int) right = twice
bool same = left == right
)", "TYPE_MISMATCH");
 bad_code(R"(void first()
    print("first")
void second()
    print("second")
parallel
    first()
    second()
)", "PARALLEL_CONFLICT");
 llvm_contains(R"(int first_value()
    return 20
int second_value()
    return 22
int first
int second
parallel
    first = first_value()
    second = second_value()
int sum = first + second
)", "@quidra_parallel_block");
 llvm_contains(R"(int echo(int value)
    return value

int result
parallel
    result = echo(100)
)", "@quidra_parallel_call_begin");
 llvm_contains(R"(nat echo(nat value)
    return value

nat result
parallel
    result = echo(100)
)", "@quidra_parallel_call_begin");
 llvm_contains(R"(real echo(real value)
    return value

real result
parallel
    result = echo(100.0)
)", "@quidra_parallel_call_begin");
 llvm_contains(R"(real echo(real value)
    return value

real result
parallel
    result = echo(0.000000000000000000000000000000000000000000001)
)", "@quidra_parallel_call_begin");
 llvm_contains(R"(nat[] input = [1, 2]
int[] output
parallel
    output = int(input)
print(output[0])
print(NL)
)", "@quidra_array_cast_");
 llvm_contains(R"(nat[2] input = [1, 2]
int[2] output
parallel
    output = int(input)
print(output[0])
print(NL)
)", "@quidra_array_cast_");
 llvm_contains(R"(nat[2][2] input = [[1, 2], [3, 4]]
int[2][2] output
parallel
    output = int(input)
print(output[0][0])
print(NL)
)", "@quidra_array_cast_");
 llvm_contains(R"(int[] input = [1, 2]
nat[] output
parallel
    output = nat(input)
print(output[0])
print(NL)
)", "@quidra_array_cast_");
 good(R"(int task = 1
int atomic = 2
int value = task + atomic
)");
 bad_code(R"(task.all([])
)", "UNKNOWN_NAME");
 bad_code(R"(atomic.counter(0)
)", "UNKNOWN_NAME");
 good(R"(class Holder this(local int32 &value_initial)
    int32 &value

    &this.value = &value_initial
int32 storage = 5
Holder first = Holder(&storage)
Holder[] holders = [first]
Holder second = holders[0]
bool same = &first.value == &second.value
second.value = int32(9)
int32 observed = first.value
)");
 good(R"(class Holder this(local int &source)
    int &value

    &this.value = &source
    void .replace(int next)
        this.value = next
int read(Holder holder)
    return holder.value
void .write(Holder holder)
    .holder.replace(17)
int source = 1
Holder holder = Holder(&source)
int observed = read(holder)
.write(holder)
)");
 bad_code(R"(class Holder()
    int &value
Holder holder = Holder()
int observed = holder.value
)", "UNBOUND_REFERENCE");
 good(R"(class Holder()
    int &value
int read(Holder holder)
    return holder.value
Holder holder = Holder()
int observed = read(holder)
)");
 good(R"(class Cell()
    int value
int ref = 1
Cell cell = Cell()
cell.value = ref
)");
 bad_code(R"(ref.Cell<int> removed
)", "UNKNOWN_GENERIC");
 bad_code(R"(func<int>(int) operation
int result = operation(1)
)", "UNINITIALIZED");
 good(R"(int twice(int value)
    return value * 2
int apply_ref(const func<int>(int) operation, int value)
    return operation(value)
func<int>(int) operation = twice
int result = apply_ref(operation, 4)
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
for auto &value = &values[]
    value = 7
for 0 <= nat i < 3 step 1
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
 R"(class PrivateCounter this()
    private int value = 0

    private void .increment_raw()
        this.value = this.value + 1

    void .increment()
        .increment_raw()

    int get()
        return this.value

PrivateCounter counter = PrivateCounter()
.counter.increment()
print(counter.get())
print(NL)
)",
 R"(class Point this(local int x_value, local int y_value)
    int x
    int y


    this.x = x_value
    this.y = y_value

    int sum()
        return this.x + this.y

class LabeledPoint this(local Point point_value, local string label_value)
    Point point
    string label


    this.point = point_value
    this.label = label_value

    int sum()
        return this.point.sum()

class OffsetPoint this(local Point point_value, local int offset_value)
    Point point
    int offset


    this.point = point_value
    this.offset = offset_value

    int sum()
        return this.point.sum() + this.offset

class Box this(local Point point_value)
    Point point


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
 R"(class PartialPoint()
    int x
    int y

PartialPoint p = PartialPoint()
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
 R"(class ResetCounter this()
    int value

    void .reset()
        this.value = 0

    void .increment()
        this.value = this.value + 1

ResetCounter counter = ResetCounter()
.counter.reset()
.counter.increment()
print(counter.value)
print(NL)
)",
 R"(class RefPoint this(local int x_value)
    int x
    int y


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
 R"(class DefaultPoint this(local int x_value = 1)
    int x = 1
    int y = 2


    this.x = x_value

    int sum()
        return this.x + this.y

class DefaultOffset this()
    DefaultPoint point = DefaultPoint()
    int offset = 1

    int sum()
        return this.point.sum() + this.offset

DefaultPoint a = DefaultPoint()
a.x = 1
a.y = 2
DefaultPoint b = DefaultPoint(1)
bool same = a == b
int[] left = [1, 2, 3]
int[] right = [1, 2, 3]
bool arrays_same = left == right
DefaultOffset shifted = DefaultOffset()
print(a.sum())
print(NL)
print(shifted.sum())
print(NL)
print(same)
print(NL)
print(arrays_same)
print(NL)
)",
 R"(class ArrayEqualityPoint this(local int x_value, local int y_value)
    int x
    int y


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
 R"(class DefaultBag()
    int[] values = [1]

DefaultBag a = DefaultBag()
DefaultBag b = DefaultBag()
a.values[0] = 9
print(b.values[0])
print(NL)
)",
 R"(class Box<T> this(local T value_value)
    T value


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
 R"(class GenericBase<T> this(local T value_value)
    T value


    this.value = value_value

    T read()
        return this.value

class GenericHolder<T> this(local GenericBase<T> base_value)
    GenericBase<T> base


    this.base = base_value

    T read()
        return this.base.read()

GenericHolder<int> child = GenericHolder<int>(GenericBase<int>(9))
print(child.read())
print(NL)
)",
 R"(class GenericMethod()
    T identity<T>(T value)
        return value

GenericMethod g = GenericMethod()
print(g.identity<int>(8))
print(NL)
)",
 R"(class GenericParent()


    return
    T echo<T>(T value)
        return value

class GenericChild this(local GenericParent parent_value)
    GenericParent parent


    this.parent = parent_value

    T echo<T>(T value)
        return this.parent.echo<T>(value)

GenericChild child = GenericChild(GenericParent())
print(child.echo<int>(11))
print(NL)
)",
 R"(class NestedBox<T> this(local T value_value)
    T value


    this.value = value_value

NestedBox<NestedBox<int>> outer = NestedBox<NestedBox<int>>(NestedBox<int>(12))
print(outer.value.value)
print(NL)
)",
 R"(class OneArgMethod()
    T choose<T>(T value)
        return value

class TwoArgMethod()
    T choose<T, U>(T value, U ignored)
        return value

OneArgMethod one = OneArgMethod()
print(one.choose<int>(13))
print(NL)
)",
 R"(class GoodGenericMethod()
    T route<T>(T value)
        return value

class UnusedGenericMethod()
    T route<T>(T value)
        return value + 1

GoodGenericMethod good = GoodGenericMethod()
print(good.route<string>("ok"))
print(NL)
)",
 R"(class GenericRouter()


    return
    T pass<T>(T value)
        return value

class RouterFactory()


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
 R"(class ReturnModel this(local real64 bb_value)
    real64 bb


    this.bb = bb_value

ReturnModel build_model()
    return ReturnModel(2.0)

ReturnModel model = build_model()
print(model.bb)
print(NL)
)",
 R"(class ReturnPoint this(local int x_value, local int y_value = 0)
    int x
    int y


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
 R"(class ForwardProduct this(local int value_value)
    int value


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
 R"(class MethodProduct this(local int value_value)
    int value


    this.value = value_value

class MethodFactory()
    MethodProduct outer()
        return inner()

    MethodProduct inner()
        return MethodProduct(9)

MethodFactory factory = MethodFactory()
MethodProduct product = factory.outer()
print(product.value)
print(NL)
)",
 R"(class NestedData()
    real64[] ys

class NestedOwner this()
    NestedData data

    real64 .first()
        this.data.ys = [3.0]
        return this.data.ys[0]

    void .initialize()
        this.data.ys = [4.0]

NestedData empty = NestedData()
NestedOwner owner = NestedOwner()
owner.data = empty
print(.owner.first())
print(NL)
.owner.initialize()
print(owner.data.ys[0])
print(NL)
)",
 R"(class ReplaceInner this(local int x_value, local int y_value)
    int x
    int y


    this.x = x_value
    this.y = y_value

class ReplaceOuter this(local ReplaceInner inner_value)
    ReplaceInner inner


    this.inner = inner_value

    void .reset()
        ReplaceInner replacement = ReplaceInner(5, 6)
        this.inner = replacement

ReplaceOuter outer = ReplaceOuter(ReplaceInner(1, 2))
.outer.reset()
print(outer.inner.x)
print(NL)
)",
 R"(class ConditionalInner this(local int x_value, local int y_value)
    int x
    int y


    this.x = x_value
    this.y = y_value

class ConditionalOuter this(local ConditionalInner inner_value)
    ConditionalInner inner


    this.inner = inner_value

    void .maybe_reset(bool replace)
        if replace
            ConditionalInner replacement = ConditionalInner(5, 6)
            this.inner = replacement

ConditionalOuter outer = ConditionalOuter(ConditionalInner(1, 2))
.outer.maybe_reset(false)
print(outer.inner.x)
print(NL)
)",
 R"(class RepairInner this(local int x_value, local int y_value)
    int x
    int y


    this.x = x_value
    this.y = y_value

class RepairOuter this(local RepairInner inner_value)
    RepairInner inner


    this.inner = inner_value

    void .reset()
        RepairInner replacement = RepairInner(5, 6)
        this.inner = replacement

RepairOuter outer = RepairOuter(RepairInner(1, 2))
.outer.reset()
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
bin slice = data[0:<8]
nat8[] decoded = nat8[](data)
bin copy = data
copy[0] = bin.fill(1, 1)
bool same = data == copy
for auto value = data[]
    bin x = value
for auto &value = &copy[]
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
        for auto bit = bits[]
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

 good(R"(class Pair this(local int a_value, local int b_value)
    int a
    int b


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

 const std::string const_ir_surface = R"(void inspect(const int value)
    print(value)
    print(NL)

void modify(int &value)
    value = 1

int data = 0
inspect(data)
modify(&data)
)";
 ir_contains(const_ir_surface, "function inspect(const int value)");
 ir_contains(const_ir_surface, "function modify(int &value)");
 llvm_contains(const_ir_surface, "define void @n_inspect(i64 %arg.value)");
 llvm_contains(const_ir_surface, "define void @n_modify(ptr nocapture nonnull %arg.value)");

 const std::string local_const_reference_ir = R"(int data = 1
const int view = data
int &writer = &data
const int frozen = 3
print(view)
print(NL)
writer = 2
print(frozen)
print(NL)
)";
 ir_contains(local_const_reference_ir, "local $local.view.1 : int");
 ir_contains(local_const_reference_ir, "reference.bind");
 inspect_no_source_contains(
     local_const_reference_ir,
     "\"inferred_type\":\"int\",\"authority\":\"read_write_reference\"");
 inspect_no_source_contains(
     local_const_reference_ir,
     "\"inferred_type\":\"int\",\"authority\":\"const_value\"");
 inspect_no_source_contains(
     const_ir_surface,
     "\"parameter_authority\":{\"value\":\"const_value\"}");
 inspect_no_source_contains(
     const_ir_surface,
     "\"parameter_authority\":{\"value\":\"read_write_reference\"}");
 bad_code(R"(int data = 1
const int view = data
int &writer = &view
)", "REFERENCE_BINDING");
 bad_code(R"(int data = 1
const int view = data
view = 2
)", "WRITE_CAPABILITY");
 bad_code(R"(int data = 1
int other = 2
const int view = data
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
 R"(class PartialReturn this(local int x_value)
    int x
    int y


    this.x = x_value

PartialReturn make_partial()
    return PartialReturn(1)

PartialReturn p = make_partial()
print(p.y)
print(NL)
)",
 R"(class ReplaceInnerBad this(local int x_value, local int y_value)
    int x
    int y


    this.x = x_value
    this.y = y_value

class ReplaceOuterBad this(local ReplaceInnerBad inner_value)
    ReplaceInnerBad inner


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
 R"(class ConditionalInnerBad this(local int x_value, local int y_value)
    int x
    int y


    this.x = x_value
    this.y = y_value

class ConditionalOuterBad this(local ConditionalInnerBad inner_value)
    ConditionalInnerBad inner


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
 "int f(int x = 1, int y)\n    return y\n", "void f(int &x = 1)\n    return\n", "int f(int x, int y = x)\n    return y\n",
 "int f(int x)\n    return x\nprint(f(x: 1))\n", "int f(int x, int y)\n    return x\nprint(f(x = 1, 2))\n",
 "int | none x = 1\nmatch x\n    int\n        print(x)\n", "int | none x = 1\nmatch x\n    int\n        print(x)\n    int y\n        print(y)\n    none\n        print(int(0))\n",
 "int | none x = 1\nmatch x\n    int\n        x = none\n    none\n        print(int(0))\n",
 "auto x = 9223372036854775808\n", "auto x = []\n", "auto x = range(3)\n",
 "int8 x = 128\n", "nat8 x = -1\n", "int8 x = int8(300)\n",
 "bin x = bin(2, fill = 0)\n", "bin x = bin.fill(-1, 0)\n", "bin x = bin.fill(2, 2)\n", "string x = string(2, fill = \"a\")\n", "string x = string.repeat(\"a\", -1)\n",
 "string HT = \"x\"\n", "void f(string NL)\n    return\n",
 "int x = 1\nif true\n    int x = 2\n", "int x = 1\nint x = 2\n",
 "class A()\n"
 "    int x\n"
 "    int x\n",
 "class A()\n"
 "    int HT\n",
 "class A this(local int start)\n"
 "    int x\n"
 "\n"
 "    this.x = start\n"
 "A a = A(1)\n"
 "A &b = a\n",
 "class A this(local int start)\n"
 "    int x\n"
 "    int y\n"
 "\n"
 "    this.x = start\n"
 "A a = A(1)\n"
 "print(a.y)\n",
 "class Counter this()\n"
 "    int value\n"
 "    void increment()\n"
 "        this.value = this.value + 1\n"
 "Counter c = Counter()\n"
 "c.increment()\n",
 "int x\nint &y = &x\nprint(y)\n",
 "int a = 1\nint b = 2\nint &r = &a\nr = &b\n",
 "int a = 1\nint b = 2\nint &r = &a\n&r = b\n",
 "int &x = &5\n",
 R"(class EqPartial this(local int x_value)
    int x
    int y


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
 R"(class GenericOnly<T>()
    T value
GenericOnly value
)",
 "#if DEBUG\nprint(int(1))\n", "# comment\nprint(int(1))\n"}) bad(s);
 bad_message("int a = 1\nint b = 2\nint &r = &a\n&r = b\n",
             "REFERENCE_BINDING", "write '&r = &x' for a local reference");
 good(R"(int super = 1
int override = 2
print(super + override)
print(NL)
)");
 good(R"(class Linear()
    int unused = 0
class Rbf()
    real64 gamma = 1.0


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
 bad_code(R"(class Partial this(local int x_value)
    int x
    int y


    this.x = x_value
Partial value = Partial(1)
Partial | none maybe = value
)", "UNINITIALIZED_UNION_PAYLOAD");
 bad_code(R"(class Partial(local int value)
    int x
    int y
    x = value
Partial item = Partial(1)
(Partial, int) | none result = (item, 2)
)", "UNINITIALIZED_UNION_PAYLOAD");
 good(R"(class Box(int value)
(Box, int) | none make()
    return (Box(7), 1)
match make()
    (Box b, int n)
        print("{b.value} {n}{NL}")
    none
        print("none{NL}")
)");
 good(R"(class LocalReset this()
    int value
    void .reset()
        this.value = 0
void run()
    LocalReset item = LocalReset()
    .item.reset()
    print(item.value)
    print(NL)
run()
)");
 bad_code(R"(class EarlyReceiver this()
    real64[] values
    void | error initialize(int count)
        if count < 0
            return error("bad")
        this.values = array(count, fill = 0.0)
        return
EarlyReceiver item = EarlyReceiver()
auto result = item.initialize(-1)
print(item.values[0])
print(NL)
)", "UNINITIALIZED");
 bad_code(R"(class EarlyUnit this()
    int value
    void reset(bool ok)
        if not ok
            return
        this.value = 1
EarlyUnit item = EarlyUnit()
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
 inspect_contains(R"(class SummaryReceiver this()
    int value
    void .reset(bool ok)
        if not ok
            return
        this.value = 1
)", "\"function\":\"$method.SummaryReceiver.reset\",\"receiver\":{\"requires\":[],\"writes\":[\"value\"],\"initializes\":[],\"invalidates\":[]}");

 // Nested whole-self publication requires the generated child binding and
 // every ordinary child field, without inventing a "$complete" field.
 good(R"(class Leaf leaf()
    int value = 7
    Leaf copy()
        return leaf
class Holder()
    Leaf child = Leaf()
    Leaf copy_child()
        return child.copy()
Holder holder = Holder()
Leaf copied = holder.copy_child()
print(copied.value)
print(NL)
)");
 bad_code(R"(class EmptyLeaf leaf()
    EmptyLeaf copy()
        return leaf
class Holder()
    EmptyLeaf child
    EmptyLeaf copy_child()
        return child.copy()
Holder holder = Holder()
EmptyLeaf copied = holder.copy_child()
)", "UNINITIALIZED");
 bad_code(R"(class PartialLeaf leaf()
    int value
    PartialLeaf copy()
        return leaf
class Holder()
    PartialLeaf child = PartialLeaf()
    PartialLeaf copy_child()
        return child.copy()
Holder holder = Holder()
PartialLeaf copied = holder.copy_child()
)", "UNINITIALIZED");

 // Aliased reference preconditions are all checked against call-entry state.
 // A later write through another alias must not retroactively make an earlier
 // read requirement safe.
 bad_code(R"(void initialize_then_read(int &destination, const int observation)
    destination = 1
    print(observation)
    print(NL)

int value
initialize_then_read(&value, value)
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
 bad_code(R"(class AliasReceiver this()
    int value

    void initialize_then_read(int &observation)
        this.value = 1
        print(observation)
        print(NL)

AliasReceiver item = AliasReceiver()
item.initialize_then_read(&item.value)
)", "UNINITIALIZED");

 // Aliasing itself is legal once every read precondition is satisfied.
 good(R"(void observe_and_update(const int observation, int &destination)
    print(observation)
    print(NL)
    destination = observation + 1

int value = 4
observe_and_update(value, &value)
print(value)
print(NL)
)");

 // Const authority is path-local and one-way: a readonly path may observe a
 // writable alias, but it cannot be promoted back into write authority.
 bad_code(R"(int value = 1
const int view = value
int &writer = &view
)", "REFERENCE_BINDING");

 bad_code(R"(class ConstNested this(local int[] values_value)
    int[] values


    this.values = values_value

const ConstNested item = ConstNested([1, 2])
item.values[0] = 3
)", "WRITE_CAPABILITY");

 bad_code(R"(class ConstMethod this(local int value_value)
    int value


    this.value = value_value

    void change_value()
        this.value = 2

const ConstMethod item = ConstMethod(1)
item.change_value()
)", "WRITE_CAPABILITY");

 good(R"(T observe_generic<T>(const T value)
    return value

int source = 7
print(observe_generic(source))
print(NL)
)");


 // An Array index removes its outer dimension before tensor-axis inference.
 good(R"(tensor<T> keep_tensor<T: floating>(tensor<T> value)
    return value

tensor<real32>[] values = [tensor.ones<real32>([1])]
tensor<real32> result = keep_tensor(values[0])
)");
 good(R"(tensor<T> keep_tensor<T: floating>(tensor<T> value)
    return value

class Moments(local tensor<real32>[] incoming)
    private tensor<real32>[] values = incoming

    tensor<real32> first()
        return keep_tensor(values[0])

Moments moments = Moments([tensor.ones<real32>([1])])
tensor<real32> result = moments.first()
)");
 // A tensor without an Array dimension still projects its tensor axes.
 good(R"(tensor<T>{3} keep_row<T: floating>(tensor<T>{3} value)
    return value

tensor<real32>{2, 3} grid = tensor.ones<real32>([2, 3])
tensor<real32>{3} row = keep_row(grid[0])
)");
 bad_code(R"(tensor<T> choose_tensor<T: floating>(tensor<T> left, tensor<T> right)
    return left

tensor<real32>[] first = [tensor.ones<real32>([1])]
tensor<real64>[] second = [tensor.ones<real64>([1])]
tensor<real32> result = choose_tensor(first[0], second[0])
)", "GENERIC_INFERENCE");
 // Inferring a field's type does not initialize its storage.
 bad_code(R"(tensor<T> keep_tensor<T: floating>(tensor<T> value)
    return value

class Unready()
    tensor<real32>[] values
    tensor<real32> seen = keep_tensor(values[0])
)", "UNINITIALIZED");

 // Model and already-materialized generic-class arguments share one inference.
 good(R"(class Model()
    int value = 1

class State<T>(local T seed)
    T value = seed

void checkpoint<M>(M &model, State<M> state)
    return

void forward(Model &model, State<Model> state)
    checkpoint(&model, state)

Model model = Model()
State<Model> state = State<Model>(model)
forward(&model, state)
)");
 // The class argument alone also determines the function's type parameter.
 good(R"(class Box<T>(local T seed)
    T value = seed

T unwrap<T>(Box<T> box)
    return box.value

Box<int> box = Box<int>(7)
int result = unwrap(box)
)");
 bad_code(R"(class Model()
    int value = 1

class Other()
    int value = 2

class State<T>(local T seed)
    T value = seed

void checkpoint<M>(M &model, State<M> state)
    return

Model model = Model()
State<Model> matching = State<Model>(model)
State<Other> different = State<Other>(Other())
checkpoint(&model, different)
)", "GENERIC_INFERENCE");

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
for 0 <= nat i < 1
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
 llvm_contains(R"(int sum_values(const int[] values, int n)
    int total = 0
    for 0 <= int i < n
        total += values[i]
    return total

int[] values = array(8, fill = 1)
print(sum_values(values, int(len(values))))
print(NL)
)", "call i1 @quidra_array_initialization_complete");
 llvm_contains(R"(int sum_values(const int[] values, int n)
    int total = 0
    for 0 <= int i < n
        total += values[i]
    return total

int[] values = array(8, fill = 1)
print(sum_values(values, int(len(values))))
print(NL)
)", "call ptr @quidra_array_slot_proven");
 llvm_contains(R"(int sum_values(const int[] values, int n)
    int total = 0
    for 0 <= int i < n
        total += values[i]
    return total

int[] values = array(8, fill = 1)
print(sum_values(values, int(len(values))))
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
 bad_code(R"(class Secret()
    private int value = 1
Secret secret = Secret()
print(secret.value)
print(NL)
)", "PRIVATE_MEMBER");
 good(R"(class Secret this(local int value_value)
    private int value


    this.value = value_value
Secret secret = Secret(1)
)");
 bad_code(R"(class Secret()
    private void hidden()
        return
Secret secret = Secret()
secret.hidden()
)", "PRIVATE_MEMBER");
 bad_code(R"(class Secret()
    private int value = 1
Secret secret = Secret()
secret.value = 2
)", "PRIVATE_MEMBER");
 bad_code(R"(class Secret()
    private int value = 1
Secret secret = Secret()
int &alias = &secret.value
)", "PRIVATE_MEMBER");
 good(R"(class Secret this()
    private int value = 1

    void .copy_from(Secret other)
        this.value = other.value
)");
 good(R"(class Secret()
    private int value = 1

    void update_other(Secret other)
        int &alias = &other.value
        alias = 2
)");
 good(R"(class Secret()
    private void hidden()
        return

    void call_other(Secret other)
        other.hidden()
)");
 bad_code(R"(class Box<T> this(local T value_value)
    private T value


    this.value = value_value
Box<int> box = Box<int>(1)
print(box.value)
print(NL)
)", "PRIVATE_MEMBER");
 bad_code(R"(class Box<T>()
    private void hidden()
        return
Box<int> box = Box<int>()
box.hidden()
)", "PRIVATE_MEMBER");
 bad_code(R"(class Secret()
    private T echo<T>(T value)
        return value
Secret secret = Secret()
print(secret.echo<int>(1))
print(NL)
)", "PRIVATE_MEMBER");
 bad_code("class Secret()\n"
          "    private private int value\n", "PARSE_ERROR");
 bad_code("private int value = 1\n", "PARSE_ERROR");
 good(R"(class SecretRef()
    private int value = 1
    int read_other(SecretRef other)
        const int alias = other.value
        return alias
SecretRef a = SecretRef()
SecretRef b = SecretRef()
print(a.read_other(b))
print(NL)
)");
 good(R"(class PrivateInit this(local int value_value)
    private int value


    this.value = value_value
    int read()
        return this.value
PrivateInit item = PrivateInit(9)
print(item.read())
print(NL)
)");
 bad_code(R"(class PrivateInit this(local int start)
    private int value


    this.value = start
PrivateInit item = PrivateInit(other = 9)
)", "ARGUMENT_MISMATCH");
 bad_code("class A()\n"
          "    int x\n"
          "class A()\n"
          "    int y\n", "DUPLICATE_NAME");
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
 // A concrete primitive member outside every constrained generic domain
 // may retain a different cross-argument relationship, with no implicit cast.
 good(R"(string select<T: floating, K: floating>(tensor<T> pixels, tensor<K> kernel, K divisor = 1.0, K offset = 0.0)
    return "floating"
string select(tensor<nat8> pixels, tensor<int64> kernel, int divisor = 1, int offset = 0)
    return "integer"
tensor<nat8> pixels = tensor.ones<nat8>([1])
tensor<int64> kernel = tensor.ones<int64>([1])
int divisor = 1
print(select(pixels, kernel, divisor))
print(NL)
tensor<real32> floating_pixels = tensor.ones<real32>([1])
tensor<real64> floating_kernel = tensor.ones<real64>([1])
print(select(floating_pixels, floating_kernel, real64(2.0)))
print(NL)
)");
 // Overlapping and unconstrained domains keep the original exact pattern.
 bad_code(R"(string select<T: numeric, K: numeric>(tensor<T> pixels, tensor<K> kernel, K divisor)
    return "generic"
string select(tensor<nat8> pixels, tensor<int64> kernel, int divisor)
    return "concrete"
)", "DUPLICATE_NAME");
 bad_code(R"(string select<T, K>(tensor<T> pixels, tensor<K> kernel, K divisor)
    return "generic"
string select(tensor<nat8> pixels, tensor<int64> kernel, int divisor)
    return "concrete"
)", "DUPLICATE_NAME");
 // No single conflicting variable is proof of a disjoint primitive domain.
 bad_code(R"(string select<T: floating, K: integer>(T first, K second, K divisor)
    return "generic"
string select(real32 first, int64 second, int divisor)
    return "concrete"
)", "DUPLICATE_NAME");
 // Nominal types are not a primitive-domain proof, even with floating bounds.
 bad_code(R"(class Item()
string select<T: floating, K: floating>(T first, K second, K divisor)
    return "generic"
string select(Item first, int64 second, int divisor)
    return "concrete"
)", "DUPLICATE_NAME");
 // Every generic member must be excluded, not just the narrowest member.
 bad_code(R"(string select<T: floating, K: floating>(tensor<T> pixels, tensor<K> kernel, K divisor)
    return "floating"
string select<T: numeric, K: numeric>(tensor<T> pixels, tensor<K> kernel, K divisor)
    return "numeric"
string select(tensor<nat8> pixels, tensor<int64> kernel, int divisor)
    return "concrete"
)", "DUPLICATE_NAME");
 // A new exception must keep the result relationship and shape key.
 bad_code(R"(tensor<T> select<T: floating, K: floating>(tensor<T> pixels, tensor<K> kernel, K divisor)
    return pixels
tensor<int64> select(tensor<nat8> pixels, tensor<int64> kernel, int divisor)
    return kernel
)", "DUPLICATE_NAME");
 bad_code(R"(string select<T: floating, K: floating>(T[] pixels, K second, K divisor)
    return "generic"
string select(nat8[2] pixels, int64 second, int divisor)
    return "concrete"
)", "DUPLICATE_NAME");
 // Admission does not invent a call conversion for an unmatched mixed input.
 bad_code(R"(string select<T: floating, K: floating>(tensor<T> pixels, tensor<K> kernel, K divisor)
    return "floating"
string select(tensor<nat8> pixels, tensor<int64> kernel, int divisor)
    return "integer"
tensor<real32> pixels = tensor.ones<real32>([1])
tensor<int64> kernel = tensor.ones<int64>([1])
int divisor = 1
print(select(pixels, kernel, divisor))
)", "SPECIALIZATION_NO_MATCH");
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
tensor<nat8>{1, 2, 2} pixels = tensor.ones<nat8>([1, 2, 2])
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
 good(R"(class MethodDomain()
    string classify(nat8 value)
        return "byte"
    string classify<T: floating>(T value)
        return "floating"

MethodDomain domain = MethodDomain()
print(domain.classify(nat8(1)))
print(NL)
print(domain.classify(real32(1.0)))
print(NL)
)");

 good(R"(class MethodPriority()
    T choose<T: numeric>(T value)
        return value
    T choose<T: floating>(T value)
        return value

MethodPriority priority = MethodPriority()
print(priority.choose(int(2)))
print(NL)
print(priority.choose(real32(3.0)))
print(NL)
)");

 bad_code(R"(class MethodArityOverload()
    int parse(int value)
        return value
    int parse(int value, int base)
        return value + base
)", "DUPLICATE_NAME");


 good(R"(class MethodPairDomain()
    T1 combine<T1: numeric, T2: integer>(T1 left, T2 right)
        return left
    T1 combine<T1: floating, T2: integer>(T1 left, T2 right)
        return left

MethodPairDomain domain = MethodPairDomain()
real32 narrow = domain.combine(real32(2.0), int(1))
int broad = domain.combine(int(2), int(1))
print(narrow)
print(NL)
print(broad)
print(NL)
)");

 bad_code(R"(class MethodAmbiguous()
    T choose<T: ordered>(T value)
        return value
    T choose<T: equatable>(T value)
        return value
)", "AMBIGUOUS_SPECIALIZATION");

 bad_code(R"(class MethodDefaultShape()
    T choose<T: numeric>(T value, int mode = 0)
        return value
    T choose<T: floating>(T value, int mode)
        return value
)", "DUPLICATE_NAME");

 bad_code(R"(class MethodRelation()
    T choose<T>(T left, T right)
        return left
    T choose<T, U>(T left, U right)
        return left
)", "DUPLICATE_NAME");

 bad_code(R"(class TooManyConstructors()
    int value
    void .construct()
        value = 0
    void .construct(int value_value)
        value = value_value
)", "DUPLICATE_NAME");
 good(R"(class DefaultConstructorParameter this(local int value_value = 7)
    int value

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
    string text = try .handle.read()
    .handle.close()
    return void
)");
 good(R"(void | error read_open_bin(string path)
    file.Handle handle = try file.open(path)
    bin data = try .handle.read_bin()
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
)", "call void @quidra_runtime_fail_error(");
 // Error sites of user code: a constant record with detail slots (a scan
 // literal), the record of a user error's literal, and a conversion site's
 // record array indexed by the reason recorded at run time.
 llvm_contains(R"(int n = 0
scan("n={&n}")
)", "x i8] c\"MISSING_TEXT\\00\"");
 llvm_contains(R"(int n = 0
scan("n={&n}")
)", "{ ptr, ptr, i32, i32, ptr, i64, ptr, ptr, ptr, ptr, ptr, ptr }");
 llvm_contains(R"(error e = error("plain")
)", "= getelementptr inbounds i8, ptr @.quidra.err.");
 llvm_contains(R"(real value = 2.5
int | error converted = int(value)
)", "= getelementptr inbounds [80 x i8], ptr @.quidra.err.");
 llvm_contains(R"(real value = 2.5
int | error converted = int(value)
)", "call i32 @quidra_runtime_last_reason()");
 // A parse site's two records, indexed by the reason; a file error made by
 // the runtime with the operation's code and FILE_OPEN's mode (APPEND).
 llvm_contains(R"(string text = "12"
int64 | error parsed = int64.parse(text)
)", "= getelementptr inbounds [72 x i8], ptr @.quidra.err.");
 llvm_contains(R"(file.Handle | error handle = file.append("log.txt")
)", "@quidra_error_file(i32 3, i32 2, ptr ");
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
    string text = try .handle.read()
    return void
)", "ptr @quidra_file_handle_drop)");
 llvm_contains(R"(void | error copy_handle(string path)
    file.Handle first = try file.open(path)
    file.Handle second = first
    return void
)", "call ptr @quidra_file_handle_clone");
 bad_code(R"(void close_readonly(const file.Handle handle)
    handle.close()
)", "WRITE_CAPABILITY");
 bad_code(R"(void read_readonly(const file.Handle handle)
    auto text = handle.read()
)", "WRITE_CAPABILITY");
 bad_code(R"(void read_bin_readonly(const file.Handle handle)
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
 good(R"(class Box<T: equatable> this(local T value_value)
    T value


    this.value = value_value
Box<string> box = Box<string>("ok")
print(box.value)
print(NL)
)");
 bad_code(R"(class CallbackHolder()
    func<void>() callback
class Box<T: equatable>()
    T value
Box<CallbackHolder> box
)", "GENERIC_CONSTRAINT");
 bad_code(R"(class ResourceHolder()
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
 bad_code("class ReservedBuiltinField()\n"
          "    int array\n", "SHADOWING");
 bad_code("class ReservedBuiltinMethod()\n"
          "    int array()\n"
          "        return 1\n", "SHADOWING");
 bad_code("class ReservedNamespaceMethod()\n"
          "    int tensor()\n"
          "        return 1\n", "SHADOWING");
 
 bad_code("int scan = 1\n", "SHADOWING");
 bad_code("int len = 1\n", "SHADOWING");
 bad_code("void use(int print)\n    return\n", "SHADOWING");
  bad_code("range(3)\n", "UNKNOWN_NAME");
 bad_code("int main()\n    return 0\n", "RESERVED_MAIN");
 bad_code("return\n", "RETURN_OUTSIDE_FUNCTION");
 bad_code(R"(int | error read()
    return 1
int use()
    return try read()
)", "TRY_CONTEXT");
 bad_code(R"(class P this(local int x_value)
    int x
    int y


    this.x = x_value
int read_y(P value)
    return value.y
P value = P(1)
print(read_y(value))
print(NL)
)", "UNINITIALIZED_ARGUMENT");
 bad_code(R"(class P this(local int x_value)
    int x
    int y


    this.x = x_value
P a = P(1)
P b = P(1)
bool same = a == b
)", "UNINITIALIZED_FIELD_EQUALITY");
 good(R"(class DefaultLeaf()
    int value = 7

    return
class DefaultTree()
    DefaultLeaf[] leaves = [DefaultLeaf(), DefaultLeaf()]

    return
DefaultTree tree = DefaultTree()
print(tree.leaves[0].value)
)");
 bad_code(R"(class LaterDefaultTree()
    LaterDefaultLeaf[] leaves = [LaterDefaultLeaf()]

    return
class LaterDefaultLeaf()
    int value = 11

    return
LaterDefaultTree tree = LaterDefaultTree()
print(tree.leaves[0].value)
)", "SOURCE_ORDER");
 good(R"(class DefaultArgumentLeaf()
    int value = 13

    return
int use_default(DefaultArgumentLeaf value = DefaultArgumentLeaf())
    return value.value
print(use_default())
)");
 bad_code(R"(class PartialDefaultLeaf()
    int value

    return
class PartialDefaultTree()
    PartialDefaultLeaf[] leaves = [PartialDefaultLeaf()]

    return
)", "UNINITIALIZED_ARGUMENT");
 bad_code(R"(class PartialDefaultArgument()
    int value

    return
int use_partial_default(PartialDefaultArgument value = PartialDefaultArgument())
    return value.value
)", "UNINITIALIZED_ARGUMENT");
 bad_code(R"(class ArrayPartialLiteral this(local int x_value)
    int x
    int y


    this.x = x_value
ArrayPartialLiteral partial = ArrayPartialLiteral(1)
ArrayPartialLiteral[] values = [partial]
)", "UNINITIALIZED_ARGUMENT");
 bad_code(R"(class ArrayPartialFill this(local int x_value)
    int x
    int y


    this.x = x_value
ArrayPartialFill partial = ArrayPartialFill(1)
ArrayPartialFill[] values = array(2, fill = partial)
)", "UNINITIALIZED_ARGUMENT");
 bad_code(R"(class ArrayPartialAppend this(local int x_value)
    int x
    int y


    this.x = x_value
ArrayPartialAppend partial = ArrayPartialAppend(1)
ArrayPartialAppend[] values = []
values = values.append(partial)
)", "UNINITIALIZED_ARGUMENT");
 bad_code(R"(class ArrayPartialAssign this(local int x_value)
    int x
    int y


    this.x = x_value
ArrayPartialAssign partial = ArrayPartialAssign(1)
ArrayPartialAssign[] values = array(1)
values[0] = partial
)", "UNINITIALIZED_ARGUMENT");
 good(R"(class IndexedPoint this(local int x_value, local int y_value)
    int x
    int y


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
 bad_code(R"(class IndexedPartialPoint this(local int x_value)
    int x
    int y


    this.x = x_value

IndexedPartialPoint partial = IndexedPartialPoint(1)
IndexedPartialPoint[] values = array(1)
values[0] = partial
)", "UNINITIALIZED_ARGUMENT");
 bad_code("class A this(local int start)\n"
          "    int x\n"
          "\n"
          "    this.x = start\n"
          "A a = A(1)\n"
          "print(a.y)\n", "UNKNOWN_MEMBER");
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
 good(R"(tensor<int64>{2} a = tensor.ones<int64>([2])
tensor<int64>{2} b = tensor.ones<int64>([2])
tensor<bool>{2} eq = a == b
tensor<bool>{2} ne = a != b
tensor<bool>{2} lt = a < b
tensor<bool>{2} le = a <= b
tensor<bool>{2} gt = a > b
tensor<bool>{2} ge = a >= b
bool same = eq.all()
bool different = ne.any()
tensor<bool>{2} scalar_left = 1 < a
tensor<bool>{2} scalar_right = a >= 1
)");
 llvm_contains(R"(tensor<int64>{2} a = tensor.ones<int64>([2])
tensor<int64>{2} b = tensor.ones<int64>([2])
tensor<bool>{2} mask = a == b
bool same = mask.all()
)", "@quidra_tensor_compare");
 llvm_contains(R"(tensor<int64>{2} a = tensor.ones<int64>([2])
bool any = (a != 1).any()
)", "@quidra_tensor_bool_reduce");
 bad_code(R"(tensor<int64>{2} a = tensor.ones<int64>([2])
tensor<int64>{3} b = tensor.ones<int64>([3])
tensor<bool> same = a == b
)", "TENSOR_SHAPE");
 good(R"(tensor<int64>{2} a = tensor.ones<int64>([2])
tensor<bool>{2} same = a == 1
tensor<bool>{2} reverse = 1 == a
)");
 bad_code(R"(tensor<int64>{2} a = tensor.ones<int64>([2])
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
 bad_code(R"(class A()
    int x = 0
A value = A()
print(value.missing<int>(1))
print(NL)
)", "UNKNOWN_GENERIC_METHOD");
 bad_code("int[] values = [1]\nvalues.missing<int>()\n", "GENERIC_RECEIVER");

 // Tensor shape patterns fix rank exactly; '_' keeps only that extent unconstrained.
 good(R"(tensor<real32>{2, 3} matrix = tensor.zeros<real32>([2, 3])
nat[2] dimensions = matrix.shape()
tensor<real32> erased = matrix
nat[2] inferred_dimensions = erased.shape()
tensor<real32>{3} row = matrix[0]
tensor<real32>{2} column = matrix[:, 0]
tensor<real32> cell = matrix[0, 0]
real32 value = cell.item()
tensor<real32>{6} reshaped = matrix.reshape([6])
tensor<real32>{2, 3} contiguous = matrix.contiguous()
tensor<real64>{2, 3} converted = real64(matrix)
)");
 good(R"(tensor<T>{3, _} first_three<T>(tensor<T>{3, _} value)
    return value
tensor<real32> source = tensor.ones<real32>([3, 2])
tensor<real32>{3, _} constrained = first_three(source)
)");
 // Generic dtype inference may proceed through an unknown shape, but the
 // specialized call still enforces the exact-rank shape pattern.
 bad_code(R"(tensor<T>{3, _} first_three<T>(tensor<T>{3, _} value)
    return value
tensor<real32> source = tensor.ones<real32>([2, 2])
tensor<real32>{3, _} constrained = first_three(source)
)", "TYPE_MISMATCH");
 good(R"(tensor<real32> | error direct = tensor.zeros<real32>([2, 2])
tensor<real32>{2, _} | error constrained = tensor.ones<real32>([2, 2])
tensor<real32> | error widened = constrained
match widened
    tensor<real32> pixels
        print(pixels.shape()[0])
        print(NL)
    error problem
        print(problem)
        print(NL)
)");
 bad_code("tensor<real32>{3, _} wrong = tensor.zeros<real32>([2, 2])\n", "TYPE_MISMATCH");
 bad_code("tensor<real32>{2, 4} wrong = tensor.zeros<real32>([2, 3])\n", "TYPE_MISMATCH");
 bad_code("tensor<real32>{3, _} wrong_rank = tensor.zeros<real32>([3, 4, 5])\n", "TYPE_MISMATCH");
 good("tensor<real32>{3, _, _} exact_rank = tensor.zeros<real32>([3, 4, 5])\n");
 good(R"(tensor<real32> contextual_dtype = tensor.zeros([2, 3])
nat[2] contextual_shape = contextual_dtype.shape()
tensor<real32>{2, 3} contextual_exact = tensor.ones([2, 3])
)");
 bad_code("auto missing_dtype = tensor.zeros([2, 3])\n", "GENERIC_ARITY");
 good(R"(auto generated = tensor.zeros<real32>([2, 3])
nat[2] generated_shape = generated.shape()
)");
 bad_code(R"(auto generated = tensor.zeros<real32>([2, 3])
tensor<real32>{2, 4} impossible = generated
)", "TYPE_MISMATCH");
 bad_code(R"(tensor<real32>{2, 3} source = tensor.zeros<real32>([2, 3])
auto transposed = source.transpose(0, 1)
tensor<real32>{2, 3} impossible = transposed
)", "TYPE_MISMATCH");
 good(R"(tensor<real32>{2, 3} source = tensor.zeros<real32>([2, 3])
auto transposed = source.transpose(0, 1)
tensor<real32>{3, 2} exact_shape = transposed
auto reshaped = source.reshape([3, 2])
tensor<real32>{3, 2} reshaped_exact = reshaped
)");
 bad_code(R"(tensor<real32>{2, 3} source = tensor.zeros<real32>([2, 3])
auto reshaped = source.reshape([3, 2])
tensor<real32>{2, 3} impossible = reshaped
)", "TYPE_MISMATCH");
 ir_contains(R"(tensor<real32> erase_shape(tensor<real32> value)
    return value
auto unknown = erase_shape(tensor.zeros<real32>([2, 3]))
tensor<real32>{2, 4} checked_at_runtime = unknown
)", "shape.constraint");
 bad_code(R"(tensor<real32>{2, 3} source = tensor.zeros<real32>([2, 3])
auto invalid = source.transpose(0, 2)
)", "ARGUMENT_MISMATCH");
 // The static shape of tensor arithmetic follows the broadcast rule: a 1
 // takes the other side's extent, 0 included, in either operand order, so
 // these results are known exactly (and shapes cannot be misreported as the
 // left operand's).
 good(R"(tensor<real32>{1, 3} one = tensor.ones<real32>([1, 3])
tensor<real32>{0, 3} empty = tensor.zeros<real32>([0, 3])
tensor<real32>{2, 3} two = tensor.ones<real32>([2, 3])
tensor<real32>{0, 3} one_empty = one + empty
tensor<real32>{0, 3} empty_one = empty * one
tensor<real32>{2, 3} one_two = one - two
tensor<real32>{2, 3} two_one = two / one
tensor<real32>{1, 3} one_one = one + one
tensor<real32>{0, 3} scalar = empty + real32(2)
)");
 good(R"(tensor<real32> one = tensor.ones<real32>([1, 3])
tensor<real32> empty = tensor.zeros<real32>([0, 3])
tensor<real32> flow = one + empty
tensor<real32>{0, 3} refined = flow
)");
 bad_code(R"(tensor<real32>{1, 3} one = tensor.ones<real32>([1, 3])
tensor<real32>{0, 3} empty = tensor.zeros<real32>([0, 3])
tensor<real32>{1, 3} stale = one + empty
)", "TYPE_MISMATCH");
 bad_code(R"(tensor<real32>{1, 3} one = tensor.ones<real32>([1, 3])
tensor<real32>{2, 3} two = tensor.ones<real32>([2, 3])
tensor<real32>{1, 3} stale = one * two
)", "TYPE_MISMATCH");
 // A 1 against an unknown extent leaves the result extent open: checked at
 // runtime, not rejected.
 ir_contains(R"(tensor<real32> erase_shape(tensor<real32> value)
    return value
tensor<real32>{1, 3} one = tensor.ones<real32>([1, 3])
auto unknown = erase_shape(tensor.zeros<real32>([4, 3]))
tensor<real32>{4, 3} checked_at_runtime = one + unknown
)", "shape.constraint");
 // A 1 against an extent known to be 1 keeps the 1 in the result's source
 // pattern, whether the right operand's 1 comes from its pattern, a `_`
 // pattern plus flow facts, flow facts alone or a constructor, so the static
 // type stays identical to the left operand's wherever reference arguments,
 // generic inference or array literals require it.
 good(R"(void bump(tensor<real32>{1, 3} &x)
    x = x * real32(2)
real32 first(const tensor<real32>{1, 3} x)
    return x[0, 0].item()
T pick<T>(T first_value, T second_value)
    return first_value
tensor<real32>{1, 3} a = tensor.ones<real32>([1, 3])
tensor<real32>{1, 3} patterned = tensor.ones<real32>([1, 3])
tensor<real32>{_, 3} wildcard = tensor.ones<real32>([1, 3])
tensor<real32> flow = tensor.ones<real32>([1, 3])
auto from_pattern = a + patterned
bump(&from_pattern)
auto from_wildcard = a - wildcard
bump(&from_wildcard)
auto from_flow = a * flow
bump(&from_flow)
auto from_constructor = a / tensor.ones<real32>([1, 3])
bump(&from_constructor)
real32 value = first(from_flow)
auto picked = pick(a + flow, a)
auto items = [a + flow, a]
)");
 // Against an unknown extent the result axis is a wildcard, so its type is
 // no longer the left operand's: a reference argument needs a binding
 // declared with the pattern, which checks the extent at runtime.
 bad_message(R"(tensor<real32> erase_shape(tensor<real32> value)
    return value
void bump(tensor<real32>{1, 3} &x)
    x = x * real32(2)
tensor<real32>{1, 3} one = tensor.ones<real32>([1, 3])
auto unknown = erase_shape(tensor.ones<real32>([1, 3]))
auto result = one + unknown
bump(&result)
)", "TYPE_MISMATCH", "Reference arguments require identical declared types");
 good(R"(tensor<real32> erase_shape(tensor<real32> value)
    return value
void bump(tensor<real32>{1, 3} &x)
    x = x * real32(2)
tensor<real32>{1, 3} one = tensor.ones<real32>([1, 3])
auto unknown = erase_shape(tensor.ones<real32>([1, 3]))
tensor<real32>{1, 3} result = one + unknown
bump(&result)
)");
 // The wildcard also keeps facts derived from the result honest: keeping
 // the left operand's 1 there would make the transposed result [3, 1] and
 // reject these bindings, which hold for a [2, 3] or [0, 3] right operand.
 // Each one is checked at runtime instead.
 good(R"(tensor<real32> erase_shape(tensor<real32> value)
    return value
tensor<real32>{1, 3} one = tensor.ones<real32>([1, 3])
auto unknown = erase_shape(tensor.ones<real32>([2, 3]))
auto turned = (one + unknown).transpose(0, 1)
tensor<real32>{3, 2} wide = turned
tensor<real32>{3, 0} empty = turned
tensor<real32>{3, 1} narrow = turned
)");
 ir_contains(R"(tensor<real32> erase_shape(tensor<real32> value)
    return value
tensor<real32>{1, 3} one = tensor.ones<real32>([1, 3])
auto unknown = erase_shape(tensor.ones<real32>([0, 3]))
tensor<real32>{3, 0} empty = (one + unknown).transpose(0, 1)
)", "shape.constraint");
 good(R"(T identity<T>(T value)
    return value
int integer_value = 1
real64 float_value = 1.0
int integer_result = identity(integer_value)
real64 float_result = identity(float_value)
)");
 bad_code("tensor<real32>{2, 3} value = tensor.ones<real32>([2, 3])\nint[3] wrong_shape = value.shape()\n", "TYPE_MISMATCH");
 good(R"(tensor<real32> erase(tensor<real32> value)
    return value
tensor<real32>{2, _} known = erase(tensor.zeros<real32>([2, 2]))
)");
 bad_code("tensor<real32> value = tensor.ones<real32>([1])\nreal32 scalar = value.item()\n", "TYPE_MISMATCH");
 bad_code("tensor<real32>{2, 2} value = tensor.ones<real32>([2, 2])\nauto bad = value[0, 0, 0]\n", "INDEX_ARITY");

 // Shape-pattern match cases select only exact-rank compatible tensor alternatives.
 good(R"(void classify(tensor<real32>{3, 4} | tensor<real32>{1, 4} value)
    match value
        tensor<real32>{3, _} rgb
            print(rgb.shape()[0])
            print(NL)
        tensor<real32>{1, _} gray
            print(gray.shape()[0])
            print(NL)
)");
 bad_code(R"(void invalid_case(tensor<real32>{1, 4} | tensor<real32>{4, 4} value)
    match value
        tensor<real32>{3, _} impossible
            print(impossible.shape()[0])
            print(NL)
        tensor<real32>{1, _} gray
            print(gray.shape()[0])
            print(NL)
        tensor<real32>{4, _} rgba
            print(rgba.shape()[0])
            print(NL)
)", "MATCH_CASE");

 // Tracking preserves the tensor's exact shape contract.
 good(R"(tensor<real32> source = tensor.ones<real32>([3, 8, 8])
tensor<real32>{3, _, _} value = source.track()
tensor<real32>{3, _, _} restored = value.untrack()
)");
 good(R"(tensor<real64> source = tensor.ones<real64>([2, 4])
tensor<real64>{2, _} value = source.track()
tensor<real64>{2, _} restored = value.untrack()
)");
 bad_code(R"(tensor<real32> source = tensor.ones<real32>([3, 8, 8])
tensor<real32>{3, _} wrong = source.track()
)", "TYPE_MISMATCH");

 // Dependent shape expressions in function signatures are checked at the boundary.
 good(R"(tensor<real64>{nat(n), 2} keep_shape(int n, tensor<real64>{nat(n), 2} value)
    return value
tensor<real64> unknown = tensor.ones<real64>([3, 2])
tensor<real64>{3, 2} checked = keep_shape(3, unknown)
)");
 good(R"(int[][nat(n)] keep_rows(int n, int[][nat(n)] rows)
    return rows
int[][] dynamic_rows = [[1, 2], [3, 4]]
int[][] checked_rows = keep_rows(2, dynamic_rows)
)");

 // Runtime extent expressions are captured per binding; mutable sources remain legal.
 good(R"(int n = 3
int m = 4
tensor<real64>{nat(n * 2 + 1), 224} captured = tensor.ones<real64>([7, 224])
n = 10
tensor<real64>{nat(n), 224} later = tensor.ones<real64>([10, 224])
real64[nat(n * m)] dynamic_fixed
dynamic_fixed[0] = 1.0
tensor<real64>{3, 224} contextual = tensor.zeros()
tensor<real64>{_, 224} explicit_shape = tensor.zeros([3, 224])
)");
 bad_code(R"(int n = 3
tensor<real64>{nat(n), 224} wrong = tensor.ones<real64>([3, 224, 1])
)", "TYPE_MISMATCH");

 // Numeric container casts preserve array structure and tensor shape facts.
 good(R"(int[][] values = [[1, 2], [3, 4]]
real64[][] converted = real64(values)
int[2][2] fixed = [[1, 2], [3, 4]]
real64[2][2] fixed_converted = real64(fixed)
tensor<int64>{2, 2} matrix = tensor.ones<int64>([2, 2])
tensor<real64>{2, 2} tensor_converted = real64(matrix)
tensor<real32>{2, 2} tracked_source = tensor.ones<real32>([2, 2])
tensor<real32>{2, 2} tracked = tracked_source.track()
tensor<real64>{2, 2} tracked_converted = real64(tracked.untrack())
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
tensor<int64>{2, 2} matrix = tensor.ones<int64>([2, 2])
auto | error tensor_narrowed = int8(matrix)
match tensor_narrowed
    tensor<int8>{2, 2} converted
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
 good("real64[] values = [1.0, 2.0]\nint[] converted = int(values)\n");
 bad_code("tensor<real64> values = tensor.ones<real64>([2])\nauto converted = int(values)\n", "NUMERIC_CAST");
 bad_code("tensor<int64> values = tensor.ones<int64>([2])\nauto converted = values.cast<real64>()\n", "UNKNOWN_MEMBER");

 // Tensor flow facts weaken at joins. A later exact-shape binding accepts the
 // unknown fact set and emits a runtime constraint check.
 good(R"(void branch_shape(bool flag)
    tensor<real32> value = tensor.zeros<real32>([3, 4])
    if flag
        value = tensor.zeros<real32>([3, 5])
    tensor<real32>{3, 4} exact_shape = value
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
    for auto item = items[]
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
tensor<real32>{2} contextual_gpu = tensor.ones(gpu = 1)
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
    for auto item = items[]
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
 std::string postfix = "class P this(local int start)\n"
                       "    int v\n"
                       "\n"
                       "\n"
                       "    this.v = start\n"
                       "\n"
                       "    P grow()\n"
                       "        return P(this.v + 1)\n"
                       "\n"
                       "P p = P(1)\n"
                       "P q = p";
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

 // Every complex kind is numeric; none has relational ordering.
 for(const auto* kind:{"com16","com16b","com32","com64","com"}){
     const std::string name=kind;
     good("T identity<T: numeric>(T value)\n    return value\n"+name+" z = 1.0 + 2.0i\n"+name+" result = identity(z)\n");
     bad_code("T identity<T: ordered>(T value)\n    return value\n"+name+" z = 1.0 + 2.0i\n"+name+" result = identity(z)\n","GENERIC_CONSTRAINT");
 }
 good(R"(int domain<T: numeric>(T value)
    return 1
int domain<T: ordered>(T value)
    return 2
com32 z = 1.0 + 2.0i
int complex_domain = domain(z)
int real_domain = domain(real16(2.0))
)");
 // Every ordered numeric pair admits an explicit conversion.
 for(const auto* source:{"nat8","nat16","nat32","nat64","nat","int8","int16","int32","int64","int","real16","real16b","real32","real64","real"}){
     const std::string from=source;
     const auto literal=from.starts_with("real")?"1.0":"1";
     for(const auto* target:{"nat8","nat16","nat32","nat64","nat","int8","int16","int32","int64","int","real16","real16b","real32","real64","real"})
         good(from+" source = "+literal+"\n"+target+" converted = "+target+"(source)\n");
 }
 for(const auto* source:{"com16","com16b","com32","com64","com"})
     bad_code(std::string(source)+" z = 1.0 + 0.0i\nreal64 scalar = real64(z)\n","NUMERIC_CAST");
 // Collection reference aliases use the container's binding proof; their
 // element loads still check element initialization at run time.
 good("void double_all(int[] &values)\n    for auto &value = &values[]\n        value = value * 2\nint[] values = [1, 2]\ndouble_all(&values)\n");
 good("int[] values = [1, 2]\nfor auto &value = &values[]\n    value = value + 1\n");
 good("int[] values = array(2)\nfor auto &value = &values[]\n    print(value)\n");
 good("(int, int)[] values = array(2)\nfor (int &left, int &right) = &values[]\n    print(left)\n");
 good("int root = 1\nfor int &value = &reflect.each<int>(&root)[]\n    print(value)\n");
 bad_code("int[] values\nfor auto &value = &values[]\n    print(value)\n", "UNINITIALIZED");
 // Class visits retain only the fields initialized in every visited value.
 bad_code(R"(class Slot()
    int value

    return
class Model this()
    Slot first

    this.first = Slot()
Model model = Model()
for Slot &slot = &reflect.each<Slot>(&model)[]
    print(slot.value)
)", "UNINITIALIZED");

 // Large tuple returns keep pointer attributes separated from their type.
 llvm_contains(R"((tensor<real32>, tensor<real32>, tensor<real32>) triple(tensor<real32> value)
    return (value, value, value)
tensor<real32> value = tensor.ones<real32>([1])
(tensor<real32> output, _, _) = triple(value)
print(output.item())
)", "ptr noalias sret({ ptr, ptr, ptr }) %quidra.result");

 // Tuple patterns infer auto/discard leaves from checked values without loosening typed leaves.
 good(R"((tensor<real32>, tensor<real32>, tensor<real32>) normalize_value(tensor<real32> value, bool training)
    return (value, value, value)
tensor<real32> infer(tensor<real32> value)
    (tensor<real32> output, _, _) = normalize_value(value, false)
    return output
tensor<real32> value = tensor.ones<real32>([1])
print(infer(value).item())
)");
 good(R"(((int, string), bool) make()
    return ((7, "x"), true)
((auto count, _), auto flag) = make()
print("{count} {flag}")
)");
 good(R"((int, string) make()
    return (7, "x")
int count = 0
(count, _) = make()
print(count)
)");
 good(R"((int, string) | error make()
    return (7, "x")
int | error read()
    (auto count, _) = try make()
    return count
int count = read()
print(count)
)");
 bad_code(R"((real64, string) make()
    return (7.0, "x")
(int count, _) = make()
)", "TYPE_MISMATCH");
 bad_code(R"((int count, _) = (7, 2)
)", "DISCARD");
 bad_code(R"((int, string) make()
    return (7, "x")
(int, bool) other()
    return (7, true)
(int count, _) = if true then make() else other()
)", "TYPE_MISMATCH");
 // Explicit local bindings use ordinary block scope and cannot be captured by definitions.
 good("local (int count, string label) = (7, \"x\")\nprint(count)\n");
 good("int identity(local int value)\n    local int result = value\n    return result\nlocal int value = 7\nint copy = identity(value)\nif true\n    local int inner = value\n    for int i < 2\n        local int next = inner + i\n        print(next)\n");
 good("int identity(int value);\nint identity(local int value)\n    return value\nint result = identity(7)\n");
 good("T identity<T>(local T value)\n    local T result = value\n    return result\nlocal int value = 7\nint copy = identity(value)\n");
 inspect_contains("local int value = 7\nprint(value)\n", "\"local\":true");
 bad_code("print(value)\nlocal int value = 7\n", "UNKNOWN_NAME");
 bad_code("if true\n    local int inner = 7\nprint(inner)\n", "UNKNOWN_NAME");
 bad_code("if true\n    local int inner = 7\nelse\n    print(inner)\n", "UNKNOWN_NAME");
 bad_code("local int outer = 7\nint read()\n    return outer\n", "UNKNOWN_NAME");
 bad_code("local const literal real unit = 1.0\nreal read()\n    return unit\n", "UNKNOWN_NAME");
 bad_code("local int outer = 7\nclass Box()\n    int read()\n        return outer\n", "UNKNOWN_NAME");

 // Proxy writes invalidate the actual target; tuple slots do not prove class fields.
 bad_code(R"(class Child()
    int value

    return
class Parent this()
    Child child

    this.child = Child()
    this.child.value = 7
Parent root = Parent()
for Parent &visit = &reflect.each<Parent>(&root)[]
    visit.child = Child()
print(root.child.value)
)", "UNINITIALIZED");
 bad_code(R"(class Child()
    int value

    return
class Model this()
    (Child, int) pair

    this.pair = (Child(), int(0))
Model root = Model()
for Child &visit = &reflect.each<Child>(&root)[]
    print(visit.value)
)", "UNINITIALIZED");

 // A traversal names visits and has no storable or callable value.
 good("int root = 1\nfor int &value = &reflect.each<int>(&root)[]\n    value = 2\n");
 good("int[][] root = [[1], [2]]\nfor auto value = reflect.each<int>(root)[nat i]\n    print(i)\n");
 bad_code("int root = 1\nauto saved = reflect.each<int>(root)\n","TRAVERSAL");
 bad_code("int root = 1\nfor auto value = reflect.each(root)[]\n    print(value)\n","TRAVERSAL");
 bad_code("reflect.Each<int> saved\n","TRAVERSAL");
 bad_code("void consume(reflect.Each<int> value)\n    return\n","TRAVERSAL");
 bad_code("reflect.Each<int> walk(int &root)\n    int local_value = 0\n    return reflect.each<int>(&root)\n","TRAVERSAL");
 bad_code("reflect.Each<int> walk(int &root)\n    return walk(&root)\n","TRAVERSAL");
 bad_code(""
          "class Model()\n"
          "    const int fixed = 1\n"
          "\n"
          "    return\n"
          "Model model = Model()\n"
          "for int &value = &reflect.each<int>(&model)[]\n"
          "    value = 2\n","TRAVERSAL");
 bad_code(""
          "class Model()\n"
          "    int[] values = [1]\n"
          "\n"
          "    return\n"
          "Model model = Model()\n"
          "for int &value = &reflect.each<int>(&model)[]\n"
          "    model.values = [2]\n","WRITE_CAPABILITY");
 bad_code("reflect.Each<int> walk(int root)\n    return reflect.each<int>(root)\nint root = 1\nfor int &value = &walk(root)[]\n    value = 2\n","TRAVERSAL");
 // A holder's bound reference proves target completeness, including opaque
 // nested state; it does not store the target's fields inside the holder.
 const std::string nested_reference_holder = R"(class Leaf()
    private autograd.Target state = autograd.target()

    void .clear()
        .state.clear_grad()
class Model()
    Leaf leaf
void clear_model(Model &model)
    .model.leaf.clear()
class Holder this(local Model &model_initial)
    private Model &model

    &model = &model_initial
    void .clear()
        clear_model(&model)
)";
 good(nested_reference_holder + R"(Model model = Model()
model.leaf = Leaf()
Holder holder = Holder(&model)
.holder.clear()
)");
 bad_code(nested_reference_holder + R"(Model model = Model()
Holder holder = Holder(&model)
)", "UNINITIALIZED");
 bad_code(R"(class Leaf()
    private autograd.Target state = autograd.target()

    void .clear()
        .state.clear_grad()
class Model()
    Leaf leaf
void clear_model(Model &model)
    .model.leaf.clear()
class Holder()
    private Model &model

    void .clear()
        clear_model(&model)
Holder holder = Holder()
.holder.clear()
)", "UNINITIALIZED");
 bad_code(R"(class Value()
    int item
void overwrite(Value &value)
    value = Value()
Value value = Value()
value.item = 7
overwrite(&value)
print(value.item)
)", "UNINITIALIZED");
 bad_code(R"(class Leaf()
    private autograd.Target state

    void .clear()
        .state.clear_grad()
class Model()
    Leaf leaf
class Holder this(local Model &model_initial)
    private Model &model

    &model = &model_initial
Model model = Model()
model.leaf = Leaf()
Holder holder = Holder(&model)
)", "UNINITIALIZED");
 bad_code(R"(class Value()
    int item
class Holder(local Value &value)
    private Value &target
    &target = &value
    int read()
        return target.item
Value value = Value()
value.item = 7
Holder holder = Holder(&value)
value = Value()
print(holder.read())
)", "REFERENCE_TARGET");
 // Binding a stored reference records the caller's initialization contract.
 const std::string stored_reference_holder = R"(class Value()
    int item
class Holder this(local Value &value)
    private Value &target

    &this.target = &value
    int read()
        return this.target.item
)";
 good(stored_reference_holder + R"(Value value = Value()
value.item = 7
Holder holder = Holder(&value)
print("{holder.read()}{NL}")
)");
 good(stored_reference_holder + R"(Holder forward(Value &value)
    return Holder(&value)
Value value = Value()
value.item = 11
Holder holder = forward(&value)
print("{holder.read()}{NL}")
)");
 bad_code(stored_reference_holder + R"(Value value = Value()
Holder holder = Holder(&value)
)", "UNINITIALIZED");
 bad_code(stored_reference_holder + R"(Holder forward(Value &value)
    return Holder(&value)
Value value = Value()
Holder holder = forward(&value)
)", "UNINITIALIZED");
 good(R"(class Value()
    int item
class Holder this(local Value &value) | error
    private Value &target

    &this.target = &value
    int read()
        return this.target.item
Value value = Value()
value.item = 17
auto holder = Holder(&value)
print("{holder.read()}{NL}")
)");
 good("int[] values = [-2, 0, 3, 4]\nauto output = [for int x = values[] if x > 0 then x * x]\nfor int value = output[]\n    print(value)\n");
 good("int[] values = []\nauto output = [for auto x = values[] then x]\nint[] contextual = [for int x = values[] then 1]\nstring[] labels = [for int x = values[] then \"item\"]\n");
 good("int[] values = [1, 2]\nauto output = [for int x = values[] then (x, \"tag\")]\n");
 good("int[] values = [1, 2]\nint[] first\nint[] second\nparallel\n    first = [for int x = values[] then x]\n    second = [for int x = values[] then x * x]\nfor int value = first[]\n    print(value)\nfor int value = second[]\n    print(value)\n");
 bad_code("void consume(int value)\n    return\nint[] values = []\nauto output = [for int x = values[] then consume(x)]\n", "TYPE_MISMATCH");
 good("int identity(int value)\n    return value\nint[] values = [1, 2]\nauto output = [for int x = values[] then identity(x)]\n");
 good("T identity<T>(T value)\n    return value\nint[] values = [1, 2]\nauto output = [for auto x = values[] then identity(x)]\nauto copied = identity([for int item = values[] then item])\n");
 good("int change(int &value)\n    value = value + 1\n    return value\nint[] values = [1, 2]\nauto output = [for int x = values[] then change(&x)]\n");
 bad_code("int[] values = []\nauto output = [for int x = values[] then x]\nprint(x)\n", "UNKNOWN_NAME");
 bad_code("int[] values = []\nauto output = [for int x = values[] if x then x]\n", "TYPE_MISMATCH");
 bad_code("int[] values = []\nauto output = [for real32 x = values[] then x]\n", "TYPE_MISMATCH");
 bad_code("string value = \"abc\"\nauto output = [for auto x = value[] then x]\n", "ARRAY_COMPREHENSION");
 bad_code("int[] values = []\nint x = 3\nauto output = [for int x = values[] then x]\n", "SHADOWING");
 {
     const auto require = [](bool condition, const char* message) {
         if (!condition) { std::cerr << message << "\n"; std::exit(1); }
     };
     const auto source = std::string("int[] values = [1, 2]\nauto output = [for int x = values[] if x > 0 then x * x]\n");
     const auto formatted = quidra::format_source(source);
     require(formatted == quidra::format_source(formatted), "generation formatting is stable");
     const auto compiled = quidra::compile(formatted);
     const auto inspected = quidra::inspect_source_json(formatted, compiled.checked, "test.qui");
     require(inspected.find("array_comprehension") != std::string::npos &&
             inspected.find("generation_binding") != std::string::npos,
             "generation source inventory contains expression and binding");
 }

 good(R"(class Capture(local int &input)
    local int &temporary
    &temporary = &input
    int captured = temporary
int original = 5
Capture value = Capture(&original)
print(value.captured)
)");
 good(R"(int read_alias(int &input)
    int &first = &input
    int &second = &first
    return second
int value = 7
print(read_alias(&value))
)");
 good(R"(void initialize_alias(int &input)
    int &target = &input
    target = 9
int value
initialize_alias(&value)
print(value)
)");
 good(R"(class Pair()
    int first
    int second
int read_first(Pair &input)
    int &target = &input.first
    return target
Pair value = Pair()
value.first = 4
print(read_first(&value))
)");
 good(R"(int read_element(int[] &input)
    int &target = &input[0]
    return target
int[] values = [6]
print(read_element(&values))
)");
 llvm_contains(R"(int read_rebound_element(int[] &input)
    int &target
    &target = &input[0]
    int &alias = &target
    return alias
int[] values = array(1)
print(read_rebound_element(&values))
)", "call void @quidra_init_check(ptr %ref.load.addr.");
 bad_code(R"(int read_unknown_element(int[] &input, bool choice)
    int missing
    int &target
    if choice
        &target = &input[0]
    else
        &target = &missing
    return target
)", "UNINITIALIZED");
 good(R"(class Pair(local int first_value, local int second_value)
    int first = first_value
    int second = second_value
int read_first(Pair[] &input)
    int &target = &input[0].first
    return target
Pair[] values = [Pair(4, 5)]
print(read_first(&values))
)");
 bad_code(R"(class Pair(local int first_value)
    int first = first_value
    int second
int read_first(Pair[] &input)
    int &target = &input[0].first
    return target
Pair[] values = [Pair(4)]
print(read_first(&values))
)", "UNINITIALIZED_ARGUMENT");
 bad_code(R"(class Capture(local int &input)
    local int &temporary
    &temporary = &input
    int captured = temporary
int original
Capture value = Capture(&original)
)", "UNINITIALIZED");
 bad_code(R"(int read_never()
    int source
    int &target = &source
    return target
)", "UNINITIALIZED");
 bad_code(R"(int read_unknown(int &input, bool choice)
    int missing
    int &target
    if choice
        &target = &input
    else
        &target = &missing
    return target
)", "UNINITIALIZED");
 bad_code(R"(int read_unbound(int &input)
    int &target
    return target
)", "UNBOUND_REFERENCE");
 bad_code(R"(class Pair()
    int first
    int second
int read_first(Pair &input)
    int &target = &input.first
    return target
Pair value = Pair()
print(read_first(&value))
)", "UNINITIALIZED");

 llvm_contains(R"(int[] data = array(1, fill = 7)
int[] partial = array(1)
int[] entries = array(2, fill = 1)
for int entry = entries[]
    if entry == 1
        data = partial
        continue
print(data[0])
)", "call void @quidra_init_check(ptr %arr.get.slot.");
 llvm_contains(R"(int[] data = array(1, fill = 7)
int[] partial = array(1)
for 0 <= int index < 2
    if index == 0
        data = partial
        break
print(data[0])
)", "call void @quidra_init_check(ptr %arr.get.slot.");
 llvm_contains(R"(int[] data = array(1, fill = 7)
int[] partial = array(1)
for 0 <= int index < 2
    print(data[0])
    data = partial
)", "call void @quidra_init_check(ptr %arr.get.slot.");
 llvm_not_contains(R"(int[] data = array(1, fill = 7)
for 0 <= int index < 2
    print(data[0])
print(data[0])
)", "call void @quidra_init_check(ptr %arr.get.slot.");
 llvm_contains(R"(int[] data = array(1, fill = 7)
int[] partial = array(1)
nat count = 0
while count < 2
    print(data[0])
    data = partial
    count += 1
)", "call void @quidra_init_check(ptr %arr.get.slot.");
 std::cout<<"all compiler tests passed\n";
}
