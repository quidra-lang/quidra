#include "llvm_backend/type_helpers.hpp"

#include "llvm_backend/half_precision.hpp"
#include "llvm_backend/node_owned_type.hpp"
#include "llvm_backend/runtime_abi.hpp"
#include "llvm_backend/type_lowering.hpp"
#include "llvm_text/function_text.hpp"
#include "llvm_text/llvm_builder.hpp"
#include "quidra/abi/layout.hpp"
#include "quidra/ir/initialization_mask.hpp"
#include "quidra/standard_classes.hpp"

#include <stdexcept>
#include <set>
#include <algorithm>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace quidra::llvm_backend {

using namespace llvm_text;
using namespace llvm_text::types;

namespace {
// "@" + prefix + type_identifier(t), written into the identifier's string.
std::string helper_name(std::string_view prefix,const Type&t){
    auto name=type_identifier(t);
    name.insert(0,prefix);
    name.insert(name.begin(),'@');
    return name;
}
} // namespace

std::string clone_name(const Type&t){return helper_name(runtime_abi::generated_helper_prefix::clone,t);}
std::string drop_name(const Type&t){return helper_name(runtime_abi::generated_helper_prefix::drop,t);}
bool holds_reference_fields(const Type& type,const std::unordered_map<std::string,ir::ClassLayout>& layouts){
    std::set<std::string> visiting;
    const auto holds=[&](const auto& self,const Type& t)->bool {
        if(t.kind==TypeKind::Tuple) return std::any_of(t.parameters.begin(),t.parameters.end(),[&](const Type& c){return self(self,c);});
        if(t.kind==TypeKind::Array) return t.first && self(self,*t.first);
        if(t.kind==TypeKind::Union) return std::any_of(t.cases.begin(),t.cases.end(),[&](const Type& c){return self(self,c);});
        if(t.kind!=TypeKind::Class || !visiting.insert(t.class_name).second) return false;
        const auto& layout=layouts.at(t.class_name);
        if(!layout.reference_fields.empty())return true;
        return std::any_of(layout.fields.begin(),layout.fields.end(),[&](const Type& c){return self(self,c);});
    };
    return holds(holds,type);
}

std::string equality_name(const Type&t){return helper_name(runtime_abi::generated_helper_prefix::equal,t);}
std::string slice_get_name(const Type&t){return helper_name(runtime_abi::generated_helper_prefix::slice_get,t);}
std::string slice_set_name(const Type&t){return helper_name(runtime_abi::generated_helper_prefix::slice_set,t);}

// The entry block of a helper whose parameter %src may be null: a null
// source returns null (block null.ret), any other continues at `body`.
void return_null_for_null_source(LlvmBuilder& b, LlvmBlock body) {
    b.block("entry");
    b.icmp("%null", IntPredicate::eq, ptr, "%src", "null");
    b.br("%null", "null.ret", body);
    b.block("null.ret");
    b.ret({ptr, "null"});
    b.block(body);
}

namespace {

void collect_clone_type(const Type&t,std::map<std::string,Type>&types,const std::unordered_map<std::string,ir::ClassLayout>&layouts){
 if(t.kind==TypeKind::Tuple) {
     if(!owns_managed(t))return;
     const auto id=type_identifier(t);if(types.contains(id))return;types[id]=t;
     for(const auto& element:t.parameters) collect_clone_type(element,types,layouts);
     return;
 }
 if(!requires_value_clone(t))return;
 const auto id=type_identifier(t);if(types.contains(id))return;
 types[id]=t;
 if(t.kind==TypeKind::Array){collect_clone_type(*t.first,types,layouts);}
 else if(t.kind==TypeKind::Union){for(const auto&c:t.cases)collect_clone_type(c,types,layouts);}
 else if(t.kind==TypeKind::Class){for(const auto&f:layouts.at(t.class_name).fields)collect_clone_type(f,types,layouts);}
}

void collect_drop_type(const Type& type, std::map<std::string,Type>& types,
                       const std::unordered_map<std::string,ir::ClassLayout>& layouts) {
    if (!owns_managed(type)) return;
    if (is_bare_integer(type) || type.kind == TypeKind::Real || type.kind == TypeKind::Com ||
        type.kind == TypeKind::String || type.kind == TypeKind::Error ||
        (type.kind == TypeKind::Class &&
         standard_class::is_runtime_handle(type.class_name))) return;
    if (type.kind == TypeKind::Class && !layouts.contains(type.class_name)) return;
    const auto id = type_identifier(type);
    if (types.contains(id)) return;
    types[id] = type;
    if(type.kind==TypeKind::Tuple) for(const auto& element:type.parameters) collect_drop_type(element,types,layouts);
    else if (type.kind == TypeKind::Array) collect_drop_type(*type.first, types, layouts);
    else if (type.kind == TypeKind::Union)
        for (const auto& current : type.cases) collect_drop_type(current, types, layouts);
    else if (type.kind == TypeKind::Class)
        for (const auto& field : layouts.at(type.class_name).fields)
            collect_drop_type(field, types, layouts);
}

} // namespace

std::string drop_callback_for(const Type& type,
                              const std::unordered_map<std::string,ir::ClassLayout>& layouts) {
    if(type.kind==TypeKind::Tuple) return drop_name(type);
    if (is_bare_integer(type)) return runtime_abi::global_name(runtime_abi::exact::bigint_drop);
    if (type.kind == TypeKind::Real) return runtime_abi::global_name(runtime_abi::exact::bigreal_drop);
    if (type.kind == TypeKind::Com) return runtime_abi::global_name(runtime_abi::complex_support::com_drop);
    if (type.kind == TypeKind::Class) {
        if (const auto* handle = runtime_abi::runtime_handle_class(type.class_name))
            return runtime_abi::global_name(*handle->drop);
    }
    if (type.kind == TypeKind::Tuple || type.kind == TypeKind::Bin || type.kind == TypeKind::Array ||
        type.kind == TypeKind::Tensor ||
        type.kind == TypeKind::Union ||
        (type.kind == TypeKind::Class && layouts.contains(type.class_name))) {
        return drop_name(type);
    }
    return "null";
}

