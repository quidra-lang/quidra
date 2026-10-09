// Operator lowering: binary operators. A comparison with an empty string
// length or with a one-byte ASCII string literal is tested without building
// the operands; a chain of string + is one concatenation; `and` and `or`
// branch around their right operand; every other operator tree is lowered
// bottom-up without recursion through the host stack. Also the exact-number
// builtins real.atom and real.unary, whose provider and opcode are
// literals the checker verified.

#include "lowering/checked_types.hpp"
#include "lowering/lowerer.hpp"
#include "lowering/string_concat_chain.hpp"
#include "operator_policy.hpp"
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace quidra::lowering {

ValueId OperatorLowering::lower_binary(const Expr& e, const BinaryExpr& n) {
    if((n.op=="=="||n.op=="!=") && type_of(checked_, e).kind==TypeKind::Bool){
        if (const auto out = lower_empty_string_test(n)) return *out;
        if (const auto out = lower_ascii_character_test(n)) return *out;
    }
    if(n.op=="+" && type_of(checked_, e).kind==TypeKind::String) return lower_string_concatenation(e);
    if(n.op=="and"||n.op=="or") return lower_short_circuit(n);
    return lower_operator_tree(e);
}

std::optional<ValueId> OperatorLowering::lower_empty_string_test(const BinaryExpr& n) {
    const Expr* empty_test_text=nullptr;
    const auto matches_empty_length =
        [&](const Expr& length_expression,
            const Expr& zero_expression) {
            const auto* zero =
                std::get_if<IntegerExpr>(&zero_expression.data);
            if (!zero || !zero->fits_u64 || zero->value != 0)
                return false;
            const auto* length_call =
                std::get_if<CallExpr>(&length_expression.data);
            if (!length_call || length_call->callee != "len" ||
                length_call->args.size() != 1 ||
                length_call->args.front().name ||
                length_call->args.front().writable ||
                !length_call->args.front().value)
                return false;
            const auto resolution =
                checked_.call_resolutions.find(&length_expression);
            if (resolution == checked_.call_resolutions.end() ||
                resolution->second.kind != CallKind::Builtin ||
                resolution->second.builtin != BuiltinCallable::Len ||
                type_of(checked_,*length_call->args.front().value).kind !=
                    TypeKind::String)
                return false;
            empty_test_text =
                length_call->args.front().value.get();
            return true;
        };
    if (matches_empty_length(*n.left,*n.right) ||
        matches_empty_length(*n.right,*n.left)) {
        auto text=lowerer_.lower(*empty_test_text);
        auto out=builder_.fresh();
        builder_.emit(
            StringEmpty{out,text,n.op=="!="});
        lifetime_.release_temporary(*empty_test_text,text);
        return out;
    }
    return std::nullopt;
}

std::optional<ValueId> OperatorLowering::lower_ascii_character_test(const BinaryExpr& n) {
    const IndexExpr* indexed=nullptr;
    const StringExpr* literal=nullptr;
    if(const auto* left_index=std::get_if<IndexExpr>(&n.left->data);
       left_index && std::holds_alternative<StringExpr>(n.right->data)){
        indexed=left_index;
        literal=&std::get<StringExpr>(n.right->data);
    }else if(const auto* right_index=std::get_if<IndexExpr>(&n.right->data);
              right_index && std::holds_alternative<StringExpr>(n.left->data)){
        indexed=right_index;
        literal=&std::get<StringExpr>(n.left->data);
    }
    if(indexed && literal && indexed->items.size()==1 &&
       !indexed->items.front().slice && indexed->items.front().index &&
       type_of(checked_,*indexed->base).kind==TypeKind::String &&
       literal->value.size()==1){
        const auto byte=static_cast<unsigned char>(literal->value[0]);
        if(byte!=0 && byte<0x80U){
            const bool base_owned=lifetime_.expression_owns_result(*indexed->base);
            auto text=lowerer_.lower(*indexed->base);
            auto index=lowerer_.lower_int64(*indexed->items.front().index);
            auto out=builder_.fresh();
            // An element index failure reports the index operand.
            const auto& at=indexed->items.front().index->span.start;
            builder_.emit(StringIndexAsciiCompare{
                out,text,index,byte,n.op=="!=",
                static_cast<std::uint32_t>(at.line),
                static_cast<std::uint32_t>(at.column)});
            if(base_owned)
                builder_.emit(
                    Release{text,Type::simple(TypeKind::String)});
            return out;
        }
    }
    return std::nullopt;
}

ValueId OperatorLowering::lower_string_concatenation(const Expr& e) {
    const auto parts=string_concat_chain(e,checked_);
    std::vector<ValueId> values;
    values.reserve(parts.size());
    for(const auto* part:parts) values.push_back(lowerer_.lower(*part));
    auto out=builder_.fresh();
    builder_.emit(StringConcat{out,values});
    for(std::size_t i=0;i<parts.size();++i)
        lifetime_.release_temporary(*parts[i],values[i]);
    return out;
}

ValueId OperatorLowering::lower_short_circuit(const BinaryExpr& n) {
    const auto bool_type=Type::simple(TypeKind::Bool);
    const auto result_name=builder_.hidden(n.op=="and"?"and.result":"or.result");
    scope_.local_type(result_name)=bool_type;
    auto left=lowerer_.lower(*n.left);
    const auto rhs=builder_.label(n.op=="and"?"and.rhs":"or.rhs");
    const auto merge=builder_.label(n.op=="and"?"and.end":"or.end");
    auto shortcut=builder_.const_bool(n.op=="or");
    builder_.emit(StoreLocal{result_name,shortcut,bool_type,true});
    builder_.emit(
        n.op=="and"?Instruction{Branch{left,rhs,merge}}
                    :Instruction{Branch{left,merge,rhs}});
    auto& rb=builder_.add_block(rhs);
    builder_.enter(rb);
    auto right=lowerer_.lower(*n.right);
    builder_.emit(StoreLocal{result_name,right,bool_type,true});
    builder_.emit(Jump{merge});
    auto& mb=builder_.add_block(merge);
    builder_.enter(mb);
    auto out=builder_.fresh();
    builder_.emit(LoadLocal{out,result_name,bool_type});
    return out;
}

ValueId OperatorLowering::lower_operator_tree(const Expr& e) {
    const auto eager_binary = [](const Expr& expression) -> const BinaryExpr* {
        const auto* binary=std::get_if<BinaryExpr>(&expression.data);
        if(!binary || operator_policy::is_short_circuit(binary->op)) return nullptr;
        return binary;
    };

    // Operator trees are frequently left-deep. Lower every eager binary
    // node iteratively so scalar and tensor expressions do not
    // depend on the host compiler process stack size.
    std::vector<std::pair<const Expr*,bool>> pending;
    std::unordered_map<const Expr*,ValueId> values;
    pending.push_back({&e,false});
    while(!pending.empty()){
        const auto [current,visited]=pending.back();
        pending.pop_back();
        const auto* binary=eager_binary(*current);
        if(!binary){
            values[current]=lowerer_.lower(*current);
            continue;
        }
        if(!visited){
            pending.push_back({current,true});
            pending.push_back({binary->right.get(),false});
            pending.push_back({binary->left.get(),false});
            continue;
        }

        const auto left_value=values.at(binary->left.get());
        const auto right_value=values.at(binary->right.get());
        auto left=left_value;
        auto right=right_value;
        auto left_type=type_of(checked_,*binary->left);
        auto right_type=type_of(checked_,*binary->right);
        auto out=builder_.fresh();

        if(left_type.kind==TypeKind::Tensor || right_type.kind==TypeKind::Tensor){
            const auto tensor_type=
                left_type.kind==TypeKind::Tensor?left_type:right_type;
            const auto element=*tensor_type.first;
            const bool comparison=
                binary->op=="==" || binary->op=="!=" || binary->op=="<" ||
                binary->op=="<=" || binary->op==">" || binary->op==">=";
            if(comparison){
                builder_.emit(TensorCompare{
                    out,binary->op,left,right,left_type,right_type,type_of(checked_,*current),
                    static_cast<std::uint32_t>(current->span.start.line),
                    static_cast<std::uint32_t>(current->span.start.column)});
            }else{
                if(left_type.kind!=TypeKind::Tensor && left_type!=element){
                    left=conversions_.convert(left,left_type,element);
                    left_type=element;
                }
                if(right_type.kind!=TypeKind::Tensor && right_type!=element){
                    right=conversions_.convert(right,right_type,element);
                    right_type=element;
                }
                builder_.emit(TensorBinary{
                    out,binary->op,left,right,left_type,right_type,type_of(checked_,*current),
                    static_cast<std::uint32_t>(current->span.start.line),
                    static_cast<std::uint32_t>(current->span.start.column)});
            }
        }else{
            builder_.emit(Binary{
                out,binary->op,left,right,left_type,type_of(checked_,*current),
                static_cast<std::uint32_t>(current->span.start.line),
                static_cast<std::uint32_t>(current->span.start.column),
                facts_.integer_ranges.overflow_proven(*current),
                facts_.integer_ranges.inline_proven(*current)});
        }

        // Source operands are evaluated once. Release the original values;
        // tensor scalar conversions above are non-owning numeric values.
        lifetime_.release_temporary(*binary->left,left_value);
        lifetime_.release_temporary(*binary->right,right_value);

        ValueId result=out;
        if(current!=&e){
            const auto from=checked_.raw_types.at(current);
            const auto to=type_of(checked_,*current);
            result=conversions_.convert(out,from,to);
            if(lifetime_.expression_owns_result(*current)&&array_shape_conversion(from,to))
                builder_.emit(Release{out,from});
        }
        values[current]=result;
    }
    return values.at(&e);
}

ValueId OperatorLowering::lower_exact_atom(const Expr& e, const CallExpr& n) {
    const auto* provider =
        std::get_if<StringExpr>(&n.args[0].value->data);
    const auto* opcode =
        std::get_if<IntegerExpr>(&n.args[1].value->data);
    if (!provider || !opcode || !opcode->fits_u64 ||
        opcode->value >
            std::numeric_limits<std::uint32_t>::max()) {
        throw std::logic_error(
            "checked real.atom lost its literal contract");
    }
    auto out = builder_.fresh();
    builder_.emit(ExactAtom{
        out, provider->value,
        static_cast<std::uint32_t>(opcode->value),
        checked_.raw_types.at(&e)});
    return out;
}

ValueId OperatorLowering::lower_exact_unary(const Expr& e, const CallExpr& n) {
    const auto* provider =
        std::get_if<StringExpr>(&n.args[0].value->data);
    const auto* opcode =
        std::get_if<IntegerExpr>(&n.args[1].value->data);
    if (!provider || !opcode || !opcode->fits_u64 ||
        opcode->value >
            std::numeric_limits<std::uint32_t>::max()) {
        throw std::logic_error(
            "checked real.unary lost its literal contract");
    }
    auto input=lowerer_.lower(*n.args[2].value);
    auto out=builder_.fresh();
    builder_.emit(ExactUnary{
        out,provider->value,
        static_cast<std::uint32_t>(opcode->value),
        input,checked_.raw_types.at(&e)});
    lifetime_.release_temporary(*n.args[2].value,input);
    return out;
}
} // namespace quidra::lowering
