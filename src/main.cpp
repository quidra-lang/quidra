#include "quidra/compiler.hpp"
#include "quidra/formatter.hpp"
#include "quidra/ir.hpp"
#include "quidra/language.hpp"
#include "quidra/manifest.hpp"
#include "quidra/source_tools.hpp"
#include "quidra/source_patch.hpp"
#include "quidra/tooling.hpp"
#include "quidra/version.hpp"
#include "repl_cli.hpp"
#include "lsp_server.hpp"
#include "package_cli.hpp"
#include "native_build.hpp"
#include "run_artifact.hpp"
#include "jit.hpp"
#include "device_cli.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <io.h>
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open input file: " + path.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void write_file(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write file: " + path.string());
    out << text;
}

void write_file_atomic(const fs::path& path, const std::string& text) {
    const auto original_permissions = fs::status(path).permissions();
    std::random_device rd;
    const auto temp = path.parent_path() /
        ("." + path.filename().string() + ".qui-tmp-" + std::to_string(rd()) + ".tmp");
    try {
        write_file(temp, text);
        std::error_code ec;
        fs::permissions(temp, original_permissions, fs::perm_options::replace, ec);
        if (ec) throw std::runtime_error("cannot preserve source permissions: " + ec.message());
#ifdef _WIN32
        if (!MoveFileExW(
                temp.c_str(), path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::runtime_error(
                "cannot replace source atomically: Windows error " +
                std::to_string(GetLastError()));
        }
#else
        fs::rename(temp, path, ec);
        if (ec) throw std::runtime_error("cannot replace source atomically: " + ec.message());
#endif
    } catch (...) {
        std::error_code ec;
        fs::remove(temp, ec);
        throw;
    }
}

void validate_patched_file(const fs::path& path, std::string_view source) {
    std::random_device rd;
    const auto temp = path.parent_path() /
        ("." + path.stem().string() + ".patch-check-" + std::to_string(rd()) + ".qui");
    try {
        write_file(temp, std::string(source));
        (void)quidra::check_file(temp, {}, fs::current_path());
        std::error_code ec;
        fs::remove(temp, ec);
    } catch (...) {
        std::error_code ec;
        fs::remove(temp, ec);
        throw;
    }
}

std::string json_escape(const std::string& s) {
    std::ostringstream out;
    for (char raw : s) {
        const auto c = static_cast<unsigned char>(raw);
        switch (c) {
            case '\\': out << "\\\\"; break;
            case '"': out << "\\\""; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) {
                    const char* hex = "0123456789abcdef";
                    out << "\\u00" << hex[(c >> 4) & 0xf] << hex[c & 0xf];
                } else {
                    out << static_cast<char>(c);
                }
        }
    }
    return out.str();
}

void require_qui_source(const fs::path& path) {
    if (path.extension() != quidra::source_extension) {
        throw std::runtime_error(
            "Quidra source files use the .qui extension: " + path.string());
    }
}

class TemporaryBuild {
public:
    TemporaryBuild() {
        const auto base = fs::temp_directory_path();
        std::random_device rd;
        const auto now = static_cast<unsigned long long>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count());
        for (unsigned attempt = 0; attempt < 64; ++attempt) {
            const auto nonce = (static_cast<unsigned long long>(rd()) << 32U) ^ rd() ^ now ^ attempt;
            dir_ = base / ("quidra-" + std::to_string(nonce));
            std::error_code ec;
            if (fs::create_directory(dir_, ec)) return;
        }
        throw std::runtime_error("cannot create temporary build directory");
    }

    TemporaryBuild(const TemporaryBuild&) = delete;
    TemporaryBuild& operator=(const TemporaryBuild&) = delete;

    ~TemporaryBuild() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    fs::path executable() const {
#ifdef _WIN32
        return dir_ / "program.exe";
#else
        return dir_ / "program";
#endif
    }

    fs::path llvm() const { return dir_ / "program.ll"; }

private:
    fs::path dir_;
};

std::size_t parse_max_errors(const std::string& text) {
    std::size_t consumed = 0;
    unsigned long long value = 0;
    try {
        value = std::stoull(text, &consumed);
    } catch (...) {
        throw std::runtime_error("--max-errors requires a positive integer");
    }
    if (consumed != text.size() || value == 0) {
        throw std::runtime_error("--max-errors requires a positive integer");
    }
    return static_cast<std::size_t>(value);
}

