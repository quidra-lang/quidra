#include "llvm_backend/type_helpers.hpp"

#include "llvm_backend/runtime_abi.hpp"
#include "llvm_backend/type_lowering.hpp"
#include "llvm_text/function_text.hpp"
#include "llvm_text/llvm_builder.hpp"
#include "quidra/abi/layout.hpp"
#include "quidra/ir/initialization_mask.hpp"
#include "quidra/standard_classes.hpp"

#include <stdexcept>
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
std::string equality_name(const Type&t){return helper_name(runtime_abi::generated_helper_prefix::equal,t);}

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
 if(!requires_value_clone(t))return;
 const auto id=type_identifier(t);if(types.contains(id))return;
 types[id]=t;
 if(t.kind==TypeKind::Array){collect_clone_type(*t.first,types,layouts);}
 else if(t.kind==TypeKind::Union){for(const auto&c:t.cases)collect_clone_type(c,types,layouts);}
 else if(t.kind==TypeKind::Class){for(const auto&f:layouts.at(t.class_name).fields)collect_clone_type(f,types,layouts);}
}

void collect_drop_type(const Type& type, std::map<std::string,Type>& types,
                       const std::unordered_map<std::string,ir::ClassLayout>& layouts) {
    if (!requires_lifetime_management(type)) return;
    if (is_bare_integer(type) || type.kind == TypeKind::Real ||
        type.kind == TypeKind::String || type.kind == TypeKind::Error ||
        (type.kind == TypeKind::Class &&
         standard_class::is_runtime_handle(type.class_name))) return;
    if (type.kind == TypeKind::Class && !layouts.contains(type.class_name)) return;
    const auto id = type_identifier(type);
    if (types.contains(id)) return;
    types[id] = type;
    if (type.kind == TypeKind::Array) collect_drop_type(*type.first, types, layouts);
    else if (type.kind == TypeKind::Union)
        for (const auto& current : type.cases) collect_drop_type(current, types, layouts);
    else if (type.kind == TypeKind::Class)
        for (const auto& field : layouts.at(type.class_name).fields)
            collect_drop_type(field, types, layouts);
}

} // namespace

std::string drop_callback_for(const Type& type,
                              const std::unordered_map<std::string,ir::ClassLayout>& layouts) {
    if (is_bare_integer(type)) return runtime_abi::global_name(runtime_abi::exact::bigint_drop);
    if (type.kind == TypeKind::Real) return runtime_abi::global_name(runtime_abi::exact::bigreal_drop);
    if (type.kind == TypeKind::Class) {
        if (const auto* handle = runtime_abi::runtime_handle_class(type.class_name))
            return runtime_abi::global_name(*handle->drop);
    }
    if (type.kind == TypeKind::Bin || type.kind == TypeKind::Array ||
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
    if (t.kind != TypeKind::Bin && t.kind != TypeKind::Array && t.kind != TypeKind::Class) return;
    const auto id = type_identifier(t);
    if (types.contains(id)) return;
    types[id] = t;
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
                         const std::string& right, const std::string& id) {
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
    } else if (type.kind == TypeKind::Bool) {
        b.icmp(result, IntPredicate::eq, i1, left, right);
    } else if (is_fixed_real(type)) {
        b.fcmp(result, FloatPredicate::oeq, llvm_type(type), left, right);
    } else if (type.kind == TypeKind::String || type.kind == TypeKind::Error) {
        b.call(result, runtime_abi::text::equal, {{ptr, left}, {ptr, right}});
    } else if (type.kind == TypeKind::Bin || type.kind == TypeKind::Array || type.kind == TypeKind::Class) {
        b.call(result, i1, equality_name(type), {{ptr, left}, {ptr, right}});
    }
}

// Stores a copy of `value`, of `type`, into `slot`: a deep copy through the
// type's clone helper (named `copy`), the value itself retained when its
// storage is shared, or the value itself.
void store_copy(LlvmBuilder& b, const Type& type, std::string_view value, std::string_view copy,
                std::string_view slot) {
    if (requires_value_clone(type)) {
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
                  std::string_view callback) {
    if (type.kind == TypeKind::Real) {
        b.load(value, exact_real_type, slot, Align::one);
        b.call(void_type, LlvmOperand::global(real_helper::release), {{exact_real_type, value}});
        return;
    }
    if (is_bare_integer(type)) {
        b.load(value, bare_integer_type, slot, Align::one);
        b.call(void_type, LlvmOperand::global(int_helper::release), {{bare_integer_type, value}});
        return;
    }
    b.load(value, ptr, slot, Align::one);
    b.call(runtime_abi::memory::managed_release, {{ptr, value}, {ptr, callback}});
}

// The body of an element loop of an equality helper, from the byte offset
// %offset of the element in both arrays: compares the two elements and
// continues at loop.next, or returns through different.
void compare_elements_at_offset(LlvmBuilder& b, const Type& element) {
    b.getelementptr("%left.slot", Inbounds::yes, i8, "%left", {{i64, "%offset"}});
    b.getelementptr("%right.slot", Inbounds::yes, i8, "%right", {{i64, "%offset"}});
    b.load("%left.value", llvm_type(element), "%left.slot", Align::one);
    b.load("%right.value", llvm_type(element), "%right.slot", Align::one);
    emit_equality_value(b, element, "%left.value", "%right.value", "element.equal");
    b.br("%element.equal", "loop.next", "different");
    b.block("loop.next");
    b.binary("%next", BinaryOp::add, i64, "%index", 1);
    b.br("loop.cond");
}

} // namespace

