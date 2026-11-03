#include "lowering/statement_sites.hpp"

#include <iostream>
#include <stdexcept>

namespace {
using namespace quidra;
StmtPtr expression(std::size_t line) {
    auto statement = std::make_unique<Stmt>();
    statement->data = ExprStmt{};
    statement->span.start.line = line;
    statement->span.start.offset = line * 17;
    return statement;
}
void require(bool value, const char* reason) {
    if (!value) throw std::runtime_error(reason);
}
void authored_identity_and_synthetic_lifetime() {
    std::vector<StmtPtr> original, shifted;
    for (const auto line : {2U, 3U, 4U}) original.push_back(expression(line));
    for (const auto line : {20U, 50U, 91U}) shifted.push_back(expression(line));
    lowering::StatementSites first, second;
    first.register_tree("function", original);
    second.register_tree("function", shifted);
    require(first.identity("function", *original[2]) == 3,
            "authored statement order must precede lowering/emission order");
    for (std::size_t i = 0; i < original.size(); ++i)
        require(first.identity("function", *original[i]) == second.identity("function", *shifted[i]),
                "moving lines/comments must retain statement identity");
    std::vector<StmtPtr> expansion;
    expansion.push_back(expression(300));
    first.register_expansion("function", *original[1], expansion);
    require(first.identity("function", *expansion[0]) == 2,
            "pattern bindings must retain their original user statement");
    auto synthetic = expression(400);
    const auto a = first.identity("function", *synthetic);
    const auto b = first.identity("function", *synthetic);
    require(a != b, "a transient/reused synthetic address must not merge different emissions");
    first.register_tree("other", shifted);
    require(first.identity("other", *shifted[0]) == 1,
            "each function owns an independent statement namespace");
}
} // namespace
int main() {
    try { authored_identity_and_synthetic_lifetime(); std::cout << "Statement sites passed\n"; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