namespace {

void collect_equality_type(const Type& t, std::map<std::string,Type>& types,
                           const std::unordered_map<std::string,ir::ClassLayout>& layouts) {
    if (t.kind != TypeKind::Union && t.kind != TypeKind::Tuple && t.kind != TypeKind::Bin && t.kind != TypeKind::Array && t.kind != TypeKind::Class) return;
    const auto id = type_identifier(t);
    if (types.contains(id)) return;
    types[id] = t;
    if(t.kind==TypeKind::Tuple){for(const auto& child:t.parameters)collect_equality_type(child,types,layouts);return;}
    if(t.kind==TypeKind::Union){for(const auto& child:t.cases)collect_equality_type(child,types,layouts);return;}
    if (t.kind == TypeKind::Bin) return;
    if (t.kind == TypeKind::Array) {
        collect_equality_type(*t.first, types, layouts);
        return;
    }
    const auto& layout = layouts.at(t.class_name);
    for (const auto& field : layout.fields) collect_equality_type(field, types, layouts);
}

// %<id> = whether the two values of `type` are equal: through the runtime
// for big numbers and strings, icmp or fcmp for scalars, the type's
// equality helper for bins, arrays and classes.
void emit_equality_value(LlvmBuilder& b, const Type& type, const std::string& left,
                         const std::string& right, const std::string& id,
                         const std::unordered_map<std::string,ir::ClassLayout>* layouts=nullptr,
                         const std::string& line="0",const std::string& column="0") {
    const auto result = "%" + id;
    if (is_bare_integer(type)) {
        b.call(result, i1, LlvmOperand::global(int_helper::eq),
               {{bare_integer_type, left}, {bare_integer_type, right}});
    } else if (type.kind == TypeKind::Real) {
        const auto comparison = result + ".cmp";
        b.call(comparison, i32, LlvmOperand::global(real_helper::compare),
               {{exact_real_type, left}, {exact_real_type, right}, {i64, 0}, {i64, 0}});
        b.icmp(result, IntPredicate::eq, i32, comparison, 0);
    } else if (is_fixed_integer(type)) {
        b.icmp(result, IntPredicate::eq, llvm_type(type), left, right);
    } else if (type.kind == TypeKind::Com) {
        b.call(result, runtime_abi::complex_support::com_equal, {{ptr, left}, {ptr, right}});
    } else if (is_fixed_complex(type)) {
        // Both parts equal, compared as their part type compares (real16b
        // through binary32).
        const auto part = complex_part_type(type);
        const auto part_type = part.kind == TypeKind::Real16b ? float_type : llvm_type(part);
        std::string parts[4];
        const std::string sources[4] = {left, right, left, right};
        for (unsigned i = 0; i < 4; ++i) {
            parts[i] = result + ".part" + std::to_string(i);
            b.extractvalue(parts[i], llvm_type(type), sources[i], i < 2 ? 0 : 1);
            if (part.kind == TypeKind::Real16b) {
                const auto wide = parts[i] + ".wide";
                b.call(wide, float_type, LlvmOperand::global(half_helper::bf16_widen),
                       {{i16, parts[i]}});
                parts[i] = wide;
            }
        }
        b.fcmp(result + ".re", FloatPredicate::oeq, part_type, parts[0], parts[1]);
        b.fcmp(result + ".im", FloatPredicate::oeq, part_type, parts[2], parts[3]);
        b.binary(result, BinaryOp::and_, i1, result + ".re", result + ".im");
    } else if (type.kind == TypeKind::Bool) {
        b.icmp(result, IntPredicate::eq, i1, left, right);
    } else if (type.kind == TypeKind::Real16b) {
        // bfloat16 bits compare as the binary32 values they widen to, so
        // -0.0 equals 0.0.
        b.call(result + ".left", float_type, LlvmOperand::global(half_helper::bf16_widen),
               {{i16, left}});
        b.call(result + ".right", float_type, LlvmOperand::global(half_helper::bf16_widen),
               {{i16, right}});
        b.fcmp(result, FloatPredicate::oeq, float_type, result + ".left", result + ".right");
    } else if (is_fixed_real(type)) {
        b.fcmp(result, FloatPredicate::oeq, llvm_type(type), left, right);
    } else if (type.kind == TypeKind::String) {
        b.call(result, runtime_abi::text::equal, {{ptr, left}, {ptr, right}});
    } else if (type.kind == TypeKind::Error) {
        // Errors compare their codes, details and causes.
        b.call(result, runtime_abi::error::equal, {{ptr, left}, {ptr, right}});
    } else if(type.kind==TypeKind::Tuple){
        std::vector<LlvmValue> arguments{{llvm_type(type),left},{llvm_type(type),right}};
        if(layouts && holds_reference_fields(type,*layouts)){arguments.push_back({i64,line});arguments.push_back({i64,column});}
        const auto callee=equality_name(type);
        LlvmCall call(i1,callee);call.result(result);for(const auto& argument:arguments)call.argument(argument);b.call(call);
    } else if (type.kind == TypeKind::None) {
        b.icmp(result,IntPredicate::eq,i1,"false","false");
    } else if (type.kind == TypeKind::Union || type.kind == TypeKind::Bin || type.kind == TypeKind::Array || type.kind == TypeKind::Class) {
        std::vector<LlvmValue> arguments{{ptr,left},{ptr,right}};
        if(layouts && holds_reference_fields(type,*layouts)){arguments.push_back({i64,line});arguments.push_back({i64,column});}
        const auto callee=equality_name(type);
        LlvmCall call(i1,callee);call.result(result);for(const auto& argument:arguments)call.argument(argument);b.call(call);
    }
}

// Stores a copy of `value`, of `type`, into `slot`: a deep copy through the
// type's clone helper (named `copy`), the value itself retained when its
// storage is shared, or the value itself.
void store_copy(LlvmBuilder& b, const Type& type, std::string_view value, std::string_view copy,
                std::string_view slot) {
    if(type.kind==TypeKind::Tuple) {
        if(owns_managed(type))b.call(void_type,clone_name(type),{{ptr,slot},{llvm_type(type),value}});
        else b.store({llvm_type(type),value},slot,Align::one);
    } else if (requires_value_clone(type)) {
        b.call(copy, ptr, clone_name(type), {{ptr, value}});
        b.store({ptr, copy}, slot, Align::one);
    } else if (type.kind == TypeKind::Real) {
        b.call(void_type, LlvmOperand::global(real_helper::retain), {{exact_real_type, value}});
        b.store({exact_real_type, value}, slot, Align::one);
    } else if (is_bare_integer(type)) {
        b.call(void_type, LlvmOperand::global(int_helper::retain), {{bare_integer_type, value}});
        b.store({bare_integer_type, value}, slot, Align::one);
    } else if (uses_shared_immutable_storage(type)) {
        b.call(runtime_abi::memory::managed_retain, {{ptr, value}});
        b.store({ptr, value}, slot, Align::one);
    } else {
        b.store({llvm_type(type), value}, slot, Align::one);
    }
}

// A clone helper that is one runtime call: %dst = call ptr runtime(ptr %src).
std::string runtime_clone_helper(const Type& type, const LlvmCallee& runtime) {
    FunctionText text;
    LlvmBuilder b(text);
    b.define(LlvmFunction(ptr, clone_name(type)).parameter(ptr, "", "%src"));
    b.block("entry");
    b.call("%dst", runtime, {{ptr, "%src"}});
    b.ret({ptr, "%dst"});
    b.end_function();
    return text.str();
}

// Releases the owned value of `type` stored at `slot`, loaded as `value`:
// a pointer with the drop callback `callback`, a `real` or an
// arbitrary-precision integer word through its release helper.
void release_slot(LlvmBuilder& b, const Type& type, std::string_view value, std::string_view slot,
                  std::string_view callback,bool hot_slot=false) {
    if(type.kind==TypeKind::Tuple) {
        b.call(void_type,drop_name(type),{{ptr,slot}});
        if(hot_slot)b.call(runtime_abi::program::parallel_cleanup_rethrow,{});
        return;
    }
    const auto clear_before_drop=[&] {
        if(!hot_slot)return;
        b.call("%slot.release.active",runtime_abi::program::parallel_cleanup_active,{});
        b.br("%slot.release.active","slot.release.clear","slot.release.ready");
        b.block("slot.release.clear");
        const auto empty=type.kind==TypeKind::Real?std::string(exact_real_none):
            is_bare_integer(type)?std::string("0"):std::string("null");
        b.store({llvm_type(type),empty},slot,Align::one);
        b.br("slot.release.ready");b.block("slot.release.ready");
    };
    if (type.kind == TypeKind::Real) {
        b.load(value, exact_real_type, slot, Align::one);
        clear_before_drop();
        b.call(void_type, LlvmOperand::global(real_helper::release), {{exact_real_type, value}});
        if(hot_slot)b.call(runtime_abi::program::parallel_cleanup_rethrow,{});
        return;
    }
    if (is_bare_integer(type)) {
        b.load(value, bare_integer_type, slot, Align::one);
        clear_before_drop();
        b.call(void_type, LlvmOperand::global(int_helper::release), {{bare_integer_type, value}});
        if(hot_slot)b.call(runtime_abi::program::parallel_cleanup_rethrow,{});
        return;
    }
    b.load(value, ptr, slot, Align::one);
    clear_before_drop();
    b.call(runtime_abi::memory::managed_release, {{ptr, value}, {ptr, callback}});
    if(hot_slot)b.call(runtime_abi::program::parallel_cleanup_rethrow,{});
}

// The body of an element loop of an equality helper, from the byte offset
// %offset of the element in both arrays: compares the two elements and
// continues at loop.next, or returns through different.
void compare_elements_at_offset(LlvmBuilder& b, const Type& element, const std::unordered_map<std::string,ir::ClassLayout>& layouts, const std::string& line,const std::string& column) {
    b.getelementptr("%left.slot", Inbounds::yes, i8, "%left", {{i64, "%offset"}});
    b.getelementptr("%right.slot", Inbounds::yes, i8, "%right", {{i64, "%offset"}});
    b.load("%left.value", llvm_type(element), "%left.slot", Align::one);
    b.load("%right.value", llvm_type(element), "%right.slot", Align::one);
    emit_equality_value(b, element, "%left.value", "%right.value", "element.equal",&layouts,line,column);
    b.br("%element.equal", "loop.next", "different");
    b.block("loop.next");
    b.binary("%next", BinaryOp::add, i64, "%index", 1);
    b.br("loop.cond");
}

} // namespace

void collect_clone_types(const ir::Module& m,std::map<std::string,Type>& types,
                         const std::unordered_map<std::string,ir::ClassLayout>& layouts) {
    for(const auto& f:m.functions) {
        // A writable native array borrow detaches a shared outer allocation
        // using the same typed clone as an ordinary array value copy.
        if(f.external_symbol)
            for(const auto& parameter:f.parameters)
                if(parameter.writable && !parameter.is_const && is_foreign_tensor_array(parameter.type))
                    collect_clone_type(parameter.type,types,layouts);
        for(const auto& block:f.blocks)
            for(const auto& instruction:block.instructions)
                if(const auto* clone=std::get_if<ir::Clone>(&instruction))
                    collect_clone_type(clone->type,types,layouts);
    }
}

