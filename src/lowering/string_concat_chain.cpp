// string_concat_chain (string_concat_chain.hpp).

#include "lowering/string_concat_chain.hpp"

namespace quidra::lowering {

std::vector<const Expr*> string_concat_chain(const Expr& root, const CheckedProgram& checked) {
    std::vector<const Expr*> parts;
    std::vector<const Expr*> pending{&root};
    while(!pending.empty()){
        const auto* current=pending.back();
        pending.pop_back();
        const auto* binary=std::get_if<BinaryExpr>(&current->data);
        if(binary && binary->op=="+" &&
           checked.expr_types.at(current).kind==TypeKind::String){
            pending.push_back(binary->right.get());
            pending.push_back(binary->left.get());
        }else{
            parts.push_back(current);
        }
    }
    return parts;
}

} // namespace quidra::lowering
