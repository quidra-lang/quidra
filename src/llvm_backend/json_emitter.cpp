#include "llvm_backend/json_emitter.hpp"

#include "llvm_backend/runtime_abi.hpp"
#include "llvm_backend/type_lowering.hpp"
#include "quidra/standard_classes.hpp"

#include <string>
#include <type_traits>

namespace quidra::llvm_backend {

using namespace llvm_text;
using namespace llvm_text::types;

void JsonEmitter::emit(const ir::JsonParse& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.result_type);
    const auto raw=names_.value("json.parse.raw"),ok=names_.value("json.parse.ok"),result=symbols_.value(n.out);
    builder_.call(raw,runtime_abi::json::parse_raw,{{ptr,symbols_.value(n.text)}});
    union_box_.allocate(result,n.result_type);
    builder_.icmp(ok,IntPredicate::ne,ptr,raw,"null");
    const auto yes=names_.label("json.parse.ok"),bad=names_.label("json.parse.error"),done=names_.label("json.parse.done");
    builder_.br(ok,yes,bad);
    builder_.block(yes);
    union_box_.store_tag(result,case_index(n.result_type,Type::class_type(standard_class::json_value)));
    const auto good_payload=names_.value("json.parse.value");
    union_box_.payload_slot(good_payload,result);
    builder_.store({ptr,raw},good_payload,Align::none);
    builder_.br(done);
    builder_.block(bad);
    union_box_.store_tag(result,case_index(n.result_type,Type::simple(TypeKind::Error)));
    const auto message=names_.value("json.parse.message"),error_payload=names_.value("json.parse.error.payload");
    builder_.call(message,runtime_abi::json::last_error_copy,{});
    union_box_.payload_slot(error_payload,result);
    builder_.store({ptr,message},error_payload,Align::none);
    builder_.br(done);
    builder_.block(done);
}

void JsonEmitter::emit(const ir::JsonKind& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::simple(TypeKind::String));
    builder_.call(symbols_.value(n.out),runtime_abi::json::kind,{{ptr,symbols_.value(n.value)}});
}

void JsonEmitter::emit(const ir::JsonSize& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.result_type);
    const auto raw=names_.value("json.size.raw"),ok=names_.value("json.size.ok"),result=symbols_.value(n.out);
    builder_.call(raw,runtime_abi::json::size,{{ptr,symbols_.value(n.value)}});
    union_box_.allocate(result,n.result_type);
    builder_.icmp(ok,IntPredicate::sge,i64,raw,0);
    const auto yes=names_.label("json.size.ok"),bad=names_.label("json.size.error"),done=names_.label("json.size.done");
    builder_.br(ok,yes,bad);
    builder_.block(yes);
    // The size is a value of the union's integer case: the word of a bare
    // integer (a size is inline) or the raw int64.
    Type size_type=Type::simple(TypeKind::Int64);
    for(const auto& current:n.result_type.cases)
        if(current.kind!=TypeKind::Error) size_type=current;
    union_box_.store_tag(result,case_index(n.result_type,size_type));
    const auto good_payload=names_.value("json.size.value");
    union_box_.payload_slot(good_payload,result);
    std::string size=raw;
    if(is_bare_integer(size_type)){
        size=names_.value("json.size.word");
        builder_.binary(size,BinaryOp::shl,NoWrap::nsw,i64,raw,1);
    }
    builder_.store({i64,size},good_payload,Align::none);
    builder_.br(done);
    builder_.block(bad);
    union_box_.store_tag(result,case_index(n.result_type,Type::simple(TypeKind::Error)));
    const auto error_payload=names_.value("json.size.error.payload");
    union_box_.payload_slot(error_payload,result);
    builder_.store({ptr,"@.err.json.type"},error_payload,Align::none);
    builder_.br(done);
    builder_.block(done);
}

// A lookup in a JSON container (JsonLookup): a member of an object by its
// name, or an element of an array by its index. The runtime first tests the
// value's kind (kind_test), then looks the key up (function; null when
// absent); the container word and the prefix name the temporaries and
// labels.
struct JsonLookup {
    const char* container;
    const LlvmCallee* kind_test;
    const LlvmCallee* function;
    LlvmType key_type;
    const char* prefix;
};

namespace {

constexpr JsonLookup json_member{"object",&runtime_abi::json::is_object,&runtime_abi::json::get,ptr,"json.get"};
constexpr JsonLookup json_element{"array",&runtime_abi::json::is_array,&runtime_abi::json::at,i64,"json.at"};

} // namespace