namespace {
std::string partial_name(std::string_view operation,const Type& type) {
    return "@.quidra.partial."+std::string(operation)+"."+type_identifier(type);
}
void partial_begin(LlvmBuilder& b,const std::string& cleanup,std::uint64_t bytes=16) {
    b.alloca_slot("%partial.stack",LlvmType::array(bytes,i8),Align::sixteen);
    b.call("%partial.frame",runtime_abi::program::parallel_frame_create,
        {{i64,bytes},{i64,16},{ptr,cleanup}});
    b.icmp("%partial.active",IntPredicate::ne,ptr,"%partial.frame","null");
    b.select("%partial.cell","%partial.active",{ptr,"%partial.frame"},{ptr,"%partial.stack"});
    b.getelementptr("%partial.count",Inbounds::no,i8,"%partial.cell",{{i64,8}});
    b.store({ptr,"null"},"%partial.cell",Align::eight);
    b.store({i64,0},"%partial.count",Align::eight);
}
void partial_publish(LlvmBuilder& b,std::string_view bytes) {
    // The callback is dormant outside a node and does not own this stack cell.
    b.br("%partial.active","partial.zero","partial.ready");
    b.block("partial.zero");
    b.call(runtime_abi::c_library::memset,{{ptr,"%dst"},{i32,0},{i64,bytes}});
    b.br("partial.ready");
    b.block("partial.ready");
    b.store({ptr,"%dst"},"%partial.cell",Align::eight);
}
void partial_finish(LlvmBuilder& b) {
    b.store({ptr,"null"},"%partial.cell",Align::eight);
    b.call(runtime_abi::program::parallel_frame_leave,{{ptr,"%partial.frame"}});
}
std::string partial_leaf_drop(const Type& type,const std::string& name,
    const std::unordered_map<std::string,ir::ClassLayout>& layouts) {
    FunctionText text;LlvmBuilder b(text);
    b.define(LlvmFunction(void_type,name).parameter(ptr,"","%slot"));b.block("entry");
    if(node_owns_value(type)) {
        b.load("%value",llvm_type(type),"%slot",Align::one);
        const auto empty=type.kind==TypeKind::Real?"zeroinitializer":
            is_bare_integer(type)?"0":is_pointer_runtime_type(type)?"null":"zeroinitializer";
        b.store({llvm_type(type),empty},"%slot",Align::one);
        if(type.kind==TypeKind::Real)
            b.call(void_type,LlvmOperand::global(real_helper::release),{{exact_real_type,"%value"}});
        else if(is_bare_integer(type))
            b.call(void_type,LlvmOperand::global(int_helper::release),{{bare_integer_type,"%value"}});
        else if(is_pointer_runtime_type(type))
            b.call(runtime_abi::memory::managed_release,{{ptr,"%value"},{ptr,drop_callback_for(type,layouts)}});
        else {
            // Unboxed managed aggregate: clear each typed leaf independently.
            // Whole SSA is loaded before the root cell is cleared, then leaves
            // are placed in independent stable cells for the captured callbacks.
            // The aggregate cell itself is still stable; restore the value and
            // clear each child rather than dispatching managed heap release.
            b.store({llvm_type(type),"%value"},"%slot",Align::one);
            for(std::size_t i=0;i<type.parameters.size();++i) {
                const auto& child=type.parameters[i];if(!node_owns_value(child))continue;
                const auto slot="%child."+std::to_string(i);
                b.getelementptr(slot,Inbounds::no,llvm_type(type),"%slot",{{i32,0},{i32,static_cast<long long>(i)}});
                b.call(runtime_abi::program::parallel_frame_cleanup,
                    {{ptr,name+"."+std::to_string(i)},{ptr,slot}});
            }
            b.store({llvm_type(type),"zeroinitializer"},"%slot",Align::one);
        }
    }
    b.ret_void();b.end_function();
    auto out=text.str();
    if(!is_pointer_runtime_type(type)&&type.kind!=TypeKind::Real&&!is_bare_integer(type))
        for(std::size_t i=0;i<type.parameters.size();++i)
            if(node_owns_value(type.parameters[i]))out+=partial_leaf_drop(type.parameters[i],name+"."+std::to_string(i),layouts);
    return out;
}
std::string reference_slot_drop(const std::string& name) {
    FunctionText text;LlvmBuilder b(text);
    b.define(LlvmFunction(void_type,name).parameter(ptr,"","%slot"));b.block("entry");
    b.load("%address",ptr,"%slot",Align::one);
    b.store({ptr,"null"},"%slot",Align::one);
    b.getelementptr("%token",Inbounds::no,i8,"%slot",{{i64,8}});
    b.store({i64,0},"%token",Align::one);
    b.call(runtime_abi::memory::managed_unpin,{{ptr,"%address"}});
    b.ret_void();b.end_function();return text.str();
}

std::string partial_container_drop(const Type& type,const std::string& name,
    const std::unordered_map<std::string,ir::ClassLayout>& layouts,
    const ArrayLayoutPolicy& array_layout,bool slice=false) {
    FunctionText text;LlvmBuilder b(text);
    b.define(LlvmFunction(void_type,name).parameter(ptr,"","%frame"));b.block("entry");
    b.load("%array",ptr,"%frame",Align::eight);b.store({ptr,"null"},"%frame",Align::eight);
    b.icmp("%null",IntPredicate::eq,ptr,"%array","null");b.br("%null","done","body");b.block("body");
    b.getelementptr("%count.slot",Inbounds::no,i8,"%frame",{{i64,8}});b.load("%count",i64,"%count.slot",Align::eight);
    std::string extras;
    if(type.kind==TypeKind::Tuple&&!slice) {
        for(std::size_t i=0;i<type.parameters.size();++i) {
            const auto& child=type.parameters[i];if(!node_owns_value(child))continue;
            const auto index=std::to_string(i),leaf=name+".leaf."+index;
            b.icmp("%has."+index,IntPredicate::ugt,i64,"%count",static_cast<long long>(i));
            b.br("%has."+index,"drop."+index,"next."+index);b.block("drop."+index);
            b.getelementptr("%slot."+index,Inbounds::no,i8,"%array",{{i64,tuple_element_offset(type,i)}});
            b.call(runtime_abi::program::parallel_frame_cleanup,{{ptr,leaf},{ptr,"%slot."+index}});
            b.br("next."+index);b.block("next."+index);extras+=partial_leaf_drop(child,leaf,layouts);
        }
        b.br("done");
    } else if(type.kind==TypeKind::Class&&!slice) {
        const auto& layout=layouts.at(type.class_name);
        for(std::size_t i=0;i<layout.fields.size();++i) {
            const auto& field=layout.fields[i];
            const bool reference=layout.reference_fields.contains(i);
            if(!reference && !node_owns_value(field))continue;
            const auto index=std::to_string(i),leaf=name+".leaf."+index;
            b.icmp("%has."+index,IntPredicate::ugt,i64,"%count",static_cast<long long>(i));
            b.br("%has."+index,"drop."+index,"next."+index);b.block("drop."+index);
            b.getelementptr("%slot."+index,Inbounds::no,i8,"%array",{{i64,class_field_offset(layout,i)}});
            b.call(runtime_abi::program::parallel_frame_cleanup,{{ptr,leaf},{ptr,"%slot."+index}});
            b.br("next."+index);b.block("next."+index);
            extras+=reference?reference_slot_drop(leaf):partial_leaf_drop(field,leaf,layouts);
        }
        b.br("free");
    } else if(type.kind==TypeKind::Union&&!slice) {
        std::vector<std::string> labels;
        for(std::size_t i=0;i<type.cases.size();++i)labels.push_back("case."+std::to_string(i));
        std::vector<LlvmCase> cases;
        for(std::size_t i=0;i<type.cases.size();++i)cases.push_back({static_cast<long long>(i+1),labels[i]});
        b.switch_on({i64,"%count"},"free",cases);
        for(std::size_t i=0;i<type.cases.size();++i) {
            const auto index=std::to_string(i),leaf=name+".leaf."+index;b.block("case."+index);
            if(node_owns_value(type.cases[i])) {
                b.getelementptr("%slot."+index,Inbounds::no,i8,"%array",{{i64,abi::union_box_layout::payload_offset}});
                b.call(runtime_abi::program::parallel_frame_cleanup,{{ptr,leaf},{ptr,"%slot."+index}});
                extras+=partial_leaf_drop(type.cases[i],leaf,layouts);
            }b.br("free");
        }
    } else {
        const auto element=is_fixed_array(type)&&!slice?fixed_array_storage(type,array_layout).leaf:*type.first;
        const auto stride=runtime_storage_bytes(element);
        const auto offset=is_fixed_array(type)&&!slice?0:abi::array_layout::payload_offset;
        b.br("cond");b.block("cond");b.phi("%i",i64,{{0,"body"},{"%next","loop"}});
        b.icmp("%more",IntPredicate::ult,i64,"%i","%count");b.br("%more","loop","free");b.block("loop");
        if(node_owns_value(element)) {
            b.binary("%distance",BinaryOp::mul,i64,"%i",stride);b.binary("%offset",BinaryOp::add,i64,"%distance",offset);
            b.getelementptr("%slot",Inbounds::no,i8,"%array",{{i64,"%offset"}});
            b.call(runtime_abi::program::parallel_frame_cleanup,{{ptr,name+".leaf"},{ptr,"%slot"}});
            extras+=partial_leaf_drop(element,name+".leaf",layouts);
        }
        b.binary("%next",BinaryOp::add,i64,"%i",1);b.br("cond");
    }
    b.block("free");
    if(type.kind!=TypeKind::Tuple || slice)b.call(runtime_abi::memory::managed_release,{{ptr,"%array"},{ptr,"null"}});
    b.br("done");
    b.block("done");b.ret_void();b.end_function();return text.str()+extras;
}
} // namespace

