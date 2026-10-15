// Operator lowering: binary operators. A comparison with an empty string
// length or with a one-byte ASCII string literal is tested without building
// the operands; a chain of string + is one concatenation; `and` and `or`
// branch around their right operand; every other operator tree is lowered
// bottom-up without recursion through the host stack. Also the exact-number
// builtins real.atom, real.unary and real.op, whose provider and opcode are
// literals the checker verified.

#include "lowering/checked_types.hpp"
#include "lowering/complex_literals.hpp"
#include "lowering/finite_checks.hpp"
#include "lowering/lowerer.hpp"
#include "lowering/string_concat_chain.hpp"
#include "operator_policy.hpp"
#include <cstdint>
#include <functional>
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
    if (type_of(checked_, e).kind == TypeKind::Array &&
        (type_of(checked_, *n.left).kind == TypeKind::Array ||
         type_of(checked_, *n.right).kind == TypeKind::Array))
        return lower_array_elementwise(e, n);
    if(n.op=="+" && type_of(checked_, e).kind==TypeKind::String) return lower_string_concatenation(e);
    if(n.op=="and"||n.op=="or") return lower_short_circuit(n);
    return lower_operator_tree(e);
}

// Materialize elementwise array arithmetic/comparison with the scalar IR
// operations at leaves. Nested arrays recurse, checking each pair of row
// lengths as it is reached. Never broadcast one array over another.
ValueId OperatorLowering::lower_array_elementwise(const Expr& e, const BinaryExpr& n) {
    const Type lhs_type = type_of(checked_, *n.left);
    const Type rhs_type = type_of(checked_, *n.right);
    const Type result_type = checked_.raw_types.at(&e);
    const auto lhs = lowerer_.lower(*n.left);
    const auto rhs = lowerer_.lower(*n.right);
    const auto index_type = Type::simple(TypeKind::Int64);
    const auto bool_type = Type::simple(TypeKind::Bool);
    const SourcePosition site{
        static_cast<std::uint32_t>(e.span.start.line),
        static_cast<std::uint32_t>(e.span.start.column)};
    std::function<ValueId(ValueId, const Type&, ValueId, const Type&, const Type&)> apply;
    apply = [&](ValueId l, const Type& lt, ValueId r, const Type& rt,
                const Type& ot) -> ValueId {
        if (ot.kind != TypeKind::Array) {
            auto result = builder_.fresh();
            emit_checked_binary(builder_, Binary{
                result, n.op, l, r, lt, ot, site.line, site.column});
            return result;
        }
        const bool l_array = lt.kind == TypeKind::Array;
        const bool r_array = rt.kind == TypeKind::Array;
        if (!l_array && !r_array)
            throw std::logic_error("Checked elementwise array operation lacks an array");
        auto length = builder_.fresh();
        builder_.emit(ArrayLength{length, l_array ? l : r});
        if (l_array && r_array) {
            auto other = builder_.fresh();
            builder_.emit(ArrayLength{other, r});
            builder_.emit(ExtentEqualCheck{length, other, site});
        }
        auto result = builder_.fresh();
        builder_.emit(ArrayAlloc{result, length, ot});
        const auto index_slot = builder_.hidden("array.elementwise.index");
        scope_.local_type(index_slot) = index_type;
        const auto zero = builder_.const_int(0), one = builder_.const_int(1);
        builder_.emit(StoreLocal{index_slot, zero, index_type});
        const auto cond = builder_.label("array.elementwise.cond");
        const auto body = builder_.label("array.elementwise.body");
        const auto done = builder_.label("array.elementwise.done");
        builder_.emit(Jump{cond});
        builder_.enter(builder_.add_block(cond));
        auto index = builder_.fresh(), more = builder_.fresh();
        builder_.emit(LoadLocal{index, index_slot, index_type});
        builder_.emit(Binary{more, "<", index, length, index_type, bool_type});
        builder_.emit(Branch{more, body, done});
        builder_.enter(builder_.add_block(body));
        ValueId left = l, right = r;
        if (l_array) {
            left = builder_.fresh();
            builder_.emit(ArrayGet{left, l, index, *lt.first, site, false, true});
        }
        if (r_array) {
            right = builder_.fresh();
            builder_.emit(ArrayGet{right, r, index, *rt.first, site, false, true});
        }
        const auto mapped = apply(
            left, l_array ? *lt.first : lt,
            right, r_array ? *rt.first : rt, *ot.first);
        builder_.emit(ArraySet{result, index, mapped, *ot.first, site, false, true});
        auto next = builder_.fresh();
        builder_.emit(Binary{next, "+", index, one, index_type, index_type,
                             0, 0, true});
        builder_.emit(StoreLocal{index_slot, next, index_type});
        builder_.emit(Jump{cond});
        builder_.enter(builder_.add_block(done));
        return result;
    };
    const auto output = apply(lhs, lhs_type, rhs, rhs_type, result_type);
    lifetime_.release_temporary(*n.left, lhs);
    lifetime_.release_temporary(*n.right, rhs);
    return output;
}