void collect_clone_types(const ir::Module&m,std::map<std::string,Type>&types,const std::unordered_map<std::string,ir::ClassLayout>&layouts){for(const auto&f:m.functions)for(const auto&b:f.blocks)for(const auto&i:b.instructions)if(const auto*c=std::get_if<ir::Clone>(&i))collect_clone_type(c->type,types,layouts);}

std::string emit_clone_helper(const Type&t,const std::unordered_map<std::string,ir::ClassLayout>&layouts,const ArrayLayoutPolicy&array_layout){
 if(t.kind==TypeKind::Tensor) return runtime_clone_helper(t,runtime_abi::tensor::clone);
 const auto layout=t.kind==TypeKind::Class?layouts.find(t.class_name):layouts.end();
 if(layout!=layouts.end() &&
    standard_class::is_ref_cell_instance(t.class_name,layout->second.standard_library)){
    FunctionText text;
    LlvmBuilder b(text);
    b.define(LlvmFunction(ptr, clone_name(t)).parameter(ptr, "", "%src"));
    b.block("entry");
    b.call(runtime_abi::memory::managed_retain, {{ptr, "%src"}});
    b.ret({ptr, "%src"});
    b.end_function();
    return text.str();
 }
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
    b.call("%dst", runtime_abi::prelude::alloc, {{i64, union_box_bytes(t)}});
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
        }
        b.br("done");
    }
    b.block("done");
    b.ret({ptr, "%dst"});
    b.end_function();
    return text.str();
 }
 if(t.kind==TypeKind::Class){
    const auto&layout=layouts.at(t.class_name);
    FunctionText text;
    LlvmBuilder b(text);
    const auto bytes=class_storage_bytes(layout);
    b.define(LlvmFunction(ptr, clone_name(t)).parameter(ptr, "", "%src"));
    return_null_for_null_source(b, "clone.body");
    b.call("%dst", runtime_abi::prelude::alloc, {{i64, bytes}});
    for(std::size_t i=0;i<layout.fields.size();++i){
        const auto&field=layout.fields[i];
        const auto offset=class_field_offset(layout,i);
        const auto index=std::to_string(i);
        const auto source="%sp"+index;
        const auto destination="%dp"+index;
        const auto value="%v"+index;
        b.getelementptr(source, Inbounds::yes, i8, "%src", {{i64, offset}});
        b.getelementptr(destination, Inbounds::yes, i8, "%dst", {{i64, offset}});
        b.load(value, llvm_type(field), source, Align::one);
        store_copy(b, field, value, "%copy"+index, destination);
    }
    b.ret({ptr, "%dst"});
    b.end_function();
    return text.str();
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
    b.call("%dst", runtime_abi::prelude::alloc, {{i64, bytes}});
    b.call(runtime_abi::memory::init_clone, {{ptr, "%dst"}, {ptr, "%src"}, {i64, storage.count}, {i64, stride}, {i64, 0}});
    b.br("cond");
    b.block("cond");
    b.phi("%i", i64, {{0, "clone.body"}, {"%next", "body"}});
    b.icmp("%more", IntPredicate::slt, i64, "%i", storage.count);
    b.br("%more", "body", "done");
    b.block("body");
    b.binary("%off", BinaryOp::mul, i64, "%i", stride);
    b.getelementptr("%sp", Inbounds::yes, i8, "%src", {{i64, "%off"}});
    b.getelementptr("%dp", Inbounds::yes, i8, "%dst", {{i64, "%off"}});
    b.load("%v", llvm_type(elem), "%sp", Align::one);
    store_copy(b, elem, "%v", "%copy", "%dp");
    b.binary("%next", BinaryOp::add, i64, "%i", 1);
    b.br("cond");
    b.block("done");
    b.ret({ptr, "%dst"});
    b.end_function();
    return text.str();
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
 b.call("%dst", runtime_abi::prelude::alloc, {{i64, "%bytes"}});
 b.store({i64, "%len"}, "%dst", Align::one);
 b.getelementptr("%src.data", Inbounds::yes, i8, "%src", {{i64, abi::array_layout::payload_offset}});
 b.call(runtime_abi::memory::init_clone, {{ptr, "%dst"}, {ptr, "%src.data"}, {i64, "%len"}, {i64, stride}, {i64, abi::array_layout::payload_offset}});
 b.br("cond");
 b.block("cond");
 b.phi("%i", i64, {{0, "clone.body"}, {"%next", "body"}});
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
 b.br("cond");
 b.block("done");
 b.ret({ptr, "%dst"});
 b.end_function();
 return text.str();
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
                    if constexpr (std::is_same_v<T,ir::DeclareLocal> ||
                                  std::is_same_v<T,ir::StoreLocal> ||
                                  std::is_same_v<T,ir::Clone> ||
                                  std::is_same_v<T,ir::Retain> ||
                                  std::is_same_v<T,ir::Release>) {
                        collect_drop_type(current.type, types, layouts);
                    } if constexpr (std::is_same_v<T,ir::FieldSet>) {
                        collect_drop_type(current.field_type, types, layouts);
                    } if constexpr (std::is_same_v<T,ir::ArraySet>) {
                        collect_drop_type(current.element_type, types, layouts);
                    }
                }, instruction);
            }
        }
    }
}