std::string emit_clone_helper(const Type&t,const std::unordered_map<std::string,ir::ClassLayout>&layouts,const ArrayLayoutPolicy&array_layout){
 if(t.kind==TypeKind::Tuple){
    FunctionText text;LlvmBuilder b(text);
    b.define(LlvmFunction(void_type,clone_name(t)).parameter(ptr,"","%dst").parameter(llvm_type(t),"","%value"));
    b.block("entry");
    const auto partial=partial_name("clone",t);
    partial_begin(b,partial,16+runtime_storage_bytes(t));
    b.getelementptr("%partial.payload",Inbounds::no,i8,"%partial.cell",{{i64,16}});
    b.store({ptr,"%partial.payload"},"%partial.cell",Align::eight);
    // A Node clone owns completed children only in its stable partial payload.
    // Publishing them into %dst as well would let both frames drop the same
    // leaf if a later child clone fails. Ordinary calls keep their old stores.
    b.select("%clone.target","%partial.active",{ptr,"%partial.payload"},{ptr,"%dst"});
    for(std::size_t i=0;i<t.parameters.size();++i){
        const auto id=std::to_string(i);const auto& child=t.parameters[i];
        b.extractvalue("%element."+id,llvm_type(t),"%value",i);
        b.getelementptr("%slot."+id,Inbounds::yes,i8,"%clone.target",{{i64,tuple_element_offset(t,i)}});
        store_copy(b,child,"%element."+id,"%copy."+id,"%slot."+id);
        b.store({i64,static_cast<long long>(i+1)},"%partial.count",Align::eight);
    }
    b.br("%partial.active","tuple.publish","tuple.finish");b.block("tuple.publish");
    b.load("%tuple.copy",llvm_type(t),"%partial.payload",Align::one);
    b.store({llvm_type(t),"%tuple.copy"},"%dst",Align::one);
    b.br("tuple.finish");b.block("tuple.finish");
    partial_finish(b);b.ret_void();b.end_function();
    return partial_container_drop(t,partial,layouts,array_layout)+text.str();
 }

 if(t.kind==TypeKind::Tensor) return runtime_clone_helper(t,runtime_abi::tensor::clone);
 if(t.kind==TypeKind::Class){
    const auto* handle=runtime_abi::runtime_handle_class(t.class_name);
    if(handle && handle->clone) return runtime_clone_helper(t,*handle->clone);
 }
 if(t.kind==TypeKind::Bin) return runtime_clone_helper(t,runtime_abi::bin::clone);
 if(t.kind==TypeKind::Union){
    FunctionText text;
    LlvmBuilder b(text);
    b.define(LlvmFunction(ptr, clone_name(t)).parameter(ptr, "", "%src"));
    return_null_for_null_source(b, "clone.body");
    const auto partial=partial_name("clone",t);partial_begin(b,partial);
    b.call("%dst", runtime_abi::prelude::alloc, {{i64, union_box_bytes(t)}});
    partial_publish(b,std::to_string(union_box_bytes(t)));
    b.load("%tag", i64, "%src", Align::one);
    b.store({i64, "%tag"}, "%dst", Align::one);
    b.getelementptr("%sp", Inbounds::no, i8, "%src", {{i64, abi::union_box_layout::payload_offset}});
    b.getelementptr("%dp", Inbounds::no, i8, "%dst", {{i64, abi::union_box_layout::payload_offset}});
    std::vector<std::string> labels;
    for(std::size_t i=0;i<t.cases.size();++i) labels.push_back("case"+std::to_string(i));
    std::vector<LlvmCase> cases;
    for(std::size_t i=0;i<t.cases.size();++i) cases.push_back(LlvmCase{i, labels[i]});
    b.switch_on({i64, "%tag"}, "done", cases);
    for(std::size_t i=0;i<t.cases.size();++i){
        const auto&c=t.cases[i];
        b.block(labels[i]);
        if(c.kind!=TypeKind::Void&&c.kind!=TypeKind::None){
            const auto index=std::to_string(i);
            const auto value="%v"+index;
            b.load(value, llvm_type(c), "%sp", Align::one);
            store_copy(b, c, value, "%copy"+index, "%dp");
            b.store({i64,static_cast<long long>(i+1)},"%partial.count",Align::eight);
        }
        b.br("done");
    }
    b.block("done");
    partial_finish(b);
 b.ret({ptr, "%dst"});
    b.end_function();
    return partial_container_drop(t,partial,layouts,array_layout)+text.str();
 }
 if(t.kind==TypeKind::Class){
    const auto&layout=layouts.at(t.class_name);
    FunctionText text;
    LlvmBuilder b(text);
    const auto bytes=class_storage_bytes(layout);
    b.define(LlvmFunction(ptr, clone_name(t)).parameter(ptr, "", "%src"));
    return_null_for_null_source(b, "clone.body");
    const auto partial=partial_name("clone",t);partial_begin(b,partial);
    b.call("%dst", runtime_abi::prelude::alloc, {{i64, bytes}});
    partial_publish(b,std::to_string(bytes));
    for(std::size_t i=0;i<layout.fields.size();++i){
        const auto&field=layout.fields[i];
        const auto offset=class_field_offset(layout,i);
        const auto index=std::to_string(i);
        const auto source="%sp"+index;
        const auto destination="%dp"+index;
        const auto value="%v"+index;
        b.getelementptr(source, Inbounds::yes, i8, "%src", {{i64, offset}});
        b.getelementptr(destination, Inbounds::yes, i8, "%dst", {{i64, offset}});
        if(layout.reference_fields.contains(i)) {
            b.load(value,ptr,source,Align::one);
            b.call(runtime_abi::memory::managed_pin,{{ptr,value}});
            b.store({ptr,value},destination,Align::one);
            const auto source_root=source+".root",destination_root=destination+".root",token=value+".root";
            b.getelementptr(source_root,Inbounds::yes,i8,source,{{i64,8}});
            b.getelementptr(destination_root,Inbounds::yes,i8,destination,{{i64,8}});
            b.load(token,i64,source_root,Align::one);
            b.store({i64,token},destination_root,Align::one);
        } else {
            b.load(value, llvm_type(field), source, Align::one);
            store_copy(b, field, value, "%copy"+index, destination);
        }
        b.store({i64,static_cast<long long>(i+1)},"%partial.count",Align::eight);
    }
    partial_finish(b);
 b.ret({ptr, "%dst"});
    b.end_function();
    return partial_container_drop(t,partial,layouts,array_layout)+text.str();
 }
 if(is_fixed_array(t)){
    const auto storage=fixed_array_storage(t,array_layout);
    const auto elem=storage.leaf;
    const auto stride=runtime_storage_bytes(elem);
    const auto bytes=checked_storage_product(storage.count,stride);
    FunctionText text;
    LlvmBuilder b(text);
    b.define(LlvmFunction(ptr, clone_name(t)).parameter(ptr, "", "%src"));
    return_null_for_null_source(b, "clone.body");
    const auto partial=partial_name("clone",t);partial_begin(b,partial);
    b.call("%dst", runtime_abi::prelude::alloc, {{i64, bytes}});
    partial_publish(b,std::to_string(bytes));
    b.call(runtime_abi::memory::init_clone, {{ptr, "%dst"}, {ptr, "%src"}, {i64, storage.count}, {i64, stride}, {i64, 0}});
    b.br("cond");
    b.block("cond");
    b.phi("%i", i64, {{0, "partial.ready"}, {"%next", "body"}});
    b.icmp("%more", IntPredicate::slt, i64, "%i", storage.count);
    b.br("%more", "body", "done");
    b.block("body");
    b.binary("%off", BinaryOp::mul, i64, "%i", stride);
    b.getelementptr("%sp", Inbounds::yes, i8, "%src", {{i64, "%off"}});
    b.getelementptr("%dp", Inbounds::yes, i8, "%dst", {{i64, "%off"}});
    b.load("%v", llvm_type(elem), "%sp", Align::one);
    store_copy(b, elem, "%v", "%copy", "%dp");
    b.binary("%next", BinaryOp::add, i64, "%i", 1);
 b.store({i64,"%next"},"%partial.count",Align::eight);
    b.br("cond");
    b.block("done");
    partial_finish(b);
 b.ret({ptr, "%dst"});
    b.end_function();
    return partial_container_drop(t,partial,layouts,array_layout)+text.str();
 }
 const auto elem=*t.first;
 const auto stride=runtime_storage_bytes(elem);
 FunctionText text;
 LlvmBuilder b(text);
 b.define(LlvmFunction(ptr, clone_name(t)).parameter(ptr, "", "%src"));
 return_null_for_null_source(b, "clone.body");
 b.load("%len", i64, "%src", Align::one);
 b.binary("%data.bytes", BinaryOp::mul, i64, "%len", stride);
 b.binary("%bytes", BinaryOp::add, i64, "%data.bytes", abi::array_layout::payload_offset);
 const auto partial=partial_name("clone",t);partial_begin(b,partial);
 b.call("%dst", runtime_abi::prelude::alloc, {{i64, "%bytes"}});
 partial_publish(b,"%bytes");
 b.store({i64, "%len"}, "%dst", Align::one);
 b.getelementptr("%src.data", Inbounds::yes, i8, "%src", {{i64, abi::array_layout::payload_offset}});
 b.call(runtime_abi::memory::init_clone, {{ptr, "%dst"}, {ptr, "%src.data"}, {i64, "%len"}, {i64, stride}, {i64, abi::array_layout::payload_offset}});
 b.br("cond");
 b.block("cond");
 b.phi("%i", i64, {{0, "partial.ready"}, {"%next", "body"}});
 b.icmp("%more", IntPredicate::slt, i64, "%i", "%len");
 b.br("%more", "body", "done");
 b.block("body");
 b.binary("%off0", BinaryOp::mul, i64, "%i", stride);
 b.binary("%off", BinaryOp::add, i64, "%off0", abi::array_layout::payload_offset);
 b.getelementptr("%sp", Inbounds::yes, i8, "%src", {{i64, "%off"}});
 b.getelementptr("%dp", Inbounds::yes, i8, "%dst", {{i64, "%off"}});
 b.load("%v", llvm_type(elem), "%sp", Align::one);
 store_copy(b, elem, "%v", "%copy", "%dp");
 b.binary("%next", BinaryOp::add, i64, "%i", 1);
 b.store({i64,"%next"},"%partial.count",Align::eight);
 b.br("cond");
 b.block("done");
 partial_finish(b);
 b.ret({ptr, "%dst"});
 b.end_function();
 return partial_container_drop(t,partial,layouts,array_layout)+text.str();
}

void collect_drop_types(const ir::Module& module, std::map<std::string,Type>& types,
                        const std::unordered_map<std::string,ir::ClassLayout>& layouts) {
    for (const auto& function : module.functions) {
        collect_drop_type(function.result, types, layouts);
        for (const auto& parameter : function.parameters)
            collect_drop_type(parameter.type, types, layouts);
        for (const auto& block : function.blocks) {
            for (const auto& instruction : block.instructions) {
                std::visit([&](const auto& current) {
                    using T = std::decay_t<decltype(current)>;
                    // Owning SSA temporaries can have a less constrained type
                    // than their destination storage. Node cleanup needs their
                    // callbacks even when no local retains that exact type.
                    if constexpr (requires { current.type; }) {
                        collect_drop_type(current.type, types, layouts);
                    }
                    if constexpr (std::is_same_v<T,ir::Call> ||
                                  std::is_same_v<T,ir::IndirectCall>) {
                        collect_drop_type(current.result, types, layouts);
                    }
                    if constexpr (requires { current.result_type; }) {
                        collect_drop_type(current.result_type, types, layouts);
                    }
                    if constexpr (requires { current.target_type; }) {
                        collect_drop_type(current.target_type, types, layouts);
                    }
                    if constexpr (std::is_same_v<T,ir::VariantMake>) {
                        collect_drop_type(current.container_type, types, layouts);
                    }
                    if constexpr (std::is_same_v<T,ir::ArraySlice>) {
                        collect_drop_type(Type::array(*current.array_type.first), types, layouts);
                    }
                    if constexpr (std::is_same_v<T,ir::ArrayGrowMove> ||
                                  std::is_same_v<T,ir::ArraySorted>) {
                        collect_drop_type(current.array_type, types, layouts);
                    }
                    if constexpr (std::is_same_v<T,ir::TimeNow>) {
                        collect_drop_type(Type::class_type(standard_class::time_instant), types, layouts);
                    }
                    if constexpr (std::is_same_v<T,ir::TimeSince> ||
                                  std::is_same_v<T,ir::TimeSeconds>) {
                        collect_drop_type(Type::class_type(standard_class::time_duration), types, layouts);
                    }
                    if constexpr (std::is_same_v<T,ir::FieldSet>) {
                        collect_drop_type(current.field_type, types, layouts);
                    } if constexpr (std::is_same_v<T,ir::ArraySet>) {
                        collect_drop_type(current.element_type, types, layouts);
                    } if constexpr (std::is_same_v<T,ir::ArraySliceSet>) {
                        // The slice helper releases the elements it replaces.
                        collect_drop_type(*current.array_type.first, types, layouts);
                    }
                }, instruction);
            }
        }
    }
}

