// Text lowering: the methods of strings (contains, find, slice, split,
// utf8, ...), the string type's own methods (string.from_utf8,
// string.repeat) and the implicit .string() of a value that has no
// .string() method of its own. The receiver is lowered once, before the
// method is chosen, and an owned receiver is released after the call.

#include "lowering/checked_types.hpp"
#include "lowering/lowerer.hpp"
#include <optional>

namespace quidra::lowering {

std::optional<ValueId> TextLowering::try_string_type_method(
    const Expr& e, const MethodCallExpr& n) {
    const auto* receiver_name=std::get_if<NameExpr>(&n.receiver->data);
    if(receiver_name && receiver_name->name=="string" && n.method=="from_utf8"){
        auto data=lowerer_.lower(*n.args[0].value),out=builder_.fresh();
        builder_.emit(StringFromUtf8{out,data,checked_.raw_types.at(&e)});
        lifetime_.release_temporary(*n.args[0].value,data);
        return out;
    }
    if(receiver_name && receiver_name->name=="string" && n.method=="repeat"){
        auto fill=lowerer_.lower(*n.args[0].value),count=lowerer_.lower_int64(*n.args[1].value),out=builder_.fresh();
        builder_.emit(StringRepeat{out,count,fill});
        lifetime_.release_temporary(*n.args[0].value,fill);
        return out;
    }
    return std::nullopt;
}

std::optional<ValueId> TextLowering::try_string_method(
    const Expr& e, const MethodCallExpr& n, const Type& receiver_type) {
    if(receiver_type.kind==TypeKind::String){
        auto receiver=lowerer_.lower(*n.receiver);
        if(n.method=="string") return receiver;
        auto finish_string_receiver=[&](ValueId out){
            lifetime_.release_temporary(*n.receiver,receiver);
            return out;
        };
        if(n.method=="contains"){
            auto a=lowerer_.lower(*n.args[0].value),out=builder_.fresh();
            builder_.emit(StringContains{out,receiver,a});
            lifetime_.release_temporary(*n.args[0].value,a);
            return finish_string_receiver(out);
        }
        if(n.method=="starts_with"){
            auto a=lowerer_.lower(*n.args[0].value),out=builder_.fresh();
            builder_.emit(StringStartsWith{out,receiver,a});
            lifetime_.release_temporary(*n.args[0].value,a);
            return finish_string_receiver(out);
        }
        if(n.method=="ends_with"){
            auto a=lowerer_.lower(*n.args[0].value),out=builder_.fresh();
            builder_.emit(StringEndsWith{out,receiver,a});
            lifetime_.release_temporary(*n.args[0].value,a);
            return finish_string_receiver(out);
        }
        if(n.method=="find"){
            auto a=lowerer_.lower(*n.args[0].value),out=builder_.fresh();
            builder_.emit(StringFind{out,receiver,a,checked_.raw_types.at(&e)});
            lifetime_.release_temporary(*n.args[0].value,a);
            return finish_string_receiver(out);
        }
        if(n.method=="slice"){
            auto a=lowerer_.lower_int64(*n.args[0].value),b=lowerer_.lower_int64(*n.args[1].value),out=builder_.fresh();
            builder_.emit(StringSlice{out,receiver,a,b,
                static_cast<std::uint32_t>(e.span.start.line),
                static_cast<std::uint32_t>(e.span.start.column)});
            return finish_string_receiver(out);
        }
        if(n.method=="trim"){
            auto out=builder_.fresh();
            builder_.emit(StringTrim{out,receiver});
            return finish_string_receiver(out);
        }
        if(n.method=="split"){
            auto a=lowerer_.lower(*n.args[0].value),out=builder_.fresh();
            builder_.emit(StringSplit{out,receiver,a});
            lifetime_.release_temporary(*n.args[0].value,a);
            return finish_string_receiver(out);
        }
        if(n.method=="utf8"){
            auto out=builder_.fresh();
            builder_.emit(StringUtf8{out,receiver});
            return finish_string_receiver(out);
        }
        if(n.method=="codepoints"){
            auto out=builder_.fresh();
            builder_.emit(StringCodepoints{out,receiver});
            return finish_string_receiver(out);
        }
    }
    return std::nullopt;
}

std::optional<ValueId> TextLowering::try_implicit_string_method(
    const Expr& e, const MethodCallExpr& n) {
    if(n.method=="string" && !checked_.method_calls.contains(&e)){
        auto value=lowerer_.lower(*n.receiver),out=builder_.fresh();
        builder_.emit(ToString{out,value,type_of(checked_,*n.receiver)});
        lifetime_.release_temporary(*n.receiver,value);
        return out;
    }
    return std::nullopt;
}

} // namespace quidra::lowering
