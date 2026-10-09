// quidra_golden_dump: every observable output of the compiler for one input
// (README.md, Views). The gates compare these outputs between a base
// and a head; see tests/ir_golden.sh.
//
//   quidra_golden_dump [--root DIR] --out DIR [--views LIST] [--cwd DIR]
//                      (FILE | --memory FILE | --fixture NAME | --repl-session FILE)
//   quidra_golden_dump --list-fixtures
//   quidra_golden_dump --batch LIST --order forward|reverse --out DIR [--views LIST]
//   quidra_golden_dump --time-stage lower|optimize|emit --repeat N [--cwd DIR] FILE
//
// The `inputs` view of a file entry (what the compilation reads from the file
// system, as quidra/compile_inputs.hpp records it) is written only when it is
// requested with --views: captures do not ask for it, so that captures of
// compilers without the recorder stay comparable.
//
// Each view is written to DIR/<view>; a view whose stage throws is written to
// DIR/<view>.error instead. DIR/status.tsv lists `<view>\t<status>` in view
// name order, where status is `ok` or `error:<code>`.
//
// Views are computed in the order they are listed, and each compilation stage
// runs once, at the first view that needs it, so a permuted view list changes
// the order of compilations inside the process (selfcheck uses this).
#include "fixtures/backend_fixtures.hpp"
#include "fixtures/optimizer_probes.hpp"
#include "census.hpp"
#include "failure.hpp"
#include "ir_full.hpp"
#include "optimizer_fixtures.hpp"
#include "repl_session.hpp"

#include "quidra/compile_inputs.hpp"
#include "quidra/compiler.hpp"
#include "quidra/diagnostic.hpp"
#include "quidra/frontend.hpp"
#include "quidra/llvm_backend.hpp"
#include "quidra/source_tools.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace fs = std::filesystem;