std::string node_aggregate_drop(const Type& type,const std::string& name,
    const std::unordered_map<std::string,ir::ClassLayout>& layouts,
    const ArrayLayoutPolicy& array_layout) {
    FunctionText text;LlvmBuilder b(text);std::string extras;
    b.define(LlvmFunction(void_type,name).parameter(ptr,"","%src"));b.block("entry");
    const auto capture=[&](const Type& child,const std::string& index,
                           const std::string& slot,bool reference=false) {
        if(!reference && !node_owns_value(child))return;
        const auto callback=name+".leaf."+index;
        b.call(runtime_abi::program::parallel_frame_cleanup,{{ptr,callback},{ptr,slot}});
        extras+=reference?reference_slot_drop(callback):partial_leaf_drop(child,callback,layouts);
    };
    if(type.kind==TypeKind::Tuple) {
        for(std::size_t i=0;i<type.parameters.size();++i) {
            if(!node_owns_value(type.parameters[i]))continue;
            const auto index=std::to_string(i),slot="%slot."+index;
            b.getelementptr(slot,Inbounds::no,i8,"%src",{{i64,tuple_element_offset(type,i)}});
            capture(type.parameters[i],index,slot);
        }
    } else if(type.kind==TypeKind::Class) {
        const auto& layout=layouts.at(type.class_name);
        for(std::size_t i=0;i<layout.fields.size();++i) {
            const bool reference=layout.reference_fields.contains(i);
            if(!reference && !node_owns_value(layout.fields[i]))continue;
            const auto index=std::to_string(i),slot="%slot."+index;
            b.getelementptr(slot,Inbounds::no,i8,"%src",{{i64,class_field_offset(layout,i)}});
            capture(layout.fields[i],index,slot,reference);
        }
    } else if(type.kind==TypeKind::Union) {
        b.load("%tag",i64,"%src",Align::one);
        std::vector<std::string> labels;
        for(std::size_t i=0;i<type.cases.size();++i)labels.push_back("case."+std::to_string(i));
        std::vector<LlvmCase> cases;
        for(std::size_t i=0;i<type.cases.size();++i)cases.push_back({static_cast<long long>(i),labels[i]});
        b.switch_on({i64,"%tag"},"done",cases);
        for(std::size_t i=0;i<type.cases.size();++i) {
            const auto index=std::to_string(i);b.block("case."+index);
            if(node_owns_value(type.cases[i])) {
                const auto slot="%slot."+index;
                b.getelementptr(slot,Inbounds::no,i8,"%src",{{i64,abi::union_box_layout::payload_offset}});
                capture(type.cases[i],index,slot);
            }
            b.br("done");
        }
        b.block("done");
    } else {
        const bool fixed=is_fixed_array(type);
        const auto child=fixed?fixed_array_storage(type,array_layout).leaf:*type.first;
        if(node_owns_value(child)) {
            const auto stride=runtime_storage_bytes(child);
            std::string count;
            if(fixed)count=std::to_string(fixed_array_storage(type,array_layout).count);
            else{b.load("%count",i64,"%src",Align::one);count="%count";}
            b.br("cond");b.block("cond");
            b.phi("%i",i64,{{0,"entry"},{"%next","body"}});
            b.icmp("%more",IntPredicate::ult,i64,"%i",count);b.br("%more","body","done");b.block("body");
            b.binary("%distance",BinaryOp::mul,i64,"%i",stride);
            b.binary("%offset",BinaryOp::add,i64,"%distance",fixed?0:abi::array_layout::payload_offset);
            b.getelementptr("%slot",Inbounds::no,i8,"%src",{{i64,"%offset"}});
            capture(child,"element","%slot");
            b.binary("%next",BinaryOp::add,i64,"%i",1);b.br("cond");b.block("done");
        }
    }
    b.ret_void();b.end_function();return text.str()+extras;
}

std::string emit_drop_helper(const Type& type,
                             const std::unordered_map<std::string,ir::ClassLayout>& layouts,
                             const ArrayLayoutPolicy& array_layout) {
    FunctionText text;
    LlvmBuilder b(text);
    b.define(LlvmFunction(void_type, drop_name(type)).parameter(ptr, "", "%src"));
    b.block("entry");
    const bool aggregate=type.kind==TypeKind::Tuple || type.kind==TypeKind::Class ||
        type.kind==TypeKind::Union || type.kind==TypeKind::Array;
    const auto node_drop="@.quidra.node.drop."+type_identifier(type);
    if(aggregate) {
        b.call("%node.active",runtime_abi::program::parallel_cleanup_active,{});
        b.br("%node.active","node.drop","ordinary.drop");b.block("node.drop");
        b.call(void_type,node_drop,{{ptr,"%src"}});b.ret_void();b.block("ordinary.drop");
    }
    const auto finish=[&] {
        return text.str()+(aggregate?node_aggregate_drop(type,node_drop,layouts,array_layout):std::string{});
    };

    if(type.kind==TypeKind::Tuple) {
        for(std::size_t i=0;i<type.parameters.size();++i) if(owns_managed(type.parameters[i])) {
            const auto slot="%tuple.slot."+std::to_string(i);
            b.getelementptr(slot,Inbounds::yes,i8,"%src",{{i64,tuple_element_offset(type,i)}});
            release_slot(b,type.parameters[i],"%tuple.value."+std::to_string(i),slot,drop_callback_for(type.parameters[i],layouts));
        }
        b.ret_void();b.end_function();return finish();
    }
    if (type.kind == TypeKind::Bin) {
        b.ret_void();
        b.end_function();
        return finish();
    }

    if (type.kind == TypeKind::Tensor) {
        b.call(runtime_abi::tensor::drop, {{ptr, "%src"}});
        b.ret_void();
        b.end_function();
        return finish();
    }
    if (type.kind == TypeKind::Union) {
        b.load("%tag", i64, "%src", Align::one);
        b.getelementptr("%payload", Inbounds::yes, i8, "%src", {{i64, abi::union_box_layout::payload_offset}});
        std::vector<std::string> labels;
        for (std::size_t i = 0; i < type.cases.size(); ++i) labels.push_back("case" + std::to_string(i));
        std::vector<LlvmCase> cases;
        for (std::size_t i = 0; i < type.cases.size(); ++i) cases.push_back(LlvmCase{i, labels[i]});
        b.switch_on({i64, "%tag"}, "done", cases);
        for (std::size_t i = 0; i < type.cases.size(); ++i) {
            const auto& current = type.cases[i];
            b.block(labels[i]);
            if (owns_managed(current))
                release_slot(b, current, "%v" + std::to_string(i), "%payload", drop_callback_for(current, layouts));
            b.br("done");
        }
        b.block("done");
        b.ret_void();
        b.end_function();
        return finish();
    }

    if (type.kind == TypeKind::Class) {
        const auto& layout = layouts.at(type.class_name);
        for (std::size_t i = 0; i < layout.fields.size(); ++i) {
            const auto& field = layout.fields[i];
            if(layout.reference_fields.contains(i)) {
                const auto index=std::to_string(i),slot="%ref.slot"+index,value="%ref.value"+index;
                b.getelementptr(slot,Inbounds::yes,i8,"%src",{{i64,class_field_offset(layout,i)}});
                b.load(value,ptr,slot,Align::one);
                b.store({ptr,"null"},slot,Align::one);
                b.call(runtime_abi::memory::managed_unpin,{{ptr,value}});
                continue;
            }
            if (!owns_managed(field)) continue;
            const auto index = std::to_string(i);
            const auto slot = "%slot" + index;
            b.getelementptr(slot, Inbounds::yes, i8, "%src", {{i64, class_field_offset(layout, i)}});
            release_slot(b, field, "%v" + index, slot, drop_callback_for(field, layouts));
        }
        b.ret_void();
        b.end_function();
        return finish();
    }

    if (is_fixed_array(type)) {
        const auto storage = fixed_array_storage(type, array_layout);
        const auto element = storage.leaf;
        const auto stride = runtime_storage_bytes(element);
        if (!owns_managed(element)) {
            b.ret_void();
            b.end_function();
            return finish();
        }
        b.br("cond");
        b.block("cond");
        b.phi("%i", i64, {{0, "ordinary.drop"}, {"%next", "body"}});
        b.icmp("%more", IntPredicate::slt, i64, "%i", storage.count);
        b.br("%more", "body", "done");
        b.block("body");
        b.binary("%off", BinaryOp::mul, i64, "%i", stride);
        b.getelementptr("%slot", Inbounds::yes, i8, "%src", {{i64, "%off"}});
        release_slot(b, element, "%v", "%slot", drop_callback_for(element, layouts));
        b.binary("%next", BinaryOp::add, i64, "%i", 1);
        b.br("cond");
        b.block("done");
        b.ret_void();
        b.end_function();
        return finish();
    }

    const auto element = *type.first;
    const auto stride = runtime_storage_bytes(element);
    b.load("%len", i64, "%src", Align::one);
    if (!owns_managed(element)) {
        b.ret_void();
        b.end_function();
        return finish();
    }
    b.br("cond");
    b.block("cond");
    b.phi("%i", i64, {{0, "ordinary.drop"}, {"%next", "body"}});
    b.icmp("%more", IntPredicate::slt, i64, "%i", "%len");
    b.br("%more", "body", "done");
    b.block("body");
    b.binary("%off0", BinaryOp::mul, i64, "%i", stride);
    b.binary("%off", BinaryOp::add, i64, "%off0", abi::array_layout::payload_offset);
    b.getelementptr("%slot", Inbounds::yes, i8, "%src", {{i64, "%off"}});
    release_slot(b, element, "%v", "%slot", drop_callback_for(element, layouts));
    b.binary("%next", BinaryOp::add, i64, "%i", 1);
    b.br("cond");
    b.block("done");
    b.ret_void();
    b.end_function();
    return finish();
}

void collect_equality_types(const ir::Module& module, std::map<std::string,Type>& types,
                            const std::unordered_map<std::string,ir::ClassLayout>& layouts) {
    for (const auto& function : module.functions) {
        for (const auto& block : function.blocks) {
            for (const auto& instruction : block.instructions) {
                if (const auto* binary = std::get_if<ir::Binary>(&instruction);
                    binary && (binary->op == "==" || binary->op == "!=") &&
                    (binary->operand_type.kind == TypeKind::Union || binary->operand_type.kind == TypeKind::Tuple || binary->operand_type.kind == TypeKind::Bin ||
                     binary->operand_type.kind == TypeKind::Array ||
                     binary->operand_type.kind == TypeKind::Class)) {
                    collect_equality_type(binary->operand_type, types, layouts);
                }
            }
        }
    }
}

