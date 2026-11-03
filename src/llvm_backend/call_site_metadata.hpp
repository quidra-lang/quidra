#pragma once

// CallSiteMetadata: the call sites and functions of a module emitted for a
// native artifact (quidra/call_site_info.hpp), and the debug metadata that
// carries them through LLVM (docs/spec/architecture.md, "The call path of
// plain code").
//
// - Functions: every function with a body gets a function id, in module
//   order (in --debug builds, every function DebugModule gives a
//   subprogram), then the C entry point of each exported function.
// - Sites: every call written into such a function gets a site id (from 1;
//   0 is the unknown site of merged calls), in the order the calls are
//   written, then stably grouped by logical owner when metadata is finalized.
//   Tail operands are resolved through that exact id remapping.
// - Release metadata: one compile unit with emissionKind LineTablesOnly and
//   a synthetic file; a subprogram named by the function's LLVM symbol on
//   every function with an id (the C entry points artificial); a location
//   on every call whose line is the call's site id. The prelude's helper
//   functions and the public wrapper of a self-depth function get no
//   subprogram, so a helper inlined into a function takes the location of
//   the call it replaced.
// - --debug metadata: DebugModule's full debug information with real
//   positions; each call's location is its own position, in a lexical block
//   file whose discriminator is the call's site id, and every variable
//   declaration gets the function's location.
// - A library's module references the anchor the table module defines, so
//   that a host's link of the archive takes the table
//   (toolchain/call_site_table.hpp).
//
// Owns: the CallSiteInfo being built, the symbol of every function id, the
// metadata ids of the release metadata (from 0; nothing else in a release
// module is metadata) or, in --debug builds, the ids it takes from
// DebugModule's counter, and the location records of every call.

#include "quidra/call_site_info.hpp"
#include "quidra/ir/module.hpp"
#include "llvm_backend/debug_module.hpp"
#include "llvm_backend/module_context.hpp"
#include "llvm_text/llvm_builder.hpp"
#include "llvm_text/llvm_module.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace quidra::llvm_backend {

class StatementAttribution;
class TailRecordEmitter;

// The symbol of the anchor a library's table module defines.
inline constexpr std::string_view call_site_anchor_symbol = "__quidra_calls_anchor";

class CallSiteMetadata {
public:
    enum class Mode { release, debug };

    // `debug` is the module's DebugModule in Mode::debug (null in release).
    CallSiteMetadata(const ir::Module& module, const ModuleContext& context, Mode mode,
                     bool library, DebugModule* debug);

    Mode mode() const { return mode_; }
    std::size_t source_file_id(const std::string& file) const { return debug_ ? debug_->source_file_id(file) : 0; }

    // The function id of module function `index`, or none (an external
    // declaration; in --debug builds a function without a subprogram).
    std::optional<std::uint32_t> function_id(std::size_t index) const {
        return function_ids_[index];
    }
    // The function id of the C entry point `symbol` of exported module
    // function `index` (release builds only), made on first use.
    std::uint32_t export_entry_id(std::size_t index, const std::string& symbol);
    // A transparent native Node entry: its subprogram and body-call location.
    std::pair<std::size_t,std::size_t> parallel_entry(const ir::Function& function);
    // Release: the metadata id of a function's subprogram.
    std::size_t subprogram(std::uint32_t function);
    // The function a callee symbol names, if any.
    std::optional<std::uint32_t> symbol_function(std::string_view symbol) const;

    // Records a call site and returns the metadata id of its call's
    // location: in `scope` (the function's subprogram) with the site id as
    // its line (release), or at `line`:`column` of debug file `file` in a
    // lexical block file of `scope` whose discriminator is the site id
    // (--debug).
    std::size_t locate(const CallSite& site, std::size_t scope, std::size_t file,
                       std::uint32_t line, std::uint32_t column, std::size_t inlined_at = 0);