int build_native(const fs::path& source, const fs::path& output, bool keep_llvm = false,
                 std::size_t max_errors = 20, bool debug = false,
                 const std::vector<fs::path>& link_inputs = {}) {
    require_qui_source(source);
    auto result = quidra::compile_file(
        source, quidra::CompileOptions{max_errors, debug}, fs::current_path());
    auto ll = output;
    ll += ".ll";
    write_file(ll, result.llvm);

    const auto rc = quidra::native::link_llvm(
        ll, output, quidra::native::LinkOptions{debug, true, link_inputs});
    if (!keep_llvm) {
        std::error_code ec;
        fs::remove(ll, ec);
    }
    return rc == 0 ? 0 : 1;
}

int run_native(const fs::path& source, std::size_t max_errors = 20,
               const std::vector<std::string>& program_args = {},
               const std::vector<fs::path>& link_inputs = {}) {
    quidra::run_artifact::TemporaryArtifact temp(source);
    const auto& executable = temp.executable();
    if (build_native(source, executable, false, max_errors, false, link_inputs) != 0) return 1;
    return quidra::native::run_program(executable, program_args);
}

std::string description_json() {
    return std::string(quidra::language_manifest_json);
}

void usage(std::ostream& out) {
    out << "Quidra " << quidra::compiler_version << "\n"
        << "usage:\n"
        << "  quidra                            start REPL when stdin is a TTY\n"
        << "  quidra repl                       start REPL explicitly\n"
        << "  quidra lsp                        start Language Server Protocol server on stdio\n"
        << "  quidra install PACKAGE[@VERSION] install latest compatible tagged package release\n"
        << "  quidra remove NAME                remove an installed package\n"
        << "  quidra list                       list installed packages and versions\n"
        << "  quidra package-info NAME [--json] show installed package metadata\n"
        << "  quidra lock FILE.qui [--check]    write or verify quidra.lock\n"
        << "  quidra package-path               print the default package store path\n"
        << "  quidra gpu                        list supported GPU devices and backends\n"
        << "  quidra info                       print CPU/GPU backend information\n"
        << "  quidra FILE.qui [ARGS...]         AOT compile/link and run with program arguments\n"
        << "  quidra run FILE.qui [--link FILE] [-- ARGS...]\n"
        << "                                      compile and run; --link is repeatable\n"
        << "  quidra check FILE.qui [--json] [--max-errors N]\n"
        << "                                      type-check without building\n"
        << "  quidra build FILE.qui [-o FILE] [--debug] [--link FILE] [--max-errors N]\n"
        << "                                      build native executable; --link is repeatable\n"
        << "  quidra debug FILE.qui [--link FILE] [-- ARGS...]\n"
        << "                                      build with debug symbols and launch lldb/gdb\n"
        << "  quidra fmt FILE.qui [--check]     format source; --check only verifies canonical form\n"
        << "  quidra ir FILE.qui                print typed Quidra IR\n"
        << "  quidra llvm FILE.qui              print generated LLVM IR\n"
        << "  quidra inspect FILE.qui [--no-source] [--no-effects] [--kind KIND] [--depth N]\n"
        << "                                      print filtered typed source nodes as JSON\n"
        << "  quidra patch FILE.qui PATCH.json [--write]\n"
        << "                                      validate/apply a revision-safe structural patch\n"
        << "  quidra describe [grammar|patch-schema|llm]\n"
        << "                                      print machine-readable language/tooling contracts\n"
        << "  quidra --version                   print compiler version\n";
}

