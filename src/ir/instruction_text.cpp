// instruction_text (instruction_text.hpp): an explicit arm for every
// instruction kind. This diagnostic view is independent of operand analysis.
#include "ir/instruction_text.hpp"

#include "quidra/abi/error_record.hpp"
#include "quidra/abi/tensor_index.hpp"

#include <sstream>

namespace quidra::ir {

namespace {

template<class> inline constexpr bool missing_instruction_text = false;

const char* finite_cause_name(FiniteCause cause){
    switch(cause){
        case FiniteCause::addition: return "+";
        case FiniteCause::subtraction: return "-";
        case FiniteCause::multiplication: return "*";
        case FiniteCause::division: return "/";
        case FiniteCause::power: return "^";
    }
    return "?";
}

// error.make: a user error's text and code, or a Core code's spelling and
// its details by name (a name detail with a closed list unquoted); then
// the cause.
void write_error_make(std::ostream& out,const ErrorMake& n){
    out<<"%"<<n.out<<" = error.make";
    const auto operand=[&](const ErrorDetailOperand& detail,bool quote){
        if(detail.value)out<<" %"<<*detail.value;
        else if(quote)out<<" \""<<detail.constant<<"\"";
        else out<<" "<<detail.constant;
    };
    if(const auto* schema=abi::error_value_schema(n.schema)){
        out<<" "<<schema->spelling;
        for(std::size_t i=0;i<n.details.size()&&i<schema->details.size();++i){
            if(n.details[i].absent)continue;
            out<<" "<<schema->details[i].name;
            operand(n.details[i],schema->details[i].values.empty());
        }
    }else{
        for(const auto& detail:n.details)operand(detail,true);
        if(!n.code.empty())out<<" code \""<<n.code<<"\"";
    }
    if(n.cause)out<<" cause %"<<*n.cause;
}


// A slice item's operands as written: ` [%s<:%e:%k]`, an omitted side empty.
std::string slice_bounds_text(const SliceBounds& b){
    std::string text=" [";
    if(b.start) text+="%"+std::to_string(*b.start);
    if(b.start_form==SliceStart::exclusive_ascending) text+="<";
    if(b.start_form==SliceStart::exclusive_descending) text+=">";
    text+=":";
    if(b.end_form==SliceEnd::exclusive_ascending) text+="<";
    if(b.end_form==SliceEnd::exclusive_descending) text+=">";
    if(b.end) text+="%"+std::to_string(*b.end);
    if(b.step) text+=":%"+std::to_string(*b.step);
    text+="]";
    return text;
}
} // namespace

std::string instruction_text(const Instruction& i){ std::ostringstream out; std::visit([&](const auto& n){using T=std::decay_t<decltype(n)>;
    if constexpr(std::is_same_v<T,SourceLocation>)out<<"source "<<n.position.line<<":"<<n.position.column;
    else if constexpr(std::is_same_v<T,StatementBoundary>)out<<"statement "<<n.identity;
    else if constexpr(std::is_same_v<T,InlineScopeBegin>)out<<"inline.begin "<<n.scope;
    else if constexpr(std::is_same_v<T,InlineScopeCleanup>)out<<"inline.cleanup "<<n.scope;
    else if constexpr(std::is_same_v<T,InlineScopeEnd>)out<<"inline.end "<<n.scope;
    else if constexpr(std::is_same_v<T,InlineBindAddress>)out<<"inline.bind "<<n.name<<" = %"<<n.address<<" : "<<type_name(n.type);
    else if constexpr(std::is_same_v<T,InlineExit>)out<<"inline.exit "<<n.scope<<" %"<<n.status;
    else if constexpr(std::is_same_v<T,ConstantInt>)out<<"%"<<n.out<<" = const.int "<<n.value<<" : "<<type_name(n.type);
    else if constexpr(std::is_same_v<T,ConstantFloat>)out<<"%"<<n.out<<" = const.float "<<n.value<<" : "<<type_name(n.type);
    else if constexpr(std::is_same_v<T,ConstantExact>)out<<"%"<<n.out<<" = const.exact "<<n.spelling<<" : "<<type_name(n.type);
    else if constexpr(std::is_same_v<T,ConstantBool>)out<<"%"<<n.out<<" = const.bool "<<(n.value?"true":"false");
    else if constexpr(std::is_same_v<T,ConstantString>)out<<"%"<<n.out<<" = const.string \""<<n.value<<"\"";
    else if constexpr(std::is_same_v<T,StringQuote>)out<<"%"<<n.out<<" = string.quote %"<<n.text;
    else if constexpr(std::is_same_v<T,TupleMake>){out<<"%"<<n.out<<" = tuple.make "<<type_name(n.type);for(auto value:n.elements)out<<" %"<<value;}
    else if constexpr(std::is_same_v<T,TupleGet>)out<<"%"<<n.out<<" = tuple.get %"<<n.tuple<<", "<<n.index<<(n.take?" take":"");
    else if constexpr(std::is_same_v<T,AddressTupleElement>)out<<"%"<<n.out<<" = address.tuple.element %"<<n.address<<", "<<n.index;
    else if constexpr(std::is_same_v<T,ArrayMake>)out<<"%"<<n.out<<" = array.make "<<type_name(n.type);
    else if constexpr(std::is_same_v<T,ArrayAlloc>)out<<"%"<<n.out<<" = array.alloc %"<<n.length;
    else if constexpr(std::is_same_v<T,ClassMake>)out<<"%"<<n.out<<" = class.make "<<type_name(n.type);
    else if constexpr(std::is_same_v<T,FieldGet>)out<<"%"<<n.out<<" = field.get %"<<n.object<<", "<<n.index;
    else if constexpr(std::is_same_v<T,FieldSet>)out<<"field.set %"<<n.object<<", "<<n.index<<", %"<<n.value;
    else if constexpr(std::is_same_v<T,DeclareLocal>)out<<"local "<<n.name<<" : "<<type_name(n.type);
    else if constexpr(std::is_same_v<T,DeclareReference>)out<<(n.is_const?"const reference ":"reference ")<<n.name<<" : "<<type_name(n.type);
    else if constexpr(std::is_same_v<T,AddressLocal>)out<<"%"<<n.out<<" = address.local "<<n.name;
    else if constexpr(std::is_same_v<T,AddressField>)out<<"%"<<n.out<<" = address.field %"<<n.object<<", "<<n.index;
    else if constexpr(std::is_same_v<T,AddressElement>)out<<"%"<<n.out<<" = address.element %"<<n.array<<", %"<<n.index<<" : "<<type_name(n.element_type);
    else if constexpr(std::is_same_v<T,LoadAddress>)out<<"%"<<n.out<<" = address.load %"<<n.address;
    else if constexpr(std::is_same_v<T,StoreAddress>)out<<"address.store %"<<n.address<<", %"<<n.value<<" : "<<type_name(n.type);
    else if constexpr(std::is_same_v<T,BindReference>)out<<"reference.bind "<<n.name<<", %"<<n.address;
    else if constexpr(std::is_same_v<T,ReferenceAddress>)out<<"%"<<n.out<<" = reference.address "<<n.name;
    else if constexpr(std::is_same_v<T,LoadReference>)out<<"%"<<n.out<<" = reference.load "<<n.name<<" : "<<type_name(n.type);
    else if constexpr(std::is_same_v<T,StoreReference>)out<<"reference.store "<<n.name<<", %"<<n.value<<" : "<<type_name(n.type);
    else if constexpr(std::is_same_v<T,ArrayLength>)out<<"%"<<n.out<<" = array.length %"<<n.array;
    else if constexpr(std::is_same_v<T,ArrayCanAppendMove>)out<<"%"<<n.out<<" = array.can_append_move %"<<n.array;
    else if constexpr(std::is_same_v<T,ArrayGrowMove>)out<<"%"<<n.out<<" = array.grow_move %"<<n.array<<" : "<<type_name(n.array_type);
    else if constexpr(std::is_same_v<T,ArraySorted>){out<<"%"<<n.out<<" = array.sorted %"<<n.array;if(n.descending_constant)out<<" descending";else if(n.descending)out<<" descending %"<<*n.descending;out<<" : "<<type_name(n.array_type);}
    else if constexpr(std::is_same_v<T,StringIndex>)out<<"%"<<n.out<<" = string.index %"<<n.text<<", %"<<n.index;
    else if constexpr(std::is_same_v<T,StringIndexAsciiCompare>)out<<"%"<<n.out<<" = string.index_ascii_compare %"<<n.text<<", %"<<n.index<<", "<<static_cast<unsigned>(n.byte)<<(n.negate?" !=":" ==");
    else if constexpr(std::is_same_v<T,StringAsciiCountPrefix>)out<<"%"<<n.out<<" = string.ascii_count_prefix %"<<n.text<<", %"<<n.count<<", %"<<n.initial<<", "<<static_cast<unsigned>(n.byte)<<(n.negate?" !=":" ==");
    else if constexpr(std::is_same_v<T,StringLength>)out<<"%"<<n.out<<" = string.length %"<<n.text;
    else if constexpr(std::is_same_v<T,StringEmpty>)out<<"%"<<n.out<<" = string."<<(n.negate?"nonempty ":"empty ")<<"%"<<n.text;
    else if constexpr(std::is_same_v<T,StringContains>)out<<"%"<<n.out<<" = string.contains %"<<n.text<<", %"<<n.needle;
    else if constexpr(std::is_same_v<T,StringStartsWith>)out<<"%"<<n.out<<" = string.starts_with %"<<n.text<<", %"<<n.prefix;
    else if constexpr(std::is_same_v<T,StringEndsWith>)out<<"%"<<n.out<<" = string.ends_with %"<<n.text<<", %"<<n.suffix;
    else if constexpr(std::is_same_v<T,StringFind>)out<<"%"<<n.out<<" = string.find %"<<n.text<<", %"<<n.needle;
    else if constexpr(std::is_same_v<T,BinSlicePlanned>)out<<"%"<<n.out<<" = bin.slice_planned %"<<n.bin<<slice_bounds_text(n.bounds);
    else if constexpr(std::is_same_v<T,StringSliceStepped>)out<<"%"<<n.out<<" = string.slice_stepped %"<<n.text<<slice_bounds_text(n.bounds);
    else if constexpr(std::is_same_v<T,StringSlice>)out<<"%"<<n.out<<" = string.slice %"<<n.text<<", %"<<n.start<<", %"<<n.end;
    else if constexpr(std::is_same_v<T,StringTrim>)out<<"%"<<n.out<<" = string.trim %"<<n.text;
    else if constexpr(std::is_same_v<T,StringSplit>)out<<"%"<<n.out<<" = string.split %"<<n.text<<", %"<<n.separator;
    else if constexpr(std::is_same_v<T,StringSplitIterBegin>)out<<"%"<<n.out<<" = string.split_iter.begin %"<<n.text<<", %"<<n.separator<<(n.move_source?" move":"");
    else if constexpr(std::is_same_v<T,StringSplitIterNext>)out<<"%"<<n.text<<", %"<<n.has_value<<" = string.split_iter.next %"<<n.cursor;
    else if constexpr(std::is_same_v<T,StringSplitIterEnd>)out<<"string.split_iter.end %"<<n.cursor;
    else if constexpr(std::is_same_v<T,StringParseTwoSigned>)out<<"%"<<n.left<<", %"<<n.right<<", %"<<n.ok<<" = string.parse_two_signed %"<<n.text<<", "<<static_cast<unsigned>(n.separator);
    else if constexpr(std::is_same_v<T,StringUtf8>)out<<"%"<<n.out<<" = string.utf8 %"<<n.text;
    else if constexpr(std::is_same_v<T,StringFromUtf8>)out<<"%"<<n.out<<" = string.from_utf8 %"<<n.bin;
    else if constexpr(std::is_same_v<T,StringFromUtf8ArrayDirect>)out<<"%"<<n.text<<", %"<<n.ok<<", %"<<n.error<<" = string.from_utf8_array_direct %"<<n.array;
    else if constexpr(std::is_same_v<T,StringCodepoints>)out<<"%"<<n.out<<" = string.codepoints %"<<n.text;
    else if constexpr(std::is_same_v<T,StringJoin>)out<<"%"<<n.out<<" = string.join %"<<n.values<<", %"<<n.separator;
    else if constexpr(std::is_same_v<T,StringConcat>){out<<"%"<<n.out<<" = string.concat";for(const auto value:n.values)out<<" %"<<value;}
    else if constexpr(std::is_same_v<T,StringBuild>){out<<"%"<<n.out<<" = string.build";for(const auto& part:n.parts){out<<" %"<<part.value<<":"<<type_name(part.type);if(part.kind==StringBuildPartKind::QuotedString)out<<":quoted";}out<<" sep %"<<n.separator;}
    else if constexpr(std::is_same_v<T,StringBuildAppendMove>){out<<"%"<<n.out<<", %"<<n.added_length<<" = string.build_append %"<<n.text;for(const auto& part:n.parts){out<<" %"<<part.value<<":"<<type_name(part.type);if(part.kind==StringBuildPartKind::QuotedString)out<<":quoted";}out<<" sep %"<<n.separator;}
    else if constexpr(std::is_same_v<T,StringCanAppendMove>)out<<"%"<<n.out<<" = string.can_append_move %"<<n.text;
    else if constexpr(std::is_same_v<T,StringAppendMove>){out<<"%"<<n.out<<" = string.append_move %"<<n.text;for(const auto value:n.suffixes)out<<" %"<<value;}
    else if constexpr(std::is_same_v<T,BinAlloc>)out<<"%"<<n.out<<" = bin.alloc %"<<n.length<<", %"<<n.fill;
    else if constexpr(std::is_same_v<T,BinLength>)out<<"%"<<n.out<<" = bin.length %"<<n.bin;
    else if constexpr(std::is_same_v<T,BinGet>)out<<"%"<<n.out<<" = bin.get %"<<n.bin<<", %"<<n.index;
    else if constexpr(std::is_same_v<T,BinSet>)out<<"bin.set %"<<n.bin<<", %"<<n.index<<", %"<<n.value;
    else if constexpr(std::is_same_v<T,ParseBin>)out<<"%"<<n.out<<" = bin.parse %"<<n.text<<" : "<<type_name(n.result_type)<<(n.success_proven?" success-proven":"");
    else if constexpr(std::is_same_v<T,NumericConvert>)out<<"%"<<n.out<<" = convert %"<<n.value<<" : "<<type_name(n.source_type)<<" -> "<<type_name(n.target_type)<<(n.checked_range?" checked":"")<<(n.inline_proven?" inline":"");
    else if constexpr(std::is_same_v<T,FallibleNumericConvert>)out<<"%"<<n.out<<" = convert.fallible %"<<n.value<<" : "<<type_name(n.source_type)<<" -> "<<type_name(n.result_type);
    else if constexpr(std::is_same_v<T,TensorCreate>)out<<"%"<<n.out<<" = tensor.create %"<<n.shape<<" : "<<type_name(n.type)<<" init="<<(n.fill_mode==tensor_fill_mode::uninitialized?"uninitialized":n.fill_mode==tensor_fill_mode::zeros?"zeros":"ones")<<(n.gpu?" gpu=%"+std::to_string(*n.gpu):" cpu");
    else if constexpr(std::is_same_v<T,TensorTransfer>)out<<"%"<<n.out<<" = tensor."<<(n.gpu?"gpu":"cpu")<<" %"<<n.tensor<<(n.gpu?", %"+std::to_string(*n.gpu):"")<<" : "<<type_name(n.type);
    else if constexpr(std::is_same_v<T,TensorReshape>)out<<"%"<<n.out<<" = tensor.reshape %"<<n.tensor<<", %"<<n.shape<<" : "<<type_name(n.type);
    else if constexpr(std::is_same_v<T,TensorTranspose>)out<<"%"<<n.out<<" = tensor.transpose %"<<n.tensor<<", %"<<n.axis0<<", %"<<n.axis1<<" : "<<type_name(n.type);
    else if constexpr(std::is_same_v<T,TensorContiguous>)out<<"%"<<n.out<<" = tensor.contiguous %"<<n.tensor<<" : "<<type_name(n.type);
    else if constexpr(std::is_same_v<T,TensorGather>)out<<"%"<<n.out<<" = tensor.gather %"<<n.tensor<<", %"<<n.indices<<", %"<<n.shape<<" : "<<type_name(n.type);
    else if constexpr(std::is_same_v<T,TensorScatter>)out<<"%"<<n.out<<" = tensor.scatter %"<<n.tensor<<", %"<<n.indices<<", %"<<n.shape<<" : "<<type_name(n.type);
    else if constexpr(std::is_same_v<T,TensorShape>)out<<"%"<<n.out<<" = tensor.shape %"<<n.tensor<<" : "<<type_name(n.type);
    else if constexpr(std::is_same_v<T,TensorDevice>)out<<"%"<<n.out<<" = tensor.device %"<<n.tensor;
    else if constexpr(std::is_same_v<T,TensorIsContiguous>)out<<"%"<<n.out<<" = tensor.is_contiguous %"<<n.tensor;
    else if constexpr(std::is_same_v<T,TensorIsTracked>)out<<"%"<<n.out<<" = tensor.is_tracked %"<<n.tensor;
    else if constexpr(std::is_same_v<T,TensorHasGrad>)out<<"%"<<n.out<<" = tensor.has_grad %"<<n.tensor;
    else if constexpr(std::is_same_v<T,TensorClearGrad>)out<<"tensor.clear_grad %"<<n.tensor;
    else if constexpr(std::is_same_v<T,TensorItem>)out<<"%"<<n.out<<" = tensor.item %"<<n.tensor<<" : "<<type_name(n.element_type);
    else if constexpr(std::is_same_v<T,TensorTrack>)out<<"%"<<n.out<<" = tensor."<<(n.mode==tensor_track_mode::untrack?"untrack":n.mode==tensor_track_mode::track?"track":"retrack")<<" %"<<n.tensor<<(n.target?" target %"+std::to_string(n.target):"");
    else if constexpr(std::is_same_v<T,TensorBackward>){out<<"tensor.backward %"<<n.tensor;for(const auto&target:n.targets)out<<" "<<(target.autograd_target?"target":"tensor")<<" %"<<target.value;}
    else if constexpr(std::is_same_v<T,TensorGrad>)out<<"%"<<n.out<<" = tensor.grad %"<<n.tensor;
    else if constexpr(std::is_same_v<T,TensorBinary>)out<<"%"<<n.out<<" = tensor.binary "<<n.op<<" %"<<n.left<<", %"<<n.right<<" : "<<type_name(n.result_type);
    else if constexpr(std::is_same_v<T,TensorIndex>){
        out<<"%"<<n.out<<" = tensor.index %"<<n.tensor;
        for(const auto& item:n.items){
            if(item.kind!=abi::tensor_index_spec::index){
                // A marked slice shows its markers and the word marked; the
                // half-open start:<end shows none.
                namespace spec=abi::tensor_index_spec;
                const bool marked=spec::is_marked_slice(item.kind);
                out<<(marked?" marked [":" [");
                if(item.start)out<<"%"<<*item.start;
                if(marked&&spec::marked_start(item.kind)==SliceStart::exclusive_ascending)out<<"<";
                out<<":";
                if(marked&&spec::marked_end(item.kind)==SliceEnd::exclusive_ascending)out<<"<";
                if(item.stop)out<<"%"<<*item.stop;
                if(item.step)out<<":%"<<*item.step;
                out<<"]";
            }else if(item.index){
                out<<" %"<<*item.index;
            }
        }
        out<<" : "<<type_name(n.type);
    }
    else if constexpr(std::is_same_v<T,ArrayNumericCast>)out<<"%"<<n.out<<" = array.numeric_cast %"<<n.array<<" : "<<type_name(n.source_type)<<" -> "<<type_name(n.target_type);
    else if constexpr(std::is_same_v<T,TensorCast>)out<<"%"<<n.out<<" = tensor.numeric_cast %"<<n.tensor<<" : "<<type_name(n.source_type)<<" -> "<<type_name(n.target_type);
    else if constexpr(std::is_same_v<T,ShapedConstraintCheck>){out<<"shape.constraint %"<<n.value<<" rank"<<(n.ellipsis ? ">=" : "=")<<n.extents.size();if(n.ellipsis)out<<" ellipsis="<<*n.ellipsis;}
    else if constexpr(std::is_same_v<T,ExtentEqualCheck>)out<<(n.for_collection_lengths?"for.length.check %":"extent.check %")<<n.actual<<", %"<<n.expected;
    else if constexpr(std::is_same_v<T,ParseNumber>)out<<"%"<<n.out<<" = parse %"<<n.text<<" : "<<type_name(n.target_type)<<" -> "<<type_name(n.result_type);
    else if constexpr(std::is_same_v<T,ParseNumberDirect>)out<<"%"<<n.value_out<<", %"<<n.ok_out<<", %"<<n.error_out<<" = parse.direct %"<<n.text<<" : "<<type_name(n.target_type);
    else if constexpr(std::is_same_v<T,ExactAtom>)out<<"%"<<n.out<<" = exact.atom "<<n.provider<<":"<<n.opcode<<" : "<<type_name(n.type);
    else if constexpr(std::is_same_v<T,ExactUnary>)out<<"%"<<n.out<<" = exact.unary "<<n.provider<<":"<<n.opcode<<" %"<<n.input<<" : "<<type_name(n.type);
    else if constexpr(std::is_same_v<T,RealLoopCount>)out<<"%"<<n.out<<" = real.loop_count %"<<n.start<<", %"<<n.end<<", %"<<n.step<<" : "<<type_name(n.type)<<" "<<(n.descending?"descending":"ascending")<<" "<<n.start_inclusive<<" "<<n.end_inclusive;
    else if constexpr(std::is_same_v<T,RealLoopValue>)out<<"%"<<n.out<<" = real.loop_value %"<<n.start<<", %"<<n.step<<", %"<<n.multiplier<<" : "<<type_name(n.type)<<" fma %"<<n.native_fma;
    else if constexpr(std::is_same_v<T,ExactLoopPlan>)out<<"%"<<n.out<<" = exact.loop_plan %"<<n.start<<", %"<<n.end<<", %"<<n.step<<" "<<n.descending<<" "<<n.start_inclusive<<" "<<n.end_inclusive;
    else if constexpr(std::is_same_v<T,ExactFromScaled>)out<<"%"<<n.out<<" = exact.from_scaled %"<<n.numerator<<", %"<<n.denominator;
    else if constexpr(std::is_same_v<T,ExactLoopValue>)out<<"%"<<n.out<<" = exact.loop_value %"<<n.start<<", %"<<n.step<<", %"<<n.multiplier;
    else if constexpr(std::is_same_v<T,ExactScaledCompare>)out<<"%"<<n.out<<" = exact.scaled_compare %"<<n.numerator<<", %"<<n.denominator<<" "<<n.op<<" "<<n.constant_n<<"/"<<n.constant_d;
    else if constexpr(std::is_same_v<T,ExactScaledConvert>)out<<"%"<<n.out<<" = exact.scaled_convert %"<<n.numerator<<", %"<<n.denominator<<" : "<<type_name(n.target)<<" result "<<type_name(n.result_type);
    else if constexpr(std::is_same_v<T,ExactRound>){
        out<<"%"<<n.out<<" = exact.round %"<<n.value<<" : "<<type_name(n.type);
        if(!n.slot.empty())out<<" -> "<<n.slot;
    }
    else if constexpr(std::is_same_v<T,ExactConstant>)out<<"%"<<n.out<<" = exact.constant "<<n.slot<<" : "<<type_name(n.type);
    else if constexpr(std::is_same_v<T,ExactOp>){
        out<<"%"<<n.out<<" = exact.op "<<n.provider<<":"<<n.opcode;
        for(std::size_t i=0;i<n.inputs.size();++i)out<<(i?", %":" %")<<n.inputs[i];
        out<<" : "<<type_name(n.type);
    }
    else if constexpr(std::is_same_v<T,ArrayInitializationComplete>)out<<"%"<<n.out<<" = array.initialization.complete %"<<n.array;
    else if constexpr(std::is_same_v<T,ArraySlice>)out<<"%"<<n.out<<" = array.slice %"<<n.array<<slice_bounds_text(n.bounds)<<" : "<<type_name(n.array_type);
    else if constexpr(std::is_same_v<T,SliceLoopPlan>)out<<"%"<<n.out<<" = slice.loop_plan %"<<n.length<<slice_bounds_text(n.bounds)<<", first-address %"<<n.first_address;
    else if constexpr(std::is_same_v<T,ArraySliceSet>)out<<"array.slice_set %"<<n.array<<slice_bounds_text(n.bounds)<<", %"<<n.value<<" : "<<type_name(n.array_type);
    else if constexpr(std::is_same_v<T,ArrayGet>)out<<"%"<<n.out<<" = array.get %"<<n.array<<", %"<<n.index<<(n.bounds_guard?" bounds-guard %"+std::to_string(*n.bounds_guard):"");
    else if constexpr(std::is_same_v<T,ArraySet>)out<<"array.set %"<<n.array<<", %"<<n.index<<", %"<<n.value<<(n.bounds_guard?" bounds-guard %"+std::to_string(*n.bounds_guard):"");
    else if constexpr(std::is_same_v<T,Clone>)out<<"%"<<n.out<<" = clone %"<<n.value<<" : "<<type_name(n.type);
    else if constexpr(std::is_same_v<T,Retain>)out<<"%"<<n.out<<" = retain %"<<n.value<<" : "<<type_name(n.type);
    else if constexpr(std::is_same_v<T,Release>)out<<"release %"<<n.value<<" : "<<type_name(n.type);
    else if constexpr(std::is_same_v<T,Unary>)out<<"%"<<n.out<<" = "<<n.op<<" %"<<n.operand<<(n.inline_proven?" inline":"");
    else if constexpr(std::is_same_v<T,Binary>)out<<"%"<<n.out<<" = "<<n.op<<" %"<<n.left<<", %"<<n.right<<(n.overflow_proven?" no-overflow":"")<<(n.inline_proven?" inline":"");
    else if constexpr(std::is_same_v<T,ToString>)out<<"%"<<n.out<<" = text %"<<n.value<<" : "<<type_name(n.source_type);
    else if constexpr(std::is_same_v<T,FormatNumber>){
        out<<"%"<<n.out<<" = format %"<<n.value<<" : "<<type_name(n.source_type);
        if(n.integer_width)out<<" int="<<*n.integer_width;
        if(n.fractional_digits)out<<" frac="<<*n.fractional_digits;
        if(n.significant_digits)out<<" sig="<<*n.significant_digits;
        if(n.zero)out<<" zero";
    }
    else if constexpr(std::is_same_v<T,ParallelBlock>){out<<"%"<<n.out<<" = parallel "<<n.table_name;if(n.iterations)out<<" for %"<<n.iterations->count;for(const auto& node:n.nodes){out<<" node "<<node.function<<" pred";for(auto p:node.predecessors)out<<" "<<p;}}
    else if constexpr(std::is_same_v<T,AddressReferenceSlot>)out<<"%"<<n.out<<" = ref.slot "<<n.name;
    else if constexpr(std::is_same_v<T,LoadLocal>)out<<"%"<<n.out<<(n.move_out?" = load.move ":" = load ")<<n.name;
    else if constexpr(std::is_same_v<T,StoreLocal>)out<<(n.borrowed?"store.borrow ":"store ")<<n.name<<", %"<<n.value;
    else if constexpr(std::is_same_v<T,FunctionRef>)out<<"%"<<n.out<<" = fn.ref "<<n.function<<" : "<<type_name(n.type);
    else if constexpr(std::is_same_v<T,IndirectCall>){
        if(n.result.kind!=TypeKind::Void&&n.result.kind!=TypeKind::Never)out<<"%"<<n.out<<" = ";
        out<<"call.indirect %"<<n.callee<<"(";
        for(std::size_t index=0;index<n.args.size();++index){if(index)out<<", ";out<<"%"<<n.args[index];}
        out<<")";
    }
    else if constexpr(std::is_same_v<T,Call>){
        if(n.result.kind!=TypeKind::Void&&n.result.kind!=TypeKind::Never)out<<"%"<<n.out<<" = ";
        out<<(n.shape_forwarder?"call.shape_forward ":"call ")<<n.callee<<"(";
        for(std::size_t index=0;index<n.args.size();++index){
            if(index)out<<", ";
            if(n.args[index].writable_address)out<<"&%"<<*n.args[index].writable_address;
            else out<<"%"<<n.args[index].value;
        }
        out<<")";
    }
    else if constexpr(std::is_same_v<T,VariantMake>)out<<"%"<<n.out<<" = variant "<<n.tag;
    else if constexpr(std::is_same_v<T,VariantTag>)out<<"%"<<n.out<<" = variant.tag %"<<n.container;
    else if constexpr(std::is_same_v<T,VariantPayload>)out<<"%"<<n.out<<" = variant.payload %"<<n.container;
    else if constexpr(std::is_same_v<T,Print>)out<<"%"<<n.out<<" = print %"<<n.value<<" : "<<type_name(n.type);
    else if constexpr(std::is_same_v<T,ReplDisplay>)out<<"repl.display %"<<n.value<<" : "<<type_name(n.type);
    else if constexpr(std::is_same_v<T,ReplReplayMode>)out<<"repl.replay "<<(n.active?"on":"off");
    else if constexpr(std::is_same_v<T,Input>)out<<"%"<<n.out<<" = input";
    else if constexpr(std::is_same_v<T,Exit>)out<<"exit %"<<n.status;
    else if constexpr(std::is_same_v<T,FailError>)out<<"fail.error %"<<n.error;
    else if constexpr(std::is_same_v<T,ForStepCheck>)out<<"for.check_step %"<<n.step<<(n.descending?" descending":" ascending");
    else if constexpr(std::is_same_v<T,Return>)out<<"return %"<<n.value;
    else if constexpr(std::is_same_v<T,ReturnVoid>)out<<"return";
    else if constexpr(std::is_same_v<T,Jump>)out<<"jump "<<n.target;
    else if constexpr(std::is_same_v<T,Branch>)out<<"branch %"<<n.condition<<", "<<n.if_true<<", "<<n.if_false;
    else if constexpr(std::is_same_v<T,Pin>)out<<"pin %"<<n.value;
    else if constexpr(std::is_same_v<T,Unpin>)out<<"unpin %"<<n.value;
    else if constexpr(std::is_same_v<T,TraversalSiteCheck>)out<<"for.check_site %"<<n.expected<<", %"<<n.current;
    else if constexpr(std::is_same_v<T,IterationShapeCheck>)out<<"for.check_shape %"<<n.array<<", %"<<n.entry_array<<", %"<<n.entry_length;
    else if constexpr(std::is_same_v<T,ErrorMake>)write_error_make(out,n);
    else if constexpr(std::is_same_v<T,ErrorMessage>)out<<"%"<<n.out<<" = error.message %"<<n.error;
    else if constexpr(std::is_same_v<T,ErrorHop>){
        out<<"%"<<n.out<<" = error.hop %"<<n.container<<" call "<<n.name<<" at "<<n.position.line<<":"<<n.position.column<<" via "<<abi::error_hop_via(n.kind);
        if(n.callee!=0)out<<" callee %"<<n.callee;
    }
    else if constexpr(std::is_same_v<T,ErrorCode>)out<<"%"<<n.out<<" = error.code %"<<n.error;
    else if constexpr(std::is_same_v<T,ErrorCause>)out<<"%"<<n.out<<" = error.cause %"<<n.error<<" : "<<type_name(n.result_type);
    else if constexpr(std::is_same_v<T,ErrorDetail>)out<<"%"<<n.out<<" = error.detail %"<<n.error<<" "<<(n.detail<abi::error_detail_names.size()?abi::error_detail_names[n.detail]:std::string_view("?"))<<" : "<<type_name(n.result_type);
    else if constexpr(std::is_same_v<T,FieldReferenceAddress>)out<<"%"<<n.out<<" = field.reference %"<<n.object<<", "<<n.index;
    else if constexpr(std::is_same_v<T,FieldReferenceRoot>)out<<"%"<<n.out<<" = field.reference_root %"<<n.object<<", "<<n.index;
    else if constexpr(std::is_same_v<T,BindFieldReference>)out<<"field.bind_reference %"<<n.object<<", "<<n.index<<", %"<<n.address<<", root %"<<n.root;
    else if constexpr(std::is_same_v<T,SelfReferenceCheck>)out<<"reference.self_check %"<<n.holder_root<<", %"<<n.target_root;
    else if constexpr(std::is_same_v<T,StoredTargetsCheck>)out<<"reference.targets_check %"<<n.value<<", root %"<<n.destination_root;
    else if constexpr(std::is_same_v<T,RootSerial>)out<<"%"<<n.out<<" = reference.root_serial";
    else if constexpr(std::is_same_v<T,ReferenceBoundCheck>)out<<"reference.check_bound %"<<n.address<<", '"<<n.path<<"'";
    else if constexpr(std::is_same_v<T,InitializedCheck>)out<<"init.check %"<<n.flag<<", "<<(n.subject==InitializedSubject::binding?"binding":n.subject==InitializedSubject::field?"field":"argument")<<" '"<<n.path<<"'";
    else if constexpr(std::is_same_v<T,FiniteCheck>){
        // Tested values and operation provenance used in failure diagnostics.
        const auto checked=[&](const CheckedReal& c){out<<" %"<<c.value<<" ("<<finite_cause_name(c.cause)<<" %"<<c.left<<", %"<<c.right<<")";};
        out<<"finite.check";for(const auto& c:n.primary)checked(c);
        if(!n.covered.empty()){out<<" covers";for(const auto& c:n.covered)checked(c);}
    }
    else if constexpr(std::is_same_v<T,FiniteFlagUpdate>)out<<"finite.flag "<<n.flag<<", %"<<n.value.value<<" ("<<finite_cause_name(n.value.cause)<<" %"<<n.value.left<<", %"<<n.value.right<<")";
    else if constexpr(std::is_same_v<T,FiniteFlagCheck>)out<<"finite.flag.check "<<n.flag<<" ("<<finite_cause_name(n.value.cause)<<")";
    else if constexpr(std::is_same_v<T,ComplexMake>)out<<"%"<<n.out<<" = complex %"<<n.re<<", %"<<n.im<<" : "<<type_name(n.type);
    else if constexpr(std::is_same_v<T,ComplexPart>)out<<"%"<<n.out<<" = complex."<<(n.part==0?"real":"imag")<<" %"<<n.value<<" : "<<type_name(n.type);
    else if constexpr(std::is_same_v<T,StringRepeat>)out<<"%"<<n.out<<" = string.repeat %"<<n.count<<", %"<<n.fill;
    else if constexpr(std::is_same_v<T,BinSlice>)out<<"%"<<n.out<<" = bin.slice %"<<n.bin<<", %"<<n.start<<", %"<<n.end;
    else if constexpr(std::is_same_v<T,BinConvert>)out<<"%"<<n.out<<" = bin.convert %"<<n.value<<" : "<<type_name(n.source_type)<<" -> "<<type_name(n.target_type);
    else if constexpr(std::is_same_v<T,TensorCompare>)out<<"%"<<n.out<<" = tensor.compare "<<n.op<<" %"<<n.left<<", %"<<n.right<<" : "<<type_name(n.result_type);
    else if constexpr(std::is_same_v<T,TensorBoolReduce>)out<<"%"<<n.out<<" = tensor."<<(n.all?"all":"any")<<" %"<<n.tensor;
    else if constexpr(std::is_same_v<T,TensorSet>){
        out<<"tensor.set %"<<n.tensor<<" [";
        for(std::size_t axis=0;axis<n.indices.size();++axis){if(axis)out<<", ";out<<"%"<<n.indices[axis];}
        out<<"], %"<<n.value<<" : "<<type_name(n.element_type);
    }
    else if constexpr(std::is_same_v<T,CliArgument>)out<<"%"<<n.out<<" = cli.argument %"<<n.name<<", %"<<n.index<<" : "<<type_name(n.type);
    else if constexpr(std::is_same_v<T,CliArgumentOptional>)out<<"%"<<n.out<<" = cli.argument.optional %"<<n.name<<", %"<<n.index<<", %"<<n.default_value<<" : "<<type_name(n.type);
    else if constexpr(std::is_same_v<T,CliOption>)out<<"%"<<n.out<<" = cli.option %"<<n.name<<", %"<<n.default_value<<" : "<<type_name(n.type);
    else if constexpr(std::is_same_v<T,CliFlag>)out<<"%"<<n.out<<" = cli.flag %"<<n.name;
    else if constexpr(std::is_same_v<T,CliFinish>)out<<"cli.finish";
    else if constexpr(std::is_same_v<T,Flush>)out<<"%"<<n.out<<" = flush : "<<type_name(n.result_type);
    else if constexpr(std::is_same_v<T,FileOpen>)out<<"%"<<n.out<<" = file.open %"<<n.path<<" : "<<type_name(n.result_type);
    else if constexpr(std::is_same_v<T,FileCreate>)out<<"%"<<n.out<<" = file.create %"<<n.path<<" : "<<type_name(n.result_type);
    else if constexpr(std::is_same_v<T,FileAppend>)out<<"%"<<n.out<<" = file.append %"<<n.path<<" : "<<type_name(n.result_type);
    else if constexpr(std::is_same_v<T,FileHandleRead>)out<<"%"<<n.out<<" = file.handle.read %"<<n.handle<<" : "<<type_name(n.result_type);
    else if constexpr(std::is_same_v<T,FileHandleReadLine>)out<<"%"<<n.out<<" = file.handle.read_line %"<<n.handle<<" : "<<type_name(n.result_type);
    else if constexpr(std::is_same_v<T,FileHandleReadBin>)out<<"%"<<n.out<<" = file.handle.read_bin %"<<n.handle<<" : "<<type_name(n.result_type);
    else if constexpr(std::is_same_v<T,FileHandleWrite>)out<<"%"<<n.out<<" = file.handle.write %"<<n.handle<<", %"<<n.text<<(n.newline?" newline":"")<<" : "<<type_name(n.result_type);
    else if constexpr(std::is_same_v<T,FileHandleFlush>)out<<"%"<<n.out<<" = file.handle.flush %"<<n.handle<<" : "<<type_name(n.result_type);
    else if constexpr(std::is_same_v<T,FileHandleSeek>)out<<"%"<<n.out<<" = file.handle.seek %"<<n.handle<<", %"<<n.offset<<" : "<<type_name(n.result_type);
    else if constexpr(std::is_same_v<T,FileHandleClose>)out<<"file.handle.close %"<<n.handle;
    else if constexpr(std::is_same_v<T,FileRead>)out<<"%"<<n.out<<" = file.read %"<<n.path<<" : "<<type_name(n.result_type);
    else if constexpr(std::is_same_v<T,FileReadBin>)out<<"%"<<n.out<<" = file.read_bin %"<<n.path<<" : "<<type_name(n.result_type);
    else if constexpr(std::is_same_v<T,FileWrite>)out<<"%"<<n.out<<" = file.write %"<<n.path<<", %"<<n.text<<" : "<<type_name(n.result_type);
    else if constexpr(std::is_same_v<T,FileWriteBin>)out<<"%"<<n.out<<" = file.write_bin %"<<n.path<<", %"<<n.bin<<" : "<<type_name(n.result_type);
    else if constexpr(std::is_same_v<T,FileExists>)out<<"%"<<n.out<<" = file.exists %"<<n.path<<" : "<<type_name(n.result_type);
    else if constexpr(std::is_same_v<T,FileIsDirectory>)out<<"%"<<n.out<<" = file.is_directory %"<<n.path<<" : "<<type_name(n.result_type);
    else if constexpr(std::is_same_v<T,FileRemove>)out<<"%"<<n.out<<" = file.remove %"<<n.path<<" : "<<type_name(n.result_type);
    else if constexpr(std::is_same_v<T,FileCopy>)out<<"%"<<n.out<<" = file.copy %"<<n.source<<", %"<<n.destination<<" : "<<type_name(n.result_type);
    else if constexpr(std::is_same_v<T,FileMove>)out<<"%"<<n.out<<" = file.move %"<<n.source<<", %"<<n.destination<<" : "<<type_name(n.result_type);
    else if constexpr(std::is_same_v<T,FileMkdir>)out<<"%"<<n.out<<" = file.mkdir %"<<n.path<<" : "<<type_name(n.result_type);
    else if constexpr(std::is_same_v<T,FileList>)out<<"%"<<n.out<<" = file.list %"<<n.path<<", recursive %"<<n.recursive<<" : "<<type_name(n.result_type);
    else if constexpr(std::is_same_v<T,EnvironmentGet>)out<<"%"<<n.out<<" = environment.get %"<<n.name<<" : "<<type_name(n.result_type);
    else if constexpr(std::is_same_v<T,EnvironmentHas>)out<<"%"<<n.out<<" = environment.has %"<<n.name;
    else if constexpr(std::is_same_v<T,TestAssert>)out<<"test.assert %"<<n.condition;
    else if constexpr(std::is_same_v<T,TimeNow>)out<<"%"<<n.out<<" = time.now %"<<n.sync;
    else if constexpr(std::is_same_v<T,TimeSince>)out<<"%"<<n.out<<" = time.since %"<<n.start<<", %"<<n.sync;
    else if constexpr(std::is_same_v<T,TimeSeconds>)out<<"%"<<n.out<<" = time.seconds %"<<n.seconds;
    else if constexpr(std::is_same_v<T,TimeSleep>)out<<"time.sleep %"<<n.duration;
    else if constexpr(std::is_same_v<T,GpuSync>)out<<"gpu.sync %"<<n.index;
    else if constexpr(std::is_same_v<T,AutogradTargetCreate>)out<<"%"<<n.out<<" = autograd.target.create";
    else if constexpr(std::is_same_v<T,AutogradTargetHasGrad>)out<<"%"<<n.out<<" = autograd.target.has_grad %"<<n.target;
    else if constexpr(std::is_same_v<T,AutogradTargetClearGrad>)out<<"autograd.target.clear_grad %"<<n.target;
    else if constexpr(std::is_same_v<T,AutogradTargetGradient>)out<<"%"<<n.out<<" = autograd.target.gradient %"<<n.target<<" : "<<type_name(n.type);
    else if constexpr(std::is_same_v<T,RandomGenerator>)out<<"%"<<n.out<<" = random.generator %"<<n.seed;
    else if constexpr(std::is_same_v<T,RandomInt>)out<<"%"<<n.out<<" = random.int %"<<n.generator<<", %"<<n.start<<", %"<<n.end;
    else if constexpr(std::is_same_v<T,RandomFloat>)out<<"%"<<n.out<<" = random.float %"<<n.generator;
    else if constexpr(std::is_same_v<T,RandomBool>)out<<"%"<<n.out<<" = random.bool %"<<n.generator;
    else if constexpr(std::is_same_v<T,ProcessRun>)out<<"%"<<n.out<<" = process.run %"<<n.program<<", %"<<n.args;
    else if constexpr(std::is_same_v<T,ProcessShell>)out<<"%"<<n.out<<" = process.shell %"<<n.command;
    else if constexpr(std::is_same_v<T,JsonParse>)out<<"%"<<n.out<<" = json.parse %"<<n.text<<" : "<<type_name(n.result_type);
    else if constexpr(std::is_same_v<T,JsonKind>)out<<"%"<<n.out<<" = json.kind %"<<n.value;
    else if constexpr(std::is_same_v<T,JsonSize>)out<<"%"<<n.out<<" = json.size %"<<n.value<<" : "<<type_name(n.result_type);
    else if constexpr(std::is_same_v<T,JsonGet>)out<<"%"<<n.out<<" = json.get %"<<n.value<<", %"<<n.key<<" : "<<type_name(n.result_type);
    else if constexpr(std::is_same_v<T,JsonAt>)out<<"%"<<n.out<<" = json.at %"<<n.value<<", %"<<n.index<<" : "<<type_name(n.result_type);
    else if constexpr(std::is_same_v<T,JsonText>)out<<"%"<<n.out<<" = json.text %"<<n.value<<" : "<<type_name(n.result_type);
    else if constexpr(std::is_same_v<T,JsonInteger>)out<<"%"<<n.out<<" = json.integer %"<<n.value<<" : "<<type_name(n.result_type);
    else if constexpr(std::is_same_v<T,JsonNumber>)out<<"%"<<n.out<<" = json.number %"<<n.value<<" : "<<type_name(n.result_type);
    else if constexpr(std::is_same_v<T,JsonBigInt>)out<<"%"<<n.out<<" = json.bigint %"<<n.value<<" : "<<type_name(n.result_type);
    else if constexpr(std::is_same_v<T,JsonBigReal>)out<<"%"<<n.out<<" = json.bigreal %"<<n.value<<" : "<<type_name(n.result_type);
    else if constexpr(std::is_same_v<T,JsonBoolean>)out<<"%"<<n.out<<" = json.boolean %"<<n.value<<" : "<<type_name(n.result_type);
    else if constexpr(std::is_same_v<T,JsonEncode>)out<<"%"<<n.out<<" = json.encode %"<<n.value;
    else if constexpr(std::is_same_v<T,JsonEqual>)out<<"%"<<n.out<<" = json.equal %"<<n.left<<", %"<<n.right;
    else if constexpr(std::is_same_v<T,HttpGet>)out<<"%"<<n.out<<" = http.get %"<<n.url<<" : "<<type_name(n.result_type);
    else if constexpr(std::is_same_v<T,HttpHeader>)out<<"%"<<n.out<<" = http.header %"<<n.response<<", %"<<n.name<<" : "<<type_name(n.result_type);
    else static_assert(missing_instruction_text<T>, "instruction needs an explicit compact renderer");
},i);return out.str();}

} // namespace quidra::ir