// A comparison chain compares operand 0 with operand 1, then, while every
// link holds, the kept value of operand i with operand i + 1, which is
// evaluated only then (left to right, short-circuiting like `and`). The
// result goes through a hidden local, as for `and` and `or`. A middle
// operand crosses into the next link's block through a hidden local and is
// never evaluated again. A managed middle operand is held owned: a
// temporary already is, and a read of storage is retained when a later
// operand runs code, which might write that storage; it is released after
// its second comparison, or on the way out when a link fails first.
ValueId OperatorLowering::lower_comparison_chain(const Expr& e, const ComparisonChainExpr& n) {
    (void)e;
    const auto bool_type=Type::simple(TypeKind::Bool);
    const auto result_name=builder_.hidden("chain.result");
    scope_.local_type(result_name)=bool_type;
    const auto merge=builder_.label("chain.end");
    const auto compare=[&](std::size_t link, ValueId left, ValueId right){
        auto out=builder_.fresh();
        const auto& at=n.operands[link]->span.start;
        builder_.emit(Binary{
            out,n.ops[link],left,right,type_of(checked_,*n.operands[link]),bool_type,
            static_cast<std::uint32_t>(at.line),static_cast<std::uint32_t>(at.column)});
        return out;
    };
    auto left=lowerer_.lower(*n.operands[0]);
    auto right=lowerer_.lower(*n.operands[1]);
    auto result=compare(0,left,right);
    lifetime_.release_temporary(*n.operands[0],left);
    for(std::size_t link=1;link<n.ops.size();++link){
        const auto& middle=*n.operands[link];
        const auto middle_type=type_of(checked_,middle);
        bool owned=lifetime_.expression_owns_result(middle);
        auto held=right;
        if(!owned && owns_managed(middle_type)){
            bool later_code=false;
            for(std::size_t i=link+1;i<n.operands.size();++i)
                later_code=later_code||expression_runs_code(*n.operands[i]);
            if(later_code){
                if(middle_type.kind==TypeKind::Tuple) held=lifetime_.copy_value(right,middle_type);
                else {
                    held=builder_.fresh();builder_.emit(Retain{held,right,middle_type});
                }
                owned=true;
            }
        }
        const auto slot=builder_.hidden("chain.operand");
        scope_.local_type(slot)=middle_type;
        builder_.emit(StoreLocal{slot,held,middle_type,true});
        builder_.emit(StoreLocal{result_name,result,bool_type,true});
        const auto next=builder_.label("chain.next");
        if(owned){
            const auto failed=builder_.label("chain.false");
            builder_.emit(Branch{result,next,failed});
            builder_.enter(builder_.add_block(failed));
            auto dropped=builder_.fresh();
            builder_.emit(LoadLocal{dropped,slot,middle_type});
            release_owned(builder_,dropped,middle_type);
            builder_.emit(Jump{merge});
        }else{
            builder_.emit(Branch{result,next,merge});
        }
        builder_.enter(builder_.add_block(next));
        auto kept=builder_.fresh();
        builder_.emit(LoadLocal{kept,slot,middle_type});
        right=lowerer_.lower(*n.operands[link+1]);
        result=compare(link,kept,right);
        if(owned) release_owned(builder_,kept,middle_type);
    }
    lifetime_.release_temporary(*n.operands.back(),right);
    builder_.emit(StoreLocal{result_name,result,bool_type,true});
    builder_.emit(Jump{merge});
    builder_.enter(builder_.add_block(merge));
    auto out=builder_.fresh();
    builder_.emit(LoadLocal{out,result_name,bool_type});
    return out;
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
                {static_cast<std::uint32_t>(at.line),
                static_cast<std::uint32_t>(at.column)}});
            if(base_owned)
                release_owned(builder_,text,Type::simple(TypeKind::String));
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
    const auto eager_binary = [&](const Expr& expression) -> const BinaryExpr* {
        const auto* binary=std::get_if<BinaryExpr>(&expression.data);
        if(!binary || operator_policy::is_short_circuit(binary->op)) return nullptr;
        // Nested array operators generate their own indexed IR loops, rather
        // than using the scalar eager-binary path for their operand trees.
        if (type_of(checked_, expression).kind == TypeKind::Array &&
            (type_of(checked_, *binary->left).kind == TypeKind::Array ||
             type_of(checked_, *binary->right).kind == TypeKind::Array))
            return nullptr;
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
        if(!visited)
            if(const auto complex=type_of(checked_,*current);is_complex(complex))
                if(const auto parts=complex_literal_parts(*current)){
                    values[current]=emit_complex_literal(builder_,*parts,complex);
                    continue;
                }
        if(!visited) if(const auto scaled=lowerer_.try_scaled_expression(*current)) {
            values[current]=*scaled;
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
                    {static_cast<std::uint32_t>(current->span.start.line),
                    static_cast<std::uint32_t>(current->span.start.column)}});
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
                    {static_cast<std::uint32_t>(current->span.start.line),
                    static_cast<std::uint32_t>(current->span.start.column)}});
            }
        }else{
            emit_checked_binary(builder_,Binary{
                out,binary->op,left,right,left_type,checked_.raw_types.at(current),
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
                release_owned(builder_,out,from);
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

ValueId OperatorLowering::lower_exact_op(const Expr& e, const CallExpr& n) {
    const auto* provider =
        std::get_if<StringExpr>(&n.args[0].value->data);
    const auto* opcode =
        std::get_if<IntegerExpr>(&n.args[1].value->data);
    if (!provider || !opcode || !opcode->fits_u64 ||
        opcode->value >
            std::numeric_limits<std::uint32_t>::max()) {
        throw std::logic_error(
            "checked real.op lost its literal contract");
    }
    std::vector<ValueId> inputs;
    inputs.reserve(n.args.size()-2);
    for(std::size_t i=2;i<n.args.size();++i)
        inputs.push_back(lowerer_.lower(*n.args[i].value));
    auto out=builder_.fresh();
    builder_.emit(ExactOp{
        out,provider->value,
        static_cast<std::uint32_t>(opcode->value),
        inputs,checked_.raw_types.at(&e)});
    for(std::size_t i=2;i<n.args.size();++i)
        lifetime_.release_temporary(*n.args[i].value,inputs[i-2]);
    return out;
}
} // namespace quidra::lowering