void print_diagnostics(const fs::path& input, const std::vector<quidra::Diagnostic>& diagnostics,
                       bool truncated, bool json) {
    if (json) {
        std::cout << "{\"ok\":false,\"truncated\":" << (truncated ? "true" : "false")
                  << ",\"diagnostics\":[";
        for (std::size_t i = 0; i < diagnostics.size(); ++i) {
            if (i) std::cout << ",";
            const auto& d = diagnostics[i];
            std::cout << "{\"code\":\"" << json_escape(d.code)
                      << "\",\"message\":\"" << json_escape(d.message)
                      << "\",\"file\":\"" << json_escape(input.string())
                      << "\",\"span\":{\"start\":{\"line\":" << d.span.start.line
                      << ",\"column\":" << d.span.start.column
                      << "},\"end\":{\"line\":" << d.span.end.line
                      << ",\"column\":" << d.span.end.column << "}}}";
        }
        std::cout << "]}\n";
        return;
    }

    for (const auto& d : diagnostics) {
        std::cerr << input.string() << ":" << d.span.start.line << ":" << d.span.start.column
                  << ": error[" << d.code << "] " << d.message << "\n";
    }
    std::cerr << diagnostics.size() << " error(s) generated.";
    if (truncated) std::cerr << " Further diagnostics suppressed.";
    std::cerr << "\n";
}

void print_compile_error(const fs::path& input, const quidra::CompileError& error, bool json) {
    print_diagnostics(input, {error.diagnostic()}, false, json);
}

void print_compile_errors(const fs::path& input, const quidra::CompileErrors& errors, bool json) {
    print_diagnostics(input, errors.diagnostics(), errors.truncated(), json);
}

