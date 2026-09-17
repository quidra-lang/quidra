#include "quidra/compiler.hpp"
#include "quidra/frontend.hpp"
#include "quidra/lexer.hpp"
#include "quidra/llvm_backend.hpp"
#include "quidra/parser.hpp"
#include "quidra/language.hpp"

namespace quidra {
namespace {

ResolvedProgram resolve_source(std::string_view source, CompileOptions options) {
    if (options.max_errors == 0) options.max_errors = 1;
    Lexer lexer(source);
    Parser parser(lexer.scan(), options.max_errors);
    auto program = parser.parse();
    if (!program.imports.empty()) {
        const auto& first = program.imports.front();
        if (!first.local_path && is_standard_module(first.target)) {
            throw CompileError(Diagnostic{
                "STANDARD_NAMESPACE_IMPORT",
                "Standard namespace '" + first.target +
                    "' is always available and cannot be imported.",
                first.span});
        }
        throw CompileError(Diagnostic{
            "IMPORT_CONTEXT",
            "Imports require file-aware compilation so paths and module roots are well-defined.",
            first.span});
    }
    // Standard namespaces are wired up by the module loader, so string input must
    // go through it too; otherwise `math.sqrt(...)` and friends never resolve here.
    return load_program_with_root_source(
        std::filesystem::path("<memory>.qui"), source,
        std::filesystem::current_path(), options.max_errors,
        /*enforce_package_lock=*/false);
}

CheckedProgram finish_check(ResolvedProgram program, CompileOptions options) {
    if (options.max_errors == 0) options.max_errors = 1;
    auto concrete = expand_generics(std::move(program));
    Checker checker(options.max_errors);
    return checker.check(std::move(concrete));
}

Compilation finish_compile(ResolvedProgram program, CompileOptions options) {
    auto checked = finish_check(std::move(program), options);
    auto lowered = ir::lower(checked);
    auto llvm = emit_llvm(lowered);
    return Compilation{std::move(checked), std::move(lowered), std::move(llvm)};
}

ReplCompilation finish_repl_compile(
    ResolvedProgram program, CompileOptions options, std::size_t replay_prefix_bytes) {
    auto checked = finish_check(std::move(program), options);

    const Expr* repl_expression = nullptr;
    std::optional<Type> expression_type;
    if (!checked.program.statements.empty()) {
        const auto& last = checked.program.statements.back();
        if (const auto* expression = std::get_if<ExprStmt>(&last->data)) {
            repl_expression = expression->value.get();
            expression_type = checked.expr_types.at(repl_expression);
        }
    }

    auto lowered = ir::lower(checked, repl_expression, replay_prefix_bytes);
    auto llvm = emit_llvm(lowered);
    return ReplCompilation{
        Compilation{std::move(checked), std::move(lowered), std::move(llvm)},
        std::move(expression_type)};
}

} // namespace

CheckedProgram check(std::string_view source, CompileOptions options) {
    return finish_check(resolve_source(source, options), options);
}

CheckedProgram check_file(
    const std::filesystem::path& source,
    CompileOptions options,
    const std::filesystem::path& command_working_directory) {
    if (options.max_errors == 0) options.max_errors = 1;
    auto program = load_program_with_modules(
        source, command_working_directory, options.max_errors);
    return finish_check(std::move(program), options);
}

CheckedProgram check_file_source(
    const std::filesystem::path& source_path,
    std::string_view source,
    CompileOptions options,
    const std::filesystem::path& command_working_directory) {
    if (options.max_errors == 0) options.max_errors = 1;
    auto program = load_program_with_root_source(
        source_path, source, command_working_directory, options.max_errors);
    return finish_check(std::move(program), options);
}

Compilation compile(std::string_view source, CompileOptions options) {
    return finish_compile(resolve_source(source, options), options);
}

Compilation compile_file(
    const std::filesystem::path& source,
    CompileOptions options,
    const std::filesystem::path& command_working_directory) {
    if (options.max_errors == 0) options.max_errors = 1;
    auto program = load_program_with_modules(
        source, command_working_directory, options.max_errors);
    return finish_compile(std::move(program), options);
}

ReplCompilation compile_repl_file(
    const std::filesystem::path& source,
    CompileOptions options,
    const std::filesystem::path& command_working_directory,
    std::size_t replay_prefix_bytes) {
    if (options.max_errors == 0) options.max_errors = 1;
    auto program = load_program_with_modules(
        source, command_working_directory, options.max_errors);
    return finish_repl_compile(std::move(program), options, replay_prefix_bytes);
}

} // namespace quidra