void JsonEmitter::emit_lookup(const JsonLookup& lookup,ir::ValueId result_value,ir::ValueId json,ir::ValueId key,const Type& result_type){
    symbols_.set_type(result_value,result_type);
    const std::string prefix=lookup.prefix;
    const auto container=names_.value(prefix+"."+lookup.container),result=symbols_.value(result_value);
    builder_.call(container,*lookup.kind_test,{{ptr,symbols_.value(json)}});
    union_box_.allocate(result,result_type);
    const auto inspect=names_.label(prefix+".inspect"),bad=names_.label(prefix+".error"),done=names_.label(prefix+".done");
    builder_.br(container,inspect,bad);
    builder_.block(inspect);
    const auto raw=names_.value(prefix+".raw"),present=names_.value(prefix+".present");
    builder_.call(raw,*lookup.function,{{ptr,symbols_.value(json)},{lookup.key_type,symbols_.value(key)}});
    builder_.icmp(present,IntPredicate::ne,ptr,raw,"null");
    const auto yes=names_.label(prefix+".value"),missing=names_.label(prefix+".none");
    builder_.br(present,yes,missing);
    builder_.block(yes);
    union_box_.store_tag(result,case_index(result_type,Type::class_type(standard_class::json_value)));
    const auto value_payload=names_.value(prefix+".value.payload");
    union_box_.payload_slot(value_payload,result);
    builder_.store({ptr,raw},value_payload,Align::none);
    builder_.br(done);
    builder_.block(missing);
    union_box_.store_tag(result,case_index(result_type,Type::simple(TypeKind::None)));
    builder_.br(done);
    builder_.block(bad);
    union_box_.store_tag(result,case_index(result_type,Type::simple(TypeKind::Error)));
    const auto error_payload=names_.value(prefix+".error.payload");
    union_box_.payload_slot(error_payload,result);
    builder_.store({ptr,"@.err.json.type"},error_payload,Align::none);
    builder_.br(done);
    builder_.block(done);
}

void JsonEmitter::emit(const ir::JsonGet& n,const ir::Instruction&){
    emit_lookup(json_member,n.out,n.value,n.key,n.result_type);
}

void JsonEmitter::emit(const ir::JsonAt& n,const ir::Instruction&){
    emit_lookup(json_element,n.out,n.value,n.index,n.result_type);
}

void JsonEmitter::emit(const ir::JsonText& n,const ir::Instruction&){
    symbols_.set_type(n.out,n.result_type);
    const auto raw=names_.value("json.text.raw"),ok=names_.value("json.text.ok"),result=symbols_.value(n.out);
    builder_.call(raw,runtime_abi::json::text,{{ptr,symbols_.value(n.value)}});
    union_box_.allocate(result,n.result_type);
    builder_.icmp(ok,IntPredicate::ne,ptr,raw,"null");
    const auto yes=names_.label("json.text.ok"),bad=names_.label("json.text.error"),done=names_.label("json.text.done");
    builder_.br(ok,yes,bad);
    builder_.block(yes);
    union_box_.store_tag(result,case_index(n.result_type,Type::simple(TypeKind::String)));
    const auto value_payload=names_.value("json.text.value");
    union_box_.payload_slot(value_payload,result);
    builder_.store({ptr,raw},value_payload,Align::none);
    builder_.br(done);
    builder_.block(bad);
    union_box_.store_tag(result,case_index(n.result_type,Type::simple(TypeKind::Error)));
    const auto error_payload=names_.value("json.text.error.payload");
    union_box_.payload_slot(error_payload,result);
    builder_.store({ptr,"@.err.json.type"},error_payload,Align::none);
    builder_.br(done);
    builder_.block(done);
}

// A JSON scalar read (JsonScalarRead): two runtime functions, kind_test
// (whether the value has that kind) and reader (the value, of the LLVM type
// of kind); the word of the scalar names the temporaries and labels, and
// kind is the result's success case.
struct JsonScalarRead {
    const char* word;
    TypeKind kind;
    const LlvmCallee* kind_test;
    const LlvmCallee* reader;
};

namespace {

constexpr JsonScalarRead json_integer{"integer",TypeKind::Int64,&runtime_abi::json::integer_ok,&runtime_abi::json::integer};
constexpr JsonScalarRead json_number{"number",TypeKind::Real64,&runtime_abi::json::number_ok,&runtime_abi::json::number};
constexpr JsonScalarRead json_boolean{"boolean",TypeKind::Bool,&runtime_abi::json::boolean_ok,&runtime_abi::json::boolean};

} // namespace

void JsonEmitter::emit_scalar_read(const JsonScalarRead& read,ir::ValueId result_value,ir::ValueId json,const Type& result_type){
    symbols_.set_type(result_value,result_type);
    const auto value_type=llvm_type(Type::simple(read.kind));
    const std::string prefix=std::string("json.")+read.word;
    const auto ok=names_.value(prefix+".ok"),raw=names_.value(prefix+".raw"),result=symbols_.value(result_value);
    builder_.call(ok,*read.kind_test,{{ptr,symbols_.value(json)}});
    builder_.call(raw,*read.reader,{{ptr,symbols_.value(json)}});
    union_box_.allocate(result,result_type);
    const auto yes=names_.label(prefix+".ok"),bad=names_.label(prefix+".error"),done=names_.label(prefix+".done");
    builder_.br(ok,yes,bad);
    builder_.block(yes);
    union_box_.store_tag(result,case_index(result_type,Type::simple(read.kind)));
    const auto value_payload=names_.value(prefix+".value");
    union_box_.payload_slot(value_payload,result);
    builder_.store({value_type,raw},value_payload,Align::none);
    builder_.br(done);
    builder_.block(bad);
    union_box_.store_tag(result,case_index(result_type,Type::simple(TypeKind::Error)));
    const auto error_payload=names_.value(prefix+".error.payload");
    union_box_.payload_slot(error_payload,result);
    builder_.store({ptr,"@.err.json.type"},error_payload,Align::none);
    builder_.br(done);
    builder_.block(done);
}