std::string emit_equality_helper(
    const Type& type,
    const std::unordered_map<std::string,ir::ClassLayout>& layouts,
    const ArrayLayoutPolicy& array_layout) {
    FunctionText text;
    LlvmBuilder b(text);
    const bool reference_fields=holds_reference_fields(type,layouts);
    const std::string line=reference_fields?"%reference.line":"0",column=reference_fields?"%reference.column":"0";
    const auto signature=[&](LlvmFunction function){if(reference_fields)function.parameter(i64,"",line).parameter(i64,"",column);return function;};
    if(type.kind==TypeKind::Tuple) {
        b.define(signature(LlvmFunction(i1,equality_name(type)).parameter(llvm_type(type),"","%left").parameter(llvm_type(type),"","%right")));b.block("entry");
        for(std::size_t i=0;i<type.parameters.size();++i){auto id=std::to_string(i);auto left="%left."+id,right="%right."+id;b.extractvalue(left,llvm_type(type),"%left",static_cast<unsigned>(i));b.extractvalue(right,llvm_type(type),"%right",static_cast<unsigned>(i));emit_equality_value(b,type.parameters[i],left,right,"element."+id,&layouts,line,column);auto next=i+1==type.parameters.size()?"equal":"next."+id;b.br("%element."+id,next,"different");if(i+1<type.parameters.size())b.block(next);}
        b.block("equal");b.ret({i1,"true"});b.block("different");b.ret({i1,"false"});b.end_function();return text.str();
    }
    b.define(signature(LlvmFunction(i1, equality_name(type)).parameter(ptr, "", "%left").parameter(ptr, "", "%right")));
    b.block("entry");
    b.icmp("%same", IntPredicate::eq, ptr, "%left", "%right");
    if(reference_fields)b.br("null.check");else b.br("%same", "equal", "null.check");
    b.block("null.check");
    b.icmp("%left.null", IntPredicate::eq, ptr, "%left", "null");
    b.icmp("%right.null", IntPredicate::eq, ptr, "%right", "null");
    b.binary("%any.null", BinaryOp::or_, i1, "%left.null", "%right.null");
    b.br("%any.null", "different", "compare");

    if(type.kind==TypeKind::Union) {
        b.block("compare");b.load("%left.tag",i64,"%left",Align::one);b.load("%right.tag",i64,"%right",Align::one);
        b.icmp("%same.tag",IntPredicate::eq,i64,"%left.tag","%right.tag");b.br("%same.tag","payload","different");b.block("payload");
        b.getelementptr("%left.payload",Inbounds::no,i8,"%left",{{i64,abi::union_box_layout::payload_offset}});
        b.getelementptr("%right.payload",Inbounds::no,i8,"%right",{{i64,abi::union_box_layout::payload_offset}});
        std::vector<std::string> labels;
        for(std::size_t i=0;i<type.cases.size();++i)labels.push_back("case."+std::to_string(i));
        std::vector<LlvmCase> cases;
        for(std::size_t i=0;i<type.cases.size();++i)cases.push_back({i,labels[i]});
        b.switch_on({i64,"%left.tag"},"different",cases);
        for(std::size_t i=0;i<type.cases.size();++i){auto id=std::to_string(i);const auto& child=type.cases[i];b.block("case."+id);if(child.kind==TypeKind::None || child.kind==TypeKind::Void){b.br("equal");continue;}b.load("%left."+id,llvm_type(child),"%left.payload",Align::one);b.load("%right."+id,llvm_type(child),"%right.payload",Align::one);emit_equality_value(b,child,"%left."+id,"%right."+id,"case.equal."+id,&layouts,line,column);b.br("%case.equal."+id,"equal","different");}
    } else if (type.kind == TypeKind::Bin) {
        b.block("compare");
        b.call("%bin.equal", runtime_abi::bin::equal, {{ptr, "%left"}, {ptr, "%right"}});
        b.br("%bin.equal", "equal", "different");
    } else if (type.kind == TypeKind::Class) {
        const auto& layout = layouts.at(type.class_name);
        // The initialization mask is not a source field (L13).
        const auto compared = ir::source_field_count(layout);
        b.block("compare");
        if (compared == 0) {
            b.br("equal");
        } else {
            b.br("field.0");
        }
        for (std::size_t i = 0; i < compared; ++i) {
            const auto& field = layout.fields[i];
            const auto offset = class_field_offset(layout, i);
            const auto index = std::to_string(i);
            const auto left_slot = "%left.slot." + index;
            const auto right_slot = "%right.slot." + index;
            const auto left_value = "%left.value." + index;
            const auto right_value = "%right.value." + index;
            b.block("field." + index);
            b.getelementptr(left_slot, Inbounds::yes, i8, "%left", {{i64, offset}});
            b.getelementptr(right_slot, Inbounds::yes, i8, "%right", {{i64, offset}});
            if(layout.reference_fields.contains(i)) {
                const auto la=left_slot+".address",ra=right_slot+".address",ln=left_slot+".null",rn=right_slot+".null",bad=left_slot+".unbound";
                b.load(la,ptr,left_slot,Align::one);b.load(ra,ptr,right_slot,Align::one);
                b.icmp(ln,IntPredicate::eq,ptr,la,"null");b.icmp(rn,IntPredicate::eq,ptr,ra,"null");b.binary(bad,BinaryOp::or_,i1,ln,rn);
                b.br(bad,"unbound."+index,"bound."+index);b.block("unbound."+index);
                b.call(runtime_abi::prelude::fail_at,{{ptr,"@.code.reference.unbound"},{ptr,"@.msg.reference.unbound."+type_identifier(type)+"."+index},{i64,line},{i64,column}});b.unreachable();
                b.block("bound."+index);b.load(left_value,llvm_type(field),la,Align::one);b.load(right_value,llvm_type(field),ra,Align::one);
            } else {
                b.load(left_value, llvm_type(field), left_slot, Align::one);
                b.load(right_value, llvm_type(field), right_slot, Align::one);
            }
            emit_equality_value(b, field, left_value, right_value, "field.equal." + index,&layouts,line,column);
            const auto next = i + 1 < compared ? "field." + std::to_string(i + 1) : "equal";
            b.br("%field.equal." + index, next, "different");
        }
    } else if (is_fixed_array(type)) {
        const auto storage = fixed_array_storage(type, array_layout);
        const auto element = storage.leaf;
        const auto stride = runtime_storage_bytes(element);
        const auto bytes = checked_storage_product(storage.count, stride);
        b.block("compare");
        b.call(runtime_abi::memory::init_require_range, {{ptr, "%left"}, {i64, bytes}, {i64, 0}, {i64, 0}});
        b.call(runtime_abi::memory::init_require_range, {{ptr, "%right"}, {i64, bytes}, {i64, 0}, {i64, 0}});
        b.br("loop.cond");
        b.block("loop.cond");
        b.phi("%index", i64, {{0, "compare"}, {"%next", "loop.next"}});
        b.icmp("%more", IntPredicate::slt, i64, "%index", storage.count);
        b.br("%more", "loop.body", "equal");
        b.block("loop.body");
        b.binary("%offset", BinaryOp::mul, i64, "%index", stride);
        compare_elements_at_offset(b, element,layouts,line,column);
    } else {
        const auto element = *type.first;
        const auto stride = runtime_storage_bytes(element);
        b.block("compare");
        b.load("%left.len", i64, "%left", Align::one);
        b.load("%right.len", i64, "%right", Align::one);
        b.icmp("%length.equal", IntPredicate::eq, i64, "%left.len", "%right.len");
        b.br("%length.equal", "initialized.check", "different");
        b.block("initialized.check");
        b.binary("%tracked.bytes", BinaryOp::mul, i64, "%left.len", stride);
        b.getelementptr("%left.data.init", Inbounds::yes, i8, "%left", {{i64, abi::array_layout::payload_offset}});
        b.getelementptr("%right.data.init", Inbounds::yes, i8, "%right", {{i64, abi::array_layout::payload_offset}});
        b.call(runtime_abi::memory::init_require_range,
               {{ptr, "%left.data.init"}, {i64, "%tracked.bytes"}, {i64, 0}, {i64, 0}});
        b.call(runtime_abi::memory::init_require_range,
               {{ptr, "%right.data.init"}, {i64, "%tracked.bytes"}, {i64, 0}, {i64, 0}});
        b.br("loop.cond");
        b.block("loop.cond");
        b.phi("%index", i64, {{0, "initialized.check"}, {"%next", "loop.next"}});
        b.icmp("%more", IntPredicate::slt, i64, "%index", "%left.len");
        b.br("%more", "loop.body", "equal");
        b.block("loop.body");
        b.binary("%offset.base", BinaryOp::mul, i64, "%index", stride);
        b.binary("%offset", BinaryOp::add, i64, "%offset.base", abi::array_layout::payload_offset);
        compare_elements_at_offset(b, element,layouts,line,column);
    }

    b.block("equal");
    b.ret({i1, "true"});
    b.block("different");
    b.ret({i1, "false"});
    b.end_function();
    return text.str();
}

namespace {

// The opening of a slice helper whose array parameter is `array_value`: the
// plan of the slice item from quidra_slice_plan over the array's length
// (%count indices from %first), and the progression's step %k (the step,
// or +1 or -1 by the direction without one).
void begin_slice_plan(LlvmBuilder& b, const Type& array, std::string_view array_value) {
    b.alloca_slot("%first.slot", i64, Align::none);
    if (is_fixed_array(array)) {
        b.binary("%length", BinaryOp::add, i64, static_cast<long long>(array.length), 0);
    } else {
        b.load("%length", i64, array_value, Align::one);
    }
    b.call("%count", runtime_abi::slice::plan,
           {{i64, "%length"}, {i64, "%start"}, {i32, "%start.form"}, {i64, "%end"},
            {i32, "%end.form"}, {i64, "%step"}, {i32, "%has.step"}, {i32, "%direction"},
            {i64, "%line"}, {i64, "%column"}, {ptr, "%first.slot"}});
    b.load("%first", i64, "%first.slot", Align::none);
    b.icmp("%descending", IntPredicate::eq, i32, "%direction",
           static_cast<int>(SliceDirection::descending));
    b.icmp("%by.bounds", IntPredicate::eq, i32, "%direction",
           static_cast<int>(SliceDirection::by_bounds));
    b.icmp("%bounds.descending", IntPredicate::sgt, i64, "%start", "%end");
    b.binary("%inferred.descending", BinaryOp::and_, i1, "%by.bounds", "%bounds.descending");
    b.binary("%resolved.descending", BinaryOp::or_, i1, "%descending", "%inferred.descending");
    b.select("%unit.step", "%resolved.descending", {i64, -1}, {i64, 1});
    b.icmp("%stepped", IntPredicate::ne, i32, "%has.step", 0);
    b.select("%k", "%stepped", {i64, "%step"}, {i64, "%unit.step"});
}

LlvmFunction slice_helper_signature(LlvmType result, const std::string& name,
                                    std::string_view array_value) {
    LlvmFunction function(result, name);
    function.parameter(ptr, "", std::string(array_value))
        .parameter(i64, "", "%start").parameter(i32, "", "%start.form")
        .parameter(i64, "", "%end").parameter(i32, "", "%end.form")
        .parameter(i64, "", "%step").parameter(i32, "", "%has.step")
        .parameter(i32, "", "%direction");
    return function;
}

} // namespace

