#include "compiler_extension_expression.hpp"
#include "nesting_budget.hpp"

#include <charconv>
#include <limits>
#include <stdexcept>
#include <utility>

namespace quidra {
namespace {
using Category = CompilerExpression::Category;
using Node = CompilerExpression::Node;
using Kind = Node::Kind;
struct Parsed { std::uint32_t node; Category category; };
class Parser {
public:
    Parser(std::string_view text, std::string_view origin) : text_(text), origin_(origin) {}
    CompilerExpression run(Category category) {
        const auto parsed = expression(1);
        whitespace();
        if (position_ != text_.size()) fail("unexpected token");
        if (parsed.category != category) fail(category == Category::Formula ? "expected formula" : "expected predicate");
        result_.category = category; result_.root = parsed.node;
        return std::move(result_);
    }
private:
    std::string_view text_, origin_;
    std::size_t position_{}, depth_{};
    CompilerExpression result_;
    [[noreturn]] void fail(std::string_view message) const {
        throw std::runtime_error("compiler extension expression " + std::string(origin_) +
            " at byte " + std::to_string(position_) + ": " + std::string(message));
    }
    void whitespace() {
        while (position_ < text_.size() && (text_[position_] == ' ' || text_[position_] == '\t' ||
               text_[position_] == '\r' || text_[position_] == '\n')) ++position_;
    }
    static bool letter(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    }
    static bool digit(char c) { return c >= '0' && c <= '9'; }
    bool consume(std::string_view token, bool word = false) {
        whitespace();
        if (!text_.substr(position_).starts_with(token)) return false;
        const auto end = position_ + token.size();
        if (word && end < text_.size() && (letter(text_[end]) || digit(text_[end]))) return false;
        position_ = end; return true;
    }
    void expect(std::string_view token) { if (!consume(token)) fail("expected '" + std::string(token) + "'"); }
    std::string symbol() {
        whitespace(); const auto start = position_;
        if (position_ == text_.size() || !letter(text_[position_])) fail("expected symbol");
        while (position_ < text_.size() && (letter(text_[position_]) || digit(text_[position_]))) ++position_;
        return std::string(text_.substr(start, position_ - start));
    }
    std::int64_t integer(bool sign) {
        whitespace(); const auto start = position_;
        if (sign && position_ < text_.size() && text_[position_] == '-') ++position_;
        const auto digits = position_;
        while (position_ < text_.size() && digit(text_[position_])) ++position_;
        if (digits == position_) fail("expected integer");
        std::int64_t value{};
        const auto [end, error] = std::from_chars(text_.data() + start, text_.data() + position_, value);
        if (error != std::errc{} || end != text_.data() + position_) fail("integer is outside signed 64-bit range");
        return value;
    }
    Parsed append(Kind kind, Category category, std::vector<std::uint32_t> operands = {},
                  std::string symbol = {}, std::int64_t integer = 0) {
        if (result_.nodes.size() >= std::numeric_limits<std::uint32_t>::max()) fail("expression is too large");
        const auto id = static_cast<std::uint32_t>(result_.nodes.size());
        result_.nodes.push_back(Node{kind, integer, std::move(symbol), std::move(operands)});
        return {id, category};
    }
    void require(Parsed parsed, Category category) const {
        if (parsed.category != category) fail(category == Category::Formula ? "formula operand required" : "predicate operand required");
    }
    Parsed primary() {
        if (consume("not", true)) {
            const auto operand = expression(3); require(operand, Category::Predicate);
            return append(Kind::Not, Category::Predicate, {operand.node});
        }
        if (consume("(")) { const auto result = expression(1); expect(")"); return result; }
        whitespace();
        if (position_ < text_.size() && (digit(text_[position_]) || text_[position_] == '-'))
            return append(Kind::Integer, Category::Formula, {}, {}, integer(true));
        auto name = symbol();
        if ((name == "min" || name == "max" || name == "ceil") && consume("(")) {
            const auto first = expression(1); require(first, Category::Formula); expect(",");
            const auto second = expression(1); require(second, Category::Formula); expect(")");
            return append(name == "min" ? Kind::Min : name == "max" ? Kind::Max : Kind::Ceil,
                          Category::Formula, {first.node, second.node});
        }
        if (name == "align" && consume("(")) {
            auto target = symbol(); expect(")");
            return append(Kind::Align, Category::Formula, {}, std::move(target));
        }
        if (name == "policy") return append(Kind::Policy, Category::Formula);
        if (consume(".")) return append(Kind::Axis, Category::Formula, {}, std::move(name), integer(false));
        return append(Kind::Symbol, Category::Formula, {}, std::move(name));
    }
    struct Operator { Kind kind; int precedence; Category input, output; std::string_view text; bool word{}; };
    Parsed expression(int minimum) {
        if (++depth_ > nesting::max_expression_depth) { --depth_; fail("nesting exceeds compiler safety budget"); }
        struct Restore { std::size_t& depth; ~Restore() { --depth; } } restore{depth_};
        auto left = primary();
        static constexpr Operator operators[] = {
            {Kind::Or, 1, Category::Predicate, Category::Predicate, "or", true},
            {Kind::And, 2, Category::Predicate, Category::Predicate, "and", true},
            {Kind::Equal, 3, Category::Formula, Category::Predicate, "=="},
            {Kind::NotEqual, 3, Category::Formula, Category::Predicate, "!="},
            {Kind::LessEqual, 3, Category::Formula, Category::Predicate, "<="},
            {Kind::GreaterEqual, 3, Category::Formula, Category::Predicate, ">="},
            {Kind::Less, 3, Category::Formula, Category::Predicate, "<"},
            {Kind::Greater, 3, Category::Formula, Category::Predicate, ">"},
            {Kind::Add, 4, Category::Formula, Category::Formula, "+"},
            {Kind::Subtract, 4, Category::Formula, Category::Formula, "-"},
            {Kind::Multiply, 5, Category::Formula, Category::Formula, "*"},
            {Kind::Divide, 5, Category::Formula, Category::Formula, "/"}};
        while (true) {
            const Operator* found = nullptr;
            for (const auto& op : operators) {
                if (op.precedence >= minimum && consume(op.text, op.word)) { found = &op; break; }
            }
            if (!found) break;
            require(left, found->input);
            const auto right = expression(found->precedence + 1); require(right, found->input);
            left = append(found->kind, found->output, {left.node, right.node});
        }
        return left;
    }
};
} // namespace
CompilerExpression parse_compiler_expression(std::string_view text,
                                             CompilerExpression::Category category,
                                             std::string_view origin) {
    return Parser(text, origin).run(category);
}
} // namespace quidra