void JsonEmitter::emit(const ir::JsonInteger& n,const ir::Instruction&){
    emit_scalar_read(json_integer,n.out,n.value,n.result_type);
}

void JsonEmitter::emit(const ir::JsonNumber& n,const ir::Instruction&){
    emit_scalar_read(json_number,n.out,n.value,n.result_type);
}

template <class T>
void JsonEmitter::emit_big_number(const T& n){
    symbols_.set_type(n.out,n.result_type);
    const bool want_int=std::is_same_v<T,ir::JsonBigInt>;
    const auto raw=names_.value("json.exact.raw"),parsed=names_.value("json.exact.parsed"),ok=names_.value("json.exact.ok"),result=symbols_.value(n.out);
    union_box_.allocate(result,n.result_type);
    builder_.call(raw,runtime_abi::json::number_text,{{ptr,symbols_.value(n.value)}});
    const auto has_text=names_.value("json.exact.has_text");
    builder_.icmp(has_text,IntPredicate::ne,ptr,raw,"null");
    const auto parse_label=names_.label("json.exact.parse"),bad=names_.label("json.exact.error"),yes=names_.label("json.exact.ok"),done=names_.label("json.exact.done");
    builder_.br(has_text,parse_label,bad);
    builder_.block(parse_label);
    if(want_int){
        builder_.call(parsed,runtime_abi::exact::bigint_parse,{{ptr,raw}});
        builder_.call(runtime_abi::memory::managed_release,{{ptr,raw},{ptr,"null"}});
        builder_.icmp(ok,IntPredicate::ne,ptr,parsed,"null");
    }else{
        builder_.call(parsed,exact_real_type,LlvmOperand::global(real_helper::parse),{{ptr,raw}});
        builder_.call(runtime_abi::memory::managed_release,{{ptr,raw},{ptr,"null"}});
        const auto none=names_.value("json.exact.none");
        builder_.call(none,i1,LlvmOperand::global(real_helper::is_none),{{exact_real_type,parsed}});
        builder_.binary(ok,BinaryOp::xor_,i1,none,"true");
    }
    builder_.br(ok,yes,bad);
    builder_.block(yes);
    const Type exact_type=Type::simple(want_int?TypeKind::Int:TypeKind::Real);
    union_box_.store_tag(result,case_index(n.result_type,exact_type));
    const auto payload=names_.value("json.exact.value");
    union_box_.payload_slot(payload,result);
    if(want_int){
        const auto word=names_.value("json.exact.word");
        builder_.call(word,bare_integer_type,LlvmOperand::global(int_helper::adopt),{{ptr,parsed}});
        builder_.store({bare_integer_type,word},payload,Align::none);
    }else{
        builder_.store({exact_real_type,parsed},payload,Align::none);
    }
    builder_.br(done);
    builder_.block(bad);
    union_box_.store_tag(result,case_index(n.result_type,Type::simple(TypeKind::Error)));
    const auto error_payload=names_.value("json.exact.error.payload");
    union_box_.payload_slot(error_payload,result);
    builder_.store({ptr,"@.err.json.type"},error_payload,Align::none);
    builder_.br(done);
    builder_.block(done);
}

void JsonEmitter::emit(const ir::JsonBigInt& n,const ir::Instruction&){
    emit_big_number(n);
}

void JsonEmitter::emit(const ir::JsonBigReal& n,const ir::Instruction&){
    emit_big_number(n);
}

void JsonEmitter::emit(const ir::JsonBoolean& n,const ir::Instruction&){
    emit_scalar_read(json_boolean,n.out,n.value,n.result_type);
}

void JsonEmitter::emit(const ir::JsonEncode& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::simple(TypeKind::String));
    builder_.call(symbols_.value(n.out),runtime_abi::json::encode,{{ptr,symbols_.value(n.value)}});
}

void JsonEmitter::emit(const ir::JsonEqual& n,const ir::Instruction&){
    symbols_.set_type(n.out,Type::simple(TypeKind::Bool));
    builder_.call(symbols_.value(n.out),runtime_abi::json::equal,{{ptr,symbols_.value(n.left)},{ptr,symbols_.value(n.right)}});
}

} // namespace quidra::llvm_backend
