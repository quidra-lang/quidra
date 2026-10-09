// instruction_text (instruction_text.hpp): one if-constexpr line per rendered
// instruction kind.
#include "ir/instruction_text.hpp"

#include <sstream>

namespace quidra::ir {

std::string instruction_text(const Instruction& i){ std::ostringstream out; std::visit([&](const auto& n){using T=std::decay_t<decltype(n)>;
    if constexpr(std::is_same_v<T,SourceLocation>)out<<"source "<<n.line<<":"<<n.column;
    if constexpr(std::is_same_v<T,ConstantInt>)out<<"%"<<n.out<<" = const.int "<<n.value<<" : "<<type_name(n.type);
    if constexpr(std::is_same_v<T,ConstantFloat>)out<<"%"<<n.out<<" = const.float "<<n.value<<" : "<<type_name(n.type);
    if constexpr(std::is_same_v<T,ConstantExact>)out<<"%"<<n.out<<" = const.exact "<<n.spelling<<" : "<<type_name(n.type);
    if constexpr(std::is_same_v<T,ConstantBool>)out<<"%"<<n.out<<" = const.bool "<<(n.value?"true":"false");
    if constexpr(std::is_same_v<T,ConstantString>)out<<"%"<<n.out<<" = const.string \""<<n.value<<"\"";
    if constexpr(std::is_same_v<T,ArrayMake>)out<<"%"<<n.out<<" = array.make "<<type_name(n.type);
    if constexpr(std::is_same_v<T,ArrayAlloc>)out<<"%"<<n.out<<" = array.alloc %"<<n.length;
    if constexpr(std::is_same_v<T,ClassMake>)out<<"%"<<n.out<<" = class.make "<<type_name(n.type);
    if constexpr(std::is_same_v<T,FieldGet>)out<<"%"<<n.out<<" = field.get %"<<n.object<<", "<<n.index;
    if constexpr(std::is_same_v<T,FieldSet>)out<<"field.set %"<<n.object<<", "<<n.index<<", %"<<n.value;
    if constexpr(std::is_same_v<T,DeclareLocal>)out<<"local "<<n.name<<" : "<<type_name(n.type);
    if constexpr(std::is_same_v<T,DeclareReference>)out<<(n.is_const?"const reference ":"reference ")<<n.name<<" : "<<type_name(n.type);
    if constexpr(std::is_same_v<T,AddressLocal>)out<<"%"<<n.out<<" = address.local "<<n.name;
    if constexpr(std::is_same_v<T,AddressField>)out<<"%"<<n.out<<" = address.field %"<<n.object<<", "<<n.index;
    if constexpr(std::is_same_v<T,AddressElement>)out<<"%"<<n.out<<" = address.element %"<<n.array<<", %"<<n.index<<" : "<<type_name(n.element_type);
    if constexpr(std::is_same_v<T,LoadAddress>)out<<"%"<<n.out<<" = address.load %"<<n.address;
    if constexpr(std::is_same_v<T,StoreAddress>)out<<"address.store %"<<n.address<<", %"<<n.value<<" : "<<type_name(n.type);
    if constexpr(std::is_same_v<T,BindReference>)out<<"reference.bind "<<n.name<<", %"<<n.address;
    if constexpr(std::is_same_v<T,ReferenceAddress>)out<<"%"<<n.out<<" = reference.address "<<n.name;
    if constexpr(std::is_same_v<T,LoadReference>)out<<"%"<<n.out<<" = reference.load "<<n.name<<" : "<<type_name(n.type);
    if constexpr(std::is_same_v<T,StoreReference>)out<<"reference.store "<<n.name<<", %"<<n.value<<" : "<<type_name(n.type);
    if constexpr(std::is_same_v<T,ArrayLength>)out<<"%"<<n.out<<" = array.length %"<<n.array;
    if constexpr(std::is_same_v<T,ArrayCanAppendMove>)out<<"%"<<n.out<<" = array.can_append_move %"<<n.array;
    if constexpr(std::is_same_v<T,ArrayGrowMove>)out<<"%"<<n.out<<" = array.grow_move %"<<n.array<<" : "<<type_name(n.array_type);
    if constexpr(std::is_same_v<T,ArraySorted>)out<<"%"<<n.out<<" = array.sorted %"<<n.array<<" : "<<type_name(n.array_type);
    if constexpr(std::is_same_v<T,StringIndex>)out<<"%"<<n.out<<" = string.index %"<<n.text<<", %"<<n.index;
    if constexpr(std::is_same_v<T,StringIndexAsciiCompare>)out<<"%"<<n.out<<" = string.index_ascii_compare %"<<n.text<<", %"<<n.index<<", "<<static_cast<unsigned>(n.byte)<<(n.negate?" !=":" ==");
    if constexpr(std::is_same_v<T,StringAsciiCountPrefix>)out<<"%"<<n.out<<" = string.ascii_count_prefix %"<<n.text<<", %"<<n.count<<", %"<<n.initial<<", "<<static_cast<unsigned>(n.byte)<<(n.negate?" !=":" ==");
    if constexpr(std::is_same_v<T,StringLength>)out<<"%"<<n.out<<" = string.length %"<<n.text;
    if constexpr(std::is_same_v<T,StringEmpty>)out<<"%"<<n.out<<" = string."<<(n.negate?"nonempty ":"empty ")<<"%"<<n.text;
    if constexpr(std::is_same_v<T,StringContains>)out<<"%"<<n.out<<" = string.contains %"<<n.text<<", %"<<n.needle;
    if constexpr(std::is_same_v<T,StringStartsWith>)out<<"%"<<n.out<<" = string.starts_with %"<<n.text<<", %"<<n.prefix;
    if constexpr(std::is_same_v<T,StringEndsWith>)out<<"%"<<n.out<<" = string.ends_with %"<<n.text<<", %"<<n.suffix;
    if constexpr(std::is_same_v<T,StringFind>)out<<"%"<<n.out<<" = string.find %"<<n.text<<", %"<<n.needle;
    if constexpr(std::is_same_v<T,StringSlice>)out<<"%"<<n.out<<" = string.slice %"<<n.text<<", %"<<n.start<<", %"<<n.end;
    if constexpr(std::is_same_v<T,StringTrim>)out<<"%"<<n.out<<" = string.trim %"<<n.text;
    if constexpr(std::is_same_v<T,StringSplit>)out<<"%"<<n.out<<" = string.split %"<<n.text<<", %"<<n.separator;
    if constexpr(std::is_same_v<T,StringSplitIterBegin>)out<<"%"<<n.out<<" = string.split_iter.begin %"<<n.text<<", %"<<n.separator<<(n.move_source?" move":"");
    if constexpr(std::is_same_v<T,StringSplitIterNext>)out<<"%"<<n.text<<", %"<<n.has_value<<" = string.split_iter.next %"<<n.cursor;
    if constexpr(std::is_same_v<T,StringSplitIterEnd>)out<<"string.split_iter.end %"<<n.cursor;
    if constexpr(std::is_same_v<T,StringParseTwoSigned>)out<<"%"<<n.left<<", %"<<n.right<<", %"<<n.ok<<" = string.parse_two_signed %"<<n.text<<", "<<static_cast<unsigned>(n.separator);
    if constexpr(std::is_same_v<T,StringUtf8>)out<<"%"<<n.out<<" = string.utf8 %"<<n.text;
    if constexpr(std::is_same_v<T,StringFromUtf8>)out<<"%"<<n.out<<" = string.from_utf8 %"<<n.bin;
    if constexpr(std::is_same_v<T,StringFromUtf8ArrayDirect>)out<<"%"<<n.text<<", %"<<n.ok<<", %"<<n.error<<" = string.from_utf8_array_direct %"<<n.array;
    if constexpr(std::is_same_v<T,StringCodepoints>)out<<"%"<<n.out<<" = string.codepoints %"<<n.text;
    if constexpr(std::is_same_v<T,StringJoin>)out<<"%"<<n.out<<" = string.join %"<<n.values<<", %"<<n.separator;
    if constexpr(std::is_same_v<T,StringConcat>){out<<"%"<<n.out<<" = string.concat";for(const auto value:n.values)out<<" %"<<value;}
    if constexpr(std::is_same_v<T,StringBuild>){out<<"%"<<n.out<<" = string.build";for(const auto& part:n.parts)out<<" %"<<part.value<<":"<<type_name(part.type);out<<" sep %"<<n.separator;}
    if constexpr(std::is_same_v<T,StringBuildAppendMove>){out<<"%"<<n.out<<", %"<<n.added_length<<" = string.build_append %"<<n.text;for(const auto& part:n.parts)out<<" %"<<part.value<<":"<<type_name(part.type);out<<" sep %"<<n.separator;}
    if constexpr(std::is_same_v<T,StringCanAppendMove>)out<<"%"<<n.out<<" = string.can_append_move %"<<n.text;
    if constexpr(std::is_same_v<T,StringAppendMove>){out<<"%"<<n.out<<" = string.append_move %"<<n.text;for(const auto value:n.suffixes)out<<" %"<<value;}
    if constexpr(std::is_same_v<T,BinAlloc>)out<<"%"<<n.out<<" = bin.alloc %"<<n.length<<", %"<<n.fill;
    if constexpr(std::is_same_v<T,BinLength>)out<<"%"<<n.out<<" = bin.length %"<<n.bin;
    if constexpr(std::is_same_v<T,BinGet>)out<<"%"<<n.out<<" = bin.get %"<<n.bin<<", %"<<n.index;
    if constexpr(std::is_same_v<T,BinSet>)out<<"bin.set %"<<n.bin<<", %"<<n.index<<", %"<<n.value;
    if constexpr(std::is_same_v<T,ParseBin>)out<<"%"<<n.out<<" = bin.parse %"<<n.text<<" : "<<type_name(n.result_type)<<(n.success_proven?" success-proven":"");
    if constexpr(std::is_same_v<T,NumericConvert>)out<<"%"<<n.out<<" = convert %"<<n.value<<" : "<<type_name(n.source_type)<<" -> "<<type_name(n.target_type)<<(n.checked_range?" checked":"")<<(n.inline_proven?" inline":"");
    if constexpr(std::is_same_v<T,FallibleNumericConvert>)out<<"%"<<n.out<<" = convert.fallible %"<<n.value<<" : "<<type_name(n.source_type)<<" -> "<<type_name(n.result_type);
    if constexpr(std::is_same_v<T,TensorCreate>)out<<"%"<<n.out<<" = tensor.create %"<<n.shape<<" : "<<type_name(n.type)<<" init="<<(n.fill_mode==tensor_fill_mode::uninitialized?"uninitialized":n.fill_mode==tensor_fill_mode::zeros?"zeros":"ones")<<(n.gpu?" gpu=%"+std::to_string(*n.gpu):" cpu");
    if constexpr(std::is_same_v<T,TensorTransfer>)out<<"%"<<n.out<<" = tensor."<<(n.gpu?"gpu":"cpu")<<" %"<<n.tensor<<(n.gpu?", %"+std::to_string(*n.gpu):"")<<" : "<<type_name(n.type);
    if constexpr(std::is_same_v<T,TensorReshape>)out<<"%"<<n.out<<" = tensor.reshape %"<<n.tensor<<", %"<<n.shape<<" : "<<type_name(n.type);
    if constexpr(std::is_same_v<T,TensorTranspose>)out<<"%"<<n.out<<" = tensor.transpose %"<<n.tensor<<", %"<<n.axis0<<", %"<<n.axis1<<" : "<<type_name(n.type);
    if constexpr(std::is_same_v<T,TensorContiguous>)out<<"%"<<n.out<<" = tensor.contiguous %"<<n.tensor<<" : "<<type_name(n.type);
    if constexpr(std::is_same_v<T,TensorGather>)out<<"%"<<n.out<<" = tensor.gather %"<<n.tensor<<", %"<<n.indices<<", %"<<n.shape<<" : "<<type_name(n.type);
    if constexpr(std::is_same_v<T,TensorScatter>)out<<"%"<<n.out<<" = tensor.scatter %"<<n.tensor<<", %"<<n.indices<<", %"<<n.shape<<" : "<<type_name(n.type);
    if constexpr(std::is_same_v<T,TensorShape>)out<<"%"<<n.out<<" = tensor.shape %"<<n.tensor<<" : "<<type_name(n.type);
    if constexpr(std::is_same_v<T,TensorDevice>)out<<"%"<<n.out<<" = tensor.device %"<<n.tensor;
    if constexpr(std::is_same_v<T,TensorIsContiguous>)out<<"%"<<n.out<<" = tensor.is_contiguous %"<<n.tensor;
    if constexpr(std::is_same_v<T,TensorIsTracked>)out<<"%"<<n.out<<" = tensor.is_tracked %"<<n.tensor;
    if constexpr(std::is_same_v<T,TensorHasGrad>)out<<"%"<<n.out<<" = tensor.has_grad %"<<n.tensor;
    if constexpr(std::is_same_v<T,TensorClearGrad>)out<<"tensor.clear_grad %"<<n.tensor;
    if constexpr(std::is_same_v<T,TensorItem>)out<<"%"<<n.out<<" = tensor.item %"<<n.tensor<<" : "<<type_name(n.element_type);
    if constexpr(std::is_same_v<T,TensorTrack>)out<<"%"<<n.out<<" = tensor."<<(n.mode==tensor_track_mode::untrack?"untrack":n.mode==tensor_track_mode::track?"track":"retrack")<<" %"<<n.tensor<<(n.target?" target %"+std::to_string(n.target):"");
    if constexpr(std::is_same_v<T,TensorBackward>){out<<"tensor.backward %"<<n.tensor;for(const auto&target:n.targets)out<<" "<<(target.autograd_target?"target":"tensor")<<" %"<<target.value;}
    if constexpr(std::is_same_v<T,TensorGrad>)out<<"%"<<n.out<<" = tensor.grad %"<<n.tensor;
    if constexpr(std::is_same_v<T,TensorBinary>)out<<"%"<<n.out<<" = tensor.binary "<<n.op<<" %"<<n.left<<", %"<<n.right<<" : "<<type_name(n.result_type);
    if constexpr(std::is_same_v<T,TensorIndex>){
        out<<"%"<<n.out<<" = tensor.index %"<<n.tensor;
        for(const auto& item:n.items){
            if(item.slice){
                out<<" [";
                if(item.start)out<<"%"<<*item.start;
                out<<":";
                if(item.stop)out<<"%"<<*item.stop;
                if(item.step)out<<":%"<<*item.step;
                out<<"]";
            }else if(item.index){
                out<<" %"<<*item.index;
            }
        }
        out<<" : "<<type_name(n.type);
    }
    if constexpr(std::is_same_v<T,ArrayNumericCast>)out<<"%"<<n.out<<" = array.numeric_cast %"<<n.array<<" : "<<type_name(n.source_type)<<" -> "<<type_name(n.target_type);
    if constexpr(std::is_same_v<T,TensorCast>)out<<"%"<<n.out<<" = tensor.numeric_cast %"<<n.tensor<<" : "<<type_name(n.source_type)<<" -> "<<type_name(n.target_type);
    if constexpr(std::is_same_v<T,ShapedConstraintCheck>)out<<"shape.constraint %"<<n.value<<" rank="<<n.extents.size();
    if constexpr(std::is_same_v<T,ExtentEqualCheck>)out<<"extent.check %"<<n.actual<<", %"<<n.expected;
    if constexpr(std::is_same_v<T,ParseNumber>)out<<"%"<<n.out<<" = parse %"<<n.text<<" : "<<type_name(n.target_type)<<" -> "<<type_name(n.result_type);
    if constexpr(std::is_same_v<T,ParseNumberDirect>)out<<"%"<<n.value_out<<", %"<<n.ok_out<<", %"<<n.error_out<<" = parse.direct %"<<n.text<<" : "<<type_name(n.target_type);
    if constexpr(std::is_same_v<T,ExactAtom>)out<<"%"<<n.out<<" = exact.atom "<<n.provider<<":"<<n.opcode<<" : "<<type_name(n.type);
    if constexpr(std::is_same_v<T,ExactUnary>)out<<"%"<<n.out<<" = exact.unary "<<n.provider<<":"<<n.opcode<<" %"<<n.input<<" : "<<type_name(n.type);
    if constexpr(std::is_same_v<T,ArrayInitializationComplete>)out<<"%"<<n.out<<" = array.initialization.complete %"<<n.array;
    if constexpr(std::is_same_v<T,ArrayGet>)out<<"%"<<n.out<<" = array.get %"<<n.array<<", %"<<n.index<<(n.bounds_guard?" bounds-guard %"+std::to_string(*n.bounds_guard):"");
    if constexpr(std::is_same_v<T,ArraySet>)out<<"array.set %"<<n.array<<", %"<<n.index<<", %"<<n.value<<(n.bounds_guard?" bounds-guard %"+std::to_string(*n.bounds_guard):"");
    if constexpr(std::is_same_v<T,Clone>)out<<"%"<<n.out<<" = clone %"<<n.value<<" : "<<type_name(n.type);
    if constexpr(std::is_same_v<T,Retain>)out<<"%"<<n.out<<" = retain %"<<n.value<<" : "<<type_name(n.type);
    if constexpr(std::is_same_v<T,Release>)out<<"release %"<<n.value<<" : "<<type_name(n.type);
    if constexpr(std::is_same_v<T,Unary>)out<<"%"<<n.out<<" = "<<n.op<<" %"<<n.operand<<(n.inline_proven?" inline":"");
    if constexpr(std::is_same_v<T,Binary>)out<<"%"<<n.out<<" = "<<n.op<<" %"<<n.left<<", %"<<n.right<<(n.overflow_proven?" no-overflow":"")<<(n.inline_proven?" inline":"");
    if constexpr(std::is_same_v<T,ToString>)out<<"%"<<n.out<<" = text %"<<n.value<<" : "<<type_name(n.source_type);
    if constexpr(std::is_same_v<T,FormatNumber>){
        out<<"%"<<n.out<<" = format %"<<n.value<<" : "<<type_name(n.source_type);
        if(n.integer_width)out<<" int="<<*n.integer_width;
        if(n.fractional_digits)out<<" frac="<<*n.fractional_digits;
        if(n.significant_digits)out<<" sig="<<*n.significant_digits;
        if(n.zero)out<<" zero";
    }
    if constexpr(std::is_same_v<T,LoadLocal>)out<<"%"<<n.out<<" = load "<<n.name;
    if constexpr(std::is_same_v<T,StoreLocal>)out<<(n.borrowed?"store.borrow ":"store ")<<n.name<<", %"<<n.value;
    if constexpr(std::is_same_v<T,FunctionRef>)out<<"%"<<n.out<<" = fn.ref "<<n.function<<" : "<<type_name(n.type);
    if constexpr(std::is_same_v<T,IndirectCall>){
        if(n.result.kind!=TypeKind::Void&&n.result.kind!=TypeKind::Never)out<<"%"<<n.out<<" = ";
        out<<"call.indirect %"<<n.callee<<"(";
        for(std::size_t index=0;index<n.args.size();++index){if(index)out<<", ";out<<"%"<<n.args[index];}
        out<<")";
    }
    if constexpr(std::is_same_v<T,Call>){
        if(n.result.kind!=TypeKind::Void&&n.result.kind!=TypeKind::Never)out<<"%"<<n.out<<" = ";
        out<<"call "<<n.callee<<"(";
        for(std::size_t index=0;index<n.args.size();++index){
            if(index)out<<", ";
            if(n.args[index].writable_address)out<<"&%"<<*n.args[index].writable_address;
            else out<<"%"<<n.args[index].value;
        }
        out<<")";
    }
    if constexpr(std::is_same_v<T,VariantMake>)out<<"%"<<n.out<<" = variant "<<n.tag;
    if constexpr(std::is_same_v<T,VariantTag>)out<<"%"<<n.out<<" = variant.tag %"<<n.container;
    if constexpr(std::is_same_v<T,VariantPayload>)out<<"%"<<n.out<<" = variant.payload %"<<n.container;
    if constexpr(std::is_same_v<T,Print>)out<<"%"<<n.out<<" = print %"<<n.value<<" : "<<type_name(n.type);
    if constexpr(std::is_same_v<T,ReplDisplay>)out<<"repl.display %"<<n.value<<" : "<<type_name(n.type);
    if constexpr(std::is_same_v<T,ReplReplayMode>)out<<"repl.replay "<<(n.active?"on":"off");
    if constexpr(std::is_same_v<T,Input>)out<<"%"<<n.out<<" = input";
    if constexpr(std::is_same_v<T,Exit>)out<<"exit %"<<n.status;
    if constexpr(std::is_same_v<T,FailError>)out<<"fail.error %"<<n.error;
    if constexpr(std::is_same_v<T,RangeCheckStep>)out<<"range.check_step %"<<n.step;
    if constexpr(std::is_same_v<T,Return>)out<<"return %"<<n.value;
    if constexpr(std::is_same_v<T,ReturnVoid>)out<<"return";
    if constexpr(std::is_same_v<T,Jump>)out<<"jump "<<n.target;
    if constexpr(std::is_same_v<T,Branch>)out<<"branch %"<<n.condition<<", "<<n.if_true<<", "<<n.if_false;
    if constexpr(std::is_same_v<T,Pin>)out<<"pin %"<<n.value;
    if constexpr(std::is_same_v<T,Unpin>)out<<"unpin %"<<n.value;
    if constexpr(std::is_same_v<T,IterationShapeCheck>)out<<"for.check_shape %"<<n.array<<", %"<<n.entry_array<<", %"<<n.entry_length;
    if constexpr(std::is_same_v<T,InitializedCheck>)out<<"init.check %"<<n.flag<<", "<<(n.subject==InitializedSubject::binding?"binding":n.subject==InitializedSubject::field?"field":"argument")<<" '"<<n.path<<"'";
},i);return out.str();}

} // namespace quidra::ir