    // The library's reference to the table's anchor (a library build), in
    // the module's constants.
    void emit_anchor_reference(llvm_text::LlvmModule& module) const;
    // The release metadata, or the --debug call locations after
    // DebugModule's.
    void emit_metadata(llvm_text::LlvmModule& module);
    // Preserve the shared table's contiguous logical-owner ranges, then
    // resolve only compiler-generated tail operands in function call lines.
    std::string resolve_tail_operands(std::string text) const;

    std::uint32_t next_site() const { return static_cast<std::uint32_t>(info_.sites.size()); }
    CallSiteInfo take_info() { return std::move(info_); }

private:
    struct Location {
        std::size_t id{};
        std::size_t scope{};
        std::uint32_t site{};
        std::size_t inlined_at{};
        // --debug: the lexical block file's id and its file.
        std::size_t block{};
        std::size_t file{};
        std::uint32_t line{};
        std::uint32_t column{};
    };
    std::size_t next_id();
    void group_sites();

    Mode mode_;
    DebugModule* debug_{};
    CallSiteInfo info_;
    std::vector<std::optional<std::uint32_t>> function_ids_;
    std::unordered_map<std::size_t, std::uint32_t> export_ids_;
    std::unordered_map<std::string, std::uint32_t> symbols_;
    // Release: the subprogram id of each function id (0: none yet).
    std::vector<std::size_t> subprograms_;
    // Release metadata ids: 0 to 5 are the compile unit's nodes.
    std::size_t next_release_id_{6};
    std::vector<Location> locations_;
    std::vector<std::uint32_t> site_remap_;
    std::vector<std::pair<std::uint32_t,std::uint32_t>> site_ranges_;
    struct ParallelScope {std::uint32_t function;std::size_t file;std::uint32_t line;};
    std::vector<ParallelScope> parallel_scopes_;
};

// The display name of a function in a trace line (`pick()`,
// `Point.norm()`, `Point()`): no module qualifier, a generic instance by
// its declaration's name.
std::string call_site_display_name(const ir::Function& function);

// FunctionCallSites: the call locations of one function. Every call the
// builder writes is a site: its callee from the symbol (a function of the
// module, an `func` value, an extern C function, or a runtime entry, helper,
// intrinsic or C library function), whether its statement is user code,
// and its position: the instruction's own where it has one, else its
// statement's.
class FunctionCallSites final : public llvm_text::LlvmCallLocations {
public:
    FunctionCallSites(CallSiteMetadata& metadata, const ModuleContext& context,
                      const StatementAttribution* attribution, std::uint32_t owner,
                      std::size_t scope, std::size_t file, std::uint32_t function_line,
                      const ir::Function* function = nullptr);

    // The position of the instruction being written (0, 0: none).
    void set_instruction_position(std::uint32_t line, std::uint32_t column) {
        instruction_line_ = line;
        instruction_column_ = column;
    }
    // Marks the next calls as synthesized (the C entry point's call of its
    // function).
    void set_synthesized(bool synthesized) { synthesized_ = synthesized; }

    void set_tail_site(const ir::analysis::TailCallSite* site, TailRecordEmitter* emitter) {
        tail_site_ = site;
        tail_records_ = emitter;
    }
    std::size_t call_location(std::string_view symbol) override;
    void select_inline_scope(std::uint64_t identity) { inline_scope_ = identity; }

private:
    CallSiteMetadata& metadata_;
    const ModuleContext& context_;
    const StatementAttribution* attribution_;
    std::uint32_t owner_;
    std::size_t scope_;
    std::size_t file_;
    std::uint32_t function_line_;
    struct InlineLocation {
        std::uint32_t owner{};
        std::size_t scope{}, file{}, at{};
    };
    std::map<std::uint64_t, InlineLocation> inline_locations_;
    std::uint64_t inline_scope_{};
    std::uint32_t instruction_line_{};
    std::uint32_t instruction_column_{};
    bool synthesized_{};
    const ir::analysis::TailCallSite* tail_site_{};
    TailRecordEmitter* tail_records_{};
};

} // namespace quidra::llvm_backend