// quidra_slice_get_<A>(array, slice item, check, line, column): a new
// runtime-sized array of copies of the selected elements, each checked for
// initialization when `check` is set. A run of step 1 of elements without
// lifetime management is one copy.
std::string emit_slice_get_helper(const Type& array,
                                  const std::unordered_map<std::string,ir::ClassLayout>& layouts,
                                  const ArrayLayoutPolicy& array_layout) {
    const auto element = *array.first;
    const bool fixed = is_fixed_array(array);
    const bool inline_child = array_layout.inline_fixed_child(array);
    const auto source_stride = array_element_stride(array, array_layout);
    const auto result_stride = runtime_storage_bytes(element);
    const bool run = !inline_child && !owns_managed(element);
    FunctionText text;
    LlvmBuilder b(text);
    auto signature = slice_helper_signature(ptr, slice_get_name(array), "%src");
    signature.parameter(i1, "", "%check").parameter(i64, "", "%line").parameter(i64, "", "%column");
    b.define(signature);
    b.block("entry");
    begin_slice_plan(b, array, "%src");
    const auto partial=partial_name("slice",array);partial_begin(b,partial);
    b.call("%dst", runtime_abi::prelude::array_alloc,
           {{i64, "%count"}, {i64, static_cast<long long>(result_stride)}, {i32, 1}});
    b.store({ptr,"%dst"},"%partial.cell",Align::eight);
    b.getelementptr("%src.data", Inbounds::yes, i8, "%src",
                    {{i64, fixed ? 0LL : static_cast<long long>(abi::array_layout::payload_offset)}});
    b.getelementptr("%dst.data", Inbounds::yes, i8, "%dst",
                    {{i64, static_cast<long long>(abi::array_layout::payload_offset)}});
    if (run) {
        b.icmp("%unit", IntPredicate::eq, i64, "%k", 1);
        b.br("%unit", "run", "loop.cond");
        b.block("run");
        b.binary("%run.offset", BinaryOp::mul, i64, "%first", static_cast<long long>(source_stride));
        b.getelementptr("%run.src", Inbounds::yes, i8, "%src.data", {{i64, "%run.offset"}});
        b.binary("%run.bytes", BinaryOp::mul, i64, "%count", static_cast<long long>(result_stride));
        b.br("%check", "run.check", "run.copy");
        b.block("run.check");
        b.call(runtime_abi::memory::init_require_range,
               {{ptr, "%run.src"}, {i64, "%run.bytes"}, {i64, "%line"}, {i64, "%column"}});
        b.br("run.copy");
        b.block("run.copy");
        b.call(runtime_abi::c_library::memcpy, {{ptr, "%dst.data"}, {ptr, "%run.src"}, {i64, "%run.bytes"}});
        partial_finish(b);
    b.ret({ptr, "%dst"});
    } else {
        b.br("loop.cond");
    }
    b.block("loop.cond");
    b.phi("%i", i64, {{0, "entry"}, {"%next", "loop.next"}});
    b.icmp("%more", IntPredicate::slt, i64, "%i", "%count");
    b.br("%more", "loop.body", "done");
    b.block("loop.body");
    b.binary("%distance", BinaryOp::mul, i64, "%i", "%k");
    b.binary("%index", BinaryOp::add, i64, "%first", "%distance");
    b.binary("%src.offset", BinaryOp::mul, i64, "%index", static_cast<long long>(source_stride));
    b.getelementptr("%src.slot", Inbounds::yes, i8, "%src.data", {{i64, "%src.offset"}});
    b.binary("%dst.offset", BinaryOp::mul, i64, "%i", static_cast<long long>(result_stride));
    b.getelementptr("%dst.slot", Inbounds::yes, i8, "%dst.data", {{i64, "%dst.offset"}});
    b.br("%check", "loop.check", "loop.copy");
    b.block("loop.check");
    if (inline_child) {
        // A nested fixed array stored inline: every element of it.
        b.call(runtime_abi::memory::init_require_range,
               {{ptr, "%src.slot"}, {i64, static_cast<long long>(source_stride)}, {i64, "%line"},
                {i64, "%column"}});
    } else {
        b.call(runtime_abi::memory::init_check, {{ptr, "%src.slot"}, {i64, "%line"}, {i64, "%column"}});
    }
    b.br("loop.copy");
    b.block("loop.copy");
    if (inline_child) {
        // The element is the inline storage itself; its copy is a new array.
        store_copy(b, element, "%src.slot", "%copy", "%dst.slot");
    } else {
        b.load("%value", llvm_type(element), "%src.slot", Align::one);
        store_copy(b, element, "%value", "%copy", "%dst.slot");
    }
    b.br("loop.next");
    b.block("loop.next");
    b.binary("%next", BinaryOp::add, i64, "%i", 1);
    b.store({i64,"%next"},"%partial.count",Align::eight);
    b.br("loop.cond");
    b.block("done");
    partial_finish(b);
    b.ret({ptr, "%dst"});
    b.end_function();
    return partial_container_drop(array,partial,layouts,array_layout,true)+text.str();
}

// quidra_slice_set_<A>(array, slice item, value, line, column): stores a copy
// of each element of `value` (a runtime-sized array of exactly as many
// initialized elements as the slice selects) into the selected elements,
// releasing the values they replace, and marks them initialized. `value` is
// an independent array, so it never overlaps the target.
std::string emit_slice_set_helper(const Type& array,
                                  const std::unordered_map<std::string,ir::ClassLayout>& layouts,
                                  const ArrayLayoutPolicy& array_layout) {
    const auto element = *array.first;
    const bool fixed = is_fixed_array(array);
    const bool inline_child = array_layout.inline_fixed_child(array);
    const auto target_stride = array_element_stride(array, array_layout);
    const auto value_stride = runtime_storage_bytes(element);
    const bool run = !inline_child && !owns_managed(element) &&
                     target_stride == value_stride;
    const auto inline_owner=partial_name("slice.set.inline",array);
    FunctionText text;
    LlvmBuilder b(text);
    auto signature = slice_helper_signature(void_type, slice_set_name(array), "%dst");
    signature.parameter(ptr, "", "%value").parameter(i64, "", "%line").parameter(i64, "", "%column");
    b.define(signature);
    b.block("entry");
    begin_slice_plan(b, array, "%dst");
    b.load("%given", i64, "%value", Align::one);
    b.call(runtime_abi::slice::assignment_check,
           {{i64, "%count"}, {i64, "%given"}, {i64, "%line"}, {i64, "%column"}});
    b.getelementptr("%value.data", Inbounds::yes, i8, "%value",
                    {{i64, static_cast<long long>(abi::array_layout::payload_offset)}});
    b.binary("%value.bytes", BinaryOp::mul, i64, "%given", static_cast<long long>(value_stride));
    b.call(runtime_abi::memory::init_require_range,
           {{ptr, "%value.data"}, {i64, "%value.bytes"}, {i64, "%line"}, {i64, "%column"}});
    b.getelementptr("%dst.data", Inbounds::yes, i8, "%dst",
                    {{i64, fixed ? 0LL : static_cast<long long>(abi::array_layout::payload_offset)}});
    if(inline_child) {
        // Reserve stable cleanup storage before cloning. Ordinary calls never
        // allocate a Node frame or publish a temporary owner into one.
        b.call("%slice.node.active",runtime_abi::program::parallel_cleanup_active,{});
        b.icmp("%slice.has.items",IntPredicate::ugt,i64,"%count",0);
        b.binary("%slice.reserve",BinaryOp::and_,i1,"%slice.node.active","%slice.has.items");
        b.br("%slice.reserve","slice.inline.prepare","slice.inline.ready");
        b.block("slice.inline.prepare");partial_begin(b,inline_owner);
        b.br("slice.inline.ready");b.block("slice.inline.ready");
        b.phi("%slice.inline.cell",ptr,{{"%partial.cell","slice.inline.prepare"},{"null","entry"}});
    }
    if (run) {
        b.icmp("%unit", IntPredicate::eq, i64, "%k", 1);
        b.br("%unit", "run", "loop.cond");
        b.block("run");
        b.binary("%run.offset", BinaryOp::mul, i64, "%first", static_cast<long long>(target_stride));
        b.getelementptr("%run.dst", Inbounds::yes, i8, "%dst.data", {{i64, "%run.offset"}});
        b.call(runtime_abi::c_library::memcpy, {{ptr, "%run.dst"}, {ptr, "%value.data"}, {i64, "%value.bytes"}});
        b.call(runtime_abi::memory::init_mark_range, {{ptr, "%run.dst"}, {i64, "%value.bytes"}});
        b.ret_void();
    } else {
        b.br("loop.cond");
    }
    b.block("loop.cond");
    b.phi("%i", i64, {{0, inline_child?"slice.inline.ready":"entry"},
                      {"%next", inline_child||owns_managed(element)?"loop.next":"loop.body"}});
    b.icmp("%more", IntPredicate::slt, i64, "%i", "%count");
    b.br("%more", "loop.body", "done");
    b.block("loop.body");
    b.binary("%distance", BinaryOp::mul, i64, "%i", "%k");
    b.binary("%index", BinaryOp::add, i64, "%first", "%distance");
    b.binary("%dst.offset", BinaryOp::mul, i64, "%index", static_cast<long long>(target_stride));
    b.getelementptr("%dst.slot", Inbounds::yes, i8, "%dst.data", {{i64, "%dst.offset"}});
    b.binary("%value.offset", BinaryOp::mul, i64, "%i", static_cast<long long>(value_stride));
    b.getelementptr("%value.slot", Inbounds::yes, i8, "%value.data", {{i64, "%value.offset"}});
    b.load("%element", llvm_type(element), "%value.slot", Align::one);
    if (inline_child) {
        // As ArraySet: a new copy of the nested array replaces the inline
        // storage, whose own values are released first.
        b.br("%slice.node.active","slice.inline.node","slice.inline.ordinary");
        b.block("slice.inline.node");
        b.call("%copy.node",ptr,clone_name(element),{{ptr,"%element"}});
        b.store({ptr,"%copy.node"},"%slice.inline.cell",Align::eight);
        b.call(void_type,drop_name(element),{{ptr,"%dst.slot"}});
        b.call(runtime_abi::program::parallel_cleanup_rethrow,{});
        b.call(runtime_abi::c_library::memcpy,
               {{ptr,"%dst.slot"},{ptr,"%copy.node"},{i64,static_cast<long long>(target_stride)}});
        // The destination now owns all copied children; only the temporary
        // allocation backing remains, whose release must not drop those leaves.
        b.store({ptr,"null"},"%slice.inline.cell",Align::eight);
        b.call(runtime_abi::memory::managed_release,{{ptr,"%copy.node"},{ptr,"null"}});
        b.br("loop.next");b.block("slice.inline.ordinary");
        b.call("%copy", ptr, clone_name(element), {{ptr, "%element"}});
        b.call(void_type, drop_name(element), {{ptr, "%dst.slot"}});
        b.call(runtime_abi::c_library::memcpy,
               {{ptr, "%dst.slot"}, {ptr, "%copy"}, {i64, static_cast<long long>(target_stride)}});
        b.call(runtime_abi::memory::managed_release, {{ptr, "%copy"}, {ptr, "null"}});
        b.br("loop.next");b.block("loop.next");
    } else {
        if (owns_managed(element)) {
            release_slot(b, element, "%old", "%dst.slot", drop_callback_for(element, layouts),true);
            b.br("loop.next");b.block("loop.next");
        }
        store_copy(b, element, "%element", "%copy", "%dst.slot");
    }
    b.call(runtime_abi::memory::init_mark_range,
           {{ptr, "%dst.slot"}, {i64, static_cast<long long>(target_stride)}});
    b.binary("%next", BinaryOp::add, i64, "%i", 1);
    b.br("loop.cond");
    b.block("done");
    if(inline_child) {
        b.br("%slice.node.active","slice.inline.leave","slice.inline.return");
        b.block("slice.inline.leave");
        b.call(runtime_abi::program::parallel_frame_leave,{{ptr,"%slice.inline.cell"}});
        b.br("slice.inline.return");b.block("slice.inline.return");
    }
    b.ret_void();
    b.end_function();
    return (inline_child?partial_leaf_drop(element,inline_owner,layouts):std::string{})+text.str();
}