std::string emit_drop_helper(const Type& type,
                             const std::unordered_map<std::string,ir::ClassLayout>& layouts,
                             const ArrayLayoutPolicy& array_layout) {
    FunctionText text;
    LlvmBuilder b(text);
    b.define(LlvmFunction(void_type, drop_name(type)).parameter(ptr, "", "%src"));
    b.block("entry");

    if (type.kind == TypeKind::Bin) {
        b.ret_void();
        b.end_function();
        return text.str();
    }

    if (type.kind == TypeKind::Tensor) {
        b.call(runtime_abi::tensor::drop, {{ptr, "%src"}});
        b.ret_void();
        b.end_function();
        return text.str();
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
            if (requires_lifetime_management(current))
                release_slot(b, current, "%v" + std::to_string(i), "%payload", drop_callback_for(current, layouts));
            b.br("done");
        }
        b.block("done");
        b.ret_void();
        b.end_function();
        return text.str();
    }

    if (type.kind == TypeKind::Class) {
        const auto& layout = layouts.at(type.class_name);
        for (std::size_t i = 0; i < layout.fields.size(); ++i) {
            const auto& field = layout.fields[i];
            if (!requires_lifetime_management(field)) continue;
            const auto index = std::to_string(i);
            const auto slot = "%slot" + index;
            b.getelementptr(slot, Inbounds::yes, i8, "%src", {{i64, class_field_offset(layout, i)}});
            release_slot(b, field, "%v" + index, slot, drop_callback_for(field, layouts));
        }
        b.ret_void();
        b.end_function();
        return text.str();
    }

    if (is_fixed_array(type)) {
        const auto storage = fixed_array_storage(type, array_layout);
        const auto element = storage.leaf;
        const auto stride = runtime_storage_bytes(element);
        if (!requires_lifetime_management(element)) {
            b.ret_void();
            b.end_function();
            return text.str();
        }
        b.br("cond");
        b.block("cond");
        b.phi("%i", i64, {{0, "entry"}, {"%next", "body"}});
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
        return text.str();
    }

    const auto element = *type.first;
    const auto stride = runtime_storage_bytes(element);
    b.load("%len", i64, "%src", Align::one);
    if (!requires_lifetime_management(element)) {
        b.ret_void();
        b.end_function();
        return text.str();
    }
    b.br("cond");
    b.block("cond");
    b.phi("%i", i64, {{0, "entry"}, {"%next", "body"}});
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
    return text.str();
}

void collect_equality_types(const ir::Module& module, std::map<std::string,Type>& types,
                            const std::unordered_map<std::string,ir::ClassLayout>& layouts) {
    for (const auto& function : module.functions) {
        for (const auto& block : function.blocks) {
            for (const auto& instruction : block.instructions) {
                if (const auto* binary = std::get_if<ir::Binary>(&instruction);
                    binary && (binary->op == "==" || binary->op == "!=") &&
                    (binary->operand_type.kind == TypeKind::Bin ||
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
    b.define(LlvmFunction(i1, equality_name(type)).parameter(ptr, "", "%left").parameter(ptr, "", "%right"));
    b.block("entry");
    b.icmp("%same", IntPredicate::eq, ptr, "%left", "%right");
    b.br("%same", "equal", "null.check");
    b.block("null.check");
    b.icmp("%left.null", IntPredicate::eq, ptr, "%left", "null");
    b.icmp("%right.null", IntPredicate::eq, ptr, "%right", "null");
    b.binary("%any.null", BinaryOp::or_, i1, "%left.null", "%right.null");
    b.br("%any.null", "different", "compare");

    if (type.kind == TypeKind::Bin) {
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
            b.load(left_value, llvm_type(field), left_slot, Align::one);
            b.load(right_value, llvm_type(field), right_slot, Align::one);
            emit_equality_value(b, field, left_value, right_value, "field.equal." + index);
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
        compare_elements_at_offset(b, element);
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
        compare_elements_at_offset(b, element);
    }

    b.block("equal");
    b.ret({i1, "true"});
    b.block("different");
    b.ret({i1, "false"});
    b.end_function();
    return text.str();
}

void TypeHelperSet::collect_array_casts_and_clones() {
    array_cast_pairs_ = collect_array_cast_pairs(module_);
    collect_clone_types(module_, clone_types_, layouts_);
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
}

} // namespace quidra::llvm_backend
