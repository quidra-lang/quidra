#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace quidra {
enum class CompilerExtensionTableKind : std::uint8_t {
    Operation, ExecutionPolicy, Fusion, Pattern, Specialization, Backend,
    Memory, Donation, Kernel, Fragment, Intrinsic, Cost, Derivative, Tuning,
    Collective, Query, Documentation, Extension, Passthrough
};
struct CompilerDeclarationSelector {
    enum class Kind : std::uint8_t { Function, Method, Constructor } kind{Kind::Function};
    std::string spelling, name;
    std::optional<std::string> signature;
    bool operator==(const CompilerDeclarationSelector&) const = default;
};
enum class CompilerPodType : std::uint8_t { I32, I64, U32, F32, F64 };
struct CompilerPodField {
    std::string name;
    CompilerPodType type{};
    std::uint64_t offset{}, size{}, alignment{};
    bool operator==(const CompilerPodField&) const = default;
};
// Owned syntax only. Overflow/division-by-zero during later instantiation
// declines an alternative; parsing never evaluates or executes the expression.
struct CompilerExpression {
    enum class Category : std::uint8_t { Formula, Predicate } category{Category::Formula};
    struct Node {
        enum class Kind : std::uint8_t {
            Integer, Symbol, Axis, Policy, Add, Subtract, Multiply, Divide,
            Min, Max, Ceil, Align, Equal, NotEqual, Less, LessEqual, Greater,
            GreaterEqual, And, Or, Not
        } kind{};
        std::int64_t integer{};
        std::string symbol;
        std::vector<std::uint32_t> operands;
        bool operator==(const Node&) const = default;
    };
    std::vector<Node> nodes;
    std::uint32_t root{};
    bool operator==(const CompilerExpression&) const = default;
};
using CompilerExtensionValue = std::variant<std::string, std::int64_t, bool,
    std::vector<std::string>, std::map<std::string, std::string>,
    std::vector<std::int64_t>, std::vector<CompilerPodField>,
    std::vector<CompilerDeclarationSelector>, CompilerExpression>;
struct CompilerExtensionTable {
    CompilerExtensionTableKind kind{};
    std::string id;
    std::map<std::string, CompilerExtensionValue> fields;
};
struct CompilerExtensionModel {
    std::uint32_t schema_revision{2};
    std::map<std::string, CompilerExtensionTable> tables;
};
} // namespace quidra