void TypeHelperSet::collect_array_casts_and_clones() {
    array_cast_pairs_ = collect_array_cast_pairs(module_);
    collect_clone_types(module_, clone_types_, layouts_);
    for(const auto& function:module_.functions)for(const auto& block:function.blocks)for(const auto& ins:block.instructions)std::visit([&](const auto& n){
        if constexpr(requires{n.type;})if(n.type.kind==TypeKind::Tuple)collect_clone_type(n.type,clone_types_,layouts_);
        if constexpr(requires{n.element_type;})if(n.element_type.kind==TypeKind::Tuple)collect_clone_type(n.element_type,clone_types_,layouts_);
        if constexpr(requires{n.field_type;})if(n.field_type.kind==TypeKind::Tuple)collect_clone_type(n.field_type,clone_types_,layouts_);
        if constexpr(requires{n.payload_type;})if(n.payload_type.kind==TypeKind::Tuple)collect_clone_type(n.payload_type,clone_types_,layouts_);
    },ins);

    // The slice helpers copy each element as a store does.
    for (const auto& function : module_.functions)
        for (const auto& block : function.blocks)
            for (const auto& instruction : block.instructions) {
                const Type* array = nullptr;
                if (const auto* slice = std::get_if<ir::ArraySlice>(&instruction)) {
                    array = &slice->array_type;
                    slice_get_types_.emplace(type_identifier(*array), *array);
                } else if (const auto* set = std::get_if<ir::ArraySliceSet>(&instruction)) {
                    array = &set->array_type;
                    slice_set_types_.emplace(type_identifier(*array), *array);
                }
                if (array) collect_clone_type(*array->first, clone_types_, layouts_);
            }
}

std::string stored_targets_name(const Type& type) {
    return helper_name("quidra_stored_targets_",type);
}

namespace {
void collect_stored_target_type(const Type& type,std::map<std::string,Type>& types,
                               const std::unordered_map<std::string,ir::ClassLayout>& layouts) {
    if (!holds_reference_fields(type,layouts)) return;
    if (!types.emplace(type_identifier(type),type).second) return;
    if (type.kind==TypeKind::Tuple) for(const auto& child:type.parameters)collect_stored_target_type(child,types,layouts);
    else if (type.kind==TypeKind::Union) for(const auto& child:type.cases)collect_stored_target_type(child,types,layouts);
    else if (type.kind==TypeKind::Array) collect_stored_target_type(*type.first,types,layouts);
    else if (type.kind==TypeKind::Class) {
        const auto& layout=layouts.at(type.class_name);
        for(std::size_t i=0;i<ir::source_field_count(layout);++i)
            if(!layout.reference_fields.contains(i))collect_stored_target_type(layout.fields[i],types,layouts);
    }
}

std::string emit_stored_targets_helper(const Type& type,
    const std::unordered_map<std::string,ir::ClassLayout>& layouts,const ArrayLayoutPolicy& array_layout) {
    FunctionText text;LlvmBuilder b(text);
    b.define(LlvmFunction{i1,stored_targets_name(type)}.parameter(llvm_type(type),"","%src").parameter(i64,"","%root"));
    b.block("entry");b.icmp("%zero",IntPredicate::eq,i64,"%root",0);
    if(type.kind==TypeKind::Tuple)b.br("%zero","safe","body");
    else {b.icmp("%null",IntPredicate::eq,ptr,"%src","null");b.binary("%skip",BinaryOp::or_,i1,"%zero","%null");b.br("%skip","safe","body");}
    b.block("body");
    const auto child_check=[&](const Type& child,const std::string& value,const std::string& name,const std::string& next) {
        const auto helper=stored_targets_name(child);
        b.call(LlvmCall{i1,helper}.result(name).argument({llvm_type(child),value}).argument({i64,"%root"}));
        b.br(name,"bad",next);
    };
    if(type.kind==TypeKind::Tuple) {
        std::vector<std::size_t> children;for(std::size_t i=0;i<type.parameters.size();++i)if(holds_reference_fields(type.parameters[i],layouts))children.push_back(i);
        for(std::size_t n=0;n<children.size();++n) {const auto i=children[n];const auto id=std::to_string(i);if(n)b.block("tuple."+id);b.extractvalue("%child."+id,llvm_type(type),"%src",i);child_check(type.parameters[i],"%child."+id,"%bad."+id,n+1==children.size()?"safe":"tuple."+std::to_string(children[n+1]));}
        if(children.empty())b.br("safe");
    } else if(type.kind==TypeKind::Class) {
        const auto& layout=layouts.at(type.class_name);
        std::vector<std::size_t> children;for(std::size_t i=0;i<ir::source_field_count(layout);++i)if(layout.reference_fields.contains(i)||holds_reference_fields(layout.fields[i],layouts))children.push_back(i);
        for(std::size_t n=0;n<children.size();++n) {
            const auto i=children[n];const auto id=std::to_string(i);const auto next=n+1==children.size()?"safe":"field."+std::to_string(children[n+1]);if(n)b.block("field."+id);
            b.getelementptr("%slot."+id,Inbounds::yes,i8,"%src",{{i64,class_field_offset(layout,i)}});
            if(layout.reference_fields.contains(i)) {
                b.getelementptr("%token.slot."+id,Inbounds::yes,i8,"%slot."+id,{{i64,8}});b.load("%token."+id,i64,"%token.slot."+id,Align::one);
                b.icmp("%same."+id,IntPredicate::eq,i64,"%token."+id,"%root");b.br("%same."+id,"bad",next);
            } else {b.load("%child."+id,llvm_type(layout.fields[i]),"%slot."+id,Align::one);child_check(layout.fields[i],"%child."+id,"%bad."+id,next);}
        }
        if(children.empty())b.br("safe");
    } else if(type.kind==TypeKind::Union) {
        b.load("%tag",i64,"%src",Align::one);b.getelementptr("%payload",Inbounds::yes,i8,"%src",{{i64,abi::union_box_layout::payload_offset}});
        std::vector<std::string> labels;
        for(std::size_t i=0;i<type.cases.size();++i)labels.push_back("case."+std::to_string(i));
        std::vector<LlvmCase> cases;
        for(std::size_t i=0;i<type.cases.size();++i)if(holds_reference_fields(type.cases[i],layouts))cases.push_back({i,labels[i]});
        b.switch_on({i64,"%tag"},"safe",cases);
        for(std::size_t i=0;i<type.cases.size();++i)if(holds_reference_fields(type.cases[i],layouts)) {const auto id=std::to_string(i);b.block("case."+id);b.load("%child."+id,llvm_type(type.cases[i]),"%payload",Align::one);child_check(type.cases[i],"%child."+id,"%bad."+id,"safe");}
    } else if(type.kind==TypeKind::Array) {
        if(is_fixed_array(type))b.binary("%len",BinaryOp::add,i64,0,type.length);else b.load("%len",i64,"%src",Align::one);
        b.br("loop");b.block("loop");b.phi("%index",i64,{{0,"body"},{"%next","advance"}});b.icmp("%more",IntPredicate::ult,i64,"%index","%len");b.br("%more","element","safe");b.block("element");
        b.binary("%offset",BinaryOp::mul,i64,"%index",array_element_stride(type,array_layout));
        if(!is_fixed_array(type))b.binary("%data.offset",BinaryOp::add,i64,"%offset",abi::array_layout::payload_offset);
        b.getelementptr("%slot",Inbounds::yes,i8,"%src",{{i64,is_fixed_array(type)?"%offset":"%data.offset"}});
        const bool inline_child=is_fixed_array(type)&&array_layout.inline_fixed_child(type);
        if(!inline_child)b.load("%child",llvm_type(*type.first),"%slot",Align::one);
        child_check(*type.first,inline_child?"%slot":"%child","%child.bad","advance");b.block("advance");b.binary("%next",BinaryOp::add,i64,"%index",1);b.br("loop");
    } else b.br("safe");
    b.block("safe");b.ret({i1,"false"});b.block("bad");b.ret({i1,"true"});b.end_function();return text.str();
}
}

void TypeHelperSet::emit(LlvmModule& module) {
    constexpr auto helpers = LlvmModule::Section::helpers;
    for (const auto& [_, pair] : array_cast_pairs_) {
        module.append(helpers, emit_array_cast_validator(pair.source,pair.target,array_layout_));
        module.append(helpers, emit_array_cast_helper(pair.source,pair.target,array_layout_));
    }
    for (const auto& [_, type] : clone_types_)
        module.append(helpers, emit_clone_helper(type, layouts_, array_layout_));
    // The drop types are collected only after the array cast and clone helpers
    // are written, and the equality types after the drop helpers: each of these
    // steps can throw, and which exception comes first must not change.
    collect_drop_types(module_, drop_types_, layouts_);
    for (const auto& [_, type] : drop_types_)
        module.append(helpers, emit_drop_helper(type, layouts_, array_layout_));
    collect_equality_types(module_, equality_types_, layouts_);
    for (const auto& [_, type] : equality_types_)
        module.append(helpers, emit_equality_helper(type, layouts_, array_layout_));
    for (const auto& [_, type] : slice_get_types_)
        module.append(helpers, emit_slice_get_helper(type, layouts_, array_layout_));
    for (const auto& [_, type] : slice_set_types_)
        module.append(helpers, emit_slice_set_helper(type, layouts_, array_layout_));
    std::map<std::string,Type> stored_types;
    for(const auto& function:module_.functions)for(const auto& block:function.blocks)for(const auto& instruction:block.instructions)
        if(const auto* check=std::get_if<ir::StoredTargetsCheck>(&instruction))collect_stored_target_type(check->type,stored_types,layouts_);
    for(const auto& [_,type]:stored_types)module.append(helpers,emit_stored_targets_helper(type,layouts_,array_layout_));
}

} // namespace quidra::llvm_backend