namespace quidra::golden {
namespace {

// A compilation stage that runs at most once and remembers its failure.
template <class T>
class Stage {
public:
    template <class Make>
    const T* get(Make&& make) {
        if (!done_) {
            done_ = true;
            try {
                value_.emplace(make());
            } catch (...) {
                failure_ = current_failure();
            }
        }
        return value_ ? &*value_ : nullptr;
    }
    const Failure& failure() const { return *failure_; }

private:
    bool done_{};
    std::optional<T> value_;
    std::optional<Failure> failure_;
};

struct ViewResult {
    std::string content;
    std::optional<Failure> failure;
    // A view whose content records failures inline (fixture) keeps its file
    // name and reports the first failure's status.
    std::optional<std::string> status;
};

ViewResult ok(std::string content) { return {std::move(content), std::nullopt, std::nullopt}; }
ViewResult failed(const Failure& failure) { return {{}, failure, std::nullopt}; }

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path.string());
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

void write_file(const fs::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write " + path.string());
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string deps_view(const CheckedProgram& checked) {
    std::string out = "root " + quote(checked.program.root_source_file) + "\n";
    for (const auto& [path, text] : checked.program.source_texts) {
        out += "source " + quote(path) + " " + std::to_string(text.size()) + " " +
               sha256_hex(text) + "\n";
    }
    for (const auto& extension : checked.compiler_extensions) {
        out += "extension " + quote(extension.package + "." + extension.name) + " " +
               quote(extension.package_root) + " " + quote(extension.descriptor_path) + "\n";
    }
    return out;
}

std::string inputs_view(const CompileInputs& inputs) {
    std::string out;
    for (const auto& record : inputs.records()) {
        std::visit(
            [&](const auto& input) {
                using T = std::decay_t<decltype(input)>;
                if constexpr (std::is_same_v<T, InputFile>) {
                    out += std::string(input_file_kind_name(input.kind)) + " " +
                           quote(input.path.string()) + " " + std::to_string(input.size) + " " +
                           input.sha256;
                } else if constexpr (std::is_same_v<T, AbsentInput>) {
                    out += "absent " + quote(input.path.string()) + " " +
                           input_path_state_name(input.state);
                } else if constexpr (std::is_same_v<T, LocalImportInput>) {
                    out += "local_import " + quote(input.importer.string()) + " " +
                           quote(input.target) + " " + quote(input.path.string());
                } else if constexpr (std::is_same_v<T, PackageInput>) {
                    out += "package " + quote(input.name) + " " + quote(input.main.string());
                } else {
                    out += "package_tree " + quote(input.main.string()) + " " + input.sha256;
                }
                out += "\n";
            },
            record);
    }
    if (!inputs.complete()) out += "incomplete " + quote(inputs.incomplete_reason()) + "\n";
    return out;
}

// ---------------------------------------------------------------------------
// Entries

// A source file compiled the way the CLI compiles it.
class FileEntry {
public:
    FileEntry(fs::path path, fs::path cwd) : path_(std::move(path)), cwd_(std::move(cwd)) {}

    static const std::vector<std::string>& views() {
        static const std::vector<std::string> names{
            "ir",   "ir.full", "ir.lowered", "llvm",    "llvm.lowered", "llvm.lowered.debug",
            "llvm.debug", "repl", "diagnostics", "inspect", "deps",   "census",
            "census.repl"};
        return names;
    }

    ViewResult render(std::string_view view) {
        if (view == "ir" || view == "ir.full" || view == "llvm") {
            const auto* c = optimized();
            if (!c) return failed(optimized_.failure());
            if (view == "ir") return ok(ir::dump(c->ir));
            if (view == "ir.full") return ok(serialize_module(c->ir));
            return ok(c->llvm);
        }
        if (view == "llvm.debug") {
            const auto* c = debug_.get([&] { return compile_file(path_, {20, true}, cwd_); });
            if (!c) return failed(debug_.failure());
            return ok(c->llvm);
        }
        if (view == "ir.lowered") {
            const auto* m = lowered();
            if (!m) return failed(lowered_failure());
            return ok(serialize_module(*m));
        }
        if (view == "llvm.lowered" || view == "llvm.lowered.debug") {
            const bool debug = view == "llvm.lowered.debug";
            auto& stage = debug ? lowered_llvm_debug_ : lowered_llvm_;
            const auto* m = lowered();
            if (!m) return failed(lowered_failure());
            const auto* text = stage.get([&] { return emit_llvm(*m, debug); });
            if (!text) return failed(stage.failure());
            return ok(*text);
        }
        if (view == "repl") {
            const auto* r = repl();
            if (!r) return failed(repl_.failure());
            std::string out = "expression_type ";
            out += r->expression_type ? serialize_type(*r->expression_type) : "none";
            out += "\n--- ir.full\n" + serialize_module(r->compilation.ir);
            out += "--- llvm\n" + r->compilation.llvm;
            return ok(out);
        }
        if (view == "diagnostics") {
            if (!checked()) return failed(checked_.failure());
            return ok("ok\n");
        }
        if (view == "inspect") {
            const auto* c = checked();
            if (!c) return failed(checked_.failure());
            const auto* json = inspect_.get(
                [&] { return inspect_source_json(read_file(path_), *c, path_.string(), {}); });
            if (!json) return failed(inspect_.failure());
            return ok(*json + "\n");
        }
        if (view == "deps") {
            const auto* c = checked();
            if (!c) return failed(checked_.failure());
            return ok(deps_view(*c));
        }
        if (view == "inputs") {
            const auto* recorded = inputs_.get([&] {
                CompileInputs inputs;
                (void)load_program_with_modules(path_, cwd_, 20, &inputs);
                return inputs;
            });
            if (!recorded) return failed(inputs_.failure());
            return ok(inputs_view(*recorded));
        }
        if (view == "census") {
            const auto* m = lowered();
            const auto* c = optimized();
            return ok(census_view(m, c ? &c->ir : nullptr, checked()));
        }
        if (view == "census.repl") {
            // The REPL lowering (display and replay instructions), with the
            // expression the REPL displays (repl_session.hpp).
            const auto* c = checked();
            const ir::Module* lowered_repl = nullptr;
            if (c) {
                lowered_repl =
                    repl_lowered_.get([&] { return ir::lower(*c, displayed_expression(*c), 0); });
            }
            const auto* r = repl();
            return ok(census_view(lowered_repl, r ? &r->compilation.ir : nullptr, c));
        }
        throw std::runtime_error("unknown view for a file entry: " + std::string(view));
    }

private:
    const Compilation* optimized() {
        return optimized_.get([&] { return compile_file(path_, {}, cwd_); });
    }
    const CheckedProgram* checked() {
        return checked_.get([&] { return check_file(path_, {}, cwd_); });
    }
    const ir::Module* lowered() {
        const auto* c = checked();
        if (!c) return nullptr;
        return lowered_.get([&] { return ir::lower(*c); });
    }
    const Failure& lowered_failure() {
        return checked() ? lowered_.failure() : checked_.failure();
    }
    const ReplCompilation* repl() {
        return repl_.get([&] { return compile_repl_file(path_, {}, cwd_, 0); });
    }

    fs::path path_;
    fs::path cwd_;
    Stage<Compilation> optimized_;
    Stage<Compilation> debug_;
    Stage<CheckedProgram> checked_;
    Stage<ir::Module> lowered_;
    Stage<std::string> lowered_llvm_;
    Stage<std::string> lowered_llvm_debug_;
    Stage<ReplCompilation> repl_;
    Stage<ir::Module> repl_lowered_;
    Stage<std::string> inspect_;
    Stage<CompileInputs> inputs_;
};

// A source string compiled through quidra::compile, the `<memory>` path that
// tests/compiler_tests.cpp uses. A program captured from repl_ir_contains
// (sidecar FILE.expect: `helper repl_ir_contains`, `replay_prefix_bytes N`)
// compiles through compile_repl_file_source instead, as that helper does.
class MemoryEntry {
public:
    MemoryEntry(std::string source, std::optional<std::size_t> replay_prefix_bytes, fs::path cwd)
        : source_(std::move(source)),
          replay_prefix_bytes_(replay_prefix_bytes),
          cwd_(std::move(cwd)) {}

    static MemoryEntry from_file(const fs::path& path, const fs::path& cwd) {
        std::optional<std::size_t> replay_prefix_bytes;
        auto sidecar = path;
        sidecar.replace_extension(".expect");
        if (fs::exists(sidecar)) {
            std::istringstream lines(read_file(sidecar));
            std::string key;
            std::string value;
            bool repl = false;
            std::size_t prefix = 0;
            while (lines >> key >> value) {
                if (key == "helper") repl = value == "repl_ir_contains";
                if (key == "replay_prefix_bytes") prefix = std::stoull(value);
            }
            if (repl) replay_prefix_bytes = prefix;
        }
        return MemoryEntry(read_file(path), replay_prefix_bytes, cwd);
    }

    static const std::vector<std::string>& views() {
        static const std::vector<std::string> names{"compile.memory", "census"};
        return names;
    }

    ViewResult render(std::string_view view) {
        if (view == "compile.memory" && replay_prefix_bytes_) {
            const auto* r = repl();
            if (!r) return failed(repl_.failure());
            std::string out = "--- expression_type\n";
            out += r->expression_type ? serialize_type(*r->expression_type) : "none";
            out += "\n--- ir\n" + ir::dump(r->compilation.ir) + "--- ir.full\n" +
                   serialize_module(r->compilation.ir) + "--- llvm\n" + r->compilation.llvm;
            return ok(out);
        }
        if (view == "compile.memory") {
            const auto* c = optimized();
            if (!c) return failed(optimized_.failure());
            return ok("--- ir\n" + ir::dump(c->ir) + "--- ir.full\n" + serialize_module(c->ir) +
                        "--- llvm\n" + c->llvm);
        }
        if (view == "census" && replay_prefix_bytes_) {
            const auto* r = repl();
            return ok(census_view(nullptr, r ? &r->compilation.ir : nullptr));
        }
        if (view == "census") {
            const auto* checked = checked_.get([&] { return check(source_); });
            const ir::Module* lowered =
                checked ? lowered_.get([&] { return ir::lower(*checked); }) : nullptr;
            const auto* c = optimized();
            return ok(census_view(lowered, c ? &c->ir : nullptr, checked));
        }
        throw std::runtime_error("unknown view for a memory entry: " + std::string(view));
    }

private:
    const Compilation* optimized() {
        return optimized_.get([&] { return compile(source_); });
    }
    const ReplCompilation* repl() {
        return repl_.get([&] {
            return compile_repl_file_source(cwd_ / "repl_ir_contains.qui", source_, {}, cwd_,
                                            *replay_prefix_bytes_);
        });
    }

    std::string source_;
    std::optional<std::size_t> replay_prefix_bytes_;
    fs::path cwd_;
    Stage<Compilation> optimized_;
    Stage<CheckedProgram> checked_;
    Stage<ir::Module> lowered_;
    Stage<ReplCompilation> repl_;
};

// A hand-built module: ir.full of optimize(M), and for modules built for the
// backend also emit_llvm(M) without and with debug information. Each section
// records its own failure inline. The optimizer fixtures
// (tests/optimizer_fixtures.hpp) model only what the optimizer reads (callees
// without bodies, undefined operands), so they are not emitted.
class FixtureEntry {
public:
    FixtureEntry(ir::Module module, bool emit) : module_(std::move(module)), emit_(emit) {}

    static const std::vector<std::string>& views() {
        static const std::vector<std::string> names{"fixture", "census"};
        return names;
    }

    ViewResult render(std::string_view view) {
        if (view == "fixture") {
            std::string out;
            std::optional<std::string> status;
            const auto section = [&](std::string_view title, auto&& make) {
                out += "--- " + std::string(title) + "\n";
                try {
                    out += make();
                } catch (...) {
                    const auto failure = current_failure();
                    out += failure.text;
                    if (!status) status = failure.status;
                }
            };
            section("ir.full(optimize)", [&] {
                const auto* m = optimized();
                if (!m) {
                    status = optimized_.failure().status;
                    return optimized_.failure().text;
                }
                return serialize_module(*m);
            });
            if (emit_) {
                section("llvm", [&] { return emit_llvm(module_, false); });
                section("llvm.debug", [&] { return emit_llvm(module_, true); });
            }
            return {std::move(out), std::nullopt, status};
        }
        if (view == "census") {
            const auto* m = optimized();
            return ok(census_view(&module_, m));
        }
        throw std::runtime_error("unknown view for a fixture entry: " + std::string(view));
    }

private:
    const ir::Module* optimized() {
        return optimized_.get([&] { return ir::optimize(ir::Module(module_)); });
    }

    ir::Module module_;
    bool emit_{};
    Stage<ir::Module> optimized_;
};

// A REPL session: the input of `quidra repl < FILE`, compiled the way the
// REPL compiles it (repl_session.hpp).
class ReplSessionEntry {
public:
    ReplSessionEntry(std::string input, fs::path cwd) : input_(std::move(input)), cwd_(std::move(cwd)) {}

    static const std::vector<std::string>& views() {
        static const std::vector<std::string> names{"repl.session", "census"};
        return names;
    }

    ViewResult render(std::string_view view) {
        if (!text_) text_ = simulate_repl_session(input_, cwd_, &modules_);
        if (view == "repl.session") return ok(*text_);
        if (view == "census") {
            std::vector<const ir::Module*> lowered;
            std::vector<const ir::Module*> optimized;
            for (const auto& module : modules_.lowered) lowered.push_back(&module);
            for (const auto& module : modules_.optimized) optimized.push_back(&module);
            return ok(census_view(lowered, optimized));
        }
        throw std::runtime_error("unknown view for a REPL session: " + std::string(view));
    }

private:
    std::string input_;
    fs::path cwd_;
    std::optional<std::string> text_;
    SessionModules modules_;
};

struct FixtureSource {
    std::string_view group;
    std::string_view name;
    ir::Module (*build)();
    bool emit{};
};

std::vector<FixtureSource> fixtures() {
    std::vector<FixtureSource> result;
    for (const auto& fixture : optimizer_fixtures::all) {
        result.push_back({"optimizer", fixture.name, fixture.build, false});
    }
    for (const auto& fixture : optimizer_probes::all) {
        result.push_back({"optimizer", fixture.name, fixture.build, false});
    }
    for (const auto& fixture : backend_fixtures::all) {
        result.push_back({"backend", fixture.name, fixture.build, true});
    }
    return result;
}

FixtureEntry fixture_entry(std::string_view name) {
    for (const auto& fixture : fixtures()) {
        if (name == std::string(fixture.group) + "/" + std::string(fixture.name)) {
            return FixtureEntry(fixture.build(), fixture.emit);
        }
    }
    throw std::runtime_error("unknown fixture: " + std::string(name));
}

// ---------------------------------------------------------------------------
// Driver

std::vector<std::string> split(std::string_view text, char separator) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find(separator, start);
        if (end == std::string_view::npos) {
            parts.emplace_back(text.substr(start));
            break;
        }
        parts.emplace_back(text.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

enum class EntryKind { File, Memory, Fixture, ReplSession };

struct EntrySpec {
    std::string name;
    EntryKind kind{EntryKind::File};
    fs::path path;
    fs::path cwd;
};

EntryKind parse_kind(std::string_view kind) {
    if (kind == "file") return EntryKind::File;
    if (kind == "memory") return EntryKind::Memory;
    if (kind == "fixture") return EntryKind::Fixture;
    if (kind == "repl-session") return EntryKind::ReplSession;
    throw std::runtime_error("unknown entry kind: " + std::string(kind));
}

template <class Entry>
void dump_entry(Entry& entry, const std::vector<std::string>& requested, const fs::path& out) {
    fs::create_directories(out);
    const auto& views = requested.empty() ? Entry::views() : requested;
    std::map<std::string, std::string> statuses;
    for (const auto& view : views) {
        auto result = entry.render(view);
        if (result.status) {
            write_file(out / view, result.content);
            statuses[view] = *result.status;
        } else if (result.failure) {
            write_file(out / (view + ".error"), result.failure->text);
            statuses[view] = result.failure->status;
        } else {
            write_file(out / view, result.content);
            statuses[view] = "ok";
        }
    }
    std::string table;
    for (const auto& [view, status] : statuses) table += view + "\t" + status + "\n";
    write_file(out / "status.tsv", table);
}

void dump_spec(const EntrySpec& spec, const std::vector<std::string>& views, const fs::path& out) {
    if (spec.kind == EntryKind::File) {
        FileEntry entry(spec.path, spec.cwd);
        dump_entry(entry, views, out);
    } else if (spec.kind == EntryKind::Memory) {
        auto entry = MemoryEntry::from_file(spec.path, spec.cwd);
        dump_entry(entry, views, out);
    } else if (spec.kind == EntryKind::Fixture) {
        auto entry = fixture_entry(spec.path.generic_string());
        dump_entry(entry, views, out);
    } else {
        ReplSessionEntry entry(read_file(spec.path), spec.cwd);
        dump_entry(entry, views, out);
    }
}

// --batch: every entry in one process, to expose state that leaks between
// compilations. LIST lines are `name<TAB>kind<TAB>path<TAB>cwd`.
int run_batch(const fs::path& list, std::string_view order, const std::vector<std::string>& views,
              const fs::path& out) {
    std::vector<EntrySpec> specs;
    std::istringstream lines(read_file(list));
    std::string line;
    while (std::getline(lines, line)) {
        if (line.empty()) continue;
        const auto fields = split(line, '\t');
        if (fields.size() != 4) throw std::runtime_error("malformed batch line: " + line);
        specs.push_back(EntrySpec{fields[0], parse_kind(fields[1]), fields[2], fields[3]});
    }
    if (order == "reverse") {
        std::reverse(specs.begin(), specs.end());
    } else if (order != "forward") {
        throw std::runtime_error("--order must be forward or reverse");
    }
    for (const auto& spec : specs) {
        fs::current_path(spec.cwd);
        dump_spec(spec, views, out / spec.name);
    }
    return 0;
}

// --time-stage: wall time of one compiler stage, inputs prepared untimed. On
// Linux the gate counts retired instructions of the stage function instead
// (tests/golden/compile_time.py).
int run_time_stage(std::string_view stage, int repeat, const fs::path& path, const fs::path& cwd) {
    if (repeat < 1) throw std::runtime_error("--repeat must be positive");
    const auto checked = check_file(path, {}, cwd);
    std::vector<long long> samples;
    const auto timed = [&](auto&& body) {
        const auto start = std::chrono::steady_clock::now();
        body();
        const auto end = std::chrono::steady_clock::now();
        samples.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
    };
    std::size_t sink = 0;
    if (stage == "lower") {
        for (int i = 0; i < repeat; ++i) {
            timed([&] { sink += ir::lower(checked).functions.size(); });
        }
    } else if (stage == "optimize") {
        const auto lowered = ir::lower(checked);
        std::vector<ir::Module> inputs(static_cast<std::size_t>(repeat), lowered);
        for (auto& input : inputs) {
            timed([&] { sink += ir::optimize(std::move(input)).functions.size(); });
        }
    } else if (stage == "emit") {
        const auto optimized = ir::optimize(ir::lower(checked));
        for (int i = 0; i < repeat; ++i) {
            timed([&] { sink += emit_llvm(optimized).size(); });
        }
    } else {
        throw std::runtime_error("--time-stage must be lower, optimize or emit");
    }
    auto sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    std::cout << "stage " << stage << " repeat " << repeat << " ns";
    for (const auto sample : samples) std::cout << ' ' << sample;
    std::cout << " min " << sorted.front() << " median " << sorted[sorted.size() / 2]
              << " sink " << sink << "\n";
    return 0;
}

int usage() {
    std::cerr << "usage: quidra_golden_dump [--root DIR] --out DIR [--views LIST] [--cwd DIR]\n"
                 "                          (FILE | --memory FILE | --fixture NAME |"
                 " --repl-session FILE)\n"
                 "       quidra_golden_dump --list-fixtures\n"
                 "       quidra_golden_dump --batch LIST --order forward|reverse --out DIR"
                 " [--views LIST]\n"
                 "       quidra_golden_dump --time-stage lower|optimize|emit --repeat N"
                 " [--cwd DIR] FILE\n";
    return 2;
}

int run(int argc, char** argv) {
    std::optional<fs::path> root;
    std::optional<fs::path> out;
    std::optional<fs::path> cwd;
    std::optional<fs::path> batch;
    std::string order = "forward";
    std::optional<std::string> time_stage;
    int repeat = 1;
    std::vector<std::string> views;
    std::optional<EntrySpec> spec;
    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error(option + " requires a value");
            return argv[++i];
        };
        if (option == "--root") {
            root = value();
        } else if (option == "--out") {
            out = value();
        } else if (option == "--cwd") {
            cwd = value();
        } else if (option == "--views") {
            views = split(value(), ',');
        } else if (option == "--batch") {
            batch = value();
        } else if (option == "--order") {
            order = value();
        } else if (option == "--time-stage") {
            time_stage = value();
        } else if (option == "--repeat") {
            repeat = std::stoi(value());
        } else if (option == "--memory") {
            spec = EntrySpec{"", EntryKind::Memory, value(), {}};
        } else if (option == "--fixture") {
            spec = EntrySpec{"", EntryKind::Fixture, value(), {}};
        } else if (option == "--repl-session") {
            spec = EntrySpec{"", EntryKind::ReplSession, value(), {}};
        } else if (option == "--list-fixtures") {
            for (const auto& fixture : fixtures()) {
                std::cout << fixture.group << "/" << fixture.name << "\n";
            }
            return 0;
        } else if (!option.empty() && option.front() == '-') {
            return usage();
        } else {
            spec = EntrySpec{"", EntryKind::File, option, {}};
        }
    }
    if (batch) {
        if (!out) return usage();
        return run_batch(*batch, order, views, *out);
    }
    if (!spec) return usage();
    if (root && spec->kind != EntryKind::Fixture && spec->path.is_relative()) {
        spec->path = *root / spec->path;
    }
    if (cwd) fs::current_path(*cwd);
    spec->cwd = fs::current_path();
    if (time_stage) return run_time_stage(*time_stage, repeat, spec->path, spec->cwd);
    if (!out) return usage();
    dump_spec(*spec, views, *out);
    return 0;
}

} // namespace
} // namespace quidra::golden

int main(int argc, char** argv) {
    try {
        return quidra::golden::run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "quidra_golden_dump: " << error.what() << "\n";
        return 2;
    }
}
