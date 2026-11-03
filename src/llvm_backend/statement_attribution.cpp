#include "llvm_backend/statement_attribution.hpp"

#include "llvm_backend/bare_integer.hpp"
#include "llvm_backend/runtime_abi.hpp"
#include "llvm_backend/small_rational.hpp"
#include "quidra/abi/symbols.hpp"

#include <array>
#include <stdexcept>

namespace quidra::llvm_backend {

using namespace llvm_text;
using namespace llvm_text::types;

namespace {

// Calls that neither read nor change the user statement: the entries that
// set it, the failure entries and helpers that receive exact immediates
// (and call nothing that could fail elsewhere) among them the constructors
// of error records and the fail-fast stop of an error, the reason of the
// thread's last failure and the reads and comparison of error values
// (which cannot fail), the ownership counts
// of managed values, of exact reals and of integer words (retain, release,
// pin, unpin: their only failures are count overflows, internal invariants
// that name no operation of the program, and they run in every epilogue)
// and the test whether an exact real holds a value, the call-depth counter
// (located by its own stored position), and the C library. The arithmetic,
// comparison and conversion helpers of integer words are among the helpers
// with exact immediates: they report their own failures at the site they
// receive, and otherwise fail only in allocation, an internal invariant.
constexpr std::array<std::string_view, 69> unattributed_callees{
    runtime_abi::tail::record.symbol(),
    runtime_abi::tail::return_cleanup.symbol(),
    runtime_abi::program::register_sources.symbol(),
    runtime_abi::program::set_source_provenance.symbol(),
    runtime_abi::program::set_package_source_provenance.symbol(),
    runtime_abi::program::user_statement.symbol(),
    runtime_abi::program::restore_user_statement.symbol(),
    runtime_abi::failure::fail_at.symbol(),
    runtime_abi::failure::bounds_fail.symbol(),
    runtime_abi::failure::conversion_fail.symbol(),
    runtime_abi::failure::last_reason.symbol(),
    runtime_abi::failure::fail_error.symbol(),
    runtime_abi::error::make_user.symbol(),
    runtime_abi::error::make.symbol(),
    runtime_abi::error::file.symbol(),
    runtime_abi::error::json_last.symbol(),
    runtime_abi::error::http_last.symbol(),
    runtime_abi::error::detail.symbol(),
    runtime_abi::error::detail_int.symbol(),
    runtime_abi::error::equal.symbol(),
    runtime_abi::error::hop.symbol(),
    runtime_abi::error::hop_indirect.symbol(),
    runtime_abi::memory::init_check.symbol(),
    runtime_abi::memory::managed_retain.symbol(),
    runtime_abi::memory::managed_release.symbol(),
    runtime_abi::memory::managed_pin.symbol(),
    runtime_abi::memory::managed_unpin.symbol(),
    real_helper::retain,
    real_helper::release,
    real_helper::is_none,
    int_helper::retain,
    int_helper::release,
    int_helper::add,
    int_helper::sub,
    int_helper::mul,
    int_helper::div,
    int_helper::rem,
    int_helper::neg,
    int_helper::pow,
    int_helper::compare,
    int_helper::eq,
    int_helper::ne,
    int_helper::lt,
    int_helper::le,
    int_helper::gt,
    int_helper::ge,
    int_helper::from_i64,
    int_helper::from_u64,
    int_helper::to_i64,
    int_helper::to_u64,
    int_helper::try_i64,
    int_helper::try_u64,
    int_helper::to_float64,
    int_helper::to_float32,
    int_helper::nat_sub,
    int_helper::nat_from_int,
    int_helper::nat_from_i64,
    runtime_abi::prelude::fail_at.symbol(),
    runtime_abi::prelude::array_slot.symbol(),
    runtime_abi::prelude::fixed_array_slot.symbol(),
    runtime_abi::prelude::array_slot_proven.symbol(),
    runtime_abi::prelude::fixed_array_slot_proven.symbol(),
    runtime_abi::prelude::stack_enter.symbol(),
    runtime_abi::prelude::stack_leave.symbol(),
    runtime_abi::c_library::printf.symbol(),
    runtime_abi::c_library::puts.symbol(),
    runtime_abi::c_library::strlen.symbol(),
    runtime_abi::c_library::memcpy.symbol(),
    runtime_abi::c_library::memset.symbol(),
};

bool unattributed(std::string_view symbol) {
    if (symbol.starts_with("llvm.")) return true;
    if (symbol == runtime_abi::program::process_exit.symbol()) return true;
    for (const auto callee : unattributed_callees)
        if (symbol == callee) return true;
    return false;
}

} // namespace

StatementAttribution::StatementAttribution(const ir::Function& fn, const ModuleContext& context,
                                           LlvmBuilder& builder, TemporaryNames& names,
                                           StringPool& pool)
    : context_(context), builder_(builder), names_(names), pool_(pool) {
    enter_file(fn.source_file);
    root_state_ = state();
    for (const auto& scope : fn.inline_scopes) {
        InlineState frame;
        const auto found = context_.user_source_index.find(scope.source_file);
        frame.callee.user = found != context_.user_source_index.end();
        frame.callee.file_index = frame.callee.user ? found->second : 0;
        frame.saved_user = names_.value("inline.user.statement");
        if (!inline_states_.emplace(scope.identity, std::move(frame)).second)
            throw std::logic_error("duplicate inline attribution scope");
    }
}

StatementAttribution::State StatementAttribution::state() const {
    return {user_,file_index_,statement_line_,statement_column_,record_,entered_user_code_,saved_user_statement_};
}
void StatementAttribution::state(const State& saved) {
    user_ = saved.user;
    file_index_ = saved.file_index;
    statement_line_ = saved.line;
    statement_column_ = saved.column;
    record_ = saved.record;
    entered_user_code_ = saved.entered_user;
    saved_user_statement_ = saved.pending_user;
}
StatementAttribution::State StatementAttribution::location_state(const ir::SourceLocation& location) {
    State saved;
    const auto found = context_.user_source_index.find(location.source_file);
    saved.user = found != context_.user_source_index.end();
    saved.file_index = saved.user ? found->second : 0;
    saved.line = location.position.line;
    saved.column = location.position.column;
    if (saved.user) saved.record = pool_.intern_provenance(location);
    return saved;
}
void StatementAttribution::select_inline_scope(const ir::InlineScope* scope) {
    const auto next = scope ? scope->identity : 0;
    if (next == inline_scope_) return;
    if (inline_scope_) inline_states_.at(inline_scope_).callee = state();
    else root_state_ = state();
    state(next ? inline_states_.at(next).callee : root_state_);
    inline_scope_ = next;
}
void StatementAttribution::begin_inline_scope(const ir::InlineScope& scope) {
    auto& frame = inline_states_.at(scope.identity);
    auto caller = scope.caller_location ? location_state(*scope.caller_location) :
        scope.parent ? inline_states_.at(scope.parent).callee : root_state_;
    state(caller);
    // A preceding callback can have replaced the runtime user statement.
    // Restoring the owned caller record is non-fallible and also covers CFG
    // joins whose observer state was emitted in another block.
    if (user_ && !record_.empty())
        builder_.call(runtime_abi::program::restore_user_statement,
            {{ptr,LlvmOperand::global(record_)}});
    builder_.call(frame.saved_user,runtime_abi::program::user_statement,{});
    frame.caller = state();
    const auto effect = effect_of(abi::user_symbol(scope.callee));
    if (frame.caller.user && (effect & enters)) frame.caller.entered_user = true;
    state(frame.callee);
    inline_scope_ = scope.identity;
}
void StatementAttribution::end_inline_scope(const ir::InlineScope& scope) {
    auto& frame = inline_states_.at(scope.identity);
    frame.callee = state();
    // This changes only the user's statement. The innermost record still
    // identifies the callee's last statement, as after an ordinary call.
    builder_.call(runtime_abi::program::restore_user_statement,{{ptr,frame.saved_user}});
    state(frame.caller);
    inline_scope_ = scope.parent;
    if (scope.parent) inline_states_.at(scope.parent).callee = state();
    else root_state_ = state();
}

void StatementAttribution::enter_file(const std::string& file) {
    const auto found = context_.user_source_index.find(file);
    user_ = found != context_.user_source_index.end();
    file_index_ = user_ ? found->second : 0;
}

void StatementAttribution::begin_statement(const ir::SourceLocation& location) {
    enter_file(location.source_file);
    statement_line_ = location.position.line;
    statement_column_ = location.position.column;
    entered_user_code_ = false;
    record_.clear();
    if (!location.node_id.empty()) {
        const auto record = pool_.intern_provenance(location);
        builder_.call(user_ ? runtime_abi::program::set_source_provenance
                            : runtime_abi::program::set_package_source_provenance,
                      {{ptr, LlvmOperand::global(record)}});
        if (user_) record_ = record;
    } else if (user_) {
        // No public node: an empty-node record of its own, which prints no
        // provenance suffix and leaves the innermost statement alone.
        record_ = pool_.intern_provenance(location);
        builder_.call(runtime_abi::program::restore_user_statement,
                      {{ptr, LlvmOperand::global(record_)}});
    }
}

unsigned StatementAttribution::effect_of(std::string_view symbol) const {
    if (symbol.empty()) return reads | enters;  // through an `func` value
    if (unattributed(symbol)) return none;
    if (const auto found = context_.call_targets.find(std::string(symbol));
        found != context_.call_targets.end()) {
        switch (found->second) {
            case CallTarget::user_function: return enters;
            case CallTarget::package_function: return reads;
            case CallTarget::foreign: return reads;
            case CallTarget::foreign_with_callback: return reads | enters;
        }
    }
    // A runtime entry, a prelude helper without location parameters or a
    // generated helper: any may end in a failure reported at the user's
    // statement.
    return reads;
}

void StatementAttribution::before_call(std::string_view symbol) {
    const auto effect = effect_of(symbol);
    if (effect == none) return;
    if (user_) {
        if ((effect & reads) && entered_user_code_ && !record_.empty()) {
            entered_user_code_ = false;
            builder_.call(runtime_abi::program::restore_user_statement,
                          {{ptr, LlvmOperand::global(record_)}});
        }
        if (effect & enters) entered_user_code_ = true;
        return;
    }
    if (effect & enters) {
        saved_user_statement_ = names_.value("user.statement");
        builder_.call(saved_user_statement_, runtime_abi::program::user_statement, {});
    }
}

void StatementAttribution::after_call(std::string_view symbol) {
    if (saved_user_statement_.empty() || unattributed(symbol)) return;
    const auto saved = std::move(saved_user_statement_);
    saved_user_statement_.clear();
    builder_.call(runtime_abi::program::restore_user_statement, {{ptr, saved}});
}

} // namespace quidra::llvm_backend