int describe_command(int argc, char** argv) {
    if (argc == 2) {
        std::cout << description_json() << "\n";
        return 0;
    }
    if (argc != 3) {
        std::cerr << "quidra: usage: quidra describe [grammar|patch-schema|llm]\n";
        return 2;
    }
    const std::string topic = argv[2];
    if (topic == "grammar") {
        std::cout << quidra::grammar_ebnf;
        if (quidra::grammar_ebnf.empty() || quidra::grammar_ebnf.back() != '\n') std::cout << '\n';
        return 0;
    }
    if (topic == "patch-schema") {
        std::cout << quidra::patch_schema_json() << "\n";
        return 0;
    }
    if (topic == "llm") {
        std::cout << quidra::llm_interface_json << "\n";
        return 0;
    }
    std::cerr << "quidra: unknown describe topic: " << topic << "\n";
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    quidra::run_artifact::cleanup_stale();

    if (argc == 1) {
#ifdef _WIN32
        if (::_isatty(::_fileno(stdin))) return quidra::cli::run_repl();
#else
        if (::isatty(STDIN_FILENO)) return quidra::cli::run_repl();
#endif
        usage(std::cerr);
        return 2;
    }

    if (argc >= 2) {
        const std::string package_command = argv[1];
        if (package_command == "install" ||
            package_command == "remove" ||
            package_command == "list" ||
            package_command == "package-info" ||
            package_command == "lock" ||
            package_command == "package-path") {
            return quidra::cli::run_package_cli(argc - 1, argv + 1);
        }
    }

    // Legacy alias retained for projects/scripts written before the short CLI.
    if (argc >= 2 && std::string(argv[1]) == "package") {
        return quidra::cli::run_package_cli(argc - 2, argv + 2);
    }

    if (argc == 2 && std::string(argv[1]) == "gpu") {
        return quidra::cli::run_gpu_cli(false);
    }

    if (argc == 2 && std::string(argv[1]) == "info") {
        return quidra::cli::run_gpu_cli(true);
    }

    if (argc >= 2 && std::string(argv[1]) == "describe") {
        return describe_command(argc, argv);
    }

    if (argc == 3 && std::string(argv[1]) == "__jit-run") {
        try {
            return quidra::jit::run_llvm(read_file(argv[2]), argv[2], {});
        } catch (const std::exception& e) {
            std::cerr << "quidra: JIT execution failed: " << e.what() << "\n";
            return 1;
        }
    }

    if (argc == 4 && std::string(argv[1]) == "__jit-server") {
        try {
            return quidra::jit::run_server(argv[2], argv[3]);
        } catch (const std::exception& e) {
            std::cerr << "quidra: persistent JIT failed: " << e.what() << "\n";
            return 1;
        }
    }

    if (argc >= 2 && fs::path(argv[1]).extension() == quidra::source_extension) {
        const fs::path input = argv[1];
        std::vector<std::string> program_args;
        for (int i = 2; i < argc; ++i) program_args.emplace_back(argv[i]);
        try {
            return run_native(input, 20, program_args);
        } catch (const quidra::CompileErrors& e) {
            print_compile_errors(input, e, false);
            return 1;
        } catch (const quidra::CompileError& e) {
            print_compile_error(input, e, false);
            return 1;
        } catch (const std::exception& e) {
            std::cerr << "quidra: " << e.what() << "\n";
            return 1;
        }
    }

    if (argc == 2) {
        const std::string arg = argv[1];
        if (arg == "--version" || arg == "-V") {
            std::cout << "quidra " << quidra::compiler_version << "\n";
            return 0;
        }
        if (arg == "--help" || arg == "-h") {
            usage(std::cout);
            return 0;
        }
        if (arg == "repl") {
            return quidra::cli::run_repl();
        }
        if (arg == "lsp") {
            return quidra::cli::run_lsp();
        }

        usage(std::cerr);
        return 2;
    }

    if (argc < 3) {
        usage(std::cerr);
        return 2;
    }

    const std::string command = argv[1];
    const fs::path input = argv[2];
    bool json = false;
    for (int i = 3; i < argc; ++i) {
        if (std::string(argv[i]) == "--json") json = true;
    }

    try {
        require_qui_source(input);

        if (command == "check") {
            std::size_t max_errors = 20;
            for (int i = 3; i < argc; ++i) {
                const std::string option = argv[i];
                if (option == "--json") {
                    json = true;
                } else if (option == "--max-errors" && i + 1 < argc) {
                    max_errors = parse_max_errors(argv[++i]);
                } else {
                    throw std::runtime_error("unknown check option: " + option);
                }
            }
            (void)quidra::check_file(input, quidra::CompileOptions{max_errors}, fs::current_path());
            if (json) {
                std::cout << "{\"ok\":true,\"language_version\":\"" << quidra::language_version
                          << "\",\"truncated\":false,\"diagnostics\":[]}\n";
            } else {
                std::cout << input.string() << ": ok\n";
            }
            return 0;
        }

        if (command == "fmt") {
            bool check_only=false;
            for(int i=3;i<argc;++i){
                const std::string option=argv[i];
                if(option=="--check") check_only=true;
                else throw std::runtime_error("unknown fmt option: "+option);
            }
            const auto source=read_file(input);
            const auto formatted=quidra::format_source(source);
            if(check_only) return formatted==source?0:1;
            if(formatted!=source) write_file_atomic(input,formatted);
            return 0;
        }

        if (command == "ir") {
            if (argc != 3) throw std::runtime_error("quidra ir does not accept additional options");
            auto c = quidra::compile_file(input, {}, fs::current_path());
            std::cout << quidra::ir::dump(c.ir);
            return 0;
        }

        if (command == "llvm") {
            if (argc != 3) throw std::runtime_error("quidra llvm does not accept additional options");
            auto c = quidra::compile_file(input, {}, fs::current_path());
            std::cout << c.llvm;
            return 0;
        }

        if (command == "inspect") {
            quidra::InspectOptions inspect_options;
            for (int i = 3; i < argc; ++i) {
                const std::string option = argv[i];
                if (option == "--no-source") {
                    inspect_options.include_source = false;
                } else if (option == "--no-effects") {
                    inspect_options.include_effects = false;
                } else if (option == "--kind" && i + 1 < argc) {
                    inspect_options.kind = argv[++i];
                } else if (option == "--depth" && i + 1 < argc) {
                    std::size_t consumed = 0;
                    unsigned long long depth = 0;
                    try {
                        depth = std::stoull(argv[++i], &consumed);
                    } catch (...) {
                        throw std::runtime_error("--depth requires a nonnegative integer");
                    }
                    const std::string depth_text = argv[i];
                    if (consumed != depth_text.size()) {
                        throw std::runtime_error("--depth requires a nonnegative integer");
                    }
                    inspect_options.max_depth = static_cast<std::size_t>(depth);
                } else {
                    throw std::runtime_error("unknown inspect option: " + option);
                }
            }
            const auto source = read_file(input);
            auto checked = quidra::check_file(input, {}, fs::current_path());
            std::cout << quidra::inspect_source_json(
                source, checked, input.string(), std::move(inspect_options)) << "\n";
            return 0;
        }

        if (command == "patch") {
            if (argc != 4 && argc != 5) throw std::runtime_error("usage: quidra patch FILE.qui PATCH.json [--write]");
            const bool write = argc == 5 && std::string(argv[4]) == "--write";
            if (argc == 5 && !write) throw std::runtime_error("unknown patch option: " + std::string(argv[4]));
            const auto source = read_file(input);
            auto checked = quidra::check_file(input, {}, fs::current_path());
            const auto patch_text = read_file(argv[3]);
            const auto patched = quidra::apply_source_patch(
                source,
                checked,
                patch_text,
                [&](std::string_view updated) { validate_patched_file(input, updated); });
            if (write) {
                write_file_atomic(input, patched.source);
                std::cout << "{\"ok\":true,\"base_revision\":\"" << patched.base_revision
                          << "\",\"revision\":\"" << patched.revision << "\"}\n";
            } else {
                std::cout << patched.source;
            }
            return 0;
        }

        if (command == "build") {
            fs::path output = input.stem();
            bool keep = false;
            bool debug = false;
            std::size_t max_errors = 20;
            std::vector<fs::path> link_inputs;
            for (int i = 3; i < argc; ++i) {
                const std::string option = argv[i];
                if (option == "-o" && i + 1 < argc) output = argv[++i];
                else if (option == "--keep-llvm") keep = true;
                else if (option == "--debug") debug = true;
                else if (option == "--link" && i + 1 < argc) link_inputs.emplace_back(argv[++i]);
                else if (option == "--link") throw std::runtime_error("--link requires a file path");
                else if (option == "--max-errors" && i + 1 < argc) max_errors = parse_max_errors(argv[++i]);
                else throw std::runtime_error("unknown build option: " + option);
            }
            return build_native(input, output, keep, max_errors, debug, link_inputs);
        }

        if (command == "debug") {
            std::vector<std::string> program_args;
            std::vector<fs::path> link_inputs;
            bool program_mode = false;
            for (int i = 3; i < argc; ++i) {
                const std::string option = argv[i];
                if (program_mode) program_args.push_back(option);
                else if (option == "--") program_mode = true;
                else if (option == "--link" && i + 1 < argc) link_inputs.emplace_back(argv[++i]);
                else if (option == "--link") throw std::runtime_error("--link requires a file path");
                else throw std::runtime_error("unknown debug option: " + option);
            }
            TemporaryBuild temp;
            const auto executable = temp.executable();
            if (build_native(input, executable, true, 20, true, link_inputs) != 0) return 1;
            return quidra::native::run_debugger(executable, program_args);
        }

        if (command == "run") {
            std::size_t max_errors = 20;
            std::vector<std::string> program_args;
            std::vector<fs::path> link_inputs;
            bool program_mode = false;
            for (int i = 3; i < argc; ++i) {
                const std::string option = argv[i];
                if (program_mode) program_args.push_back(option);
                else if (option == "--") program_mode = true;
                else if (option == "--link" && i + 1 < argc) link_inputs.emplace_back(argv[++i]);
                else if (option == "--link") throw std::runtime_error("--link requires a file path");
                else if (option == "--max-errors" && i + 1 < argc) max_errors = parse_max_errors(argv[++i]);
                else throw std::runtime_error("unknown run option: " + option + "; use '--' before program arguments");
            }
            return run_native(input, max_errors, program_args, link_inputs);
        }

        usage(std::cerr);
        return 2;
    } catch (const quidra::PatchError& e) {
        std::cerr << input.string();
        if (!e.node_id().empty()) std::cerr << ":" << e.span().start.line << ":" << e.span().start.column;
        std::cerr << ": error[" << e.code() << "] " << e.what();
        if (!e.node_id().empty()) std::cerr << " (node " << e.node_id() << ")";
        std::cerr << "\n";
        return 1;
    } catch (const quidra::CompileErrors& e) {
        print_compile_errors(input, e, json);
        return 1;
    } catch (const quidra::CompileError& e) {
        print_compile_error(input, e, json);
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "quidra: " << e.what() << "\n";
        return 1;
    }
}
