#pragma once

// TextIdioms: statement sequences over text recognised before ordinary
// lowering and lowered as one operation. Defined in text_idioms.cpp.

#include "ir/function_builder.hpp"
#include "lowering/borrow_inference.hpp"
#include "lowering/check_elision_facts.hpp"
#include "lowering/ir_names.hpp"
#include "lowering/local_scope.hpp"
#include "lowering/loop_targets.hpp"
#include "lowering/source_locator.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace quidra::lowering {

class Lowerer;
class LifetimeLowering;

class TextIdioms {
public:
    TextIdioms(
        Lowerer& lowerer, const CheckedProgram& checked,
        ir::FunctionBuilder& builder, LocalScope& scope,
        CheckElisionFacts& facts, LoopTargets& loop_targets,
        const SourceLocator& locator, const BorrowInference& borrows,
        LifetimeLowering& lifetime)
        : lowerer_(lowerer), checked_(checked), builder_(builder),
          scope_(scope), facts_(facts), loop_targets_(loop_targets),
          locator_(locator), borrows_(borrows), lifetime_(lifetime) {}

    // text statement sequences lowered as one operation
    bool lower_string_split_for(const ForStmt& n);
    bool lower_string_ascii_count_for(const ForStmt& n);
    bool lower_utf8_array_match(const MatchStmt& match);
    bool lower_numeric_parse_match(const MatchStmt& match);
    std::optional<std::size_t> lower_text_sequence(
        const std::vector<StmtPtr>& statements, std::size_t i);

private:
    // the sequences lower_text_sequence tries, and their parts
    bool string_build_element_supported(const Expr& expression) const;
    StringBuildPart lower_string_build_element(const Expr& expression);
    bool is_signed_parse_wrapper(const std::string& target) const;
    bool split_parse_call(
        const BindingStmt& binding, const std::string& fields_name,
        std::uint64_t expected_index) const;
    bool lower_split_parse_pair(
        const Stmt& split_statement, const Stmt& left_statement,
        const Stmt& right_statement, bool fields_used_later);
    bool lower_string_build_append_quad(
        const Stmt& array_statement, const Stmt& line_statement,
        const Stmt& append_statement, const Stmt& length_statement,
        bool fields_used_later, bool line_used_later);
    bool lower_string_array_join_pair(
        const Stmt& array_statement, const Stmt& join_statement,
        bool used_later);
    bool split_source_dead_after(
        const BindingStmt& binding,
        const std::vector<StmtPtr>& statements,
        std::size_t first_following) const;
    bool lower_bound_string_split_for_pair(
        const Stmt& binding_statement, const Stmt& for_statement,
        bool used_later, bool move_source,
        const std::vector<const Stmt*>& prelude = {});

    Lowerer& lowerer_;
    const CheckedProgram& checked_;
    ir::FunctionBuilder& builder_;
    LocalScope& scope_;
    CheckElisionFacts& facts_;
    LoopTargets& loop_targets_;
    const SourceLocator& locator_;
    const BorrowInference& borrows_;
    LifetimeLowering& lifetime_;
};

} // namespace quidra::lowering
