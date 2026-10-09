// quidra_lowering_switch_build: builds a program the way `quidra build`
// does, with internal lowering switches turned on (ir::LoweringOptions,
// quidra/lowering.hpp). tests/lowering_switch_tests.sh builds every program
// with and without a switch and compares the runs.
//
//   quidra_lowering_switch_build [--switch NAME]... SOURCE OUTPUT
//
// NAME is reresolve-compound-stores (every compound assignment to an element
// or a field resolves its target again after the right-hand side),
// copy-value-loops (every value collection loop over storage iterates a copy
// taken at loop entry), check-reference-loops (every statement of a
// reference loop over a reference binding is followed by the check of the
// iterated array), copy-borrowed-arguments (every borrowed by-value
// argument that names storage is passed as a copy) or reorder-assignments
// (every plain assignment evaluates its target's subexpressions before its
// right-hand side).

#include "native_build.hpp"
#include "quidra/compiler.hpp"
#include "quidra/diagnostic.hpp"

#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

bool apply_switch(const std::string& name, quidra::ir::LoweringOptions& options) {
    if (name == "reresolve-compound-stores") {
        options.reresolve_every_compound_store = true;
        return true;
    }
    if (name == "copy-value-loops") {
        options.copy_every_value_loop_at_entry = true;
        return true;
    }
    if (name == "check-reference-loops") {
        options.check_every_reference_loop_statement = true;
        return true;
    }
    if (name == "copy-borrowed-arguments") {
        options.copy_every_borrowed_argument = true;
        return true;
    }
    if (name == "reorder-assignments") {
        options.reorder_every_assignment = true;
        return true;
    }
    return false;
}

int usage() {
    std::cerr << "usage: quidra_lowering_switch_build [--switch NAME]... SOURCE OUTPUT\n";
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    quidra::CompileOptions options;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--switch") {
            if (i + 1 >= argc) return usage();
            const std::string name = argv[++i];
            if (!apply_switch(name, options.lowering)) {
                std::cerr << "unknown lowering switch: " << name << "\n";
                return 2;
            }
        } else {
            positional.push_back(argument);
        }
    }
    if (positional.size() != 2) return usage();
    const fs::path source = positional[0];
    const fs::path output = positional[1];
    try {
        const auto result = quidra::compile_file(source, options, fs::current_path());
        auto llvm = output;
        llvm += ".ll";
        {
            std::ofstream out(llvm, std::ios::binary);
            out << result.llvm;
            if (!out) {
                std::cerr << "cannot write " << llvm.string() << "\n";
                return 1;
            }
        }
        const auto status = quidra::native::link_llvm(llvm, output);
        std::error_code ec;
        fs::remove(llvm, ec);
        return status == 0 ? 0 : 1;
    } catch (const quidra::CompileErrors& errors) {
        for (const auto& diagnostic : errors.diagnostics())
            std::cerr << diagnostic.code << ": " << diagnostic.message << "\n";
    } catch (const quidra::CompileError& error) {
        std::cerr << error.diagnostic().code << ": " << error.diagnostic().message << "\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
    }
    return 1;
}
