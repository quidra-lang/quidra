#include "quidra/llvm_backend.hpp"
#include "operator_policy.hpp"
#include <stdexcept>
#include <cctype>
#include <iomanip>
#include <bit>
#include <functional>
#include <filesystem>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace quidra {
namespace {

std::string llvm_type(const Type& t) {
    switch(t.kind){
        case TypeKind::Int: case TypeKind::UInt64: return "i64";
        case TypeKind::Int8: case TypeKind::UInt8: return "i8";
        case TypeKind::Int16: case TypeKind::UInt16: return "i16";
        case TypeKind::Int32: case TypeKind::UInt32: return "i32";
        case TypeKind::Float: return "double";
        case TypeKind::Float32: return "float";
        case TypeKind::Bool: return "i1";
        case TypeKind::BigInt: case TypeKind::BigReal:
        case TypeKind::String: case TypeKind::Bin: case TypeKind::Error:
        case TypeKind::Array: case TypeKind::Tensor: case TypeKind::Neural:
        case TypeKind::Gradients: case TypeKind::Union: case TypeKind::Class: return "ptr";
        case TypeKind::None: case TypeKind::Void: case TypeKind::Never: return "void";
        case TypeKind::Auto: case TypeKind::Range: case TypeKind::Invalid: return "void";
    }
    return "void";
}

std::string c_abi_return_attribute(const Type& type) {
    switch (type.kind) {
        case TypeKind::Int8:
        case TypeKind::Int16:
            return "signext ";
        case TypeKind::UInt8:
        case TypeKind::UInt16:
        case TypeKind::Bool:
            return "zeroext ";
        default:
            return "";
    }
}

std::string c_abi_parameter_attribute(const Type& type) {
    switch (type.kind) {
        case TypeKind::Int8:
        case TypeKind::Int16:
            return " signext";
        case TypeKind::UInt8:
        case TypeKind::UInt16:
        case TypeKind::Bool:
            return " zeroext";
        case TypeKind::String:
        case TypeKind::Bin:
            return " nocapture nonnull readonly";
        default:
            return "";
    }
}

std::string escape_bytes(const std::string& s){std::ostringstream out;for(char raw:s){auto c=static_cast<unsigned char>(raw);if(c>=32&&c<=126&&c!='"'&&c!='\\')out<<static_cast<char>(c);else out<<'\\'<<std::uppercase<<std::hex<<std::setw(2)<<std::setfill('0')<<static_cast<int>(c)<<std::dec;}out<<"\\00";return out.str();}
std::string escape_metadata(std::string_view s){std::ostringstream out;for(char raw:s){auto c=static_cast<unsigned char>(raw);if(c>=32&&c<=126&&c!='"'&&c!='\\')out<<static_cast<char>(c);else out<<'\\'<<std::uppercase<<std::hex<<std::setw(2)<<std::setfill('0')<<static_cast<int>(c)<<std::dec;}return out.str();}
std::string attach_debug_location(std::string text,std::size_t location) {
    auto position=text.find('\n');
    if(position==std::string::npos) return text;
    position+=1;
    bool saw_label=false;
    while(position<text.size()) {
        const auto end=text.find('\n',position);
        if(end==std::string::npos) break;
        const std::string_view line(text.data()+position,end-position);
        if(!saw_label) {
            if(!line.empty()&&line.back()==':') saw_label=true;
        } else if(line.size()>=2&&line[0]==' '&&line[1]==' '&&
                  line.find_first_not_of(' ')!=std::string_view::npos) {
            text.insert(end,", !dbg !"+std::to_string(location));
            break;
        }
        position=end+1;
    }
    return text;
}
std::string mangle(std::string name){std::string out="n_";for(char c:name)out+=(std::isalnum(static_cast<unsigned char>(c))||c=='_')?c:'_';return out;}
std::string local_id(std::string name){std::string out;for(char c:name)out+=(std::isalnum(static_cast<unsigned char>(c))||c=='_')?c:'_';return out;}
std::string type_id(const Type& t){return local_id(type_name(t));}

int tensor_binary_opcode(std::string_view op) {
    if (op == "+") return 1;
    if (op == "-") return 2;
    if (op == "*") return 3;
    if (op == "/") return 4;
    if (op == "%") return 5;
    throw std::logic_error("unsupported tensor binary operator");
}

int tensor_dtype_code(const Type& t) {
    switch (t.kind) {
        case TypeKind::Int: return 1;
        case TypeKind::Int8: return 2;
        case TypeKind::Int16: return 3;
        case TypeKind::Int32: return 4;
        case TypeKind::UInt8: return 5;
        case TypeKind::UInt16: return 6;
        case TypeKind::UInt32: return 7;
        case TypeKind::UInt64: return 8;
        case TypeKind::Float: return 9;
        case TypeKind::Float32: return 10;
        default: break;
    }
    throw std::logic_error("unsupported tensor element type");
}

int sortable_kind_code(const Type& type) {
    if (is_tensor_numeric(type)) return tensor_dtype_code(type);
    if (type.kind == TypeKind::Bool) return 11;
    if (type.kind == TypeKind::String) return 12;
    throw std::logic_error("unsupported sorted array element type");
}

int neural_state_kind_code(const Type& type) {
    if (is_tensor_numeric(type)) return tensor_dtype_code(type);
    if (type.kind == TypeKind::Bool) return 11;
    if (type.kind == TypeKind::String) return 12;
    if (type.kind == TypeKind::Bin) return 13;
    if (type.kind == TypeKind::Tensor) return 14;
    throw std::logic_error("unsupported .quistate leaf type");
}

bool is_fixed_array(const Type& type) {
    return type.kind == TypeKind::Array && type.length >= 0;
}

struct ArrayLayoutPolicy {
    std::unordered_set<std::string> boxed_fixed_edges;

    bool inline_fixed_child(const Type& parent) const {
        return is_fixed_array(parent) && parent.first && is_fixed_array(*parent.first) &&
               !boxed_fixed_edges.contains(type_id(parent));
    }
};

std::size_t checked_storage_product(std::size_t left, std::size_t right) {
    if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
        throw std::overflow_error("fixed array storage size overflow");
    }
    return left * right;
}

struct FixedArrayStorage {
    Type leaf;
    std::size_t count{};
};

FixedArrayStorage fixed_array_storage(const Type& type, const ArrayLayoutPolicy& policy) {
    if (!is_fixed_array(type)) throw std::logic_error("fixed array layout requested for dynamic type");
    std::size_t count = 1;
    const Type* current = &type;
    while (true) {
        count = checked_storage_product(
            count, static_cast<std::size_t>(current->length));
        const auto& element = *current->first;
        if (is_fixed_array(element) && policy.inline_fixed_child(*current)) {
            current = &element;
            continue;
        }
        return FixedArrayStorage{element, count};
    }
}

std::size_t fixed_array_storage_bytes(const Type& type, const ArrayLayoutPolicy& policy) {
    const auto storage = fixed_array_storage(type, policy);
    const auto stride = runtime_storage_bytes(storage.leaf);
    if (stride == 0) throw std::logic_error("fixed array leaf has no runtime storage");
    return checked_storage_product(storage.count, stride);
}

std::size_t array_element_stride(const Type& array, const ArrayLayoutPolicy& policy) {
    if (policy.inline_fixed_child(array)) {
        return fixed_array_storage_bytes(*array.first, policy);
    }
    return runtime_storage_bytes(*array.first);
}

ArrayLayoutPolicy collect_array_layout_policy(const ir::Module& module) {
    ArrayLayoutPolicy policy;
    for (const auto& function : module.functions) {
        for (const auto& block : function.blocks) {
            for (const auto& instruction : block.instructions) {
                if (const auto* address = std::get_if<ir::AddressElement>(&instruction);
                    address && is_fixed_array(address->array_type) &&
                    is_fixed_array(address->element_type)) {
                    // A writable reference to a logical fixed-array element needs a stable
                    // pointer slot. Keep exactly this fixed edge boxed; other fixed edges
                    // remain eligible for contiguous inlining.
                    policy.boxed_fixed_edges.insert(type_id(address->array_type));
                }
            }
        }
    }
    return policy;
}


std::string float_literal(double value, const Type& type) {
    const double materialized =
        type.kind == TypeKind::Float32
            ? static_cast<double>(static_cast<float>(value))
            : value;
    const auto bits = std::bit_cast<std::uint64_t>(materialized);
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase
        << std::setw(16) << std::setfill('0') << bits;
    return out.str();
}

struct StringPool{std::vector<std::pair<std::string,std::string>>entries;std::unordered_map<std::string,std::string>names;std::string intern(const std::string&v){if(auto it=names.find(v);it!=names.end())return it->second;auto n=".str."+std::to_string(entries.size());entries.emplace_back(n,v);names.emplace(v,n);return n;}};

std::string clone_name(const Type&t){return "@quidra_clone_"+type_id(t);}
std::string array_cast_name(const Type&from,const Type&to){return "@quidra_array_cast_"+type_id(from)+"_to_"+type_id(to);}
std::string drop_name(const Type&t){return "@quidra_drop_"+type_id(t);}
std::string equality_name(const Type&t){return "@quidra_equal_"+type_id(t);}

std::size_t class_field_offset(const ir::ClassLayout& layout, std::size_t index) {
    std::size_t offset = 0;
    for (std::size_t i = 0; i < index; ++i) offset += runtime_storage_bytes(layout.fields[i]);
    return offset;
}

std::size_t class_storage_bytes(const ir::ClassLayout& layout) {
    std::size_t bytes = 0;
    for (const auto& field : layout.fields) bytes += runtime_storage_bytes(field);
    return std::max<std::size_t>(1, bytes);
}

std::unordered_set<std::string> recursive_functions(const ir::Module& module) {
    std::unordered_set<std::string> names;
    for (const auto& function : module.functions) names.insert(function.name);

    std::unordered_map<std::string, std::vector<std::string>> graph;
    for (const auto& function : module.functions) {
        auto& edges = graph[function.name];
        for (const auto& block : function.blocks) {
            for (const auto& instruction : block.instructions) {
                if (const auto* call = std::get_if<ir::Call>(&instruction);
                    call && names.contains(call->callee)) {
                    edges.push_back(call->callee);
                }
            }
        }
    }

    std::unordered_set<std::string> result;
    for (const auto& function : module.functions) {
        const auto& start = function.name;
        std::unordered_set<std::string> visited;
        std::function<bool(const std::string&)> reaches_start =
            [&](const std::string& current) {
                const auto it = graph.find(current);
                if (it == graph.end()) return false;
                for (const auto& next : it->second) {
                    if (next == start) return true;
                    if (visited.insert(next).second && reaches_start(next)) return true;
                }
                return false;
            };
        if (reaches_start(start)) result.insert(start);
    }
    return result;
}

struct FunctionEmitter {
    const ir::Function& fn;
    const std::unordered_map<std::string,FunctionType>& signatures;
    const std::unordered_map<std::string,std::string>& external_symbols;
    StringPool& pool;
    const std::unordered_map<std::string,ir::ClassLayout>& layouts;
    const ArrayLayoutPolicy& array_layout;
    std::ostringstream out;
    std::unordered_map<ir::ValueId,Type> values;
    std::map<std::string,Type> locals;
    std::map<std::string,Type> references;
    std::unordered_set<std::string> writable_params;
    std::unordered_set<std::string> borrowed_params;
    std::unordered_set<std::string> borrowed_locals;
    struct FrameScratchSlot {
        std::string name;
        std::string type;
        std::size_t alignment{};
    };
    // Instruction-local temporary storage is planned before block emission and
    // allocated once in the function entry frame. This prevents source loops
    // from accumulating stack space through repeated LLVM alloca instructions.
    std::vector<FrameScratchSlot> frame_scratch_slots;
    std::unordered_map<const ir::Instruction*,std::vector<std::size_t>> instruction_scratch_slots;
    std::string active_repl_array_index_scratch;
    bool guard_stack_depth{};
    const std::unordered_set<std::string>& recursive_callees;
    std::optional<std::size_t> debug_subprogram;
    std::size_t temp_counter{0};

    FunctionEmitter(const ir::Function& f,const std::unordered_map<std::string,FunctionType>&s,
                    const std::unordered_map<std::string,std::string>&externals,
                    StringPool&p,const std::unordered_map<std::string,ir::ClassLayout>&l,
                    const ArrayLayoutPolicy&a,bool guard,
                    const std::unordered_set<std::string>& recursive,
                    std::optional<std::size_t> debug_id=std::nullopt)
        :fn(f),signatures(s),external_symbols(externals),pool(p),layouts(l),array_layout(a),
         guard_stack_depth(guard),recursive_callees(recursive),debug_subprogram(debug_id){}
    std::string call_symbol(const std::string& name) const {
        const auto found=external_symbols.find(name);
        return found==external_symbols.end()?mangle(name):found->second;
    }
    std::string value(ir::ValueId id)const{return "%v"+std::to_string(id);}
    std::string local(const std::string&n)const{return "%local."+local_id(n);}
    std::string arg(const std::string&n)const{return "%arg."+local_id(n);}
    std::string storage(const std::string&n)const{return writable_params.contains(n)?arg(n):local(n);}
    void plan_scratch(const ir::Instruction& instruction,std::string type,std::size_t alignment=0){
        const auto index=frame_scratch_slots.size();
        frame_scratch_slots.push_back(
            FrameScratchSlot{"%scratch."+std::to_string(index),std::move(type),alignment});
        instruction_scratch_slots[&instruction].push_back(index);
    }
    const std::string& scratch(const ir::Instruction& instruction,std::size_t index=0)const{
        const auto found=instruction_scratch_slots.find(&instruction);
        if(found==instruction_scratch_slots.end()||index>=found->second.size())
            throw std::logic_error("missing planned function scratch slot");
        return frame_scratch_slots.at(found->second[index]).name;
    }
    void emit_entry_scratch(){
        for(const auto& slot:frame_scratch_slots){
            out<<"  "<<slot.name<<" = alloca "<<slot.type;
            if(slot.alignment) out<<", align "<<slot.alignment;
            out<<"\n";
        }
    }
    std::string temp(const std::string&prefix){return "%"+prefix+"."+std::to_string(temp_counter++);}
    std::string unique_label(const std::string&prefix){return prefix+"."+std::to_string(temp_counter++);}
    void fail_if(const std::string& condition,const std::string& code,const std::string& message,
                 const std::string& prefix,std::uint32_t line=0,std::uint32_t column=0){
        const auto bad=unique_label(prefix+".fail"),ok=unique_label(prefix+".ok");
        out<<"  br i1 "<<condition<<", label %"<<bad<<", label %"<<ok<<"\n";
        out<<bad<<":\n  call void @quidra_fail_at(ptr "<<code<<", ptr "<<message
           <<", i64 "<<line<<", i64 "<<column<<")\n  unreachable\n";
        out<<ok<<":\n";
    }

    void emit_void_error_result(ir::ValueId out_id, const Type& result_type,
                                const std::string& success, const std::string& prefix) {
        values[out_id]=result_type;
        const auto result=value(out_id);
        out<<"  "<<result<<" = call ptr @quidra_alloc(i64 16)\n";
        const auto ok=unique_label(prefix+".ok"),bad=unique_label(prefix+".error"),done=unique_label(prefix+".done");
        out<<"  br i1 "<<success<<", label %"<<ok<<", label %"<<bad<<"\n";
        out<<ok<<":\n  store i64 "<<case_index(result_type,Type::simple(TypeKind::Void))<<", ptr "<<result<<"\n"
           <<"  br label %"<<done<<"\n";
        out<<bad<<":\n  store i64 "<<case_index(result_type,Type::simple(TypeKind::Error))<<", ptr "<<result<<"\n";
        const auto payload=temp(prefix+".error.payload");
        out<<"  "<<payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n"
           <<"  store ptr @.err.file, ptr "<<payload<<"\n  br label %"<<done<<"\n";
        out<<done<<":\n";
    }

    void emit_string_error_result(ir::ValueId out_id, const Type& result_type,
                                  const std::string& raw, const std::string& prefix) {
        values[out_id]=result_type;
        const auto result=value(out_id),oktest=temp(prefix+".ok");
        out<<"  "<<result<<" = call ptr @quidra_alloc(i64 16)\n";
        out<<"  "<<oktest<<" = icmp ne ptr "<<raw<<", null\n";
        const auto ok=unique_label(prefix+".ok"),bad=unique_label(prefix+".error"),done=unique_label(prefix+".done");
        out<<"  br i1 "<<oktest<<", label %"<<ok<<", label %"<<bad<<"\n";
        out<<ok<<":\n  store i64 "<<case_index(result_type,Type::simple(TypeKind::String))<<", ptr "<<result<<"\n";
        const auto payload=temp(prefix+".string.payload");
        out<<"  "<<payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n"
           <<"  store ptr "<<raw<<", ptr "<<payload<<"\n  br label %"<<done<<"\n";
        out<<bad<<":\n  store i64 "<<case_index(result_type,Type::simple(TypeKind::Error))<<", ptr "<<result<<"\n";
        const auto error_payload=temp(prefix+".error.payload");
        out<<"  "<<error_payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n"
           <<"  store ptr @.err.file, ptr "<<error_payload<<"\n  br label %"<<done<<"\n";
        out<<done<<":\n";
    }

    bool has_drop_helper(const Type& type) const {
        if (type.kind == TypeKind::Bin || type.kind == TypeKind::Array ||
            type.kind == TypeKind::Tensor || type.kind == TypeKind::Neural ||
            type.kind == TypeKind::Gradients || type.kind == TypeKind::Union) return true;
        return type.kind == TypeKind::Class && layouts.contains(type.class_name);
    }

    std::string drop_callback(const Type& type) const {
        if (type.kind == TypeKind::BigInt) return "@quidra_bigint_drop";
        if (type.kind == TypeKind::BigReal) return "@quidra_bigreal_drop";
        if (type.kind == TypeKind::Class && type.class_name == "$std.json.Value") {
            return "@quidra_json_drop";
        }
        if (type.kind == TypeKind::Class && type.class_name == "$std.http.Response") {
            return "@quidra_http_response_drop";
        }
        return has_drop_helper(type) ? drop_name(type) : "null";
    }

    void release_value(const Type& type, const std::string& raw) {
        if (!requires_lifetime_management(type)) return;
        out << "  call void @quidra_managed_release(ptr " << raw
            << ", ptr " << drop_callback(type) << ")\n";
    }

    void cleanup_owned_values() {
        for (const auto& [name, _] : references) {
            const auto address = temp("cleanup.ref");
            out << "  " << address << " = load ptr, ptr " << local(name) << "\n"
                << "  call void @quidra_managed_unpin(ptr " << address << ")\n";
        }
        for (const auto& [name, type] : locals) {
            if (writable_params.contains(name) || borrowed_params.contains(name) ||
                borrowed_locals.contains(name) || !requires_lifetime_management(type)) continue;
            const auto owned = temp("cleanup.value");
            out << "  " << owned << " = load ptr, ptr " << local(name) << "\n";
            release_value(type, owned);
        }
    }

    void scan(){
        for(const auto& p:fn.parameters){
            locals[p.name]=p.type;
            if(p.writable) writable_params.insert(p.name);
            if(p.borrowed) borrowed_params.insert(p.name);
        }
        for(const auto& b:fn.blocks){
            for(const auto& i:b.instructions){
                if(const auto* s=std::get_if<ir::StoreLocal>(&i)){
                    locals[s->name]=s->type;
                    if(s->borrowed) borrowed_locals.insert(s->name);
                }
                if(const auto* d=std::get_if<ir::DeclareLocal>(&i)) locals[d->name]=d->type;
                if(const auto* r=std::get_if<ir::DeclareReference>(&i)) references[r->name]=r->type;
                if(const auto* literal=std::get_if<ir::ConstantString>(&i)) pool.intern(literal->value);
                if(const auto* exact=std::get_if<ir::ConstantExact>(&i)) pool.intern(exact->spelling);
                if(const auto* concat=std::get_if<ir::StringConcat>(&i))
                    plan_scratch(i,"["+std::to_string(concat->values.size())+" x ptr]");
                if(const auto* append=std::get_if<ir::StringAppendMove>(&i))
                    plan_scratch(i,"["+std::to_string(append->suffixes.size())+" x ptr]");
                if(std::holds_alternative<ir::NeuralMomentUpdate>(i))
                    plan_scratch(i,"[48 x i8]",8);
                if(const auto* save=std::get_if<ir::NeuralSave>(&i)){
                    for(const auto& leaf:save->values){
                        if(leaf.type.kind!=TypeKind::String&&leaf.type.kind!=TypeKind::Bin&&
                           leaf.type.kind!=TypeKind::Tensor)
                            plan_scratch(i,llvm_type(leaf.type));
                    }
                }
                if(const auto* binary=std::get_if<ir::TensorBinary>(&i)){
                    const bool left_tensor=binary->left_type.kind==TypeKind::Tensor;
                    const bool right_tensor=binary->right_type.kind==TypeKind::Tensor;
                    if(left_tensor!=right_tensor){
                        const auto& scalar_type=left_tensor?binary->right_type:binary->left_type;
                        plan_scratch(i,llvm_type(scalar_type));
                    }
                }
                if(const auto* index=std::get_if<ir::TensorIndex>(&i))
                    plan_scratch(i,"["+std::to_string(index->items.size()*4)+" x i64]",8);
                if(const auto* set=std::get_if<ir::TensorSet>(&i)){
                    plan_scratch(i,"["+std::to_string(set->indices.size())+" x i64]",8);
                    plan_scratch(i,llvm_type(set->element_type));
                }
                if(const auto* parse=std::get_if<ir::ParseNumber>(&i)){
                    if(is_integer(parse->target_type)) plan_scratch(i,"i64");
                    else if(parse->target_type.kind==TypeKind::Float32) plan_scratch(i,"float");
                    else if(parse->target_type.kind==TypeKind::Float) plan_scratch(i,"double");
                }
                if(std::holds_alternative<ir::ImageRead>(i)) plan_scratch(i,"i32");
                if(std::holds_alternative<ir::ReplDisplay>(i)) plan_scratch(i,"i64");
                if(std::holds_alternative<ir::Input>(i)) plan_scratch(i,"ptr");
            }
        }
    }

    void emit_repl_text(const std::string& text) {
        const auto literal = pool.intern(text);
        out << "  call i32 (ptr, ...) @printf(ptr @.fmt.string.write, ptr @" << literal << ")\n";
    }

    bool repl_path_initialized(const std::vector<std::string>& initialized_paths,
                               const std::string& path) const {
        return std::find(initialized_paths.begin(), initialized_paths.end(), path) !=
               initialized_paths.end();
    }

    void emit_repl_value(const Type& type, const std::string& raw_value,
                         const std::vector<std::string>& initialized_paths = {},
                         const std::string& path_prefix = {}) {
        if (is_integer(type)) {
            std::string widened = raw_value;
            if (integer_width(type) < 64) {
                widened = temp("repl.int");
                out << "  " << widened << " = "
                    << (is_signed_integer(type) ? "sext" : "zext") << " "
                    << llvm_type(type) << " " << raw_value << " to i64\n";
            }
            out << "  call i32 (ptr, ...) @printf(ptr "
                << (is_signed_integer(type) ? "@.fmt.int.write" : "@.fmt.uint.write")
                << ", i64 " << widened << ")\n";
            return;
        }
        if (type.kind == TypeKind::BigInt || type.kind == TypeKind::BigReal) {
            const auto text = temp("repl.exact.text");
            if (type.kind == TypeKind::BigInt)
                out << "  " << text << " = call ptr @quidra_bigint_text(ptr " << raw_value << ")\n";
            else
                out << "  " << text << " = call ptr @quidra_bigreal_text(ptr " << raw_value << ", i32 34)\n";
            out << "  call i32 (ptr, ...) @printf(ptr @.fmt.string.write, ptr " << text << ")\n";
            out << "  call void @quidra_managed_release(ptr " << text << ", ptr null)\n";
            return;
        }
        if (is_float(type)) {
            std::string widened = raw_value;
            if (type.kind == TypeKind::Float32) {
                widened = temp("repl.float");
                out << "  " << widened << " = fpext float " << raw_value << " to double\n";
            }
            const auto text = temp("repl.float.text");
            out << "  " << text << " = call ptr @quidra_float_text(double " << widened << ")\n";
            out << "  call i32 (ptr, ...) @printf(ptr @.fmt.string.write, ptr " << text << ")\n";
            out << "  call void @quidra_managed_release(ptr " << text << ", ptr null)\n";
            return;
        }
        if (type.kind == TypeKind::Bool) {
            const auto text = temp("repl.bool");
            out << "  " << text << " = select i1 " << raw_value
                << ", ptr @.bool.true, ptr @.bool.false\n";
            out << "  call i32 (ptr, ...) @printf(ptr @.fmt.string.write, ptr " << text << ")\n";
            return;
        }
        if (type.kind == TypeKind::String) {
            out << "  call i32 (ptr, ...) @printf(ptr @.fmt.repl.string, ptr "
                << raw_value << ")\n";
            return;
        }
        if (type.kind == TypeKind::Error) {
            out << "  call i32 (ptr, ...) @printf(ptr @.fmt.repl.error, ptr "
                << raw_value << ")\n";
            return;
        }
        if (type.kind == TypeKind::None) {
            emit_repl_text("none");
            return;
        }
        if (type.kind == TypeKind::Bin) {
            const auto text = temp("repl.bin");
            out << "  " << text << " = call ptr @quidra_bin_string(ptr " << raw_value << ")\n";
            out << "  call i32 (ptr, ...) @printf(ptr @.fmt.string.write, ptr " << text << ")\n";
            out << "  call void @quidra_managed_release(ptr " << text << ", ptr null)\n";
            return;
        }
        if (type.kind == TypeKind::Array) {
            emit_repl_text("[");
            const bool fixed=is_fixed_array(type);
            const auto len = temp("repl.array.len");
            if(active_repl_array_index_scratch.empty())
                throw std::logic_error("REPL array display requires planned scratch storage");
            const auto& index_slot = active_repl_array_index_scratch;
            const auto cond = unique_label("repl.array.cond");
            const auto body = unique_label("repl.array.body");
            const auto done = unique_label("repl.array.done");
            const auto comma = unique_label("repl.array.comma");
            const auto item = unique_label("repl.array.item");
            if(fixed) out << "  " << len << " = add i64 0, " << type.length << "\n";
            else out << "  " << len << " = load i64, ptr " << raw_value << ", align 1\n";
            out << "  store i64 0, ptr " << index_slot << "\n";
            out << "  br label %" << cond << "\n";
            out << cond << ":\n";
            const auto index = temp("repl.array.index");
            const auto more = temp("repl.array.more");
            out << "  " << index << " = load i64, ptr " << index_slot << "\n";
            out << "  " << more << " = icmp slt i64 " << index << ", " << len << "\n";
            out << "  br i1 " << more << ", label %" << body << ", label %" << done << "\n";
            out << body << ":\n";
            const auto first = temp("repl.array.first");
            out << "  " << first << " = icmp eq i64 " << index << ", 0\n";
            out << "  br i1 " << first << ", label %" << item << ", label %" << comma << "\n";
            out << comma << ":\n";
            emit_repl_text(", ");
            out << "  br label %" << item << "\n";
            out << item << ":\n";
            const auto stride = array_element_stride(type,array_layout);
            const auto byte_index = temp("repl.array.byte.index");
            const auto offset = temp("repl.array.offset");
            const auto ptr = temp("repl.array.ptr");
            const auto element = temp("repl.array.value");
            out << "  " << byte_index << " = mul i64 " << index << ", " << stride << "\n";
            if(fixed) out << "  " << offset << " = add i64 " << byte_index << ", 0\n";
            else out << "  " << offset << " = add i64 " << byte_index << ", 8\n";
            out << "  " << ptr << " = getelementptr inbounds i8, ptr " << raw_value
                << ", i64 " << offset << "\n";
            if(fixed&&array_layout.inline_fixed_child(type)){
                out << "  " << element << " = getelementptr inbounds i8, ptr " << ptr << ", i64 0\n";
            }else{
                out << "  " << element << " = load " << llvm_type(*type.first) << ", ptr "
                    << ptr << ", align 1\n";
            }
            emit_repl_value(*type.first, element);
            const auto next = temp("repl.array.next");
            out << "  " << next << " = add i64 " << index << ", 1\n";
            out << "  store i64 " << next << ", ptr " << index_slot << "\n";
            out << "  br label %" << cond << "\n";
            out << done << ":\n";
            emit_repl_text("]");
            return;
        }
        if (type.kind == TypeKind::Class) {
            const auto& layout = layouts.at(type.class_name);
            emit_repl_text(type.class_name + "(");
            for (std::size_t i = 0; i < layout.fields.size(); ++i) {
                if (i) emit_repl_text(", ");
                emit_repl_text(layout.field_names[i] + " = ");
                const auto path = path_prefix.empty()
                    ? layout.field_names[i]
                    : path_prefix + "." + layout.field_names[i];
                if (!repl_path_initialized(initialized_paths, path)) {
                    emit_repl_text("<uninitialized>");
                    continue;
                }
                const auto field_ptr = temp("repl.class.field.ptr");
                const auto field_value = temp("repl.class.field.value");
                out << "  " << field_ptr << " = getelementptr inbounds i8, ptr " << raw_value
                    << ", i64 " << class_field_offset(layout, i) << "\n";
                out << "  " << field_value << " = load " << llvm_type(layout.fields[i])
                    << ", ptr " << field_ptr << ", align 1\n";
                emit_repl_value(layout.fields[i], field_value, initialized_paths, path);
            }
            emit_repl_text(")");
            return;
        }
        if (type.kind == TypeKind::Union) {
            const auto tag = temp("repl.union.tag");
            const auto done = unique_label("repl.union.done");
            const auto invalid = unique_label("repl.union.invalid");
            std::vector<std::string> cases;
            cases.reserve(type.cases.size());
            out << "  " << tag << " = load i64, ptr " << raw_value << ", align 1\n";
            out << "  switch i64 " << tag << ", label %" << invalid << " [";
            for (std::size_t i = 0; i < type.cases.size(); ++i) {
                cases.push_back(unique_label("repl.union.case"));
                out << " i64 " << i << ", label %" << cases.back();
            }
            out << " ]\n";
            for (std::size_t i = 0; i < type.cases.size(); ++i) {
                out << cases[i] << ":\n";
                const auto& item_type = type.cases[i];
                if (item_type.kind == TypeKind::None || item_type.kind == TypeKind::Void) {
                    emit_repl_value(item_type, {});
                } else {
                    const auto payload_ptr = temp("repl.union.payload.ptr");
                    const auto payload = temp("repl.union.payload");
                    out << "  " << payload_ptr
                        << " = getelementptr inbounds i8, ptr " << raw_value << ", i64 8\n";
                    out << "  " << payload << " = load " << llvm_type(item_type) << ", ptr "
                        << payload_ptr << ", align 1\n";
                    emit_repl_value(item_type, payload);
                }
                out << "  br label %" << done << "\n";
            }
            out << invalid << ":\n";
            emit_repl_text("<invalid union>");
            out << "  br label %" << done << "\n";
            out << done << ":\n";
            return;
        }

        emit_repl_text("<" + type_name(type) + ">");
    }

    void emit_instruction(const ir::Instruction& ins){std::visit([&](const auto&n){using T=std::decay_t<decltype(n)>;
        if constexpr(std::is_same_v<T,ir::ConstantInt>){values[n.out]=n.type;out<<"  "<<value(n.out)<<" = add "<<llvm_type(n.type)<<" 0, "<<n.value<<"\n";}
        if constexpr(std::is_same_v<T,ir::ConstantFloat>){values[n.out]=n.type;out<<"  "<<value(n.out)<<" = fadd "<<llvm_type(n.type)<<" 0.000000e+00, "<<float_literal(n.value,n.type)<<"\n";}
        if constexpr(std::is_same_v<T,ir::ConstantExact>){
            values[n.out]=n.type;
            const auto g=pool.intern(n.spelling);
            out<<"  "<<value(n.out)<<" = call ptr @"<<(n.type.kind==TypeKind::BigInt?"quidra_bigint_literal":"quidra_bigreal_literal")
               <<"(ptr @"<<g<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::ConstantBool>){values[n.out]=Type::simple(TypeKind::Bool);out<<"  "<<value(n.out)<<" = xor i1 false, "<<(n.value?"true":"false")<<"\n";}
        if constexpr(std::is_same_v<T,ir::ConstantString>){values[n.out]=Type::simple(TypeKind::String);const auto g=pool.intern(n.value);out<<"  "<<value(n.out)<<" = getelementptr inbounds ["<<(n.value.size()+1)<<" x i8], ptr @"<<g<<", i64 0, i64 0\n";}
        if constexpr(std::is_same_v<T,ir::ArrayMake>){
            values[n.out]=n.type;
            const bool fixed=is_fixed_array(n.type);
            const auto stride=array_element_stride(n.type,array_layout);
            const auto bytes=fixed
                ? fixed_array_storage_bytes(n.type,array_layout)
                : 8+stride*n.elements.size();
            out<<"  "<<value(n.out)<<" = call ptr @quidra_alloc(i64 "<<bytes<<")\n";
            if(!fixed) out<<"  store i64 "<<n.elements.size()<<", ptr "<<value(n.out)<<", align 1\n";
            if(fixed){
                const auto storage=fixed_array_storage(n.type,array_layout);
                out<<"  call void @quidra_init_create(ptr "<<value(n.out)<<", i64 "<<storage.count
                   <<", i64 "<<runtime_storage_bytes(storage.leaf)<<", i64 0, i32 1)\n";
            }else{
                out<<"  call void @quidra_init_create(ptr "<<value(n.out)<<", i64 "<<n.elements.size()
                   <<", i64 "<<stride<<", i64 8, i32 1)\n";
            }
            for(std::size_t i=0;i<n.elements.size();++i){
                auto slot="%arr.slot."+std::to_string(n.out)+"."+std::to_string(i);
                const auto offset=(fixed?0:8)+stride*i;
                out<<"  "<<slot<<" = getelementptr inbounds i8, ptr "<<value(n.out)<<", i64 "<<offset<<"\n";
                if(fixed&&array_layout.inline_fixed_child(n.type)){
                    out<<"  call ptr @memcpy(ptr "<<slot<<", ptr "<<value(n.elements[i])<<", i64 "<<stride<<")\n";
                    out<<"  call void @quidra_managed_release(ptr "<<value(n.elements[i])<<", ptr null)\n";
                }else{
                    out<<"  store "<<llvm_type(*n.type.first)<<" "<<value(n.elements[i])<<", ptr "<<slot<<", align 1\n";
                }
            }
        }
        if constexpr(std::is_same_v<T,ir::DeclareLocal>){}
        if constexpr(std::is_same_v<T,ir::DeclareReference>){}
        if constexpr(std::is_same_v<T,ir::AddressLocal>){out<<"  "<<value(n.out)<<" = getelementptr inbounds i8, ptr "<<storage(n.name)<<", i64 0\n";}
        if constexpr(std::is_same_v<T,ir::AddressField>){
            const auto object_type=values.at(n.object);
            const auto& layout=layouts.at(object_type.class_name);
            out<<"  "<<value(n.out)<<" = getelementptr inbounds i8, ptr "<<value(n.object)<<", i64 "<<class_field_offset(layout,n.index)<<"\n";
        }
        if constexpr(std::is_same_v<T,ir::AddressElement>){
            const auto stride=n.bin_element?1:array_element_stride(n.array_type,array_layout);
            if(n.bin_element||!is_fixed_array(n.array_type)){
                out<<"  "<<value(n.out)<<" = call ptr @quidra_array_slot(ptr "<<value(n.array)<<", i64 "<<value(n.index)<<", i64 "<<stride<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
            }else{
                if(array_layout.inline_fixed_child(n.array_type)){
                    throw std::logic_error("address-taken fixed-array edge was not boxed");
                }
                out<<"  "<<value(n.out)<<" = call ptr @quidra_fixed_array_slot(ptr "<<value(n.array)<<", i64 "<<value(n.index)<<", i64 "<<n.array_type.length<<", i64 "<<stride<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
            }
        }
        if constexpr(std::is_same_v<T,ir::LoadAddress>){
            values[n.out]=n.type;
            if(n.type.kind!=TypeKind::Array)
                out<<"  call void @quidra_init_check(ptr "<<value(n.address)<<", i64 0, i64 0)\n";
            out<<"  "<<value(n.out)<<" = load "<<llvm_type(n.type)<<", ptr "<<value(n.address)<<", align 1\n";
        }
        if constexpr(std::is_same_v<T,ir::StoreAddress>){
            if(requires_lifetime_management(n.type)){
                auto old=temp("address.old");
                out<<"  "<<old<<" = load ptr, ptr "<<value(n.address)<<", align 1\n";
                release_value(n.type,old);
            }
            out<<"  store "<<llvm_type(n.type)<<" "<<value(n.value)<<", ptr "<<value(n.address)<<", align 1\n";
            out<<"  call void @quidra_init_mark_range(ptr "<<value(n.address)<<", i64 "<<runtime_storage_bytes(n.type)<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::BindReference>){
            const auto old=temp("ref.old");
            out<<"  "<<old<<" = load ptr, ptr "<<local(n.name)<<"\n"
               <<"  call void @quidra_managed_pin(ptr "<<value(n.address)<<")\n"
               <<"  call void @quidra_managed_unpin(ptr "<<old<<")\n"
               <<"  store ptr "<<value(n.address)<<", ptr "<<local(n.name)<<"\n";
        }
        if constexpr(std::is_same_v<T,ir::ReferenceAddress>){out<<"  "<<value(n.out)<<" = load ptr, ptr "<<local(n.name)<<"\n";}
        if constexpr(std::is_same_v<T,ir::LoadReference>){values[n.out]=n.type;auto a="%ref.load.addr."+std::to_string(n.out);out<<"  "<<a<<" = load ptr, ptr "<<local(n.name)<<"\n";if(n.type.kind!=TypeKind::Array)out<<"  call void @quidra_init_check(ptr "<<a<<", i64 0, i64 0)\n";out<<"  "<<value(n.out)<<" = load "<<llvm_type(n.type)<<", ptr "<<a<<", align 1\n";}
        if constexpr(std::is_same_v<T,ir::StoreReference>){
            auto a=temp("ref.store.addr");out<<"  "<<a<<" = load ptr, ptr "<<local(n.name)<<"\n";
            if(requires_lifetime_management(n.type)){
                auto old=temp("ref.store.old");
                out<<"  "<<old<<" = load ptr, ptr "<<a<<", align 1\n";
                release_value(n.type,old);
            }
            out<<"  store "<<llvm_type(n.type)<<" "<<value(n.value)<<", ptr "<<a<<", align 1\n";
            out<<"  call void @quidra_init_mark_range(ptr "<<a<<", i64 "<<runtime_storage_bytes(n.type)<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::ArrayAlloc>){
            values[n.out]=n.type;
            if(is_fixed_array(n.type)){
                const auto storage=fixed_array_storage(n.type,array_layout);
                const auto bytes=fixed_array_storage_bytes(n.type,array_layout);
                out<<"  "<<value(n.out)<<" = call ptr @quidra_alloc(i64 "<<bytes<<")\n";
                out<<"  call ptr @memset(ptr "<<value(n.out)<<", i32 0, i64 "<<bytes<<")\n";
                out<<"  call void @quidra_init_create(ptr "<<value(n.out)<<", i64 "<<storage.count
                   <<", i64 "<<runtime_storage_bytes(storage.leaf)<<", i64 0, i32 "<<(n.fully_initialized?1:0)<<")\n";
            }else{
                const auto stride=array_element_stride(n.type,array_layout);
                out<<"  "<<value(n.out)<<" = call ptr @quidra_array_alloc(i64 "<<value(n.length)<<", i64 "<<stride
                   <<", i32 "<<(n.fully_initialized?1:0)<<")\n";
            }
        }
        if constexpr(std::is_same_v<T,ir::ClassMake>){
            values[n.out]=n.type;
            const auto&layout=layouts.at(n.type.class_name);
            const auto bytes=class_storage_bytes(layout);
            out<<"  "<<value(n.out)<<" = call ptr @quidra_alloc(i64 "<<bytes<<")\n";
            for(std::size_t i=0;i<n.fields.size();++i){
                auto slot="%class.slot."+std::to_string(n.out)+"."+std::to_string(i);
                out<<"  "<<slot<<" = getelementptr inbounds i8, ptr "<<value(n.out)<<", i64 "<<class_field_offset(layout,i)<<"\n";
                if(n.fields[i]) out<<"  store "<<llvm_type(layout.fields[i])<<" "<<value(*n.fields[i])<<", ptr "<<slot<<", align 1\n";
                else{
                    std::string zero=is_pointer_runtime_type(layout.fields[i])?"null":is_float(layout.fields[i])?"0.000000e+00":layout.fields[i].kind==TypeKind::Bool?"false":"0";
                    out<<"  store "<<llvm_type(layout.fields[i])<<" "<<zero<<", ptr "<<slot<<", align 1\n";
                }
            }
        }
        if constexpr(std::is_same_v<T,ir::FieldGet>){
            values[n.out]=n.field_type;
            const auto& layout=layouts.at(values.at(n.object).class_name);
            auto slot="%field.get."+std::to_string(n.out);
            out<<"  "<<slot<<" = getelementptr inbounds i8, ptr "<<value(n.object)<<", i64 "<<class_field_offset(layout,n.index)<<"\n";
            out<<"  "<<value(n.out)<<" = load "<<llvm_type(n.field_type)<<", ptr "<<slot<<", align 1\n";
        }
        if constexpr(std::is_same_v<T,ir::FieldSet>){
            const auto& layout=layouts.at(values.at(n.object).class_name);
            auto slot=temp("field.set");
            out<<"  "<<slot<<" = getelementptr inbounds i8, ptr "<<value(n.object)<<", i64 "<<class_field_offset(layout,n.index)<<"\n";
            if(requires_lifetime_management(n.field_type) && !n.replace_without_release){
                auto old=temp("field.old");
                out<<"  "<<old<<" = load ptr, ptr "<<slot<<", align 1\n";
                release_value(n.field_type,old);
            }
            out<<"  store "<<llvm_type(n.field_type)<<" "<<value(n.value)<<", ptr "<<slot<<", align 1\n";
        }
        if constexpr(std::is_same_v<T,ir::ArrayLength>){
            values[n.out]=Type::simple(TypeKind::Int);
            const auto& array_type=values.at(n.array);
            if(is_fixed_array(array_type)){
                out<<"  "<<value(n.out)<<" = add i64 0, "<<array_type.length<<"\n";
            }else{
                out<<"  "<<value(n.out)<<" = load i64, ptr "<<value(n.array)<<"\n";
            }
        }
        if constexpr(std::is_same_v<T,ir::ArrayCanAppendMove>){
            values[n.out]=Type::simple(TypeKind::Bool);
            out<<"  "<<value(n.out)<<" = call i1 @quidra_array_can_append_move(ptr "
               <<value(n.array)<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::ArrayGrowMove>){
            values[n.out]=n.array_type;
            out<<"  "<<value(n.out)<<" = call ptr @quidra_array_grow_move(ptr "
               <<value(n.array)<<", i64 "
               <<array_element_stride(n.array_type,array_layout)<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::ArraySorted>){
            values[n.out]=n.array_type;
            out<<"  "<<value(n.out)<<" = call ptr @quidra_array_sorted(ptr "
               <<value(n.array)<<", i32 "<<sortable_kind_code(*n.array_type.first)
               <<", i64 "<<array_element_stride(n.array_type,array_layout)
               <<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::StringIndex>){values[n.out]=Type::simple(TypeKind::String);out<<"  "<<value(n.out)<<" = call ptr @quidra_string_index(ptr "<<value(n.text)<<", i64 "<<value(n.index)<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";}
        if constexpr(std::is_same_v<T,ir::StringLength>){values[n.out]=Type::simple(TypeKind::Int);out<<"  "<<value(n.out)<<" = call i64 @quidra_string_length(ptr "<<value(n.text)<<")\n";}
        if constexpr(std::is_same_v<T,ir::StringContains>){values[n.out]=Type::simple(TypeKind::Bool);out<<"  "<<value(n.out)<<" = call i1 @quidra_string_contains(ptr "<<value(n.text)<<", ptr "<<value(n.needle)<<")\n";}
        if constexpr(std::is_same_v<T,ir::StringStartsWith>){values[n.out]=Type::simple(TypeKind::Bool);out<<"  "<<value(n.out)<<" = call i1 @quidra_string_starts_with(ptr "<<value(n.text)<<", ptr "<<value(n.prefix)<<")\n";}
        if constexpr(std::is_same_v<T,ir::StringEndsWith>){values[n.out]=Type::simple(TypeKind::Bool);out<<"  "<<value(n.out)<<" = call i1 @quidra_string_ends_with(ptr "<<value(n.text)<<", ptr "<<value(n.suffix)<<")\n";}
        if constexpr(std::is_same_v<T,ir::StringFind>){
            values[n.out]=n.result_type;const auto raw=temp("string.find.raw"),found=temp("string.find.found"),result=value(n.out);
            out<<"  "<<raw<<" = call i64 @quidra_string_find(ptr "<<value(n.text)<<", ptr "<<value(n.needle)<<")\n";
            out<<"  "<<result<<" = call ptr @quidra_alloc(i64 16)\n";
            out<<"  "<<found<<" = icmp sge i64 "<<raw<<", 0\n";
            const auto yes=unique_label("string.find.value"),missing=unique_label("string.find.none"),done=unique_label("string.find.done");
            out<<"  br i1 "<<found<<", label %"<<yes<<", label %"<<missing<<"\n";
            out<<yes<<":\n  store i64 "<<case_index(n.result_type,Type::simple(TypeKind::Int))<<", ptr "<<result<<"\n";
            const auto payload=temp("string.find.payload");out<<"  "<<payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n  store i64 "<<raw<<", ptr "<<payload<<"\n  br label %"<<done<<"\n";
            out<<missing<<":\n  store i64 "<<case_index(n.result_type,Type::simple(TypeKind::None))<<", ptr "<<result<<"\n  br label %"<<done<<"\n";
            out<<done<<":\n";
        }
        if constexpr(std::is_same_v<T,ir::StringSlice>){values[n.out]=Type::simple(TypeKind::String);out<<"  "<<value(n.out)<<" = call ptr @quidra_string_slice(ptr "<<value(n.text)<<", i64 "<<value(n.start)<<", i64 "<<value(n.end)<<")\n";}
        if constexpr(std::is_same_v<T,ir::StringTrim>){values[n.out]=Type::simple(TypeKind::String);out<<"  "<<value(n.out)<<" = call ptr @quidra_string_trim(ptr "<<value(n.text)<<")\n";}
        if constexpr(std::is_same_v<T,ir::StringSplit>){values[n.out]=Type::array(Type::simple(TypeKind::String));out<<"  "<<value(n.out)<<" = call ptr @quidra_string_split(ptr "<<value(n.text)<<", ptr "<<value(n.separator)<<")\n";}
        if constexpr(std::is_same_v<T,ir::StringUtf8>){values[n.out]=Type::simple(TypeKind::Bin);out<<"  "<<value(n.out)<<" = call ptr @quidra_string_utf8(ptr "<<value(n.text)<<")\n";}
        if constexpr(std::is_same_v<T,ir::StringCodepoints>){values[n.out]=Type::array(Type::simple(TypeKind::Int));out<<"  "<<value(n.out)<<" = call ptr @quidra_string_codepoints(ptr "<<value(n.text)<<")\n";}
        if constexpr(std::is_same_v<T,ir::StringJoin>){values[n.out]=Type::simple(TypeKind::String);out<<"  "<<value(n.out)<<" = call ptr @quidra_string_join(ptr "<<value(n.values)<<", ptr "<<value(n.separator)<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";}
        if constexpr(std::is_same_v<T,ir::StringConcat>){
            values[n.out]=Type::simple(TypeKind::String);
            const auto& items=scratch(ins);
            for(std::size_t i=0;i<n.values.size();++i){
                const auto slot=temp("string.concat.slot");
                out<<"  "<<slot<<" = getelementptr inbounds ["<<n.values.size()
                   <<" x ptr], ptr "<<items<<", i64 0, i64 "<<i<<"\n";
                out<<"  store ptr "<<value(n.values[i])<<", ptr "<<slot<<"\n";
            }
            out<<"  "<<value(n.out)<<" = call ptr @quidra_string_concat_many(ptr "<<items<<", i64 "<<n.values.size()<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::StringCanAppendMove>){
            values[n.out]=Type::simple(TypeKind::Bool);
            out<<"  "<<value(n.out)<<" = call i1 @quidra_string_can_append_move(ptr "<<value(n.text)<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::StringAppendMove>){
            values[n.out]=Type::simple(TypeKind::String);
            const auto& items=scratch(ins);
            for(std::size_t i=0;i<n.suffixes.size();++i){
                const auto slot=temp("string.append.slot");
                out<<"  "<<slot<<" = getelementptr inbounds ["<<n.suffixes.size()
                   <<" x ptr], ptr "<<items<<", i64 0, i64 "<<i<<"\n";
                out<<"  store ptr "<<value(n.suffixes[i])<<", ptr "<<slot<<"\n";
            }
            out<<"  "<<value(n.out)<<" = call ptr @quidra_string_append_move_many(ptr "<<value(n.text)
               <<", ptr "<<items<<", i64 "<<n.suffixes.size()<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::StringRepeat>){
            values[n.out]=Type::simple(TypeKind::String);
            out<<"  "<<value(n.out)<<" = call ptr @quidra_string_repeat(i64 "<<value(n.count)
               <<", ptr "<<value(n.fill)<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::BinAlloc>){
            values[n.out]=Type::simple(TypeKind::Bin);
            out<<"  "<<value(n.out)<<" = call ptr @quidra_bin_alloc(i64 "<<value(n.length)
               <<", i64 "<<value(n.fill)<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::BinLength>){
            values[n.out]=Type::simple(TypeKind::Int);
            out<<"  "<<value(n.out)<<" = load i64, ptr "<<value(n.bin)<<", align 1\n";
        }
        if constexpr(std::is_same_v<T,ir::BinGet>){
            values[n.out]=Type::simple(TypeKind::Bin);
            out<<"  "<<value(n.out)<<" = call ptr @quidra_bin_index(ptr "<<value(n.bin)
               <<", i64 "<<value(n.index)<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::BinSet>){
            out<<"  call void @quidra_bin_set(ptr "<<value(n.bin)<<", i64 "<<value(n.index)
               <<", ptr "<<value(n.value)<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::BinSlice>){
            values[n.out]=Type::simple(TypeKind::Bin);
            out<<"  "<<value(n.out)<<" = call ptr @quidra_bin_slice(ptr "<<value(n.bin)
               <<", i64 "<<value(n.start)<<", i64 "<<value(n.end)<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::ArrayNumericCast>){
            values[n.out]=n.target_type;
            out<<"  "<<value(n.out)<<" = call ptr "<<array_cast_name(n.source_type,n.target_type)
               <<"(ptr "<<value(n.array)<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::TensorCreate>){
            values[n.out]=n.type;
            out<<"  "<<value(n.out)<<" = call ptr @quidra_tensor_create(ptr "<<value(n.shape)
               <<", i32 "<<tensor_dtype_code(*n.type.first)<<", i32 "<<n.fill_mode
               <<", i1 "<<(n.gpu?"1":"0")
               <<", i64 "<<(n.gpu?value(*n.gpu):"0")
               <<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::TensorTransfer>){
            values[n.out]=n.type;
            if(n.gpu)
                out<<"  "<<value(n.out)<<" = call ptr @quidra_tensor_to_gpu(ptr "<<value(n.tensor)
                   <<", i64 "<<value(*n.gpu)<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
            else
                out<<"  "<<value(n.out)<<" = call ptr @quidra_tensor_to_cpu(ptr "<<value(n.tensor)
                   <<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::TensorReshape>){
            values[n.out]=n.type;
            out<<"  "<<value(n.out)<<" = call ptr @quidra_tensor_reshape(ptr "<<value(n.tensor)
               <<", ptr "<<value(n.shape)<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::TensorTranspose>){
            values[n.out]=n.type;
            out<<"  "<<value(n.out)<<" = call ptr @quidra_tensor_transpose(ptr "<<value(n.tensor)
               <<", i64 "<<value(n.axis0)<<", i64 "<<value(n.axis1)
               <<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::TensorContiguous>){
            values[n.out]=n.type;
            out<<"  "<<value(n.out)<<" = call ptr @quidra_tensor_contiguous(ptr "<<value(n.tensor)<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::TensorShape>){
            values[n.out]=n.type;
            if(n.type.length>=0)
                out<<"  "<<value(n.out)<<" = call ptr @quidra_tensor_shape_fixed(ptr "<<value(n.tensor)
                   <<", i64 "<<n.type.length<<")\n";
            else
                out<<"  "<<value(n.out)<<" = call ptr @quidra_tensor_shape(ptr "<<value(n.tensor)<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::TensorIsContiguous>){
            values[n.out]=Type::simple(TypeKind::Bool);
            out<<"  "<<value(n.out)<<" = call i1 @quidra_tensor_is_contiguous(ptr "<<value(n.tensor)<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::TensorItem>){
            values[n.out]=n.element_type;
            const auto slot=temp("tensor.item");
            out<<"  "<<slot<<" = call ptr @quidra_tensor_item_ptr(ptr "<<value(n.tensor)
               <<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
            out<<"  "<<value(n.out)<<" = load "<<llvm_type(n.element_type)<<", ptr "<<slot<<", align 1\n";
        }
        if constexpr(std::is_same_v<T,ir::TensorCast>){
            values[n.out]=n.target_type;
            out<<"  "<<value(n.out)<<" = call ptr @quidra_tensor_cast(ptr "<<value(n.tensor)
               <<", i32 "<<tensor_dtype_code(*n.target_type.first)
               <<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::ShapedConstraintCheck>){
            const bool neural=n.kind==TypeKind::Neural;
            out<<"  call void @"<<(neural?"quidra_neural_rank_check":"quidra_tensor_rank_check")
               <<"(ptr "<<value(n.value)<<", i64 "<<n.extents.size()
               <<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
            for(std::size_t axis=0;axis<n.extents.size();++axis){
                if(!n.extents[axis]) continue;
                out<<"  call void @"<<(neural?"quidra_neural_extent_check":"quidra_tensor_extent_check")
                   <<"(ptr "<<value(n.value)<<", i64 "<<axis
                   <<", i64 "<<value(*n.extents[axis])
                   <<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
            }
        }
        if constexpr(std::is_same_v<T,ir::ExtentEqualCheck>){
            const auto bad=temp("extent.mismatch");
            out<<"  "<<bad<<" = icmp ne i64 "<<value(n.actual)<<", "<<value(n.expected)<<"\n";
            fail_if(bad,"@.code.shape","@.msg.shape","shape.extent",n.line,n.column);
        }
        if constexpr(std::is_same_v<T,ir::NeuralNumericCast>){
            values[n.out]=n.target_type;
            out<<"  "<<value(n.out)<<" = call ptr @quidra_neural_cast(ptr "<<value(n.value)
               <<", i32 "<<tensor_dtype_code(*n.target_type.first)
               <<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::NeuralTrack>){
            values[n.out]=n.type;
            out<<"  "<<value(n.out)<<" = call ptr @"
               <<(n.parameter?"quidra_neural_parameter_track":"quidra_neural_track")
               <<"(ptr "<<value(n.tensor)
               <<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::NeuralUntrack>){
            values[n.out]=n.type;
            out<<"  "<<value(n.out)<<" = call ptr @quidra_neural_untrack(ptr "<<value(n.value)<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::NeuralUnary>){
            values[n.out]=n.type;
            int op=n.operation==BuiltinCallable::NeuralAbsolute?1:
                n.operation==BuiltinCallable::NeuralExponential?2:
                n.operation==BuiltinCallable::NeuralLogarithm?3:
                n.operation==BuiltinCallable::NeuralMean?4:
                n.operation==BuiltinCallable::NeuralSumLast?5:6;
            if(n.type.kind==TypeKind::Neural)
                out<<"  "<<value(n.out)<<" = call ptr @quidra_neural_unary(ptr "<<value(n.value)<<", i32 "<<op<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
            else
                out<<"  "<<value(n.out)<<" = call ptr @quidra_neural_tensor_unary(ptr "<<value(n.value)<<", i32 "<<op<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::NeuralBinary>){
            values[n.out]=n.result_type;
            const bool ln=n.left_type.kind==TypeKind::Neural,rn=n.right_type.kind==TypeKind::Neural;
            const int op=n.op=="+"?1:n.op=="-"?2:n.op=="*"?3:4;
            if(ln&&rn) out<<"  "<<value(n.out)<<" = call ptr @quidra_neural_binary(ptr "<<value(n.left)<<", ptr "<<value(n.right)<<", i32 "<<op<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
            else {
                const auto scalar=ln?n.right:n.left; const auto st=ln?n.right_type:n.left_type;
                std::string sv=value(scalar);
                if(st.kind==TypeKind::Float32){auto w=temp("neural.scalar");out<<"  "<<w<<" = fpext float "<<sv<<" to double\n";sv=w;}
                else if(is_integer(st)){auto w=temp("neural.scalar");out<<"  "<<w<<" = "<<(is_signed_integer(st)?"sitofp":"uitofp")<<" "<<llvm_type(st)<<" "<<sv<<" to double\n";sv=w;}
                out<<"  "<<value(n.out)<<" = call ptr @quidra_neural_binary_scalar(ptr "<<value(ln?n.left:n.right)<<", double "<<sv<<", i32 "<<op<<", i1 "<<(ln?"false":"true")<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
            }
        }
        if constexpr(std::is_same_v<T,ir::NeuralGrad>){
            values[n.out]=Type::simple(TypeKind::Gradients);
            out<<"  "<<value(n.out)<<" = call ptr @quidra_neural_grad(ptr "<<value(n.loss)<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::NeuralConvolve2D>){
            values[n.out]=n.result_type;
            const char* fn=n.input_type.kind==TypeKind::Neural
                ?"quidra_neural_convolve2d"
                :"quidra_neural_tensor_convolve2d";
            out<<"  "<<value(n.out)<<" = call ptr @"<<fn<<"(ptr "<<value(n.input)
               <<", ptr "<<value(n.weight)<<", ptr "<<value(n.bias)
               <<", i64 "<<value(n.stride)<<", i64 "<<value(n.padding)
               <<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::NeuralAffine>){
            values[n.out]=n.result_type;
            const char* fn=n.input_type.kind==TypeKind::Neural
                ?"quidra_neural_affine"
                :"quidra_neural_tensor_affine";
            out<<"  "<<value(n.out)<<" = call ptr @"<<fn<<"(ptr "<<value(n.input)
               <<", ptr "<<value(n.weight)<<", ptr "<<value(n.bias)
               <<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::NeuralUpdate>){
            std::string matched="0";
            for(std::size_t i=0;i<n.parameters.size();++i){
                const auto has=temp("neural.update.has_gradient");
                const auto widened=temp("neural.update.matched.bit");
                const auto next=temp("neural.update.matched");
                out<<"  "<<has<<" = call i1 @quidra_neural_parameter_has_gradient(ptr "
                   <<value(n.parameters[i].value)<<", ptr "<<value(n.gradients)
                   <<", i64 "<<n.line<<", i64 "<<n.column<<")\n"
                   <<"  "<<widened<<" = zext i1 "<<has<<" to i64\n"
                   <<"  "<<next<<" = add i64 "<<matched<<", "<<widened<<"\n";
                matched=next;
            }
            out<<"  call void @quidra_neural_validate_step(ptr "<<value(n.gradients)
               <<", i64 "<<matched<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
            for(const auto& parameter:n.parameters){
                const auto did=temp("neural.update.did");
                out<<"  "<<did<<" = call i1 @quidra_neural_update_parameter(ptr "
                   <<value(parameter.value)<<", ptr "<<value(n.gradients)
                   <<", double "<<value(n.rate)
                   <<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
            }
        }
        if constexpr(std::is_same_v<T,ir::NeuralMomentUpdate>){
            const auto& state=scratch(ins);
            const auto store_state_field=[&](std::size_t offset,const char* type,ir::ValueId field){
                const auto address=temp("neural.moment_update.field");
                out<<"  "<<address<<" = getelementptr inbounds i8, ptr "<<state
                   <<", i64 "<<offset<<"\n"
                   <<"  store "<<type<<" "<<value(field)<<", ptr "<<address<<", align 8\n";
            };
            store_state_field(0,"double",n.rate);
            store_state_field(8,"double",n.beta1);
            store_state_field(16,"double",n.beta2);
            store_state_field(24,"double",n.epsilon);
            store_state_field(32,"ptr",n.step);
            store_state_field(40,"ptr",n.moments);
            std::string matched="0";
            for(std::size_t i=0;i<n.parameters.size();++i){
                const auto has=temp("neural.moment_update.has_gradient");
                const auto widened=temp("neural.moment_update.matched.bit");
                const auto next=temp("neural.moment_update.matched");
                out<<"  "<<has<<" = call i1 @quidra_neural_parameter_has_gradient(ptr "
                   <<value(n.parameters[i].value)<<", ptr "<<value(n.gradients)
                   <<", i64 "<<n.line<<", i64 "<<n.column<<")\n"
                   <<"  "<<widened<<" = zext i1 "<<has<<" to i64\n"
                   <<"  "<<next<<" = add i64 "<<matched<<", "<<widened<<"\n";
                matched=next;
            }
            out<<"  call void @quidra_neural_validate_step(ptr "<<value(n.gradients)
               <<", i64 "<<matched<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
            if(!n.parameters.empty()){
                const auto step=temp("neural.moment_update.step");
                out<<"  "<<step<<" = call i64 @quidra_neural_moment_begin(ptr "<<state
                   <<", i64 "<<n.parameters.size()<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
                std::vector<std::string> paths;
                paths.reserve(n.parameters.size());
                for(std::size_t i=0;i<n.parameters.size();++i){
                    const auto path_name=pool.intern(n.parameters[i].path);
                    const auto path_ptr=temp("neural.parameter.path");
                    out<<"  "<<path_ptr<<" = getelementptr inbounds ["
                       <<(n.parameters[i].path.size()+1)<<" x i8], ptr @"<<path_name
                       <<", i64 0, i64 0\n"
                       <<"  call void @quidra_neural_moment_validate_parameter(ptr "
                       <<value(n.parameters[i].value)<<", ptr "<<value(n.gradients)
                       <<", ptr "<<state<<", ptr "<<path_ptr
                       <<", i64 "<<i<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
                    paths.push_back(path_ptr);
                }
                for(std::size_t i=0;i<n.parameters.size();++i){
                    const auto did=temp("neural.moment_update.did");
                    out<<"  "<<did<<" = call i1 @quidra_neural_moment_update_parameter(ptr "
                       <<value(n.parameters[i].value)<<", ptr "<<value(n.gradients)
                       <<", ptr "<<state<<", ptr "<<paths[i]
                       <<", i64 "<<i<<", i64 "<<step
                       <<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
                }
                out<<"  call void @quidra_neural_moment_finish(ptr "<<state
                   <<", i64 "<<step<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
            }
        }
        if constexpr(std::is_same_v<T,ir::NeuralNormalize>){
            values[n.out]=n.result_type;
            out<<"  "<<value(n.out)<<" = call ptr @quidra_neural_normalize(ptr "
               <<value(n.input)<<", ptr "<<value(n.scale)<<", ptr "<<value(n.bias)
               <<", ptr "<<value(n.running_mean)<<", ptr "<<value(n.running_variance)
               <<", double "<<value(n.momentum)<<", double "<<value(n.epsilon)
               <<", i1 "<<(n.training?"true":"false")
               <<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::NeuralRandomMask>){
            values[n.out]=n.result_type;
            out<<"  "<<value(n.out)<<" = call ptr @quidra_neural_random_mask(ptr "
               <<value(n.input)<<", ptr "<<value(n.state)<<", double "<<value(n.rate)
               <<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::NeuralSave>){
            const auto schema_name=pool.intern(n.schema);
            const auto schema_ptr=temp("quistate.schema");
            out<<"  "<<schema_ptr<<" = getelementptr inbounds ["<<(n.schema.size()+1)
               <<" x i8], ptr @"<<schema_name<<", i64 0, i64 0\n";
            const auto context=temp("quistate.save");
            out<<"  "<<context<<" = call ptr @quidra_neural_state_save_begin(ptr "<<value(n.path)
               <<", ptr "<<schema_ptr<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
            std::size_t scalar_scratch_index=0;
            for(const auto& leaf:n.values){
                const auto leaf_name=pool.intern(leaf.path);
                const auto leaf_ptr=temp("quistate.path");
                out<<"  "<<leaf_ptr<<" = getelementptr inbounds ["<<(leaf.path.size()+1)
                   <<" x i8], ptr @"<<leaf_name<<", i64 0, i64 0\n";
                std::string raw;
                if(leaf.type.kind==TypeKind::String || leaf.type.kind==TypeKind::Bin ||
                   leaf.type.kind==TypeKind::Tensor){
                    raw=value(leaf.value);
                }else{
                    const auto& slot=scratch(ins,scalar_scratch_index++);
                    out<<"  store "<<llvm_type(leaf.type)<<" "<<value(leaf.value)
                       <<", ptr "<<slot<<", align 1\n";
                    raw=slot;
                }
                out<<"  call void @quidra_neural_state_write(ptr "<<context<<", ptr "<<leaf_ptr
                   <<", i32 "<<neural_state_kind_code(leaf.type)<<", ptr "<<raw
                   <<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
            }
            out<<"  call void @quidra_neural_state_save_finish(ptr "<<context
               <<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::NeuralLoad>){
            const auto schema_name=pool.intern(n.schema);
            const auto schema_ptr=temp("quistate.schema");
            out<<"  "<<schema_ptr<<" = getelementptr inbounds ["<<(n.schema.size()+1)
               <<" x i8], ptr @"<<schema_name<<", i64 0, i64 0\n";
            const auto context=temp("quistate.load");
            out<<"  "<<context<<" = call ptr @quidra_neural_state_load_begin(ptr "<<value(n.path)
               <<", ptr "<<schema_ptr<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
            for(const auto& target:n.targets){
                const auto leaf_name=pool.intern(target.path);
                const auto leaf_ptr=temp("quistate.path");
                out<<"  "<<leaf_ptr<<" = getelementptr inbounds ["<<(target.path.size()+1)
                   <<" x i8], ptr @"<<leaf_name<<", i64 0, i64 0\n";
                out<<"  call void @quidra_neural_state_read(ptr "<<context<<", ptr "<<leaf_ptr
                   <<", i32 "<<neural_state_kind_code(target.type)<<", ptr "<<value(target.address)
                   <<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
            }
            out<<"  call void @quidra_neural_state_load_finish(ptr "<<context
               <<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::StatsMean>){
            values[n.out]=Type::simple(TypeKind::Float);
            out<<"  "<<value(n.out)<<" = call double @quidra_stats_mean(ptr "<<value(n.tensor)
               <<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::StatsReduce>){
            values[n.out]=n.element_type;
            const int operation =
                n.operation==BuiltinCallable::StatsSum ? 1 :
                n.operation==BuiltinCallable::StatsMin ? 2 : 3;
            const auto raw=temp("stats.reduce");
            out<<"  "<<raw<<" = call ptr @quidra_stats_reduce_ptr(ptr "<<value(n.tensor)
               <<", i32 "<<tensor_dtype_code(n.element_type)
               <<", i32 "<<operation<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
            out<<"  "<<value(n.out)<<" = load "<<llvm_type(n.element_type)
               <<", ptr "<<raw<<", align 1\n";
        }
        if constexpr(std::is_same_v<T,ir::LinearMatmul>){
            values[n.out]=n.type;
            out<<"  "<<value(n.out)<<" = call ptr @quidra_linear_matmul(ptr "<<value(n.left)
               <<", ptr "<<value(n.right)<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::LinearDot>){
            values[n.out]=n.element_type;
            if(is_integer(n.element_type)){
                const auto raw=temp("linear.dot.int");
                out<<"  "<<raw<<" = call i64 @quidra_linear_dot_integer(ptr "<<value(n.left)
                   <<", ptr "<<value(n.right)<<", i32 "<<tensor_dtype_code(n.element_type)
                   <<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
                const auto ty=llvm_type(n.element_type);
                if(integer_width(n.element_type)==64)
                    out<<"  "<<value(n.out)<<" = add i64 0, "<<raw<<"\n";
                else
                    out<<"  "<<value(n.out)<<" = trunc i64 "<<raw<<" to "<<ty<<"\n";
            }else if(n.element_type.kind==TypeKind::Float32){
                out<<"  "<<value(n.out)<<" = call float @quidra_linear_dot_float32(ptr "<<value(n.left)
                   <<", ptr "<<value(n.right)<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
            }else{
                out<<"  "<<value(n.out)<<" = call double @quidra_linear_dot_float64(ptr "<<value(n.left)
                   <<", ptr "<<value(n.right)<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
            }
        }
        if constexpr(std::is_same_v<T,ir::TensorBinary>){
            values[n.out]=n.result_type;
            const bool left_tensor=n.left_type.kind==TypeKind::Tensor;
            const bool right_tensor=n.right_type.kind==TypeKind::Tensor;
            if(left_tensor && right_tensor){
                out<<"  "<<value(n.out)<<" = call ptr @quidra_tensor_binary(ptr "<<value(n.left)
                   <<", ptr "<<value(n.right)<<", ptr null, i32 0, i32 "
                   <<tensor_binary_opcode(n.op)<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
            }else{
                const auto scalar=n.left_type.kind==TypeKind::Tensor?n.right:n.left;
                const auto scalar_type=n.left_type.kind==TypeKind::Tensor?n.right_type:n.left_type;
                const auto& slot=scratch(ins);
                out<<"  store "<<llvm_type(scalar_type)<<" "<<value(scalar)<<", ptr "<<slot<<", align 1\n";
                const auto tensor=n.left_type.kind==TypeKind::Tensor?n.left:n.right;
                const int side=n.left_type.kind==TypeKind::Tensor?2:1;
                out<<"  "<<value(n.out)<<" = call ptr @quidra_tensor_binary(ptr "<<value(tensor)
                   <<", ptr null, ptr "<<slot<<", i32 "<<side<<", i32 "
                   <<tensor_binary_opcode(n.op)<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
            }
        }
        if constexpr(std::is_same_v<T,ir::TensorIndex>){
            values[n.out]=n.type;
            const auto count=n.items.size();
            const auto& specs=scratch(ins);
            constexpr long long missing=std::numeric_limits<long long>::min();
            for(std::size_t i=0;i<count;++i){
                const auto& item=n.items[i];
                const auto base=i*4;
                const auto emit_slot=[&](std::size_t offset,const std::string& value_text){
                    const auto slot=temp("tensor.index.slot");
                    out<<"  "<<slot<<" = getelementptr inbounds ["<<(count*4)
                       <<" x i64], ptr "<<specs<<", i64 0, i64 "<<(base+offset)<<"\n";
                    out<<"  store i64 "<<value_text<<", ptr "<<slot<<", align 8\n";
                };
                emit_slot(0,item.slice?"1":"0");
                if(item.slice){
                    emit_slot(1,item.start?value(*item.start):std::to_string(missing));
                    emit_slot(2,item.stop?value(*item.stop):std::to_string(missing));
                    emit_slot(3,item.step?value(*item.step):std::to_string(missing));
                }else{
                    emit_slot(1,item.index?value(*item.index):std::to_string(missing));
                    emit_slot(2,std::to_string(missing));
                    emit_slot(3,std::to_string(missing));
                }
            }
            out<<"  "<<value(n.out)<<" = call ptr @quidra_tensor_index(ptr "<<value(n.tensor)
               <<", ptr "<<specs<<", i64 "<<count<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::TensorSet>){
            const auto count=n.indices.size();
            const auto& indices=scratch(ins,0);
            for(std::size_t i=0;i<count;++i){
                const auto slot=temp("tensor.set.index");
                out<<"  "<<slot<<" = getelementptr inbounds ["<<count
                   <<" x i64], ptr "<<indices<<", i64 0, i64 "<<i<<"\n";
                out<<"  store i64 "<<value(n.indices[i])<<", ptr "<<slot<<", align 8\n";
            }
            const auto& scalar=scratch(ins,1);
            out<<"  store "<<llvm_type(n.element_type)<<" "<<value(n.value)
               <<", ptr "<<scalar<<", align 1\n";
            out<<"  call void @quidra_tensor_set(ptr "<<value(n.tensor)<<", ptr "<<indices
               <<", i64 "<<count<<", ptr "<<scalar<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::NumericConvert>){
            values[n.out]=n.target_type;
            const auto source_ty=llvm_type(n.source_type), target_ty=llvm_type(n.target_type);
            const auto source_width=integer_width(n.source_type), target_width=integer_width(n.target_type);
            auto emit_fail_check=[&](const std::vector<std::string>& conditions){
                if(conditions.empty())return;
                std::string bad=conditions.front();
                for(std::size_t i=1;i<conditions.size();++i){
                    const auto joined=temp("cast.bad");
                    out<<"  "<<joined<<" = or i1 "<<bad<<", "<<conditions[i]<<"\n";
                    bad=joined;
                }
                const auto bad_label="cast.fail."+std::to_string(temp_counter++);
                const auto ok_label="cast.ok."+std::to_string(temp_counter++);
                out<<"  br i1 "<<bad<<", label %"<<bad_label<<", label %"<<ok_label<<"\n";
                out<<bad_label<<":\n  call void @quidra_fail_at(ptr @.code.numeric.cast, ptr @.msg.numeric.cast, i64 "<<n.line<<", i64 "<<n.column<<")\n  unreachable\n";
                out<<ok_label<<":\n";
            };
            if(n.source_type==n.target_type &&
               (is_integer(n.source_type)||is_float(n.source_type))){
                out<<"  "<<value(n.out)<<" = select i1 true, "<<target_ty<<" "<<value(n.value)
                   <<", "<<target_ty<<" "<<value(n.value)<<"\n";
            }else if((n.source_type.kind==TypeKind::BigInt||n.source_type.kind==TypeKind::BigReal) &&
               n.source_type==n.target_type){
                out<<"  call void @quidra_managed_retain(ptr "<<value(n.value)<<")\n";
                out<<"  "<<value(n.out)<<" = getelementptr inbounds i8, ptr "<<value(n.value)<<", i64 0\n";
            }else if(is_integer(n.source_type)&&n.target_type.kind==TypeKind::BigInt){
                std::string widened=value(n.value);
                if(integer_width(n.source_type)<64){
                    widened=temp("cast.bigint.widen");
                    out<<"  "<<widened<<" = "<<(is_signed_integer(n.source_type)?"sext":"zext")
                       <<" "<<source_ty<<" "<<value(n.value)<<" to i64\n";
                }
                out<<"  "<<value(n.out)<<" = call ptr @"<<(is_signed_integer(n.source_type)?"quidra_bigint_from_i64":"quidra_bigint_from_u64")
                   <<"(i64 "<<widened<<")\n";
            }else if(is_integer(n.source_type)&&n.target_type.kind==TypeKind::BigReal){
                std::string widened=value(n.value);
                if(integer_width(n.source_type)<64){
                    widened=temp("cast.bigreal.widen");
                    out<<"  "<<widened<<" = "<<(is_signed_integer(n.source_type)?"sext":"zext")
                       <<" "<<source_ty<<" "<<value(n.value)<<" to i64\n";
                }
                out<<"  "<<value(n.out)<<" = call ptr @"<<(is_signed_integer(n.source_type)?"quidra_bigreal_from_i64":"quidra_bigreal_from_u64")
                   <<"(i64 "<<widened<<")\n";
            }else if(n.source_type.kind==TypeKind::BigInt&&n.target_type.kind==TypeKind::BigReal){
                out<<"  "<<value(n.out)<<" = call ptr @quidra_bigreal_from_bigint(ptr "<<value(n.value)<<")\n";
            }else if(is_float(n.source_type)&&n.target_type.kind==TypeKind::BigReal){
                std::string widened=value(n.value);
                if(n.source_type.kind==TypeKind::Float32){
                    widened=temp("cast.bigreal.float");
                    out<<"  "<<widened<<" = fpext float "<<value(n.value)<<" to double\n";
                }
                out<<"  "<<value(n.out)<<" = call ptr @quidra_bigreal_from_float64(double "<<widened<<")\n";
            }else if(n.source_type.kind==TypeKind::BigReal&&n.target_type.kind==TypeKind::BigInt){
                out<<"  "<<value(n.out)<<" = call ptr @quidra_bigreal_to_bigint(ptr "<<value(n.value)
                   <<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
            }else if(n.source_type.kind==TypeKind::BigInt&&is_integer(n.target_type)){
                const auto raw=temp("cast.bigint.integer");
                out<<"  "<<raw<<" = call i64 @"<<(is_signed_integer(n.target_type)?"quidra_bigint_to_i64":"quidra_bigint_to_u64")
                   <<"(ptr "<<value(n.value)<<", i32 "<<target_width<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
                if(target_width<64) out<<"  "<<value(n.out)<<" = trunc i64 "<<raw<<" to "<<target_ty<<"\n";
                else out<<"  "<<value(n.out)<<" = add i64 "<<raw<<", 0\n";
            }else if(n.source_type.kind==TypeKind::BigReal&&is_integer(n.target_type)){
                const auto exact=temp("cast.bigreal.integer.exact");
                const auto raw=temp("cast.bigreal.integer");
                out<<"  "<<exact<<" = call ptr @quidra_bigreal_to_bigint(ptr "<<value(n.value)
                   <<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
                out<<"  "<<raw<<" = call i64 @"<<(is_signed_integer(n.target_type)?"quidra_bigint_to_i64":"quidra_bigint_to_u64")
                   <<"(ptr "<<exact<<", i32 "<<target_width<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
                out<<"  call void @quidra_managed_release(ptr "<<exact<<", ptr @quidra_bigint_drop)\n";
                if(target_width<64) out<<"  "<<value(n.out)<<" = trunc i64 "<<raw<<" to "<<target_ty<<"\n";
                else out<<"  "<<value(n.out)<<" = add i64 "<<raw<<", 0\n";
            }else if(n.source_type.kind==TypeKind::BigInt&&is_float(n.target_type)){
                if(n.target_type.kind==TypeKind::Float32)
                    out<<"  "<<value(n.out)<<" = call float @quidra_bigint_to_float32(ptr "<<value(n.value)<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
                else
                    out<<"  "<<value(n.out)<<" = call double @quidra_bigint_to_float64(ptr "<<value(n.value)<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
            }else if(n.source_type.kind==TypeKind::BigReal&&is_float(n.target_type)){
                if(n.target_type.kind==TypeKind::Float32)
                    out<<"  "<<value(n.out)<<" = call float @quidra_bigreal_to_float32(ptr "<<value(n.value)<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
                else
                    out<<"  "<<value(n.out)<<" = call double @quidra_bigreal_to_float64(ptr "<<value(n.value)<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
            }else if(is_integer(n.source_type)&&is_integer(n.target_type)){
                std::vector<std::string> checks;
                if(n.checked_range){
                    if(is_signed_integer(n.source_type)&&!is_signed_integer(n.target_type)){
                        const auto c=temp("cast.neg");out<<"  "<<c<<" = icmp slt "<<source_ty<<" "<<value(n.value)<<", 0\n";checks.push_back(c);
                        if(target_width<source_width){
                            const unsigned long long max=(target_width==64)?~0ULL:((1ULL<<target_width)-1ULL);
                            const auto c2=temp("cast.high");out<<"  "<<c2<<" = icmp sgt "<<source_ty<<" "<<value(n.value)<<", "<<max<<"\n";checks.push_back(c2);
                        }
                    }else if(!is_signed_integer(n.source_type)&&is_signed_integer(n.target_type)){
                        if(target_width<=source_width){
                            const unsigned long long max=(target_width==64)?0x7fffffffffffffffULL:((1ULL<<(target_width-1))-1ULL);
                            const auto c=temp("cast.high");out<<"  "<<c<<" = icmp ugt "<<source_ty<<" "<<value(n.value)<<", "<<max<<"\n";checks.push_back(c);
                        }
                    }else if(is_signed_integer(n.source_type)&&is_signed_integer(n.target_type)&&target_width<source_width){
                        const long long min=-(1LL<<(target_width-1));
                        const long long max=(1LL<<(target_width-1))-1;
                        const auto lo=temp("cast.low"),hi=temp("cast.high");
                        out<<"  "<<lo<<" = icmp slt "<<source_ty<<" "<<value(n.value)<<", "<<min<<"\n";
                        out<<"  "<<hi<<" = icmp sgt "<<source_ty<<" "<<value(n.value)<<", "<<max<<"\n";
                        checks.push_back(lo);checks.push_back(hi);
                    }else if(!is_signed_integer(n.source_type)&&!is_signed_integer(n.target_type)&&target_width<source_width){
                        const unsigned long long max=(target_width==64)?~0ULL:((1ULL<<target_width)-1ULL);
                        const auto c=temp("cast.high");out<<"  "<<c<<" = icmp ugt "<<source_ty<<" "<<value(n.value)<<", "<<max<<"\n";checks.push_back(c);
                    }
                }
                emit_fail_check(checks);
                if(source_width==target_width) out<<"  "<<value(n.out)<<" = add "<<target_ty<<" "<<value(n.value)<<", 0\n";
                else if(target_width<source_width) out<<"  "<<value(n.out)<<" = trunc "<<source_ty<<" "<<value(n.value)<<" to "<<target_ty<<"\n";
                else out<<"  "<<value(n.out)<<" = "<<(is_signed_integer(n.source_type)?"sext":"zext")<<" "<<source_ty<<" "<<value(n.value)<<" to "<<target_ty<<"\n";
            }else if(is_integer(n.source_type)&&is_float(n.target_type)){
                out<<"  "<<value(n.out)<<" = "<<(is_signed_integer(n.source_type)?"sitofp":"uitofp")<<" "<<source_ty<<" "<<value(n.value)<<" to "<<target_ty<<"\n";
                if(n.checked_range){
                    const auto back=temp("cast.back");
                    const auto sat=std::string(is_signed_integer(n.source_type)?"@llvm.fptosi.sat.i":"@llvm.fptoui.sat.i")+
                        std::to_string(source_width)+(n.target_type.kind==TypeKind::Float32?".f32":".f64");
                    out<<"  "<<back<<" = call "<<source_ty<<" "<<sat<<"("<<target_ty<<" "<<value(n.out)<<")\n";
                    const auto mismatch=temp("cast.mismatch");
                    out<<"  "<<mismatch<<" = icmp ne "<<source_ty<<" "<<back<<", "<<value(n.value)<<"\n";
                    emit_fail_check({mismatch});
                }
            }else if(is_float(n.source_type)&&is_integer(n.target_type)){
                const auto intrinsic=std::string(is_signed_integer(n.target_type)?"@llvm.fptosi.sat.i":"@llvm.fptoui.sat.i")+
                    std::to_string(target_width)+(n.source_type.kind==TypeKind::Float32?".f32":".f64");
                out<<"  "<<value(n.out)<<" = call "<<target_ty<<" "<<intrinsic<<"("<<source_ty<<" "<<value(n.value)<<")\n";
                if(n.checked_range){
                    const auto back=temp("cast.back"),same=temp("cast.same"),bad=temp("cast.bad");
                    out<<"  "<<back<<" = "<<(is_signed_integer(n.target_type)?"sitofp":"uitofp")<<" "<<target_ty<<" "<<value(n.out)<<" to "<<source_ty<<"\n";
                    out<<"  "<<same<<" = fcmp oeq "<<source_ty<<" "<<back<<", "<<value(n.value)<<"\n";
                    out<<"  "<<bad<<" = xor i1 "<<same<<", true\n";
                    emit_fail_check({bad});
                }
            }else if(is_float(n.source_type)&&is_float(n.target_type)){
                if(n.source_type.kind==TypeKind::Float32&&n.target_type.kind==TypeKind::Float){
                    out<<"  "<<value(n.out)<<" = fpext float "<<value(n.value)<<" to double\n";
                }else if(n.source_type.kind==TypeKind::Float&&n.target_type.kind==TypeKind::Float32){
                    if(n.checked_range){
                        const auto finite=temp("cast.finite");
                        const auto high=temp("cast.high");
                        const auto low=temp("cast.low");
                        const auto outside=temp("cast.outside");
                        const auto bad=temp("cast.bad");
                        out<<"  "<<finite<<" = fcmp ord double "<<value(n.value)<<", "<<value(n.value)<<"\n";
                        out<<"  "<<high<<" = fcmp ogt double "<<value(n.value)<<", 0x47EFFFFFE0000000\n";
                        out<<"  "<<low<<" = fcmp olt double "<<value(n.value)<<", 0xC7EFFFFFE0000000\n";
                        out<<"  "<<outside<<" = or i1 "<<high<<", "<<low<<"\n";
                        out<<"  "<<bad<<" = and i1 "<<finite<<", "<<outside<<"\n";
                        emit_fail_check({bad});
                    }
                    out<<"  "<<value(n.out)<<" = fptrunc double "<<value(n.value)<<" to float\n";
                }else{
                    out<<"  "<<value(n.out)<<" = fadd "<<target_ty<<" "<<value(n.value)<<", 0.000000e+00\n";
                }
            }
        }
        if constexpr(std::is_same_v<T,ir::ParseBin>){
            values[n.out]=n.result_type;
            const auto parsed=temp("bin.parse.value");
            out<<"  "<<parsed<<" = call ptr @quidra_bin_parse(ptr "<<value(n.text)<<")\n";
            if(n.result_type.kind==TypeKind::Bin){
                out<<"  "<<value(n.out)<<" = getelementptr i8, ptr "<<parsed<<", i64 0\n";
            }else{
                const auto result=value(n.out);
                out<<"  "<<result<<" = call ptr @quidra_alloc(i64 16)\n";
                const auto ok=temp("bin.parse.ok");
                const auto ok_label=unique_label("bin.parse.ok");
                const auto fail_label=unique_label("bin.parse.fail");
                const auto done_label=unique_label("bin.parse.done");
                out<<"  "<<ok<<" = icmp ne ptr "<<parsed<<", null\n";
                out<<"  br i1 "<<ok<<", label %"<<ok_label<<", label %"<<fail_label<<"\n";
                out<<ok_label<<":\n";
                out<<"  store i64 "<<case_index(n.result_type,Type::simple(TypeKind::Bin))<<", ptr "<<result<<"\n";
                const auto ok_payload=temp("bin.parse.payload");
                out<<"  "<<ok_payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n";
                out<<"  store ptr "<<parsed<<", ptr "<<ok_payload<<"\n";
                out<<"  br label %"<<done_label<<"\n";
                out<<fail_label<<":\n";
                out<<"  store i64 "<<case_index(n.result_type,Type::simple(TypeKind::Error))<<", ptr "<<result<<"\n";
                const auto err_payload=temp("bin.parse.error.payload");
                out<<"  "<<err_payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n";
                out<<"  store ptr @.err.parse, ptr "<<err_payload<<"\n";
                out<<"  br label %"<<done_label<<"\n";
                out<<done_label<<":\n";
            }
        }
        if constexpr(std::is_same_v<T,ir::BinConvert>){
            values[n.out]=n.target_type;
            if(n.source_type.kind==TypeKind::Bin){
                if(n.target_type.kind==TypeKind::Array){
                    const auto width=n.target_type.first->kind==TypeKind::Bool?1:integer_width(*n.target_type.first);
                    const auto stride=runtime_storage_bytes(*n.target_type.first);
                    out<<"  "<<value(n.out)<<" = call ptr @quidra_bin_to_array(ptr "<<value(n.value)
                       <<", i32 "<<width<<", i32 "<<stride<<")\n";
                }else{
                    const auto width=n.target_type.kind==TypeKind::Bool?1:integer_width(n.target_type);
                    const auto raw=temp("bin.scalar");
                    out<<"  "<<raw<<" = call i64 @quidra_bin_to_u64(ptr "<<value(n.value)
                       <<", i32 "<<width<<")\n";
                    if(n.target_type.kind==TypeKind::Bool){
                        out<<"  "<<value(n.out)<<" = trunc i64 "<<raw<<" to i1\n";
                    }else if(width<64){
                        out<<"  "<<value(n.out)<<" = trunc i64 "<<raw<<" to "<<llvm_type(n.target_type)<<"\n";
                    }else{
                        out<<"  "<<value(n.out)<<" = add i64 "<<raw<<", 0\n";
                    }
                }
            }else if(n.target_type.kind==TypeKind::Bin){
                if(n.source_type.kind==TypeKind::Array){
                    const auto width=n.source_type.first->kind==TypeKind::Bool?1:integer_width(*n.source_type.first);
                    const auto stride=runtime_storage_bytes(*n.source_type.first);
                    out<<"  "<<value(n.out)<<" = call ptr @quidra_bin_from_array(ptr "<<value(n.value)
                       <<", i32 "<<width<<", i32 "<<stride<<")\n";
                }else{
                    const auto width=n.source_type.kind==TypeKind::Bool?1:integer_width(n.source_type);
                    std::string widened=value(n.value);
                    if(n.source_type.kind==TypeKind::Bool){
                        widened=temp("bin.bool");
                        out<<"  "<<widened<<" = zext i1 "<<value(n.value)<<" to i64\n";
                    }else if(width<64){
                        widened=temp("bin.integer");
                        out<<"  "<<widened<<" = "<<(is_signed_integer(n.source_type)?"sext":"zext")
                           <<" "<<llvm_type(n.source_type)<<" "<<value(n.value)<<" to i64\n";
                    }
                    out<<"  "<<value(n.out)<<" = call ptr @quidra_bin_from_u64(i64 "<<widened
                       <<", i32 "<<width<<")\n";
                }
            }
        }
        if constexpr(std::is_same_v<T,ir::ParseNumber>){
            values[n.out]=n.result_type;
            const auto result=value(n.out);
            if(n.target_type.kind==TypeKind::BigInt||n.target_type.kind==TypeKind::BigReal){
                const auto parsed=temp("parse.exact"),ok=temp("parse.exact.ok");
                out<<"  "<<parsed<<" = call ptr @"<<(n.target_type.kind==TypeKind::BigInt?"quidra_bigint_parse":"quidra_bigreal_parse")
                   <<"(ptr "<<value(n.text)<<")\n";
                if(n.result_type==n.target_type){
                    out<<"  "<<value(n.out)<<" = getelementptr inbounds i8, ptr "<<parsed<<", i64 0\n";
                    return;
                }
                out<<"  "<<result<<" = call ptr @quidra_alloc(i64 16)\n";
                out<<"  "<<ok<<" = icmp ne ptr "<<parsed<<", null\n";
                const auto yes=unique_label("parse.exact.ok"),bad=unique_label("parse.exact.error"),done=unique_label("parse.exact.done");
                out<<"  br i1 "<<ok<<", label %"<<yes<<", label %"<<bad<<"\n";
                out<<yes<<":\n  store i64 "<<case_index(n.result_type,n.target_type)<<", ptr "<<result<<"\n";
                const auto payload=temp("parse.exact.payload");
                out<<"  "<<payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n"
                   <<"  store ptr "<<parsed<<", ptr "<<payload<<"\n  br label %"<<done<<"\n";
                out<<bad<<":\n  store i64 "<<case_index(n.result_type,Type::simple(TypeKind::Error))<<", ptr "<<result<<"\n";
                const auto ep=temp("parse.exact.error.payload");
                out<<"  "<<ep<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n"
                   <<"  store ptr @.err.parse, ptr "<<ep<<"\n  br label %"<<done<<"\n";
                out<<done<<":\n";
                return;
            }
            out<<"  "<<result<<" = call ptr @quidra_alloc(i64 16)\n";

            std::string parsed;
            std::string parse_ok;
            if(is_integer(n.target_type)){
                const auto& slot=scratch(ins);
                parsed=temp("parse.integer");
                parse_ok=temp("parse.ok");
                out<<"  "<<parse_ok<<" = call i1 @"<<(is_signed_integer(n.target_type)?"quidra_parse_signed":"quidra_parse_unsigned")
                   <<"(ptr "<<value(n.text)<<", ptr "<<slot<<")\n";
                out<<"  "<<parsed<<" = load i64, ptr "<<slot<<"\n";
            }else{
                const auto& slot=scratch(ins);
                parse_ok=temp("parse.ok");
                if(n.target_type.kind==TypeKind::Float32){
                    parsed=temp("parse.float");
                    out<<"  "<<parse_ok<<" = call i1 @quidra_parse_float32(ptr "<<value(n.text)<<", ptr "<<slot<<")\n";
                    out<<"  "<<parsed<<" = load float, ptr "<<slot<<"\n";
                }else{
                    parsed=temp("parse.float");
                    out<<"  "<<parse_ok<<" = call i1 @quidra_parse_float64(ptr "<<value(n.text)<<", ptr "<<slot<<")\n";
                    out<<"  "<<parsed<<" = load double, ptr "<<slot<<"\n";
                }
            }

            std::string bad=temp("parse.bad");
            out<<"  "<<bad<<" = xor i1 "<<parse_ok<<", true\n";
            if(is_integer(n.target_type)){
                const auto width=integer_width(n.target_type);
                if(width<64){
                    if(is_signed_integer(n.target_type)){
                        const long long min_value=-(1LL<<(width-1));
                        const long long max_value=(1LL<<(width-1))-1;
                        const auto low=temp("parse.low"),high=temp("parse.high"),outside=temp("parse.outside"),combined=temp("parse.bad.width");
                        out<<"  "<<low<<" = icmp slt i64 "<<parsed<<", "<<min_value<<"\n";
                        out<<"  "<<high<<" = icmp sgt i64 "<<parsed<<", "<<max_value<<"\n";
                        out<<"  "<<outside<<" = or i1 "<<low<<", "<<high<<"\n";
                        out<<"  "<<combined<<" = or i1 "<<bad<<", "<<outside<<"\n";
                        bad=combined;
                    }else{
                        const unsigned long long max_value=(1ULL<<width)-1ULL;
                        const auto high=temp("parse.high"),combined=temp("parse.bad.width");
                        out<<"  "<<high<<" = icmp ugt i64 "<<parsed<<", "<<max_value<<"\n";
                        out<<"  "<<combined<<" = or i1 "<<bad<<", "<<high<<"\n";
                        bad=combined;
                    }
                }
            }

            const auto fail_label=unique_label("parse.fail"),ok_label=unique_label("parse.ok"),done_label=unique_label("parse.done");
            out<<"  br i1 "<<bad<<", label %"<<fail_label<<", label %"<<ok_label<<"\n";
            const auto error_tag=case_index(n.result_type,Type::simple(TypeKind::Error));
            const auto target_tag=case_index(n.result_type,n.target_type);
            out<<fail_label<<":\n";
            out<<"  store i64 "<<error_tag<<", ptr "<<result<<"\n";
            const auto fail_payload=temp("parse.fail.payload");
            out<<"  "<<fail_payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n";
            out<<"  store ptr @.err.parse, ptr "<<fail_payload<<"\n";
            out<<"  br label %"<<done_label<<"\n";
            out<<ok_label<<":\n";
            out<<"  store i64 "<<target_tag<<", ptr "<<result<<"\n";
            const auto ok_payload=temp("parse.ok.payload");
            out<<"  "<<ok_payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n";
            if(is_integer(n.target_type)){
                const auto width=integer_width(n.target_type);
                if(width<64){
                    const auto narrowed=temp("parse.narrow");
                    out<<"  "<<narrowed<<" = trunc i64 "<<parsed<<" to "<<llvm_type(n.target_type)<<"\n";
                    out<<"  store "<<llvm_type(n.target_type)<<" "<<narrowed<<", ptr "<<ok_payload<<"\n";
                }else{
                    out<<"  store i64 "<<parsed<<", ptr "<<ok_payload<<"\n";
                }
            }else{
                out<<"  store "<<llvm_type(n.target_type)<<" "<<parsed<<", ptr "<<ok_payload<<"\n";
            }
            out<<"  br label %"<<done_label<<"\n";
            out<<done_label<<":\n";
        }
        if constexpr(std::is_same_v<T,ir::NumericAbs>){
            values[n.out]=n.type;
            const auto ty=llvm_type(n.type);
            if(n.type.kind==TypeKind::BigInt){
                out<<"  "<<value(n.out)<<" = call ptr @quidra_bigint_abs(ptr "<<value(n.value)<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
            }else if(n.type.kind==TypeKind::BigReal){
                out<<"  "<<value(n.out)<<" = call ptr @quidra_bigreal_abs(ptr "<<value(n.value)<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
            }else if(is_integer(n.type)){
                if(!is_signed_integer(n.type)){
                    out<<"  "<<value(n.out)<<" = add "<<ty<<" "<<value(n.value)<<", 0\n";
                }else{
                    const auto width=integer_width(n.type);
                    const auto pair=temp("abs.pair"),neg=temp("abs.neg"),overflow=temp("abs.overflow"),negative=temp("abs.negative");
                    out<<"  "<<pair<<" = call { "<<ty<<", i1 } @llvm.ssub.with.overflow.i"<<width<<"("<<ty<<" 0, "<<ty<<" "<<value(n.value)<<")\n";
                    out<<"  "<<neg<<" = extractvalue { "<<ty<<", i1 } "<<pair<<", 0\n";
                    out<<"  "<<overflow<<" = extractvalue { "<<ty<<", i1 } "<<pair<<", 1\n";
                    fail_if(overflow,"@.code.overflow","@.msg.overflow","abs",n.line,n.column);
                    out<<"  "<<negative<<" = icmp slt "<<ty<<" "<<value(n.value)<<", 0\n";
                    out<<"  "<<value(n.out)<<" = select i1 "<<negative<<", "<<ty<<" "<<neg<<", "<<ty<<" "<<value(n.value)<<"\n";
                }
            }else{
                out<<"  "<<value(n.out)<<" = call "<<ty<<" @llvm.fabs."<<(n.type.kind==TypeKind::Float32?"f32":"f64")<<"("<<ty<<" "<<value(n.value)<<")\n";
            }
        }
        if constexpr(std::is_same_v<T,ir::Sqrt>){
            const auto source=n.type;
            values[n.out]=source;
            if(source.kind==TypeKind::BigReal)
                out<<"  "<<value(n.out)<<" = call ptr @quidra_bigreal_sqrt(ptr "<<value(n.value)<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
            else{
                const auto ty=llvm_type(source);
                out<<"  "<<value(n.out)<<" = call "<<ty<<" @llvm.sqrt."<<(source.kind==TypeKind::Float32?"f32":"f64")<<"("<<ty<<" "<<value(n.value)<<")\n";
            }
        }
        if constexpr(std::is_same_v<T,ir::MathUnary>){
            values[n.out]=n.type;
            const auto ty=llvm_type(n.type);
            if(n.type.kind==TypeKind::BigReal){
                int opcode=0;
                switch(n.operation){
                    case BuiltinCallable::MathSin: opcode=1; break;
                    case BuiltinCallable::MathCos: opcode=2; break;
                    case BuiltinCallable::MathTan: opcode=3; break;
                    case BuiltinCallable::MathLog: opcode=4; break;
                    case BuiltinCallable::MathExp: opcode=5; break;
                    default: throw std::logic_error("invalid exact unary math operation");
                }
                out<<"  "<<value(n.out)<<" = call ptr @quidra_bigreal_math_unary(ptr "<<value(n.value)
                   <<", i32 "<<opcode<<", i64 0, i64 0)\n";
                return;
            }
            const char* base="";
            switch(n.operation){
                case BuiltinCallable::MathSin: base="sin"; break;
                case BuiltinCallable::MathCos: base="cos"; break;
                case BuiltinCallable::MathTan: base="tan"; break;
                case BuiltinCallable::MathLog: base="log"; break;
                case BuiltinCallable::MathExp: base="exp"; break;
                default: throw std::logic_error("invalid unary math operation");
            }
            out<<"  "<<value(n.out)<<" = call "<<ty<<" @"<<base<<(n.type.kind==TypeKind::Float32?"f":"")
               <<"("<<ty<<" "<<value(n.value)<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::MathRoundInt>){
            values[n.out]=n.result_type;
            if(n.source_type.kind==TypeKind::BigReal){
                int opcode=0;
                switch(n.operation){
                    case BuiltinCallable::MathTrunc: opcode=1; break;
                    case BuiltinCallable::MathRound: opcode=2; break;
                    case BuiltinCallable::MathFloor: opcode=3; break;
                    case BuiltinCallable::MathCeil: opcode=4; break;
                    default: throw std::logic_error("invalid exact rounding operation");
                }
                out<<"  "<<value(n.out)<<" = call ptr @quidra_bigreal_round(ptr "<<value(n.value)
                   <<", i32 "<<opcode<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
                return;
            }
            std::string input=value(n.value);
            if(n.source_type.kind==TypeKind::Float32){const auto widened=temp("math.round.widen");out<<"  "<<widened<<" = fpext float "<<input<<" to double\n";input=widened;}
            const char* function="@quidra_math_trunc_int";
            switch(n.operation){
                case BuiltinCallable::MathTrunc: function="@quidra_math_trunc_int"; break;
                case BuiltinCallable::MathRound: function="@quidra_math_round_int"; break;
                case BuiltinCallable::MathFloor: function="@quidra_math_floor_int"; break;
                case BuiltinCallable::MathCeil: function="@quidra_math_ceil_int"; break;
                default: throw std::logic_error("invalid integer rounding operation");
            }
            out<<"  "<<value(n.out)<<" = call i64 "<<function<<"(double "<<input<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::MathPow>){
            values[n.out]=n.type;
            if(n.type.kind==TypeKind::BigReal)
                out<<"  "<<value(n.out)<<" = call ptr @quidra_bigreal_pow(ptr "<<value(n.base)<<", ptr "<<value(n.exponent)<<", i64 0, i64 0)\n";
            else{
                const auto ty=llvm_type(n.type);
                out<<"  "<<value(n.out)<<" = call "<<ty<<" @pow"<<(n.type.kind==TypeKind::Float32?"f":"")
                   <<"("<<ty<<" "<<value(n.base)<<", "<<ty<<" "<<value(n.exponent)<<")\n";
            }
        }
        if constexpr(std::is_same_v<T,ir::CliArgument>){
            values[n.out]=n.type;
            const auto raw=temp("cli.argument.raw");
            out<<"  "<<raw<<" = call ptr @quidra_cli_argument(i64 "<<value(n.index)<<")\n";
            if(n.type.kind==TypeKind::String) out<<"  "<<value(n.out)<<" = getelementptr i8, ptr "<<raw<<", i64 0\n";
            else if(n.type.kind==TypeKind::Int) out<<"  "<<value(n.out)<<" = call i64 @quidra_cli_parse_int(ptr "<<raw<<")\n";
            else if(n.type.kind==TypeKind::Float) out<<"  "<<value(n.out)<<" = call double @quidra_cli_parse_float(ptr "<<raw<<")\n";
            else if(n.type.kind==TypeKind::BigInt) out<<"  "<<value(n.out)<<" = call ptr @quidra_cli_parse_bigint(ptr "<<raw<<")\n";
            else if(n.type.kind==TypeKind::BigReal) out<<"  "<<value(n.out)<<" = call ptr @quidra_cli_parse_bigreal(ptr "<<raw<<")\n";
            else if(n.type.kind==TypeKind::Bool) out<<"  "<<value(n.out)<<" = call i1 @quidra_cli_parse_bool(ptr "<<raw<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::CliOption>){
            values[n.out]=n.type;
            const auto raw=temp("cli.option.raw"),missing=temp("cli.option.missing");
            out<<"  "<<raw<<" = call ptr @quidra_cli_option(ptr "<<value(n.name)<<")\n";
            out<<"  "<<missing<<" = icmp eq ptr "<<raw<<", null\n";
            const auto use_default=unique_label("cli.option.default"),parse=unique_label("cli.option.parse"),done=unique_label("cli.option.done");
            out<<"  br i1 "<<missing<<", label %"<<use_default<<", label %"<<parse<<"\n";
            out<<use_default<<":\n";
            if(n.type.kind==TypeKind::String||n.type.kind==TypeKind::BigInt||n.type.kind==TypeKind::BigReal)
                out<<"  call void @quidra_managed_retain(ptr "<<value(n.default_value)<<")\n";
            out<<"  br label %"<<done<<"\n";
            out<<parse<<":\n";
            std::string parsed;
            if(n.type.kind==TypeKind::String) parsed=raw;
            else {
                parsed=temp("cli.option.value");
                if(n.type.kind==TypeKind::Int) out<<"  "<<parsed<<" = call i64 @quidra_cli_parse_int(ptr "<<raw<<")\n";
                else if(n.type.kind==TypeKind::Float) out<<"  "<<parsed<<" = call double @quidra_cli_parse_float(ptr "<<raw<<")\n";
                else if(n.type.kind==TypeKind::BigInt) out<<"  "<<parsed<<" = call ptr @quidra_cli_parse_bigint(ptr "<<raw<<")\n";
                else if(n.type.kind==TypeKind::BigReal) out<<"  "<<parsed<<" = call ptr @quidra_cli_parse_bigreal(ptr "<<raw<<")\n";
                else if(n.type.kind==TypeKind::Bool) out<<"  "<<parsed<<" = call i1 @quidra_cli_parse_bool(ptr "<<raw<<")\n";
            }
            out<<"  br label %"<<done<<"\n"<<done<<":\n";
            out<<"  "<<value(n.out)<<" = phi "<<llvm_type(n.type)<<" [ "<<value(n.default_value)<<", %"<<use_default<<" ], [ "<<parsed<<", %"<<parse<<" ]\n";
        }
        if constexpr(std::is_same_v<T,ir::CliFlag>){
            values[n.out]=Type::simple(TypeKind::Bool);
            out<<"  "<<value(n.out)<<" = call i1 @quidra_cli_flag(ptr "<<value(n.name)<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::CliFinish>){
            out<<"  call void @quidra_cli_finish()\n";
        }
        if constexpr(std::is_same_v<T,ir::FileRead>){
            const auto raw=temp("file.read.raw");
            out<<"  "<<raw<<" = call ptr @quidra_file_read_raw(ptr "<<value(n.path)<<")\n";
            emit_string_error_result(n.out,n.result_type,raw,"file.read");
        }
        if constexpr(std::is_same_v<T,ir::FileReadBin>){
            values[n.out]=n.result_type;
            const auto raw=temp("file.read_bin.raw"),present=temp("file.read_bin.present");
            const auto result=value(n.out);
            out<<"  "<<raw<<" = call ptr @quidra_file_read_bin_raw(ptr "<<value(n.path)<<")\n";
            out<<"  "<<result<<" = call ptr @quidra_alloc(i64 16)\n";
            out<<"  "<<present<<" = icmp ne ptr "<<raw<<", null\n";
            const auto ok=unique_label("file.read_bin.ok"),bad=unique_label("file.read_bin.error"),done=unique_label("file.read_bin.done");
            out<<"  br i1 "<<present<<", label %"<<ok<<", label %"<<bad<<"\n";
            out<<ok<<":\n  store i64 "<<case_index(n.result_type,Type::simple(TypeKind::Bin))<<", ptr "<<result<<"\n";
            const auto payload=temp("file.read_bin.payload");
            out<<"  "<<payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n"
               <<"  store ptr "<<raw<<", ptr "<<payload<<"\n  br label %"<<done<<"\n";
            out<<bad<<":\n  store i64 "<<case_index(n.result_type,Type::simple(TypeKind::Error))<<", ptr "<<result<<"\n";
            const auto error_payload=temp("file.read_bin.error.payload");
            out<<"  "<<error_payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n"
               <<"  store ptr @.err.file, ptr "<<error_payload<<"\n  br label %"<<done<<"\n";
            out<<done<<":\n";
        }
        if constexpr(std::is_same_v<T,ir::FileWrite>){
            const auto ok=temp("file.write.ok");
            out<<"  "<<ok<<" = call i1 @quidra_file_write_raw(ptr "<<value(n.path)<<", ptr "<<value(n.text)<<")\n";
            emit_void_error_result(n.out,n.result_type,ok,"file.write");
        }
        if constexpr(std::is_same_v<T,ir::FileWriteBin>){
            const auto ok=temp("file.write_bin.ok");
            out<<"  "<<ok<<" = call i1 @quidra_file_write_bin_raw(ptr "<<value(n.path)<<", ptr "<<value(n.bin)<<")\n";
            emit_void_error_result(n.out,n.result_type,ok,"file.write_bin");
        }
        if constexpr(std::is_same_v<T,ir::FileExists>){
            values[n.out]=n.result_type;
            const auto raw=temp("file.exists.raw"),is_error=temp("file.exists.error"),is_true=temp("file.exists.true");
            const auto result=value(n.out);
            out<<"  "<<raw<<" = call i32 @quidra_file_exists_raw(ptr "<<value(n.path)<<")\n";
            out<<"  "<<result<<" = call ptr @quidra_alloc(i64 16)\n";
            out<<"  "<<is_error<<" = icmp slt i32 "<<raw<<", 0\n";
            const auto bad=unique_label("file.exists.error"),ok=unique_label("file.exists.ok"),done=unique_label("file.exists.done");
            out<<"  br i1 "<<is_error<<", label %"<<bad<<", label %"<<ok<<"\n";
            out<<ok<<":\n  store i64 "<<case_index(n.result_type,Type::simple(TypeKind::Bool))<<", ptr "<<result<<"\n";
            const auto bool_payload=temp("file.exists.bool.payload");
            out<<"  "<<bool_payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n";
            out<<"  "<<is_true<<" = icmp eq i32 "<<raw<<", 1\n";
            out<<"  store i1 "<<is_true<<", ptr "<<bool_payload<<"\n  br label %"<<done<<"\n";
            out<<bad<<":\n  store i64 "<<case_index(n.result_type,Type::simple(TypeKind::Error))<<", ptr "<<result<<"\n";
            const auto error_payload=temp("file.exists.error.payload");
            out<<"  "<<error_payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n";
            out<<"  store ptr @.err.file, ptr "<<error_payload<<"\n  br label %"<<done<<"\n"<<done<<":\n";
        }
        if constexpr(std::is_same_v<T,ir::FileIsDirectory>){
            values[n.out]=n.result_type;
            const auto raw=temp("file.is_directory.raw"),is_error=temp("file.is_directory.error"),is_true=temp("file.is_directory.true");
            const auto result=value(n.out);
            out<<"  "<<raw<<" = call i32 @quidra_file_is_directory_raw(ptr "<<value(n.path)<<")\n";
            out<<"  "<<result<<" = call ptr @quidra_alloc(i64 16)\n";
            out<<"  "<<is_error<<" = icmp slt i32 "<<raw<<", 0\n";
            const auto bad=unique_label("file.is_directory.error"),ok=unique_label("file.is_directory.ok"),done=unique_label("file.is_directory.done");
            out<<"  br i1 "<<is_error<<", label %"<<bad<<", label %"<<ok<<"\n";
            out<<ok<<":\n  store i64 "<<case_index(n.result_type,Type::simple(TypeKind::Bool))<<", ptr "<<result<<"\n";
            const auto bool_payload=temp("file.is_directory.bool.payload");
            out<<"  "<<bool_payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n";
            out<<"  "<<is_true<<" = icmp eq i32 "<<raw<<", 1\n";
            out<<"  store i1 "<<is_true<<", ptr "<<bool_payload<<"\n  br label %"<<done<<"\n";
            out<<bad<<":\n  store i64 "<<case_index(n.result_type,Type::simple(TypeKind::Error))<<", ptr "<<result<<"\n";
            const auto error_payload=temp("file.is_directory.error.payload");
            out<<"  "<<error_payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n";
            out<<"  store ptr @.err.file, ptr "<<error_payload<<"\n  br label %"<<done<<"\n"<<done<<":\n";
        }
        if constexpr(std::is_same_v<T,ir::FileRemove>){
            const auto ok=temp("file.remove.ok");
            out<<"  "<<ok<<" = call i1 @quidra_file_remove_raw(ptr "<<value(n.path)<<")\n";
            emit_void_error_result(n.out,n.result_type,ok,"file.remove");
        }
        if constexpr(std::is_same_v<T,ir::FileCopy>){
            const auto ok=temp("file.copy.ok");
            out<<"  "<<ok<<" = call i1 @quidra_file_copy_raw(ptr "<<value(n.source)<<", ptr "<<value(n.destination)<<")\n";
            emit_void_error_result(n.out,n.result_type,ok,"file.copy");
        }
        if constexpr(std::is_same_v<T,ir::FileMove>){
            const auto ok=temp("file.move.ok");
            out<<"  "<<ok<<" = call i1 @quidra_file_move_raw(ptr "<<value(n.source)<<", ptr "<<value(n.destination)<<")\n";
            emit_void_error_result(n.out,n.result_type,ok,"file.move");
        }
        if constexpr(std::is_same_v<T,ir::FileMkdir>){
            const auto ok=temp("file.mkdir.ok");
            out<<"  "<<ok<<" = call i1 @quidra_file_mkdir_raw(ptr "<<value(n.path)<<")\n";
            emit_void_error_result(n.out,n.result_type,ok,"file.mkdir");
        }
        if constexpr(std::is_same_v<T,ir::FileList>){
            values[n.out]=n.result_type;
            const auto raw=temp("file.list.raw"),oktest=temp("file.list.ok");
            const auto result=value(n.out);
            const auto list_type=Type::array(Type::simple(TypeKind::String));
            out<<"  "<<raw<<" = call ptr @quidra_file_list_raw(ptr "<<value(n.path)<<", i1 "<<value(n.recursive)<<")\n";
            out<<"  "<<result<<" = call ptr @quidra_alloc(i64 16)\n";
            out<<"  "<<oktest<<" = icmp ne ptr "<<raw<<", null\n";
            const auto ok=unique_label("file.list.ok"),bad=unique_label("file.list.error"),done=unique_label("file.list.done");
            out<<"  br i1 "<<oktest<<", label %"<<ok<<", label %"<<bad<<"\n";
            out<<ok<<":\n  store i64 "<<case_index(n.result_type,list_type)<<", ptr "<<result<<"\n";
            const auto list_payload=temp("file.list.payload");
            out<<"  "<<list_payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n"
               <<"  store ptr "<<raw<<", ptr "<<list_payload<<"\n  br label %"<<done<<"\n";
            out<<bad<<":\n  store i64 "<<case_index(n.result_type,Type::simple(TypeKind::Error))<<", ptr "<<result<<"\n";
            const auto error_payload=temp("file.list.error.payload");
            out<<"  "<<error_payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n"
               <<"  store ptr @.err.file, ptr "<<error_payload<<"\n  br label %"<<done<<"\n";
            out<<done<<":\n";
        }
        if constexpr(std::is_same_v<T,ir::EnvironmentGet>){
            values[n.out]=n.result_type;
            const auto raw=temp("environment.get.raw");
            const auto missing=temp("environment.get.missing");
            const auto result=value(n.out);
            out<<"  "<<raw<<" = call ptr @quidra_environment_get(ptr "<<value(n.name)<<")\n";
            out<<"  "<<result<<" = call ptr @quidra_alloc(i64 16)\n";
            out<<"  "<<missing<<" = icmp eq ptr "<<raw<<", null\n";
            const auto none_label=unique_label("environment.get.none");
            const auto string_label=unique_label("environment.get.string");
            const auto done=unique_label("environment.get.done");
            out<<"  br i1 "<<missing<<", label %"<<none_label<<", label %"<<string_label<<"\n";
            out<<none_label<<":\n";
            out<<"  store i64 "<<case_index(n.result_type,Type::simple(TypeKind::None))<<", ptr "<<result<<"\n";
            out<<"  br label %"<<done<<"\n";
            out<<string_label<<":\n";
            out<<"  store i64 "<<case_index(n.result_type,Type::simple(TypeKind::String))<<", ptr "<<result<<"\n";
            const auto payload=temp("environment.get.payload");
            out<<"  "<<payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n";
            out<<"  store ptr "<<raw<<", ptr "<<payload<<"\n";
            out<<"  br label %"<<done<<"\n";
            out<<done<<":\n";
        }
        if constexpr(std::is_same_v<T,ir::EnvironmentHas>){
            values[n.out]=Type::simple(TypeKind::Bool);
            out<<"  "<<value(n.out)<<" = call i1 @quidra_environment_has(ptr "<<value(n.name)<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::TestAssert>){
            out<<"  call void @quidra_test_assert(i1 "<<value(n.condition)<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::TimeNow>){
            values[n.out]=Type::class_type("$std.time.Instant");
            const auto seconds=temp("time.now.seconds");
            out<<"  "<<seconds<<" = call double @quidra_time_now()\n";
            out<<"  "<<value(n.out)<<" = call ptr @quidra_alloc(i64 8)\n";
            out<<"  store double "<<seconds<<", ptr "<<value(n.out)<<", align 1\n";
        }
        if constexpr(std::is_same_v<T,ir::TimeSince>){
            values[n.out]=Type::class_type("$std.time.Duration");
            const auto start=temp("time.since.start"),now=temp("time.since.now"),elapsed=temp("time.since.elapsed");
            out<<"  "<<start<<" = load double, ptr "<<value(n.start)<<", align 1\n";
            out<<"  "<<now<<" = call double @quidra_time_now()\n";
            out<<"  "<<elapsed<<" = fsub double "<<now<<", "<<start<<"\n";
            out<<"  "<<value(n.out)<<" = call ptr @quidra_alloc(i64 8)\n";
            out<<"  store double "<<elapsed<<", ptr "<<value(n.out)<<", align 1\n";
        }
        if constexpr(std::is_same_v<T,ir::TimeSeconds>){
            values[n.out]=Type::class_type("$std.time.Duration");
            out<<"  "<<value(n.out)<<" = call ptr @quidra_alloc(i64 8)\n";
            out<<"  store double "<<value(n.seconds)<<", ptr "<<value(n.out)<<", align 1\n";
        }
        if constexpr(std::is_same_v<T,ir::TimeSleep>){
            const auto seconds=temp("time.sleep.seconds"),ok=temp("time.sleep.ok"),bad=temp("time.sleep.bad");
            out<<"  "<<seconds<<" = load double, ptr "<<value(n.duration)<<", align 1\n";
            out<<"  "<<ok<<" = call i1 @quidra_time_sleep(double "<<seconds<<")\n";
            out<<"  "<<bad<<" = xor i1 "<<ok<<", true\n";
            fail_if(bad,"@.code.time.sleep","@.msg.time.sleep","time.sleep",n.line,n.column);
        }
        if constexpr(std::is_same_v<T,ir::RandomGenerator>){
            values[n.out]=Type::class_type("$std.random.Generator");
            out<<"  "<<value(n.out)<<" = call ptr @quidra_alloc(i64 8)\n";
            out<<"  store i64 "<<value(n.seed)<<", ptr "<<value(n.out)<<", align 1\n";
        }
        if constexpr(std::is_same_v<T,ir::RandomInt>){
            values[n.out]=Type::simple(TypeKind::Int);
            const auto valid=temp("random.int.valid"),bad=temp("random.int.bad");
            out<<"  "<<valid<<" = icmp slt i64 "<<value(n.start)<<", "<<value(n.end)<<"\n";
            out<<"  "<<bad<<" = xor i1 "<<valid<<", true\n";
            fail_if(bad,"@.code.random.range","@.msg.random.range","random.int",n.line,n.column);
            out<<"  "<<value(n.out)<<" = call i64 @quidra_random_int(ptr "<<value(n.generator)
               <<", i64 "<<value(n.start)<<", i64 "<<value(n.end)<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::RandomFloat>){
            values[n.out]=Type::simple(TypeKind::Float);
            out<<"  "<<value(n.out)<<" = call double @quidra_random_float(ptr "<<value(n.generator)<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::RandomBool>){
            values[n.out]=Type::simple(TypeKind::Bool);
            out<<"  "<<value(n.out)<<" = call i1 @quidra_random_bool(ptr "<<value(n.generator)<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::ProcessRun>){
            values[n.out]=Type::class_type("$std.process.Result");
            out<<"  "<<value(n.out)<<" = call ptr @quidra_process_run(ptr "<<value(n.program)
               <<", ptr "<<value(n.args)<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::JsonParse>){
            values[n.out]=n.result_type;
            const auto raw=temp("json.parse.raw"),ok=temp("json.parse.ok"),result=value(n.out);
            out<<"  "<<raw<<" = call ptr @quidra_json_parse_raw(ptr "<<value(n.text)<<")\n";
            out<<"  "<<result<<" = call ptr @quidra_alloc(i64 16)\n";
            out<<"  "<<ok<<" = icmp ne ptr "<<raw<<", null\n";
            const auto yes=unique_label("json.parse.ok"),bad=unique_label("json.parse.error"),done=unique_label("json.parse.done");
            out<<"  br i1 "<<ok<<", label %"<<yes<<", label %"<<bad<<"\n";
            out<<yes<<":\n  store i64 "<<case_index(n.result_type,Type::class_type("$std.json.Value"))<<", ptr "<<result<<"\n";
            const auto good_payload=temp("json.parse.value");
            out<<"  "<<good_payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n"
               <<"  store ptr "<<raw<<", ptr "<<good_payload<<"\n  br label %"<<done<<"\n";
            out<<bad<<":\n  store i64 "<<case_index(n.result_type,Type::simple(TypeKind::Error))<<", ptr "<<result<<"\n";
            const auto message=temp("json.parse.message"),error_payload=temp("json.parse.error.payload");
            out<<"  "<<message<<" = call ptr @quidra_json_last_error_copy()\n";
            out<<"  "<<error_payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n"
               <<"  store ptr "<<message<<", ptr "<<error_payload<<"\n  br label %"<<done<<"\n";
            out<<done<<":\n";
        }
        if constexpr(std::is_same_v<T,ir::JsonKind>){
            values[n.out]=Type::simple(TypeKind::String);
            out<<"  "<<value(n.out)<<" = call ptr @quidra_json_kind(ptr "<<value(n.value)<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::JsonSize>){
            values[n.out]=n.result_type;
            const auto raw=temp("json.size.raw"),ok=temp("json.size.ok"),result=value(n.out);
            out<<"  "<<raw<<" = call i64 @quidra_json_size(ptr "<<value(n.value)<<")\n";
            out<<"  "<<result<<" = call ptr @quidra_alloc(i64 16)\n";
            out<<"  "<<ok<<" = icmp sge i64 "<<raw<<", 0\n";
            const auto yes=unique_label("json.size.ok"),bad=unique_label("json.size.error"),done=unique_label("json.size.done");
            out<<"  br i1 "<<ok<<", label %"<<yes<<", label %"<<bad<<"\n";
            out<<yes<<":\n  store i64 "<<case_index(n.result_type,Type::simple(TypeKind::Int))<<", ptr "<<result<<"\n";
            const auto good_payload=temp("json.size.value");
            out<<"  "<<good_payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n"
               <<"  store i64 "<<raw<<", ptr "<<good_payload<<"\n  br label %"<<done<<"\n";
            out<<bad<<":\n  store i64 "<<case_index(n.result_type,Type::simple(TypeKind::Error))<<", ptr "<<result<<"\n";
            const auto error_payload=temp("json.size.error.payload");
            out<<"  "<<error_payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n"
               <<"  store ptr @.err.json.type, ptr "<<error_payload<<"\n  br label %"<<done<<"\n";
            out<<done<<":\n";
        }
        if constexpr(std::is_same_v<T,ir::JsonGet>){
            values[n.out]=n.result_type;
            const auto object=temp("json.get.object"),result=value(n.out);
            out<<"  "<<object<<" = call i1 @quidra_json_is_object(ptr "<<value(n.value)<<")\n";
            out<<"  "<<result<<" = call ptr @quidra_alloc(i64 16)\n";
            const auto inspect=unique_label("json.get.inspect"),bad=unique_label("json.get.error"),done=unique_label("json.get.done");
            out<<"  br i1 "<<object<<", label %"<<inspect<<", label %"<<bad<<"\n";
            out<<inspect<<":\n";
            const auto raw=temp("json.get.raw"),present=temp("json.get.present");
            out<<"  "<<raw<<" = call ptr @quidra_json_get(ptr "<<value(n.value)<<", ptr "<<value(n.key)<<")\n";
            out<<"  "<<present<<" = icmp ne ptr "<<raw<<", null\n";
            const auto yes=unique_label("json.get.value"),missing=unique_label("json.get.none");
            out<<"  br i1 "<<present<<", label %"<<yes<<", label %"<<missing<<"\n";
            out<<yes<<":\n  store i64 "<<case_index(n.result_type,Type::class_type("$std.json.Value"))<<", ptr "<<result<<"\n";
            const auto value_payload=temp("json.get.value.payload");
            out<<"  "<<value_payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n"
               <<"  store ptr "<<raw<<", ptr "<<value_payload<<"\n  br label %"<<done<<"\n";
            out<<missing<<":\n  store i64 "<<case_index(n.result_type,Type::simple(TypeKind::None))<<", ptr "<<result<<"\n"
               <<"  br label %"<<done<<"\n";
            out<<bad<<":\n  store i64 "<<case_index(n.result_type,Type::simple(TypeKind::Error))<<", ptr "<<result<<"\n";
            const auto error_payload=temp("json.get.error.payload");
            out<<"  "<<error_payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n"
               <<"  store ptr @.err.json.type, ptr "<<error_payload<<"\n  br label %"<<done<<"\n";
            out<<done<<":\n";
        }
        if constexpr(std::is_same_v<T,ir::JsonAt>){
            values[n.out]=n.result_type;
            const auto array=temp("json.at.array"),result=value(n.out);
            out<<"  "<<array<<" = call i1 @quidra_json_is_array(ptr "<<value(n.value)<<")\n";
            out<<"  "<<result<<" = call ptr @quidra_alloc(i64 16)\n";
            const auto inspect=unique_label("json.at.inspect"),bad=unique_label("json.at.error"),done=unique_label("json.at.done");
            out<<"  br i1 "<<array<<", label %"<<inspect<<", label %"<<bad<<"\n";
            out<<inspect<<":\n";
            const auto raw=temp("json.at.raw"),present=temp("json.at.present");
            out<<"  "<<raw<<" = call ptr @quidra_json_at(ptr "<<value(n.value)<<", i64 "<<value(n.index)<<")\n";
            out<<"  "<<present<<" = icmp ne ptr "<<raw<<", null\n";
            const auto yes=unique_label("json.at.value"),missing=unique_label("json.at.none");
            out<<"  br i1 "<<present<<", label %"<<yes<<", label %"<<missing<<"\n";
            out<<yes<<":\n  store i64 "<<case_index(n.result_type,Type::class_type("$std.json.Value"))<<", ptr "<<result<<"\n";
            const auto value_payload=temp("json.at.value.payload");
            out<<"  "<<value_payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n"
               <<"  store ptr "<<raw<<", ptr "<<value_payload<<"\n  br label %"<<done<<"\n";
            out<<missing<<":\n  store i64 "<<case_index(n.result_type,Type::simple(TypeKind::None))<<", ptr "<<result<<"\n"
               <<"  br label %"<<done<<"\n";
            out<<bad<<":\n  store i64 "<<case_index(n.result_type,Type::simple(TypeKind::Error))<<", ptr "<<result<<"\n";
            const auto error_payload=temp("json.at.error.payload");
            out<<"  "<<error_payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n"
               <<"  store ptr @.err.json.type, ptr "<<error_payload<<"\n  br label %"<<done<<"\n";
            out<<done<<":\n";
        }
        if constexpr(std::is_same_v<T,ir::JsonText>){
            values[n.out]=n.result_type;
            const auto raw=temp("json.text.raw"),ok=temp("json.text.ok"),result=value(n.out);
            out<<"  "<<raw<<" = call ptr @quidra_json_text(ptr "<<value(n.value)<<")\n";
            out<<"  "<<result<<" = call ptr @quidra_alloc(i64 16)\n";
            out<<"  "<<ok<<" = icmp ne ptr "<<raw<<", null\n";
            const auto yes=unique_label("json.text.ok"),bad=unique_label("json.text.error"),done=unique_label("json.text.done");
            out<<"  br i1 "<<ok<<", label %"<<yes<<", label %"<<bad<<"\n";
            out<<yes<<":\n  store i64 "<<case_index(n.result_type,Type::simple(TypeKind::String))<<", ptr "<<result<<"\n";
            const auto value_payload=temp("json.text.value");
            out<<"  "<<value_payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n"
               <<"  store ptr "<<raw<<", ptr "<<value_payload<<"\n  br label %"<<done<<"\n";
            out<<bad<<":\n  store i64 "<<case_index(n.result_type,Type::simple(TypeKind::Error))<<", ptr "<<result<<"\n";
            const auto error_payload=temp("json.text.error.payload");
            out<<"  "<<error_payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n"
               <<"  store ptr @.err.json.type, ptr "<<error_payload<<"\n  br label %"<<done<<"\n";
            out<<done<<":\n";
        }
        if constexpr(std::is_same_v<T,ir::JsonInteger>){
            values[n.out]=n.result_type;
            const auto ok=temp("json.integer.ok"),raw=temp("json.integer.raw"),result=value(n.out);
            out<<"  "<<ok<<" = call i1 @quidra_json_integer_ok(ptr "<<value(n.value)<<")\n";
            out<<"  "<<raw<<" = call i64 @quidra_json_integer(ptr "<<value(n.value)<<")\n";
            out<<"  "<<result<<" = call ptr @quidra_alloc(i64 16)\n";
            const auto yes=unique_label("json.integer.ok"),bad=unique_label("json.integer.error"),done=unique_label("json.integer.done");
            out<<"  br i1 "<<ok<<", label %"<<yes<<", label %"<<bad<<"\n";
            out<<yes<<":\n  store i64 "<<case_index(n.result_type,Type::simple(TypeKind::Int))<<", ptr "<<result<<"\n";
            const auto value_payload=temp("json.integer.value");
            out<<"  "<<value_payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n"
               <<"  store i64 "<<raw<<", ptr "<<value_payload<<"\n  br label %"<<done<<"\n";
            out<<bad<<":\n  store i64 "<<case_index(n.result_type,Type::simple(TypeKind::Error))<<", ptr "<<result<<"\n";
            const auto error_payload=temp("json.integer.error.payload");
            out<<"  "<<error_payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n"
               <<"  store ptr @.err.json.type, ptr "<<error_payload<<"\n  br label %"<<done<<"\n";
            out<<done<<":\n";
        }
        if constexpr(std::is_same_v<T,ir::JsonNumber>){
            values[n.out]=n.result_type;
            const auto ok=temp("json.number.ok"),raw=temp("json.number.raw"),result=value(n.out);
            out<<"  "<<ok<<" = call i1 @quidra_json_number_ok(ptr "<<value(n.value)<<")\n";
            out<<"  "<<raw<<" = call double @quidra_json_number(ptr "<<value(n.value)<<")\n";
            out<<"  "<<result<<" = call ptr @quidra_alloc(i64 16)\n";
            const auto yes=unique_label("json.number.ok"),bad=unique_label("json.number.error"),done=unique_label("json.number.done");
            out<<"  br i1 "<<ok<<", label %"<<yes<<", label %"<<bad<<"\n";
            out<<yes<<":\n  store i64 "<<case_index(n.result_type,Type::simple(TypeKind::Float))<<", ptr "<<result<<"\n";
            const auto value_payload=temp("json.number.value");
            out<<"  "<<value_payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n"
               <<"  store double "<<raw<<", ptr "<<value_payload<<"\n  br label %"<<done<<"\n";
            out<<bad<<":\n  store i64 "<<case_index(n.result_type,Type::simple(TypeKind::Error))<<", ptr "<<result<<"\n";
            const auto error_payload=temp("json.number.error.payload");
            out<<"  "<<error_payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n"
               <<"  store ptr @.err.json.type, ptr "<<error_payload<<"\n  br label %"<<done<<"\n";
            out<<done<<":\n";
        }
        if constexpr(std::is_same_v<T,ir::JsonBigInt>||std::is_same_v<T,ir::JsonBigReal>){
            values[n.out]=n.result_type;
            const bool want_int=std::is_same_v<T,ir::JsonBigInt>;
            const auto raw=temp("json.exact.raw"),parsed=temp("json.exact.parsed"),ok=temp("json.exact.ok"),result=value(n.out);
            out<<"  "<<result<<" = call ptr @quidra_alloc(i64 16)\n";
            out<<"  "<<raw<<" = call ptr @quidra_json_number_text(ptr "<<value(n.value)<<")\n";
            const auto has_text=temp("json.exact.has_text");
            out<<"  "<<has_text<<" = icmp ne ptr "<<raw<<", null\n";
            const auto parse_label=unique_label("json.exact.parse"),bad=unique_label("json.exact.error"),yes=unique_label("json.exact.ok"),done=unique_label("json.exact.done");
            out<<"  br i1 "<<has_text<<", label %"<<parse_label<<", label %"<<bad<<"\n";
            out<<parse_label<<":\n";
            out<<"  "<<parsed<<" = call ptr @"<<(want_int?"quidra_bigint_parse":"quidra_bigreal_parse")<<"(ptr "<<raw<<")\n";
            out<<"  call void @quidra_managed_release(ptr "<<raw<<", ptr null)\n";
            out<<"  "<<ok<<" = icmp ne ptr "<<parsed<<", null\n";
            out<<"  br i1 "<<ok<<", label %"<<yes<<", label %"<<bad<<"\n";
            out<<yes<<":\n";
            const Type exact_type=Type::simple(want_int?TypeKind::BigInt:TypeKind::BigReal);
            out<<"  store i64 "<<case_index(n.result_type,exact_type)<<", ptr "<<result<<"\n";
            const auto payload=temp("json.exact.value");
            out<<"  "<<payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n"
               <<"  store ptr "<<parsed<<", ptr "<<payload<<"\n  br label %"<<done<<"\n";
            out<<bad<<":\n";
            out<<"  store i64 "<<case_index(n.result_type,Type::simple(TypeKind::Error))<<", ptr "<<result<<"\n";
            const auto error_payload=temp("json.exact.error.payload");
            out<<"  "<<error_payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n"
               <<"  store ptr @.err.json.type, ptr "<<error_payload<<"\n  br label %"<<done<<"\n";
            out<<done<<":\n";
        }
        if constexpr(std::is_same_v<T,ir::JsonBoolean>){
            values[n.out]=n.result_type;
            const auto ok=temp("json.boolean.ok"),raw=temp("json.boolean.raw"),result=value(n.out);
            out<<"  "<<ok<<" = call i1 @quidra_json_boolean_ok(ptr "<<value(n.value)<<")\n";
            out<<"  "<<raw<<" = call i1 @quidra_json_boolean(ptr "<<value(n.value)<<")\n";
            out<<"  "<<result<<" = call ptr @quidra_alloc(i64 16)\n";
            const auto yes=unique_label("json.boolean.ok"),bad=unique_label("json.boolean.error"),done=unique_label("json.boolean.done");
            out<<"  br i1 "<<ok<<", label %"<<yes<<", label %"<<bad<<"\n";
            out<<yes<<":\n  store i64 "<<case_index(n.result_type,Type::simple(TypeKind::Bool))<<", ptr "<<result<<"\n";
            const auto value_payload=temp("json.boolean.value");
            out<<"  "<<value_payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n"
               <<"  store i1 "<<raw<<", ptr "<<value_payload<<"\n  br label %"<<done<<"\n";
            out<<bad<<":\n  store i64 "<<case_index(n.result_type,Type::simple(TypeKind::Error))<<", ptr "<<result<<"\n";
            const auto error_payload=temp("json.boolean.error.payload");
            out<<"  "<<error_payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n"
               <<"  store ptr @.err.json.type, ptr "<<error_payload<<"\n  br label %"<<done<<"\n";
            out<<done<<":\n";
        }
        if constexpr(std::is_same_v<T,ir::JsonEncode>){
            values[n.out]=Type::simple(TypeKind::String);
            out<<"  "<<value(n.out)<<" = call ptr @quidra_json_encode(ptr "<<value(n.value)<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::JsonEqual>){
            values[n.out]=Type::simple(TypeKind::Bool);
            out<<"  "<<value(n.out)<<" = call i1 @quidra_json_equal(ptr "<<value(n.left)
               <<", ptr "<<value(n.right)<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::ImageRead>){
            values[n.out]=n.result_type;
            const std::vector<Type> image_elements{
                Type::simple(TypeKind::Int8), Type::simple(TypeKind::Int16),
                Type::simple(TypeKind::Int32), Type::simple(TypeKind::Int),
                Type::simple(TypeKind::UInt8), Type::simple(TypeKind::UInt16),
                Type::simple(TypeKind::UInt32), Type::simple(TypeKind::UInt64),
                Type::simple(TypeKind::Float32), Type::simple(TypeKind::Float)};
            std::vector<std::pair<int,Type>> image_cases;
            for(const auto& element:image_elements){
                for(const auto& image_type:n.result_type.cases){
                    if(image_type.kind==TypeKind::Tensor && image_type.first &&
                       *image_type.first==element){
                        image_cases.push_back({tensor_dtype_code(element),image_type});
                        break;
                    }
                }
            }
            if(image_cases.empty()) throw std::logic_error("image.read result has no tensor case");
            const int target_dtype=n.target_dtype?tensor_dtype_code(*n.target_dtype):0;
            const int expected_dtype=
                target_dtype==0 && image_cases.size()==1?image_cases.front().first:0;
            const long long expected_channels=
                n.expected_shape_prefix.size()>0?n.expected_shape_prefix[0]:-1;
            const long long expected_height=
                n.expected_shape_prefix.size()>1?n.expected_shape_prefix[1]:-1;
            const long long expected_width=
                n.expected_shape_prefix.size()>2?n.expected_shape_prefix[2]:-1;
            const auto& dtype_slot=scratch(ins);
            const auto raw=temp("image.read.raw"),ok=temp("image.read.ok"),result=value(n.out);
            out<<"  store i32 0, ptr "<<dtype_slot<<"\n";
            out<<"  "<<raw<<" = call ptr @quidra_image_read(ptr "<<value(n.path)
               <<", i32 "<<expected_dtype
               <<", i32 "<<target_dtype
               <<", i32 "<<n.target_channels
               <<", i64 "<<expected_channels
               <<", i64 "<<expected_height
               <<", i64 "<<expected_width
               <<", ptr "<<dtype_slot<<")\n";
            out<<"  "<<result<<" = call ptr @quidra_alloc(i64 16)\n";
            out<<"  "<<ok<<" = icmp ne ptr "<<raw<<", null\n";
            const auto yes=unique_label("image.read.ok"),bad=unique_label("image.read.error"),done=unique_label("image.read.done");
            out<<"  br i1 "<<ok<<", label %"<<yes<<", label %"<<bad<<"\n";
            out<<yes<<":\n";
            const auto actual_dtype=temp("image.read.dtype");
            out<<"  "<<actual_dtype<<" = load i32, ptr "<<dtype_slot<<"\n";
            std::vector<std::string> case_labels;
            case_labels.reserve(image_cases.size());
            for(std::size_t i=0;i<image_cases.size();++i)
                case_labels.push_back(unique_label("image.read.dtype"));
            out<<"  switch i32 "<<actual_dtype<<", label %"<<bad<<" [\n";
            for(std::size_t i=0;i<image_cases.size();++i)
                out<<"    i32 "<<image_cases[i].first<<", label %"<<case_labels[i]<<"\n";
            out<<"  ]\n";
            for(std::size_t i=0;i<image_cases.size();++i){
                out<<case_labels[i]<<":\n";
                out<<"  store i64 "<<case_index(n.result_type,image_cases[i].second)
                   <<", ptr "<<result<<"\n";
                const auto payload=temp("image.read.payload");
                out<<"  "<<payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n";
                out<<"  store ptr "<<raw<<", ptr "<<payload<<"\n";
                out<<"  br label %"<<done<<"\n";
            }
            out<<bad<<":\n";
            out<<"  store i64 "<<case_index(n.result_type,Type::simple(TypeKind::Error))
               <<", ptr "<<result<<"\n";
            const auto message=temp("image.read.message"),error_payload=temp("image.read.error.payload");
            out<<"  "<<message<<" = call ptr @quidra_image_last_error_copy()\n";
            out<<"  "<<error_payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n";
            out<<"  store ptr "<<message<<", ptr "<<error_payload<<"\n";
            out<<"  br label %"<<done<<"\n";
            out<<done<<":\n";
        }
        if constexpr(std::is_same_v<T,ir::ImageWrite>){
            values[n.out]=n.result_type;
            const auto image_type=values.at(n.image);
            if(image_type.kind!=TypeKind::Tensor || !image_type.first || !is_numeric(*image_type.first))
                throw std::logic_error("image.write IR requires numeric tensor input");
            const auto ok=temp("image.write.ok"),result=value(n.out);
            out<<"  "<<ok<<" = call i1 @quidra_image_write(ptr "<<value(n.path)
               <<", ptr "<<value(n.image)<<", i32 "<<tensor_dtype_code(*image_type.first)
               <<", i64 "<<value(n.quality)<<")\n";
            out<<"  "<<result<<" = call ptr @quidra_alloc(i64 16)\n";
            const auto yes=unique_label("image.write.ok"),bad=unique_label("image.write.error"),done=unique_label("image.write.done");
            out<<"  br i1 "<<ok<<", label %"<<yes<<", label %"<<bad<<"\n";
            out<<yes<<":\n";
            out<<"  store i64 "<<case_index(n.result_type,Type::simple(TypeKind::Void))
               <<", ptr "<<result<<"\n";
            out<<"  br label %"<<done<<"\n";
            out<<bad<<":\n";
            out<<"  store i64 "<<case_index(n.result_type,Type::simple(TypeKind::Error))
               <<", ptr "<<result<<"\n";
            const auto message=temp("image.write.message"),error_payload=temp("image.write.error.payload");
            out<<"  "<<message<<" = call ptr @quidra_image_last_error_copy()\n";
            out<<"  "<<error_payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n";
            out<<"  store ptr "<<message<<", ptr "<<error_payload<<"\n";
            out<<"  br label %"<<done<<"\n";
            out<<done<<":\n";
        }
        if constexpr(std::is_same_v<T,ir::ImageTensorOp>){
            values[n.out]=n.result_type;
            const auto dtype=tensor_dtype_code(n.element_type);
            if(n.args.empty())
                throw std::logic_error("image tensor IR requires an input tensor");
            const auto geometry_op =
                n.operation==BuiltinCallable::ImageTensorCrop ? 1 :
                n.operation==BuiltinCallable::ImageTensorResize ? 2 :
                n.operation==BuiltinCallable::ImageTensorFlipHorizontal ? 3 :
                n.operation==BuiltinCallable::ImageTensorFlipVertical ? 4 :
                n.operation==BuiltinCallable::ImageTensorRotate90 ? 5 :
                n.operation==BuiltinCallable::ImageTensorRotate270 ? 6 : 0;
            if(geometry_op){
                const auto arg=[&](std::size_t index)->std::string{
                    return index<n.args.size()?value(n.args[index]):"0";
                };
                out<<"  "<<value(n.out)<<" = call ptr @quidra_image_tensor_geometry(ptr "
                   <<value(n.args[0])<<", i32 "<<dtype<<", i32 "<<geometry_op
                   <<", i64 "<<arg(1)<<", i64 "<<arg(2)
                   <<", i64 "<<arg(3)<<", i64 "<<arg(4)
                   <<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
            }else if(n.operation==BuiltinCallable::ImageTensorGrayscale){
                out<<"  "<<value(n.out)<<" = call ptr @quidra_image_tensor_grayscale(ptr "
                   <<value(n.args[0])<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
            }else if(n.operation==BuiltinCallable::ImageTensorThreshold){
                out<<"  "<<value(n.out)<<" = call ptr @quidra_image_tensor_threshold(ptr "
                   <<value(n.args[0])<<", i8 "<<value(n.args[1])
                   <<", i8 "<<value(n.args[2])<<", i8 "<<value(n.args[3])
                   <<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
            }else if(n.operation==BuiltinCallable::ImageTensorBlur){
                out<<"  "<<value(n.out)<<" = call ptr @quidra_image_tensor_blur(ptr "
                   <<value(n.args[0])<<", i64 "<<value(n.args[1])
                   <<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
            }else if(n.operation==BuiltinCallable::ImageTensorFilter){
                out<<"  "<<value(n.out)<<" = call ptr @quidra_image_tensor_filter(ptr "
                   <<value(n.args[0])<<", ptr "<<value(n.args[1])
                   <<", i64 "<<value(n.args[2])<<", i64 "<<value(n.args[3])
                   <<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
            }else if(n.operation==BuiltinCallable::ImageTensorDilate ||
                     n.operation==BuiltinCallable::ImageTensorErode){
                out<<"  "<<value(n.out)<<" = call ptr @quidra_image_tensor_morphology(ptr "
                   <<value(n.args[0])<<", i32 "<<dtype<<", i64 "<<value(n.args[1])
                   <<", i1 "<<(n.operation==BuiltinCallable::ImageTensorDilate?"true":"false")
                   <<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
            }else{
                throw std::logic_error("unknown image tensor operation");
            }
        }
        if constexpr(std::is_same_v<T,ir::HttpGet>){
            values[n.out]=n.result_type;
            const auto raw=temp("http.get.raw"),ok=temp("http.get.ok"),result=value(n.out);
            out<<"  "<<raw<<" = call ptr @quidra_http_get(ptr "<<value(n.url)<<")\n";
            out<<"  "<<result<<" = call ptr @quidra_alloc(i64 16)\n";
            out<<"  "<<ok<<" = icmp ne ptr "<<raw<<", null\n";
            const auto yes=unique_label("http.get.ok"),bad=unique_label("http.get.error"),done=unique_label("http.get.done");
            out<<"  br i1 "<<ok<<", label %"<<yes<<", label %"<<bad<<"\n";
            out<<yes<<":\n  store i64 "<<case_index(n.result_type,Type::class_type("$std.http.Response"))<<", ptr "<<result<<"\n";
            const auto good_payload=temp("http.get.response");
            out<<"  "<<good_payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n"
               <<"  store ptr "<<raw<<", ptr "<<good_payload<<"\n  br label %"<<done<<"\n";
            out<<bad<<":\n  store i64 "<<case_index(n.result_type,Type::simple(TypeKind::Error))<<", ptr "<<result<<"\n";
            const auto message=temp("http.get.message"),error_payload=temp("http.get.error.payload");
            out<<"  "<<message<<" = call ptr @quidra_http_last_error_copy()\n";
            out<<"  "<<error_payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n"
               <<"  store ptr "<<message<<", ptr "<<error_payload<<"\n  br label %"<<done<<"\n";
            out<<done<<":\n";
        }
        if constexpr(std::is_same_v<T,ir::HttpHeader>){
            values[n.out]=n.result_type;
            const auto raw=temp("http.header.raw"),present=temp("http.header.present"),result=value(n.out);
            out<<"  "<<raw<<" = call ptr @quidra_http_header(ptr "<<value(n.response)
               <<", ptr "<<value(n.name)<<")\n";
            out<<"  "<<result<<" = call ptr @quidra_alloc(i64 16)\n";
            out<<"  "<<present<<" = icmp ne ptr "<<raw<<", null\n";
            const auto yes=unique_label("http.header.value"),missing=unique_label("http.header.none"),done=unique_label("http.header.done");
            out<<"  br i1 "<<present<<", label %"<<yes<<", label %"<<missing<<"\n";
            out<<yes<<":\n  store i64 "<<case_index(n.result_type,Type::simple(TypeKind::String))<<", ptr "<<result<<"\n";
            const auto value_payload=temp("http.header.value.payload");
            out<<"  "<<value_payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n"
               <<"  store ptr "<<raw<<", ptr "<<value_payload<<"\n  br label %"<<done<<"\n";
            out<<missing<<":\n  store i64 "<<case_index(n.result_type,Type::simple(TypeKind::None))<<", ptr "<<result<<"\n"
               <<"  br label %"<<done<<"\n";
            out<<done<<":\n";
        }
        if constexpr(std::is_same_v<T,ir::NumericMinMax>){
            values[n.out]=n.type;
            const auto ty=llvm_type(n.type),cmp=temp("num.cmp");
            if(n.type.kind==TypeKind::BigInt||n.type.kind==TypeKind::BigReal){
                if(n.type.kind==TypeKind::BigInt)
                    out<<"  "<<cmp<<" = call i32 @quidra_bigint_compare(ptr "<<value(n.left)<<", ptr "<<value(n.right)<<")\n";
                else
                    out<<"  "<<cmp<<" = call i32 @quidra_bigreal_compare(ptr "<<value(n.left)<<", ptr "<<value(n.right)<<", i64 0, i64 0)\n";
                const auto choose=temp("num.exact.choose");
                out<<"  "<<choose<<" = icmp "<<(n.maximum?"sgt":"slt")<<" i32 "<<cmp<<", 0\n";
                out<<"  "<<value(n.out)<<" = select i1 "<<choose<<", ptr "<<value(n.left)<<", ptr "<<value(n.right)<<"\n";
                out<<"  call void @quidra_managed_retain(ptr "<<value(n.out)<<")\n";
                return;
            }
            if(is_integer(n.type)){
                const auto pred=is_signed_integer(n.type)?(n.maximum?"sgt":"slt"):(n.maximum?"ugt":"ult");
                out<<"  "<<cmp<<" = icmp "<<pred<<" "<<ty<<" "<<value(n.left)<<", "<<value(n.right)<<"\n";
            }else{
                out<<"  "<<cmp<<" = fcmp "<<(n.maximum?"ogt":"olt")<<" "<<ty<<" "<<value(n.left)<<", "<<value(n.right)<<"\n";
            }
            out<<"  "<<value(n.out)<<" = select i1 "<<cmp<<", "<<ty<<" "<<value(n.left)<<", "<<ty<<" "<<value(n.right)<<"\n";
        }
        if constexpr(std::is_same_v<T,ir::ArrayGet>){
            values[n.out]=n.element_type;
            const auto& array_type=values.at(n.array);
            const auto stride=array_element_stride(array_type,array_layout);
            auto slot="%arr.get.slot."+std::to_string(n.out);
            if(is_fixed_array(array_type)){
                if(n.bounds_proven)
                    out<<"  "<<slot<<" = call ptr @quidra_fixed_array_slot_proven(ptr "<<value(n.array)<<", i64 "<<value(n.index)<<", i64 "<<stride<<")\n";
                else
                    out<<"  "<<slot<<" = call ptr @quidra_fixed_array_slot(ptr "<<value(n.array)<<", i64 "<<value(n.index)<<", i64 "<<array_type.length<<", i64 "<<stride<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
            }else{
                if(n.bounds_proven)
                    out<<"  "<<slot<<" = call ptr @quidra_array_slot_proven(ptr "<<value(n.array)<<", i64 "<<value(n.index)<<", i64 "<<stride<<")\n";
                else
                    out<<"  "<<slot<<" = call ptr @quidra_array_slot(ptr "<<value(n.array)<<", i64 "<<value(n.index)<<", i64 "<<stride<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
            }
            if(is_fixed_array(array_type)&&array_layout.inline_fixed_child(array_type)){
                out<<"  "<<value(n.out)<<" = getelementptr inbounds i8, ptr "<<slot<<", i64 0\n";
            }else{
                if(!n.initialization_proven)
                    out<<"  call void @quidra_init_check(ptr "<<slot<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
                out<<"  "<<value(n.out)<<" = load "<<llvm_type(n.element_type)<<", ptr "<<slot<<", align 1\n";
            }
        }
        if constexpr(std::is_same_v<T,ir::ArraySet>){
            const auto& array_type=values.at(n.array);
            const auto stride=array_element_stride(array_type,array_layout);
            auto slot="%arr.set.slot."+std::to_string(n.index)+"."+std::to_string(n.value);
            if(is_fixed_array(array_type)){
                if(n.bounds_proven)
                    out<<"  "<<slot<<" = call ptr @quidra_fixed_array_slot_proven(ptr "<<value(n.array)<<", i64 "<<value(n.index)<<", i64 "<<stride<<")\n";
                else
                    out<<"  "<<slot<<" = call ptr @quidra_fixed_array_slot(ptr "<<value(n.array)<<", i64 "<<value(n.index)<<", i64 "<<array_type.length<<", i64 "<<stride<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
            }else{
                if(n.bounds_proven)
                    out<<"  "<<slot<<" = call ptr @quidra_array_slot_proven(ptr "<<value(n.array)<<", i64 "<<value(n.index)<<", i64 "<<stride<<")\n";
                else
                    out<<"  "<<slot<<" = call ptr @quidra_array_slot(ptr "<<value(n.array)<<", i64 "<<value(n.index)<<", i64 "<<stride<<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
            }
            if(is_fixed_array(array_type)&&array_layout.inline_fixed_child(array_type)){
                out<<"  call void "<<drop_name(n.element_type)<<"(ptr "<<slot<<")\n";
                out<<"  call ptr @memcpy(ptr "<<slot<<", ptr "<<value(n.value)<<", i64 "<<stride<<")\n";
                out<<"  call void @quidra_managed_release(ptr "<<value(n.value)<<", ptr null)\n";
            }else{
                if(requires_lifetime_management(n.element_type)){
                    auto old=temp("array.old");
                    out<<"  "<<old<<" = load ptr, ptr "<<slot<<", align 1\n";
                    release_value(n.element_type,old);
                }
                out<<"  store "<<llvm_type(n.element_type)<<" "<<value(n.value)<<", ptr "<<slot<<", align 1\n";
            }
            if(!n.initialization_proven)
                out<<"  call void @quidra_init_mark_range(ptr "<<slot<<", i64 "<<stride<<")\n";
        }
        if constexpr(std::is_same_v<T,ir::Clone>){values[n.out]=n.type;out<<"  "<<value(n.out)<<" = call ptr "<<clone_name(n.type)<<"(ptr "<<value(n.value)<<")\n";}
        if constexpr(std::is_same_v<T,ir::Retain>){values[n.out]=n.type;out<<"  call void @quidra_managed_retain(ptr "<<value(n.value)<<")\n  "<<value(n.out)<<" = getelementptr inbounds i8, ptr "<<value(n.value)<<", i64 0\n";}
        if constexpr(std::is_same_v<T,ir::Release>){release_value(n.type,value(n.value));}
        if constexpr(std::is_same_v<T,ir::LoadLocal>){values[n.out]=n.type;out<<"  "<<value(n.out)<<" = load "<<llvm_type(n.type)<<", ptr "<<storage(n.name)<<"\n";}
        if constexpr(std::is_same_v<T,ir::StoreLocal>){
            if(requires_lifetime_management(n.type) && !n.borrowed &&
               !n.replace_without_release){
                auto old=temp("local.old");
                out<<"  "<<old<<" = load ptr, ptr "<<storage(n.name)<<"\n";
                release_value(n.type,old);
            }
            out<<"  store "<<llvm_type(n.type)<<" "<<value(n.value)<<", ptr "<<storage(n.name)<<"\n";
        }
        if constexpr(std::is_same_v<T,ir::Unary>){
            values[n.out]=n.type;
            if(n.type.kind==TypeKind::Tensor){
                out<<"  "<<value(n.out)<<" = call ptr @quidra_tensor_unary(ptr "
                   <<value(n.operand)<<", i32 1, i64 "<<n.line<<", i64 "<<n.column<<")\n";
            }else if(n.op=="not"){
                out<<"  "<<value(n.out)<<" = xor i1 "<<value(n.operand)<<", true\n";
            }else if(n.op=="NOT"){
                out<<"  "<<value(n.out)<<" = xor "<<llvm_type(n.type)<<" "<<value(n.operand)<<", -1\n";
            }else if(n.type.kind==TypeKind::BigInt){
                out<<"  "<<value(n.out)<<" = call ptr @quidra_bigint_neg(ptr "<<value(n.operand)
                   <<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
            }else if(n.type.kind==TypeKind::BigReal){
                out<<"  "<<value(n.out)<<" = call ptr @quidra_bigreal_neg(ptr "<<value(n.operand)
                   <<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
            }else if(is_integer(n.type)){
                const auto ty=llvm_type(n.type);
                const auto width=integer_width(n.type);
                const auto pair=temp("neg.pair"),overflow=temp("neg.overflow");
                out<<"  "<<pair<<" = call { "<<ty<<", i1 } @llvm.ssub.with.overflow.i"<<width<<"("<<ty<<" 0, "<<ty<<" "<<value(n.operand)<<")\n";
                out<<"  "<<value(n.out)<<" = extractvalue { "<<ty<<", i1 } "<<pair<<", 0\n";
                out<<"  "<<overflow<<" = extractvalue { "<<ty<<", i1 } "<<pair<<", 1\n";
                fail_if(overflow,"@.code.overflow","@.msg.overflow","neg",n.line,n.column);
            }else{
                out<<"  "<<value(n.out)<<" = fneg "<<llvm_type(n.type)<<" "<<value(n.operand)<<"\n";
            }
        }
        if constexpr(std::is_same_v<T,ir::Binary>){values[n.out]=n.result_type;const auto&ot=n.operand_type;
            if(n.op=="+"&&ot.kind==TypeKind::String){out<<"  "<<value(n.out)<<" = call ptr @quidra_string_concat2(ptr "<<value(n.left)<<", ptr "<<value(n.right)<<")\n";return;}
            if((n.op=="=="||n.op=="!=")&&(ot.kind==TypeKind::String||ot.kind==TypeKind::Error)){auto equal="%str.equal."+std::to_string(n.out);out<<"  "<<equal<<" = call i1 @quidra_string_equal(ptr "<<value(n.left)<<", ptr "<<value(n.right)<<")\n";if(n.op=="==")out<<"  "<<value(n.out)<<" = xor i1 "<<equal<<", false\n";else out<<"  "<<value(n.out)<<" = xor i1 "<<equal<<", true\n";return;}
            if((n.op=="=="||n.op=="!=")&&(ot.kind==TypeKind::Bin||ot.kind==TypeKind::Array||ot.kind==TypeKind::Class)){auto eq="%deep.eq."+std::to_string(n.out);out<<"  "<<eq<<" = call i1 "<<equality_name(ot)<<"(ptr "<<value(n.left)<<", ptr "<<value(n.right)<<")\n";if(n.op=="==")out<<"  "<<value(n.out)<<" = xor i1 "<<eq<<", false\n";else out<<"  "<<value(n.out)<<" = xor i1 "<<eq<<", true\n";return;}
            if(ot.kind==TypeKind::BigInt||ot.kind==TypeKind::BigReal){
                const bool bigint=ot.kind==TypeKind::BigInt;
                if(n.op=="+"||n.op=="-"||n.op=="*"||n.op=="/"||(bigint&&n.op=="%")){
                    int opcode=n.op=="+"?1:n.op=="-"?2:n.op=="*"?3:n.op=="/"?4:5;
                    out<<"  "<<value(n.out)<<" = call ptr @"<<(bigint?"quidra_bigint_binary":"quidra_bigreal_binary")
                       <<"(ptr "<<value(n.left)<<", ptr "<<value(n.right)<<", i32 "<<opcode
                       <<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
                    return;
                }
                const auto comparison=temp("exact.compare");
                if(bigint)
                    out<<"  "<<comparison<<" = call i32 @quidra_bigint_compare(ptr "<<value(n.left)<<", ptr "<<value(n.right)<<")\n";
                else
                    out<<"  "<<comparison<<" = call i32 @quidra_bigreal_compare(ptr "<<value(n.left)<<", ptr "<<value(n.right)
                       <<", i64 "<<n.line<<", i64 "<<n.column<<")\n";
                std::string pred;
                if(n.op=="==")pred="eq";else if(n.op=="!=")pred="ne";
                else if(n.op=="<")pred="slt";else if(n.op=="<=")pred="sle";
                else if(n.op==">")pred="sgt";else pred="sge";
                out<<"  "<<value(n.out)<<" = icmp "<<pred<<" i32 "<<comparison<<", 0\n";
                return;
            }
            if(is_integer(ot)){
                const auto ty=llvm_type(ot);
                const auto width=integer_width(ot);
                if(operator_policy::is_bitwise_logic(n.op)){
                    const auto instruction=n.op=="AND"?"and":n.op=="OR"?"or":"xor";
                    out<<"  "<<value(n.out)<<" = "<<instruction<<" "<<ty<<" "
                       <<value(n.left)<<", "<<value(n.right)<<"\n";
                    return;
                }
                if(operator_policy::is_shift(n.op)){
                    std::string invalid;
                    const auto high=temp("shift.high");
                    if(is_signed_integer(ot)){
                        const auto low=temp("shift.low");
                        invalid=temp("shift.invalid");
                        out<<"  "<<low<<" = icmp slt "<<ty<<" "<<value(n.right)<<", 0\n";
                        out<<"  "<<high<<" = icmp sge "<<ty<<" "<<value(n.right)<<", "<<width<<"\n";
                        out<<"  "<<invalid<<" = or i1 "<<low<<", "<<high<<"\n";
                    }else{
                        invalid=high;
                        out<<"  "<<high<<" = icmp uge "<<ty<<" "<<value(n.right)<<", "<<width<<"\n";
                    }
                    fail_if(invalid,"@.code.shift","@.msg.shift","shift",n.line,n.column);
                    const auto instruction=n.op=="<<"?"shl":(is_signed_integer(ot)?"ashr":"lshr");
                    out<<"  "<<value(n.out)<<" = "<<instruction<<" "<<ty<<" "
                       <<value(n.left)<<", "<<value(n.right)<<"\n";
                    return;
                }
                if(n.op=="+"||n.op=="-"||n.op=="*"){
                    const auto opname=n.op=="+"?"add":n.op=="-"?"sub":"mul";
                    const auto sign=is_signed_integer(ot)?"s":"u";
                    const auto pair=temp("arith.pair"),overflow=temp("arith.overflow");
                    out<<"  "<<pair<<" = call { "<<ty<<", i1 } @llvm."<<sign<<opname<<".with.overflow.i"<<width<<"("<<ty<<" "<<value(n.left)<<", "<<ty<<" "<<value(n.right)<<")\n";
                    out<<"  "<<value(n.out)<<" = extractvalue { "<<ty<<", i1 } "<<pair<<", 0\n";
                    out<<"  "<<overflow<<" = extractvalue { "<<ty<<", i1 } "<<pair<<", 1\n";
                    fail_if(overflow,"@.code.overflow","@.msg.overflow","arith",n.line,n.column);
                    return;
                }
                if(n.op=="/"||n.op=="%"){
                    const auto zero=temp("arith.zero");
                    out<<"  "<<zero<<" = icmp eq "<<ty<<" "<<value(n.right)<<", 0\n";
                    fail_if(zero,"@.code.divzero","@.msg.divzero","arith.zero",n.line,n.column);
                    if(is_signed_integer(ot)){
                        const std::string min_value=width==64?"-9223372036854775808":std::to_string(-(1LL<<(width-1)));
                        const auto ismin=temp("arith.min"),isnegone=temp("arith.negone"),special=temp("arith.special");
                        out<<"  "<<ismin<<" = icmp eq "<<ty<<" "<<value(n.left)<<", "<<min_value<<"\n";
                        out<<"  "<<isnegone<<" = icmp eq "<<ty<<" "<<value(n.right)<<", -1\n";
                        out<<"  "<<special<<" = and i1 "<<ismin<<", "<<isnegone<<"\n";
                        fail_if(special,"@.code.overflow","@.msg.overflow","arith.div.overflow",n.line,n.column);
                    }
                    out<<"  "<<value(n.out)<<" = "<<(is_signed_integer(ot)?"s":"u")<<(n.op=="/"?"div":"rem")<<" "<<ty<<" "<<value(n.left)<<", "<<value(n.right)<<"\n";
                    return;
                }
                std::string p;
                if(n.op=="==")p="eq";else if(n.op=="!=")p="ne";
                else if(n.op=="<")p=is_signed_integer(ot)?"slt":"ult";
                else if(n.op=="<=")p=is_signed_integer(ot)?"sle":"ule";
                else if(n.op==">")p=is_signed_integer(ot)?"sgt":"ugt";
                else p=is_signed_integer(ot)?"sge":"uge";
                out<<"  "<<value(n.out)<<" = icmp "<<p<<" "<<ty<<" "<<value(n.left)<<", "<<value(n.right)<<"\n";
                return;
            }
            if(is_float(ot)){
                const auto ty=llvm_type(ot);
                if(n.op=="+")out<<"  "<<value(n.out)<<" = fadd "<<ty<<" "<<value(n.left)<<", "<<value(n.right)<<"\n";
                else if(n.op=="-")out<<"  "<<value(n.out)<<" = fsub "<<ty<<" "<<value(n.left)<<", "<<value(n.right)<<"\n";
                else if(n.op=="*")out<<"  "<<value(n.out)<<" = fmul "<<ty<<" "<<value(n.left)<<", "<<value(n.right)<<"\n";
                else if(n.op=="/")out<<"  "<<value(n.out)<<" = fdiv "<<ty<<" "<<value(n.left)<<", "<<value(n.right)<<"\n";
                else{std::string p;if(n.op=="==")p="oeq";else if(n.op=="!=")p="une";else if(n.op=="<")p="olt";else if(n.op=="<=")p="ole";else if(n.op==">")p="ogt";else p="oge";out<<"  "<<value(n.out)<<" = fcmp "<<p<<" "<<ty<<" "<<value(n.left)<<", "<<value(n.right)<<"\n";}
                return;
            }
            if(ot.kind==TypeKind::Bool){if(n.op=="and"||n.op=="or")out<<"  "<<value(n.out)<<" = "<<n.op<<" i1 "<<value(n.left)<<", "<<value(n.right)<<"\n";else out<<"  "<<value(n.out)<<" = icmp "<<(n.op=="=="?"eq":"ne")<<" i1 "<<value(n.left)<<", "<<value(n.right)<<"\n";}
        }
        if constexpr(std::is_same_v<T,ir::ToString>){
            values[n.out]=Type::simple(TypeKind::String);
            if(n.source_type.kind==TypeKind::BigInt){
                out<<"  "<<value(n.out)<<" = call ptr @quidra_bigint_text(ptr "<<value(n.value)<<")\n";
            }else if(n.source_type.kind==TypeKind::BigReal){
                out<<"  "<<value(n.out)<<" = call ptr @quidra_bigreal_text(ptr "<<value(n.value)<<", i32 34)\n";
            }else if(is_integer(n.source_type)){
                out<<"  "<<value(n.out)<<" = call ptr @quidra_alloc(i64 32)\n";
                std::string widened=value(n.value);
                if(integer_width(n.source_type)<64){
                    widened=temp("text.int");
                    out<<"  "<<widened<<" = "<<(is_signed_integer(n.source_type)?"sext":"zext")<<" "<<llvm_type(n.source_type)<<" "<<value(n.value)<<" to i64\n";
                }
                out<<"  call i32 (ptr, i64, ptr, ...) @snprintf(ptr "<<value(n.out)<<", i64 32, ptr "<<(is_signed_integer(n.source_type)?"@.fmt.int.text":"@.fmt.uint.text")<<", i64 "<<widened<<")\n";
            }else if(is_float(n.source_type)){
                std::string widened=value(n.value);
                if(n.source_type.kind==TypeKind::Float32){
                    widened=temp("text.float");
                    out<<"  "<<widened<<" = fpext float "<<value(n.value)<<" to double\n";
                }
                out<<"  "<<value(n.out)<<" = call ptr @quidra_float_text(double "<<widened<<")\n";
            }else if(n.source_type.kind==TypeKind::Bool){
                out<<"  "<<value(n.out)<<" = select i1 "<<value(n.value)<<", ptr @.bool.true, ptr @.bool.false\n";
            }else if(n.source_type.kind==TypeKind::Bin){
                out<<"  "<<value(n.out)<<" = call ptr @quidra_bin_string(ptr "<<value(n.value)<<")\n";
            }else if(n.source_type.kind==TypeKind::String||n.source_type.kind==TypeKind::Error){
                out<<"  "<<value(n.out)<<" = getelementptr i8, ptr "<<value(n.value)<<", i64 0\n";
            }
        }
        if constexpr(std::is_same_v<T,ir::FormatNumber>){
            values[n.out]=Type::simple(TypeKind::String);
            const auto integer=n.integer_width?std::to_string(*n.integer_width):"-1";
            const auto fractional=n.fractional_digits?std::to_string(*n.fractional_digits):"-1";
            const auto significant=n.significant_digits?std::to_string(*n.significant_digits):"-1";
            if(n.source_type.kind==TypeKind::BigInt){
                out<<"  "<<value(n.out)<<" = call ptr @quidra_bigint_text(ptr "<<value(n.value)<<")\n";
            }else if(n.source_type.kind==TypeKind::BigReal){
                const int precision=n.significant_digits?static_cast<int>(*n.significant_digits):34;
                out<<"  "<<value(n.out)<<" = call ptr @quidra_bigreal_text(ptr "<<value(n.value)<<", i32 "<<precision<<")\n";
            }else if(is_integer(n.source_type)){
                std::string widened=value(n.value);
                if(integer_width(n.source_type)<64){
                    widened=temp("format.int");
                    out<<"  "<<widened<<" = "<<(is_signed_integer(n.source_type)?"sext":"zext")<<" "<<llvm_type(n.source_type)<<" "<<value(n.value)<<" to i64\n";
                }
                out<<"  "<<value(n.out)<<" = call ptr "<<(is_signed_integer(n.source_type)?"@quidra_format_signed":"@quidra_format_unsigned")
                   <<"(i64 "<<widened<<", i32 "<<integer<<", i32 "<<fractional<<", i32 "<<significant<<", i32 "<<(n.zero?1:0)<<")\n";
            }else{
                std::string widened=value(n.value);
                if(n.source_type.kind==TypeKind::Float32){
                    widened=temp("format.float");
                    out<<"  "<<widened<<" = fpext float "<<value(n.value)<<" to double\n";
                }
                out<<"  "<<value(n.out)<<" = call ptr @quidra_format_number(double "<<widened<<", i32 "<<integer
                   <<", i32 "<<fractional<<", i32 "<<significant<<", i32 "<<(n.zero?1:0)<<")\n";
            }
        }
        if constexpr(std::is_same_v<T,ir::Call>){
            if(n.result.kind!=TypeKind::Void&&n.result.kind!=TypeKind::Never) values[n.out]=n.result;
            const auto& sig=signatures.at(n.callee);
            const bool external=external_symbols.contains(n.callee);
            std::vector<std::string> call_values;
            std::vector<std::string> ffi_lengths(n.args.size());
            call_values.reserve(n.args.size());
            for(std::size_t i=0;i<n.args.size();++i){
                const auto& parameter=sig.parameters[i];
                const bool ffi_borrowed_buffer=external&&
                    (parameter.type.kind==TypeKind::String||parameter.type.kind==TypeKind::Bin);
                std::string argument;
                if(ffi_borrowed_buffer){
                    if(!n.args[i].writable_address)
                        throw std::logic_error("external borrowed buffer is missing its typed storage address");
                    const auto borrowed=temp("ffi.borrowed.value");
                    out<<"  "<<borrowed<<" = load ptr, ptr "<<value(*n.args[i].writable_address)<<"\n";
                    argument=borrowed;
                }else{
                    argument=value(n.args[i].value);
                }
                if(ffi_borrowed_buffer&&parameter.type.kind==TypeKind::Bin){
                    const auto length=temp("ffi.bin.length");
                    const auto data=temp("ffi.bin.data");
                    out<<"  "<<length<<" = call i64 @quidra_bin_byte_length(ptr "<<argument<<")\n";
                    out<<"  "<<data<<" = getelementptr inbounds i8, ptr "<<argument<<", i64 8\n";
                    ffi_lengths[i]=length;
                    argument=data;
                }else if(ffi_borrowed_buffer&&parameter.type.kind==TypeKind::String){
                    const auto length=temp("ffi.string.length");
                    out<<"  "<<length<<" = call i64 @strlen(ptr "<<argument<<")\n";
                    ffi_lengths[i]=length;
                }
                call_values.push_back(std::move(argument));
            }
            if(recursive_callees.contains(n.callee)){
                out<<"  store i64 "<<n.line<<", ptr @.quidra.source.line\n"
                   <<"  store i64 "<<n.column<<", ptr @.quidra.source.column\n";
            }
            out<<"  ";
            if(n.result.kind!=TypeKind::Void&&n.result.kind!=TypeKind::Never)
                out<<value(n.out)<<" = ";
            out<<"call ";
            if(external) out<<c_abi_return_attribute(n.result);
            out<<llvm_type(n.result)<<" @"<<call_symbol(n.callee)<<"(";
            for(std::size_t i=0;i<n.args.size();++i){
                if(i) out<<", ";
                const auto& parameter=sig.parameters[i];
                const bool ffi_borrowed_buffer=external&&
                    (parameter.type.kind==TypeKind::String||parameter.type.kind==TypeKind::Bin);
                if(ffi_borrowed_buffer){
                    out<<llvm_type(parameter.type)<<c_abi_parameter_attribute(parameter.type)
                       <<" "<<call_values[i]<<", i64 "<<ffi_lengths[i];
                }else if(parameter.writable){
                    out<<"ptr "<<value(*n.args[i].writable_address);
                }else{
                    out<<llvm_type(parameter.type);
                    if(external) out<<c_abi_parameter_attribute(parameter.type);
                    out<<" "<<call_values[i];
                }
            }
            out<<")\n";
            if(n.result.kind==TypeKind::Never) out<<"  unreachable\n";
        }
        if constexpr(std::is_same_v<T,ir::VariantMake>){values[n.out]=n.container_type;out<<"  "<<value(n.out)<<" = call ptr @quidra_alloc(i64 16)\n  store i64 "<<n.tag<<", ptr "<<value(n.out)<<"\n";if(n.payload_type.kind!=TypeKind::Void&&n.payload_type.kind!=TypeKind::None){auto p="%variant.payload.ptr."+std::to_string(n.out);out<<"  "<<p<<" = getelementptr inbounds i8, ptr "<<value(n.out)<<", i64 8\n  store "<<llvm_type(n.payload_type)<<" "<<value(n.payload)<<", ptr "<<p<<"\n";}}
        if constexpr(std::is_same_v<T,ir::VariantTag>){values[n.out]=Type::simple(TypeKind::Int);out<<"  "<<value(n.out)<<" = load i64, ptr "<<value(n.container)<<"\n";}
        if constexpr(std::is_same_v<T,ir::VariantPayload>){values[n.out]=n.payload_type;if(n.payload_type.kind!=TypeKind::Void&&n.payload_type.kind!=TypeKind::None){auto p="%variant.read.ptr."+std::to_string(n.out);out<<"  "<<p<<" = getelementptr inbounds i8, ptr "<<value(n.container)<<", i64 8\n  "<<value(n.out)<<" = load "<<llvm_type(n.payload_type)<<", ptr "<<p<<"\n";}}
        if constexpr(std::is_same_v<T,ir::ReplReplayMode>){
            out<<"  store i1 "<<(n.active?"true":"false")<<", ptr @.quidra.repl.replaying\n";
        }
        if constexpr(std::is_same_v<T,ir::Print>||std::is_same_v<T,ir::Write>){
            const auto replay=temp("repl.output.replay");
            const auto emit_label=unique_label("repl.output.emit");
            const auto done_label=unique_label("repl.output.done");
            out<<"  "<<replay<<" = load i1, ptr @.quidra.repl.replaying\n"
               <<"  br i1 "<<replay<<", label %"<<done_label<<", label %"<<emit_label<<"\n"
               <<emit_label<<":\n";
            const bool newline=std::is_same_v<T,ir::Print>;
            if(is_integer(n.type)){
                std::string widened=value(n.value);
                if(integer_width(n.type)<64){widened=temp("print.int");out<<"  "<<widened<<" = "<<(is_signed_integer(n.type)?"sext":"zext")<<" "<<llvm_type(n.type)<<" "<<value(n.value)<<" to i64\n";}
                out<<"  call i32 (ptr, ...) @printf(ptr "<<(is_signed_integer(n.type)?(newline?"@.fmt.int":"@.fmt.int.write"):(newline?"@.fmt.uint":"@.fmt.uint.write"))<<", i64 "<<widened<<")\n";
            }else if(n.type.kind==TypeKind::BigInt||n.type.kind==TypeKind::BigReal){
                auto text=temp("print.exact.text");
                if(n.type.kind==TypeKind::BigInt)
                    out<<"  "<<text<<" = call ptr @quidra_bigint_text(ptr "<<value(n.value)<<")\n";
                else
                    out<<"  "<<text<<" = call ptr @quidra_bigreal_text(ptr "<<value(n.value)<<", i32 34)\n";
                if(newline)out<<"  call i32 @puts(ptr "<<text<<")\n";
                else out<<"  call i32 (ptr, ...) @printf(ptr @.fmt.string.write, ptr "<<text<<")\n";
                out<<"  call void @quidra_managed_release(ptr "<<text<<", ptr null)\n";
            }else if(is_float(n.type)){
                std::string widened=value(n.value);
                if(n.type.kind==TypeKind::Float32){widened=temp("print.float");out<<"  "<<widened<<" = fpext float "<<value(n.value)<<" to double\n";}
                auto text=temp("print.float.text");
                out<<"  "<<text<<" = call ptr @quidra_float_text(double "<<widened<<")\n";
                if(newline)out<<"  call i32 @puts(ptr "<<text<<")\n";
                else out<<"  call i32 (ptr, ...) @printf(ptr @.fmt.string.write, ptr "<<text<<")\n";
                out<<"  call void @quidra_managed_release(ptr "<<text<<", ptr null)\n";
            }else if(n.type.kind==TypeKind::Bool){
                auto t=temp("print.bool");out<<"  "<<t<<" = select i1 "<<value(n.value)<<", ptr @.bool.true, ptr @.bool.false\n";
                if(newline)out<<"  call i32 @puts(ptr "<<t<<")\n";else out<<"  call i32 (ptr, ...) @printf(ptr @.fmt.string.write, ptr "<<t<<")\n";
            }else if(n.type.kind==TypeKind::Bin){
                auto text=temp("print.bin");
                out<<"  "<<text<<" = call ptr @quidra_bin_string(ptr "<<value(n.value)<<")\n";
                if(newline)out<<"  call i32 @puts(ptr "<<text<<")\n";
                else out<<"  call i32 (ptr, ...) @printf(ptr @.fmt.string.write, ptr "<<text<<")\n";
                out<<"  call void @quidra_managed_release(ptr "<<text<<", ptr null)\n";
            }else{
                if(newline)out<<"  call i32 @puts(ptr "<<value(n.value)<<")\n";else out<<"  call i32 (ptr, ...) @printf(ptr @.fmt.string.write, ptr "<<value(n.value)<<")\n";
            }
            out<<"  br label %"<<done_label<<"\n"<<done_label<<":\n";
        }
        if constexpr(std::is_same_v<T,ir::ReplDisplay>){
            const auto begin=pool.intern("__QUIDRA_REPL_RESULT_BEGIN_6D8F2C__");
            const auto end=pool.intern("__QUIDRA_REPL_RESULT_END_6D8F2C__");
            active_repl_array_index_scratch=scratch(ins);
            out<<"  call i32 @puts(ptr @"<<begin<<")\n";
            emit_repl_value(n.type, n.type.kind == TypeKind::None ? std::string{} : value(n.value),
                            n.initialized_paths);
            emit_repl_text("\n");
            out<<"  call i32 @puts(ptr @"<<end<<")\n";
            active_repl_array_index_scratch.clear();
        }
        if constexpr(std::is_same_v<T,ir::Input>){
            values[n.out]=n.result_type;
            const auto result=value(n.out),text_slot=scratch(ins),status=temp("input.status");
            out<<"  "<<result<<" = call ptr @quidra_alloc(i64 16)\n";
            out<<"  store ptr null, ptr "<<text_slot<<"\n";
            out<<"  "<<status<<" = call i32 @quidra_input_read(ptr "<<text_slot<<")\n";

            const auto is_ok=temp("input.is.ok"),ok_label=unique_label("input.ok");
            const auto non_ok_label=unique_label("input.non.ok"),done_label=unique_label("input.done");
            out<<"  "<<is_ok<<" = icmp eq i32 "<<status<<", 1\n";
            out<<"  br i1 "<<is_ok<<", label %"<<ok_label<<", label %"<<non_ok_label<<"\n";

            out<<ok_label<<":\n";
            out<<"  store i64 "<<case_index(n.result_type,Type::simple(TypeKind::String))<<", ptr "<<result<<"\n";
            const auto string_payload=temp("input.string.payload"),text=temp("input.text");
            out<<"  "<<string_payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n";
            out<<"  "<<text<<" = load ptr, ptr "<<text_slot<<"\n";
            out<<"  store ptr "<<text<<", ptr "<<string_payload<<"\n";
            out<<"  br label %"<<done_label<<"\n";

            out<<non_ok_label<<":\n";
            const auto is_eof=temp("input.is.eof"),eof_label=unique_label("input.eof"),error_label=unique_label("input.error");
            out<<"  "<<is_eof<<" = icmp eq i32 "<<status<<", 0\n";
            out<<"  br i1 "<<is_eof<<", label %"<<eof_label<<", label %"<<error_label<<"\n";

            out<<eof_label<<":\n";
            out<<"  store i64 "<<case_index(n.result_type,Type::simple(TypeKind::None))<<", ptr "<<result<<"\n";
            out<<"  br label %"<<done_label<<"\n";

            out<<error_label<<":\n";
            out<<"  store i64 "<<case_index(n.result_type,Type::simple(TypeKind::Error))<<", ptr "<<result<<"\n";
            const auto error_payload=temp("input.error.payload");
            out<<"  "<<error_payload<<" = getelementptr inbounds i8, ptr "<<result<<", i64 8\n";
            out<<"  store ptr @.err.input, ptr "<<error_payload<<"\n";
            out<<"  br label %"<<done_label<<"\n";

            out<<done_label<<":\n";
        }
        if constexpr(std::is_same_v<T,ir::Exit>){auto s="%exit.status."+std::to_string(n.status);out<<"  "<<s<<" = trunc i64 "<<value(n.status)<<" to i32\n";cleanup_owned_values();out<<"  call void @exit(i32 "<<s<<")\n  unreachable\n";}
        if constexpr(std::is_same_v<T,ir::RangeCheckStep>){auto z="%range.zero."+std::to_string(n.step),bad="range.bad."+std::to_string(n.step),ok="range.ok."+std::to_string(n.step);out<<"  "<<z<<" = icmp eq i64 "<<value(n.step)<<", 0\n  br i1 "<<z<<", label %"<<bad<<", label %"<<ok<<"\n"<<bad<<":\n  call void @quidra_fail_at(ptr @.code.range.step, ptr @.msg.range.step, i64 "<<n.line<<", i64 "<<n.column<<")\n  unreachable\n"<<ok<<":\n";}
        if constexpr(std::is_same_v<T,ir::Return>){cleanup_owned_values();if(guard_stack_depth)out<<"  call void @quidra_stack_leave()\n";if(n.type.kind==TypeKind::Void)out<<"  ret void\n";else out<<"  ret "<<llvm_type(n.type)<<" "<<value(n.value)<<"\n";}
        if constexpr(std::is_same_v<T,ir::ReturnVoid>){cleanup_owned_values();if(guard_stack_depth)out<<"  call void @quidra_stack_leave()\n";out<<"  ret void\n";}
        if constexpr(std::is_same_v<T,ir::Jump>)out<<"  br label %"<<n.target<<"\n";
        if constexpr(std::is_same_v<T,ir::Branch>)out<<"  br i1 "<<value(n.condition)<<", label %"<<n.if_true<<", label %"<<n.if_false<<"\n";
    },ins);}

    std::string emit(){if(fn.external_symbol){out<<"declare "<<c_abi_return_attribute(fn.result)<<llvm_type(fn.result)<<" @"<<*fn.external_symbol<<"(";bool first=true;for(const auto& parameter:fn.parameters){if(!first)out<<", ";first=false;out<<llvm_type(parameter.type)<<c_abi_parameter_attribute(parameter.type);if(parameter.type.kind==TypeKind::String||parameter.type.kind==TypeKind::Bin)out<<", i64";}out<<")\n\n";return out.str();}scan();out<<"define "<<llvm_type(fn.result)<<" @"<<(fn.entrypoint?"main":mangle(fn.name))<<"(";if(fn.entrypoint){out<<"i32 %quidra.argc, ptr %quidra.argv";}else{for(std::size_t i=0;i<fn.parameters.size();++i){if(i)out<<", ";const auto& parameter=fn.parameters[i];if(parameter.writable){out<<"ptr nocapture nonnull";if(parameter.is_const)out<<" readonly";}else out<<llvm_type(parameter.type);out<<" "<<arg(parameter.name);}}out<<")";if(debug_subprogram)out<<" !dbg !"<<*debug_subprogram;out<<" {\n";for(std::size_t bi=0;bi<fn.blocks.size();++bi){const auto&b=fn.blocks[bi];out<<b.label<<":\n";if(bi==0){if(fn.entrypoint)out<<"  call void @quidra_runtime_set_args(i32 %quidra.argc, ptr %quidra.argv)\n";if(guard_stack_depth)out<<"  call void @quidra_stack_enter()\n";for(const auto&[name,type]:locals)if(!writable_params.contains(name)){out<<"  "<<local(name)<<" = alloca "<<llvm_type(type)<<"\n";if(requires_lifetime_management(type))out<<"  store ptr null, ptr "<<local(name)<<"\n";}for(const auto&[name,type]:references){out<<"  "<<local(name)<<" = alloca ptr\n  store ptr null, ptr "<<local(name)<<"\n";}emit_entry_scratch();for(const auto&p:fn.parameters)if(!p.writable)out<<"  store "<<llvm_type(p.type)<<" "<<arg(p.name)<<", ptr "<<local(p.name)<<"\n";}for(const auto&i:b.instructions)emit_instruction(i);bool term=false;if(!b.instructions.empty()){const auto&last=b.instructions.back();term=std::holds_alternative<ir::Return>(last)||std::holds_alternative<ir::ReturnVoid>(last)||std::holds_alternative<ir::Exit>(last)||std::holds_alternative<ir::Jump>(last)||std::holds_alternative<ir::Branch>(last)||(std::holds_alternative<ir::Call>(last)&&std::get<ir::Call>(last).result.kind==TypeKind::Never);}if(!term)out<<"  unreachable\n";}out<<"}\n\n";return out.str();}
};


struct ArrayCastPair {
    Type source;
    Type target;
};

void collect_array_cast_pair(const Type& source, const Type& target,
                             std::map<std::string,ArrayCastPair>& pairs) {
    if (source.kind != TypeKind::Array || target.kind != TypeKind::Array ||
        source.length != target.length) {
        throw std::logic_error("array numeric cast must preserve array structure");
    }
    if (source.first->kind == TypeKind::Array) {
        if (target.first->kind != TypeKind::Array)
            throw std::logic_error("array numeric cast nesting mismatch");
        collect_array_cast_pair(*source.first, *target.first, pairs);
    } else if (target.first->kind == TypeKind::Array) {
        throw std::logic_error("array numeric cast nesting mismatch");
    }
    pairs.emplace(type_id(source) + "_to_" + type_id(target),
                  ArrayCastPair{source,target});
}

std::map<std::string,ArrayCastPair> collect_array_cast_pairs(const ir::Module& module) {
    std::map<std::string,ArrayCastPair> pairs;
    for (const auto& function : module.functions)
        for (const auto& block : function.blocks)
            for (const auto& instruction : block.instructions)
                if (const auto* cast = std::get_if<ir::ArrayNumericCast>(&instruction))
                    collect_array_cast_pair(cast->source_type, cast->target_type, pairs);
    return pairs;
}

std::string emit_array_cast_helper(const Type& source, const Type& target,
                                   const ArrayLayoutPolicy& array_layout) {
    if (source.kind != TypeKind::Array || target.kind != TypeKind::Array ||
        source.length != target.length) {
        throw std::logic_error("invalid array numeric cast helper types");
    }
    const auto& source_element = *source.first;
    const auto& target_element = *target.first;
    const bool nested = source_element.kind == TypeKind::Array;
    if (nested != (target_element.kind == TypeKind::Array))
        throw std::logic_error("array numeric cast helper nesting mismatch");

    const bool source_fixed = is_fixed_array(source);
    const bool target_fixed = is_fixed_array(target);
    if (source_fixed != target_fixed)
        throw std::logic_error("array numeric cast must preserve static dimensions");

    const auto source_stride = array_element_stride(source,array_layout);
    const auto target_stride = array_element_stride(target,array_layout);
    const auto source_offset = source_fixed ? 0 : 8;
    const auto target_offset = target_fixed ? 0 : 8;
    std::ostringstream o;
    o << "define ptr " << array_cast_name(source,target)
      << "(ptr %src, i64 %line, i64 %column) {\n"
      << "entry:\n"
      << "  %null = icmp eq ptr %src, null\n"
      << "  br i1 %null, label %null.ret, label %cast.body\n"
      << "null.ret:\n  ret ptr null\n"
      << "cast.body:\n";
    if (source_fixed) o << "  %len = add i64 0, " << source.length << "\n";
    else o << "  %len = load i64, ptr %src, align 1\n";

    if (target_fixed) {
        const auto storage = fixed_array_storage(target,array_layout);
        const auto bytes = fixed_array_storage_bytes(target,array_layout);
        const auto unit = runtime_storage_bytes(storage.leaf);
        o << "  %dst = call ptr @quidra_alloc(i64 " << bytes << ")\n"
          << "  call void @quidra_init_create(ptr %dst, i64 " << storage.count
          << ", i64 " << unit << ", i64 0, i32 1)\n";
    } else {
        o << "  %dst = call ptr @quidra_array_alloc(i64 %len, i64 "
          << target_stride << ", i32 1)\n";
    }

    o << "  br label %cond\n"
      << "cond:\n"
      << "  %i = phi i64 [ 0, %cast.body ], [ %next, %body ]\n"
      << "  %more = icmp slt i64 %i, %len\n"
      << "  br i1 %more, label %body, label %done\n"
      << "body:\n"
      << "  %src.off0 = mul i64 %i, " << source_stride << "\n"
      << "  %src.off = add i64 %src.off0, " << source_offset << "\n"
      << "  %src.slot = getelementptr inbounds i8, ptr %src, i64 %src.off\n"
      << "  %dst.off0 = mul i64 %i, " << target_stride << "\n"
      << "  %dst.off = add i64 %dst.off0, " << target_offset << "\n"
      << "  %dst.slot = getelementptr inbounds i8, ptr %dst, i64 %dst.off\n";

    if (nested) {
        const bool source_inline =
            source_fixed && array_layout.inline_fixed_child(source);
        const bool target_inline =
            target_fixed && array_layout.inline_fixed_child(target);
        if (source_inline) {
            o << "  %child.src = getelementptr inbounds i8, ptr %src.slot, i64 0\n";
        } else {
            o << "  call void @quidra_init_check(ptr %src.slot, i64 %line, i64 %column)\n"
              << "  %child.src = load ptr, ptr %src.slot, align 1\n";
        }
        o << "  %child.dst = call ptr "
          << array_cast_name(source_element,target_element)
          << "(ptr %child.src, i64 %line, i64 %column)\n";
        if (target_inline) {
            const auto child_bytes =
                fixed_array_storage_bytes(target_element,array_layout);
            o << "  call ptr @memcpy(ptr %dst.slot, ptr %child.dst, i64 "
              << child_bytes << ")\n"
              << "  call void @quidra_managed_release(ptr %child.dst, ptr null)\n";
        } else {
            o << "  store ptr %child.dst, ptr %dst.slot, align 1\n";
        }
    } else {
        if (!is_numeric(source_element) || !is_numeric(target_element))
            throw std::logic_error("array numeric cast leaf must be numeric");

        if (is_tensor_numeric(source_element) && is_tensor_numeric(target_element)) {
            o << "  call void @quidra_numeric_cast_element(ptr %dst.slot, ptr %src.slot, i32 "
              << tensor_dtype_code(source_element) << ", i32 "
              << tensor_dtype_code(target_element)
              << ", i64 %line, i64 %column)\n";
        } else {
            o << "  call void @quidra_init_check(ptr %src.slot, i64 %line, i64 %column)\n";
            const std::string src_value="%exact.cast.src";
            o << "  " << src_value << " = load " << llvm_type(source_element)
              << ", ptr %src.slot, align 1\n";

            if (target_element.kind == TypeKind::BigInt) {
                if (source_element.kind == TypeKind::BigInt) {
                    o << "  call void @quidra_managed_retain(ptr " << src_value << ")\n"
                      << "  store ptr " << src_value << ", ptr %dst.slot, align 1\n";
                } else if (source_element.kind == TypeKind::BigReal) {
                    o << "  %exact.cast.out = call ptr @quidra_bigreal_to_bigint(ptr "
                      << src_value << ", i64 %line, i64 %column)\n"
                      << "  store ptr %exact.cast.out, ptr %dst.slot, align 1\n";
                } else if (is_integer(source_element)) {
                    std::string widened=src_value;
                    if (integer_width(source_element)<64) {
                        widened="%exact.cast.widen";
                        o << "  " << widened << " = "
                          << (is_signed_integer(source_element)?"sext":"zext") << " "
                          << llvm_type(source_element) << " " << src_value << " to i64\n";
                    }
                    o << "  %exact.cast.out = call ptr @"
                      << (is_signed_integer(source_element)?"quidra_bigint_from_i64":"quidra_bigint_from_u64")
                      << "(i64 " << widened << ")\n"
                      << "  store ptr %exact.cast.out, ptr %dst.slot, align 1\n";
                } else {
                    throw std::logic_error("unsupported array cast to bigint");
                }
            } else if (target_element.kind == TypeKind::BigReal) {
                if (source_element.kind == TypeKind::BigReal) {
                    o << "  call void @quidra_managed_retain(ptr " << src_value << ")\n"
                      << "  store ptr " << src_value << ", ptr %dst.slot, align 1\n";
                } else if (source_element.kind == TypeKind::BigInt) {
                    o << "  %exact.cast.out = call ptr @quidra_bigreal_from_bigint(ptr "
                      << src_value << ")\n"
                      << "  store ptr %exact.cast.out, ptr %dst.slot, align 1\n";
                } else if (is_integer(source_element)) {
                    std::string widened=src_value;
                    if (integer_width(source_element)<64) {
                        widened="%exact.cast.widen";
                        o << "  " << widened << " = "
                          << (is_signed_integer(source_element)?"sext":"zext") << " "
                          << llvm_type(source_element) << " " << src_value << " to i64\n";
                    }
                    o << "  %exact.cast.out = call ptr @"
                      << (is_signed_integer(source_element)?"quidra_bigreal_from_i64":"quidra_bigreal_from_u64")
                      << "(i64 " << widened << ")\n"
                      << "  store ptr %exact.cast.out, ptr %dst.slot, align 1\n";
                } else if (is_float(source_element)) {
                    std::string widened=src_value;
                    if (source_element.kind==TypeKind::Float32) {
                        widened="%exact.cast.float";
                        o << "  " << widened << " = fpext float " << src_value << " to double\n";
                    }
                    o << "  %exact.cast.out = call ptr @quidra_bigreal_from_float64(double "
                      << widened << ")\n"
                      << "  store ptr %exact.cast.out, ptr %dst.slot, align 1\n";
                } else {
                    throw std::logic_error("unsupported array cast to bigreal");
                }
            } else if (is_integer(target_element)) {
                std::string integer_source;
                if (source_element.kind == TypeKind::BigReal) {
                    o << "  %exact.cast.integer = call ptr @quidra_bigreal_to_bigint(ptr "
                      << src_value << ", i64 %line, i64 %column)\n";
                    integer_source="%exact.cast.integer";
                } else if (source_element.kind == TypeKind::BigInt) {
                    integer_source=src_value;
                } else {
                    throw std::logic_error("unsupported exact array integer source");
                }
                const auto raw="%exact.cast.raw";
                o << "  " << raw << " = call i64 @"
                  << (is_signed_integer(target_element)?"quidra_bigint_to_i64":"quidra_bigint_to_u64")
                  << "(ptr " << integer_source << ", i32 " << integer_width(target_element)
                  << ", i64 %line, i64 %column)\n";
                if (source_element.kind == TypeKind::BigReal)
                    o << "  call void @quidra_managed_release(ptr %exact.cast.integer, ptr @quidra_bigint_drop)\n";
                if (integer_width(target_element)<64)
                    o << "  %exact.cast.out = trunc i64 " << raw << " to "
                      << llvm_type(target_element) << "\n";
                else
                    o << "  %exact.cast.out = add i64 " << raw << ", 0\n";
                o << "  store " << llvm_type(target_element)
                  << " %exact.cast.out, ptr %dst.slot, align 1\n";
            } else if (is_float(target_element)) {
                if (source_element.kind != TypeKind::BigInt &&
                    source_element.kind != TypeKind::BigReal)
                    throw std::logic_error("unsupported exact array float source");
                const bool from_real=source_element.kind==TypeKind::BigReal;
                if (target_element.kind==TypeKind::Float32)
                    o << "  %exact.cast.out = call float @"
                      << (from_real?"quidra_bigreal_to_float32":"quidra_bigint_to_float32")
                      << "(ptr " << src_value << ", i64 %line, i64 %column)\n";
                else
                    o << "  %exact.cast.out = call double @"
                      << (from_real?"quidra_bigreal_to_float64":"quidra_bigint_to_float64")
                      << "(ptr " << src_value << ", i64 %line, i64 %column)\n";
                o << "  store " << llvm_type(target_element)
                  << " %exact.cast.out, ptr %dst.slot, align 1\n";
            } else {
                throw std::logic_error("unsupported exact array numeric cast");
            }
        }
    }

    o << "  %next = add i64 %i, 1\n"
      << "  br label %cond\n"
      << "done:\n  ret ptr %dst\n"
      << "}\n\n";
    return o.str();
}

void collect_clone_type(const Type&t,std::map<std::string,Type>&types,const std::unordered_map<std::string,ir::ClassLayout>&layouts){
 if(!requires_value_clone(t))return;
 const auto id=type_id(t);if(types.contains(id))return;
 types[id]=t;
 if(t.kind==TypeKind::Array){collect_clone_type(*t.first,types,layouts);}
 else if(t.kind==TypeKind::Union){for(const auto&c:t.cases)collect_clone_type(c,types,layouts);}
 else if(t.kind==TypeKind::Class){for(const auto&f:layouts.at(t.class_name).fields)collect_clone_type(f,types,layouts);}
}
void collect_clone_types(const ir::Module&m,std::map<std::string,Type>&types,const std::unordered_map<std::string,ir::ClassLayout>&layouts){for(const auto&f:m.functions)for(const auto&b:f.blocks)for(const auto&i:b.instructions)if(const auto*c=std::get_if<ir::Clone>(&i))collect_clone_type(c->type,types,layouts);}

std::string emit_clone_helper(const Type&t,const std::unordered_map<std::string,ir::ClassLayout>&layouts,const ArrayLayoutPolicy&array_layout){
 if(t.kind==TypeKind::Neural){
    std::ostringstream o;
    o<<"define ptr "<<clone_name(t)<<"(ptr %src) {\nentry:\n"
     <<"  %dst = call ptr @quidra_neural_clone(ptr %src)\n"
     <<"  ret ptr %dst\n}\n\n";
    return o.str();
 }
 if(t.kind==TypeKind::Gradients){
    std::ostringstream o;
    o<<"define ptr "<<clone_name(t)<<"(ptr %src) {\nentry:\n"
     <<"  %dst = call ptr @quidra_neural_gradients_clone(ptr %src)\n"
     <<"  ret ptr %dst\n}\n\n";
    return o.str();
 }
 if(t.kind==TypeKind::Tensor){
    std::ostringstream o;
    o<<"define ptr "<<clone_name(t)<<"(ptr %src) {\nentry:\n"
     <<"  %dst = call ptr @quidra_tensor_clone(ptr %src)\n"
     <<"  ret ptr %dst\n}\n\n";
    return o.str();
 }
 if(t.kind==TypeKind::Class && t.class_name=="$std.http.Response"){
    std::ostringstream o;
    o<<"define ptr "<<clone_name(t)<<"(ptr %src) {\nentry:\n"
     <<"  %dst = call ptr @quidra_http_response_clone(ptr %src)\n"
     <<"  ret ptr %dst\n}\n\n";
    return o.str();
 }
 if(t.kind==TypeKind::Bin){
    std::ostringstream o;
    o<<"define ptr "<<clone_name(t)<<"(ptr %src) {\nentry:\n"
     <<"  %dst = call ptr @quidra_bin_clone(ptr %src)\n"
     <<"  ret ptr %dst\n}\n\n";
    return o.str();
 }
 if(t.kind==TypeKind::Union){
    std::ostringstream o;
    o<<"define ptr "<<clone_name(t)<<"(ptr %src) {\nentry:\n  %null = icmp eq ptr %src, null\n  br i1 %null, label %null.ret, label %clone.body\nnull.ret:\n  ret ptr null\nclone.body:\n  %dst = call ptr @quidra_alloc(i64 16)\n  %tag = load i64, ptr %src, align 1\n  store i64 %tag, ptr %dst, align 1\n  %sp = getelementptr i8, ptr %src, i64 8\n  %dp = getelementptr i8, ptr %dst, i64 8\n  switch i64 %tag, label %done [";
    for(std::size_t i=0;i<t.cases.size();++i)o<<" i64 "<<i<<", label %case"<<i;
    o<<" ]\n";
    for(std::size_t i=0;i<t.cases.size();++i){
        const auto&c=t.cases[i];
        o<<"case"<<i<<":\n";
        if(c.kind!=TypeKind::Void&&c.kind!=TypeKind::None){
            o<<"  %v"<<i<<" = load "<<llvm_type(c)<<", ptr %sp, align 1\n";
            if(requires_value_clone(c))
                o<<"  %copy"<<i<<" = call ptr "<<clone_name(c)<<"(ptr %v"<<i<<")\n  store ptr %copy"<<i<<", ptr %dp, align 1\n";
            else if(uses_shared_immutable_storage(c))
                o<<"  call void @quidra_managed_retain(ptr %v"<<i<<")\n  store ptr %v"<<i<<", ptr %dp, align 1\n";
            else o<<"  store "<<llvm_type(c)<<" %v"<<i<<", ptr %dp, align 1\n";
        }
        o<<"  br label %done\n";
    }
    o<<"done:\n  ret ptr %dst\n}\n\n";
    return o.str();
 }
 if(t.kind==TypeKind::Class){
    const auto&layout=layouts.at(t.class_name);
    std::ostringstream o;
    const auto bytes=class_storage_bytes(layout);
    o<<"define ptr "<<clone_name(t)<<"(ptr %src) {\nentry:\n  %null = icmp eq ptr %src, null\n  br i1 %null, label %null.ret, label %clone.body\nnull.ret:\n  ret ptr null\nclone.body:\n  %dst = call ptr @quidra_alloc(i64 "<<bytes<<")\n";
    for(std::size_t i=0;i<layout.fields.size();++i){
        const auto&field=layout.fields[i];
        const auto offset=class_field_offset(layout,i);
        o<<"  %sp"<<i<<" = getelementptr inbounds i8, ptr %src, i64 "<<offset<<"\n"
         <<"  %dp"<<i<<" = getelementptr inbounds i8, ptr %dst, i64 "<<offset<<"\n"
         <<"  %v"<<i<<" = load "<<llvm_type(field)<<", ptr %sp"<<i<<", align 1\n";
        if(requires_value_clone(field))
            o<<"  %copy"<<i<<" = call ptr "<<clone_name(field)<<"(ptr %v"<<i<<")\n  store ptr %copy"<<i<<", ptr %dp"<<i<<", align 1\n";
        else if(uses_shared_immutable_storage(field))
            o<<"  call void @quidra_managed_retain(ptr %v"<<i<<")\n  store ptr %v"<<i<<", ptr %dp"<<i<<", align 1\n";
        else o<<"  store "<<llvm_type(field)<<" %v"<<i<<", ptr %dp"<<i<<", align 1\n";
    }
    o<<"  ret ptr %dst\n}\n\n";
    return o.str();
 }
 if(is_fixed_array(t)){
    const auto storage=fixed_array_storage(t,array_layout);
    const auto elem=storage.leaf;
    const auto stride=runtime_storage_bytes(elem);
    const auto bytes=checked_storage_product(storage.count,stride);
    std::ostringstream o;
    o<<"define ptr "<<clone_name(t)<<"(ptr %src) {\nentry:\n  %null = icmp eq ptr %src, null\n  br i1 %null, label %null.ret, label %clone.body\nnull.ret:\n  ret ptr null\nclone.body:\n"
     <<"  %dst = call ptr @quidra_alloc(i64 "<<bytes<<")\n"
     <<"  call void @quidra_init_clone(ptr %dst, ptr %src, i64 "<<storage.count
     <<", i64 "<<stride<<", i64 0)\n"
     <<"  br label %cond\n"
     <<"cond:\n  %i = phi i64 [ 0, %clone.body ], [ %next, %body ]\n"
     <<"  %more = icmp slt i64 %i, "<<storage.count<<"\n"
     <<"  br i1 %more, label %body, label %done\n"
     <<"body:\n  %off = mul i64 %i, "<<stride<<"\n"
     <<"  %sp = getelementptr inbounds i8, ptr %src, i64 %off\n"
     <<"  %dp = getelementptr inbounds i8, ptr %dst, i64 %off\n"
     <<"  %v = load "<<llvm_type(elem)<<", ptr %sp, align 1\n";
    if(requires_value_clone(elem))
        o<<"  %copy = call ptr "<<clone_name(elem)<<"(ptr %v)\n  store ptr %copy, ptr %dp, align 1\n";
    else if(uses_shared_immutable_storage(elem))
        o<<"  call void @quidra_managed_retain(ptr %v)\n  store ptr %v, ptr %dp, align 1\n";
    else o<<"  store "<<llvm_type(elem)<<" %v, ptr %dp, align 1\n";
    o<<"  %next = add i64 %i, 1\n  br label %cond\ndone:\n  ret ptr %dst\n}\n\n";
    return o.str();
 }
 const auto elem=*t.first;
 const auto stride=runtime_storage_bytes(elem);
 std::ostringstream o;
 o<<"define ptr "<<clone_name(t)<<"(ptr %src) {\nentry:\n  %null = icmp eq ptr %src, null\n  br i1 %null, label %null.ret, label %clone.body\nnull.ret:\n  ret ptr null\nclone.body:\n"
  <<"  %len = load i64, ptr %src, align 1\n"
  <<"  %data.bytes = mul i64 %len, "<<stride<<"\n"
  <<"  %bytes = add i64 %data.bytes, 8\n"
  <<"  %dst = call ptr @quidra_alloc(i64 %bytes)\n"
  <<"  store i64 %len, ptr %dst, align 1\n"
  <<"  %src.data = getelementptr inbounds i8, ptr %src, i64 8\n"
  <<"  call void @quidra_init_clone(ptr %dst, ptr %src.data, i64 %len, i64 "<<stride<<", i64 8)\n"
  <<"  br label %cond\n"
  <<"cond:\n  %i = phi i64 [ 0, %clone.body ], [ %next, %body ]\n"
  <<"  %more = icmp slt i64 %i, %len\n"
  <<"  br i1 %more, label %body, label %done\n"
  <<"body:\n  %off0 = mul i64 %i, "<<stride<<"\n"
  <<"  %off = add i64 %off0, 8\n"
  <<"  %sp = getelementptr inbounds i8, ptr %src, i64 %off\n"
  <<"  %dp = getelementptr inbounds i8, ptr %dst, i64 %off\n"
  <<"  %v = load "<<llvm_type(elem)<<", ptr %sp, align 1\n";
 if(requires_value_clone(elem))
    o<<"  %copy = call ptr "<<clone_name(elem)<<"(ptr %v)\n  store ptr %copy, ptr %dp, align 1\n";
 else if(uses_shared_immutable_storage(elem))
    o<<"  call void @quidra_managed_retain(ptr %v)\n  store ptr %v, ptr %dp, align 1\n";
 else o<<"  store "<<llvm_type(elem)<<" %v, ptr %dp, align 1\n";
 o<<"  %next = add i64 %i, 1\n  br label %cond\ndone:\n  ret ptr %dst\n}\n\n";
 return o.str();
}


void collect_drop_type(const Type& type, std::map<std::string,Type>& types,
                       const std::unordered_map<std::string,ir::ClassLayout>& layouts) {
    if (!requires_lifetime_management(type)) return;
    if (type.kind == TypeKind::BigInt || type.kind == TypeKind::BigReal ||
        type.kind == TypeKind::String || type.kind == TypeKind::Error ||
        (type.kind == TypeKind::Class &&
         (type.class_name == "$std.json.Value" ||
          type.class_name == "$std.http.Response"))) return;
    if (type.kind == TypeKind::Class && !layouts.contains(type.class_name)) return;
    const auto id = type_id(type);
    if (types.contains(id)) return;
    types[id] = type;
    if (type.kind == TypeKind::Array) collect_drop_type(*type.first, types, layouts);
    else if (type.kind == TypeKind::Union)
        for (const auto& current : type.cases) collect_drop_type(current, types, layouts);
    else if (type.kind == TypeKind::Class)
        for (const auto& field : layouts.at(type.class_name).fields)
            collect_drop_type(field, types, layouts);
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

std::string drop_callback_for(const Type& type,
                              const std::unordered_map<std::string,ir::ClassLayout>& layouts) {
    if (type.kind == TypeKind::BigInt) return "@quidra_bigint_drop";
    if (type.kind == TypeKind::BigReal) return "@quidra_bigreal_drop";
    if (type.kind == TypeKind::Class && type.class_name == "$std.json.Value") {
        return "@quidra_json_drop";
    }
    if (type.kind == TypeKind::Class && type.class_name == "$std.http.Response") {
        return "@quidra_http_response_drop";
    }
    if (type.kind == TypeKind::Bin || type.kind == TypeKind::Array ||
        type.kind == TypeKind::Tensor || type.kind == TypeKind::Neural ||
        type.kind == TypeKind::Gradients || type.kind == TypeKind::Union ||
        (type.kind == TypeKind::Class && layouts.contains(type.class_name))) {
        return drop_name(type);
    }
    return "null";
}

std::string emit_drop_helper(const Type& type,
                             const std::unordered_map<std::string,ir::ClassLayout>& layouts,
                             const ArrayLayoutPolicy& array_layout) {
    std::ostringstream out;
    out << "define void " << drop_name(type) << "(ptr %src) {\nentry:\n";

    if (type.kind == TypeKind::Bin) {
        out << "  ret void\n}\n\n";
        return out.str();
    }

    if (type.kind == TypeKind::Tensor) {
        out << "  call void @quidra_tensor_drop(ptr %src)\n"
            << "  ret void\n}\n\n";
        return out.str();
    }
    if (type.kind == TypeKind::Neural) {
        out << "  call void @quidra_neural_drop(ptr %src)\n"
            << "  ret void\n}\n\n";
        return out.str();
    }
    if (type.kind == TypeKind::Gradients) {
        out << "  call void @quidra_neural_gradients_drop(ptr %src)\n"
            << "  ret void\n}\n\n";
        return out.str();
    }

    if (type.kind == TypeKind::Union) {
        out << "  %tag = load i64, ptr %src, align 1\n"
            << "  %payload = getelementptr inbounds i8, ptr %src, i64 8\n"
            << "  switch i64 %tag, label %done [";
        for (std::size_t i = 0; i < type.cases.size(); ++i)
            out << " i64 " << i << ", label %case" << i;
        out << " ]\n";
        for (std::size_t i = 0; i < type.cases.size(); ++i) {
            const auto& current = type.cases[i];
            out << "case" << i << ":\n";
            if (requires_lifetime_management(current)) {
                out << "  %v" << i << " = load ptr, ptr %payload, align 1\n"
                    << "  call void @quidra_managed_release(ptr %v" << i << ", ptr "
                    << drop_callback_for(current, layouts) << ")\n";
            }
            out << "  br label %done\n";
        }
        out << "done:\n  ret void\n}\n\n";
        return out.str();
    }

    if (type.kind == TypeKind::Class) {
        const auto& layout = layouts.at(type.class_name);
        for (std::size_t i = 0; i < layout.fields.size(); ++i) {
            const auto& field = layout.fields[i];
            if (!requires_lifetime_management(field)) continue;
            out << "  %slot" << i << " = getelementptr inbounds i8, ptr %src, i64 "
                << class_field_offset(layout, i) << "\n"
                << "  %v" << i << " = load ptr, ptr %slot" << i << ", align 1\n"
                << "  call void @quidra_managed_release(ptr %v" << i << ", ptr "
                << drop_callback_for(field, layouts) << ")\n";
        }
        out << "  ret void\n}\n\n";
        return out.str();
    }

    if (is_fixed_array(type)) {
        const auto storage = fixed_array_storage(type, array_layout);
        const auto element = storage.leaf;
        const auto stride = runtime_storage_bytes(element);
        if (!requires_lifetime_management(element)) {
            out << "  ret void\n}\n\n";
            return out.str();
        }
        out << "  br label %cond\n"
            << "cond:\n"
            << "  %i = phi i64 [ 0, %entry ], [ %next, %body ]\n"
            << "  %more = icmp slt i64 %i, " << storage.count << "\n"
            << "  br i1 %more, label %body, label %done\n"
            << "body:\n"
            << "  %off = mul i64 %i, " << stride << "\n"
            << "  %slot = getelementptr inbounds i8, ptr %src, i64 %off\n"
            << "  %v = load ptr, ptr %slot, align 1\n"
            << "  call void @quidra_managed_release(ptr %v, ptr "
            << drop_callback_for(element, layouts) << ")\n"
            << "  %next = add i64 %i, 1\n"
            << "  br label %cond\n"
            << "done:\n  ret void\n}\n\n";
        return out.str();
    }

    const auto element = *type.first;
    const auto stride = runtime_storage_bytes(element);
    out << "  %len = load i64, ptr %src, align 1\n";
    if (!requires_lifetime_management(element)) {
        out << "  ret void\n}\n\n";
        return out.str();
    }
    out << "  br label %cond\n"
        << "cond:\n"
        << "  %i = phi i64 [ 0, %entry ], [ %next, %body ]\n"
        << "  %more = icmp slt i64 %i, %len\n"
        << "  br i1 %more, label %body, label %done\n"
        << "body:\n"
        << "  %off0 = mul i64 %i, " << stride << "\n"
        << "  %off = add i64 %off0, 8\n"
        << "  %slot = getelementptr inbounds i8, ptr %src, i64 %off\n"
        << "  %v = load ptr, ptr %slot, align 1\n"
        << "  call void @quidra_managed_release(ptr %v, ptr "
        << drop_callback_for(element, layouts) << ")\n"
        << "  %next = add i64 %i, 1\n"
        << "  br label %cond\n"
        << "done:\n  ret void\n}\n\n";
    return out.str();
}

void collect_equality_type(const Type& t, std::map<std::string,Type>& types,
                           const std::unordered_map<std::string,ir::ClassLayout>& layouts) {
    if (t.kind != TypeKind::Bin && t.kind != TypeKind::Array && t.kind != TypeKind::Class) return;
    const auto id = type_id(t);
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

std::string equality_value_ir(const Type& type, const std::string& left,
                              const std::string& right, const std::string& id) {
    std::ostringstream out;
    if (type.kind == TypeKind::BigInt) {
        out << "  %" << id << ".cmp = call i32 @quidra_bigint_compare(ptr " << left << ", ptr " << right << ")\n"
            << "  %" << id << " = icmp eq i32 %" << id << ".cmp, 0\n";
    } else if (type.kind == TypeKind::BigReal) {
        out << "  %" << id << ".cmp = call i32 @quidra_bigreal_compare(ptr " << left << ", ptr " << right << ", i64 0, i64 0)\n"
            << "  %" << id << " = icmp eq i32 %" << id << ".cmp, 0\n";
    } else if (is_integer(type)) {
        out << "  %" << id << " = icmp eq " << llvm_type(type) << " " << left << ", " << right << "\n";
    } else if (type.kind == TypeKind::Bool) {
        out << "  %" << id << " = icmp eq i1 " << left << ", " << right << "\n";
    } else if (is_float(type)) {
        out << "  %" << id << " = fcmp oeq " << llvm_type(type) << " " << left << ", " << right << "\n";
    } else if (type.kind == TypeKind::String || type.kind == TypeKind::Error) {
        out << "  %" << id << " = call i1 @quidra_string_equal(ptr "
            << left << ", ptr " << right << ")\n";
    } else if (type.kind == TypeKind::Bin || type.kind == TypeKind::Array || type.kind == TypeKind::Class) {
        out << "  %" << id << " = call i1 " << equality_name(type) << "(ptr "
            << left << ", ptr " << right << ")\n";
    }
    return out.str();
}

std::string emit_equality_helper(
    const Type& type,
    const std::unordered_map<std::string,ir::ClassLayout>& layouts,
    const ArrayLayoutPolicy& array_layout) {
    std::ostringstream out;
    out << "define i1 " << equality_name(type) << "(ptr %left, ptr %right) {\n"
        << "entry:\n"
        << "  %same = icmp eq ptr %left, %right\n"
        << "  br i1 %same, label %equal, label %null.check\n"
        << "null.check:\n"
        << "  %left.null = icmp eq ptr %left, null\n"
        << "  %right.null = icmp eq ptr %right, null\n"
        << "  %any.null = or i1 %left.null, %right.null\n"
        << "  br i1 %any.null, label %different, label %compare\n";

    if (type.kind == TypeKind::Bin) {
        out << "compare:\n"
            << "  %bin.equal = call i1 @quidra_bin_equal(ptr %left, ptr %right)\n"
            << "  br i1 %bin.equal, label %equal, label %different\n";
    } else if (type.kind == TypeKind::Class) {
        const auto& layout = layouts.at(type.class_name);
        out << "compare:\n";
        if (layout.fields.empty()) {
            out << "  br label %equal\n";
        } else {
            out << "  br label %field.0\n";
        }
        for (std::size_t i = 0; i < layout.fields.size(); ++i) {
            const auto& field = layout.fields[i];
            const auto offset = class_field_offset(layout, i);
            out << "field." << i << ":\n"
                << "  %left.slot." << i << " = getelementptr inbounds i8, ptr %left, i64 " << offset << "\n"
                << "  %right.slot." << i << " = getelementptr inbounds i8, ptr %right, i64 " << offset << "\n"
                << "  %left.value." << i << " = load " << llvm_type(field) << ", ptr %left.slot." << i << ", align 1\n"
                << "  %right.value." << i << " = load " << llvm_type(field) << ", ptr %right.slot." << i << ", align 1\n";
            out << equality_value_ir(field, "%left.value." + std::to_string(i),
                                     "%right.value." + std::to_string(i),
                                     "field.equal." + std::to_string(i));
            out << "  br i1 %field.equal." << i << ", label %"
                << (i + 1 < layout.fields.size() ? "field." + std::to_string(i + 1) : "equal")
                << ", label %different\n";
        }
    } else if (is_fixed_array(type)) {
        const auto storage = fixed_array_storage(type, array_layout);
        const auto element = storage.leaf;
        const auto stride = runtime_storage_bytes(element);
        const auto bytes = checked_storage_product(storage.count, stride);
        out << "compare:\n"
            << "  call void @quidra_init_require_range(ptr %left, i64 " << bytes
            << ", i64 0, i64 0)\n"
            << "  call void @quidra_init_require_range(ptr %right, i64 " << bytes
            << ", i64 0, i64 0)\n"
            << "  br label %loop.cond\n"
            << "loop.cond:\n"
            << "  %index = phi i64 [ 0, %compare ], [ %next, %loop.next ]\n"
            << "  %more = icmp slt i64 %index, " << storage.count << "\n"
            << "  br i1 %more, label %loop.body, label %equal\n"
            << "loop.body:\n"
            << "  %offset = mul i64 %index, " << stride << "\n"
            << "  %left.slot = getelementptr inbounds i8, ptr %left, i64 %offset\n"
            << "  %right.slot = getelementptr inbounds i8, ptr %right, i64 %offset\n"
            << "  %left.value = load " << llvm_type(element) << ", ptr %left.slot, align 1\n"
            << "  %right.value = load " << llvm_type(element) << ", ptr %right.slot, align 1\n";
        out << equality_value_ir(element, "%left.value", "%right.value", "element.equal");
        out << "  br i1 %element.equal, label %loop.next, label %different\n"
            << "loop.next:\n"
            << "  %next = add i64 %index, 1\n"
            << "  br label %loop.cond\n";
    } else {
        const auto element = *type.first;
        const auto stride = runtime_storage_bytes(element);
        out << "compare:\n"
            << "  %left.len = load i64, ptr %left, align 1\n"
            << "  %right.len = load i64, ptr %right, align 1\n"
            << "  %length.equal = icmp eq i64 %left.len, %right.len\n"
            << "  br i1 %length.equal, label %initialized.check, label %different\n"
            << "initialized.check:\n"
            << "  %tracked.bytes = mul i64 %left.len, " << stride << "\n"
            << "  %left.data.init = getelementptr inbounds i8, ptr %left, i64 8\n"
            << "  %right.data.init = getelementptr inbounds i8, ptr %right, i64 8\n"
            << "  call void @quidra_init_require_range(ptr %left.data.init, i64 %tracked.bytes, i64 0, i64 0)\n"
            << "  call void @quidra_init_require_range(ptr %right.data.init, i64 %tracked.bytes, i64 0, i64 0)\n"
            << "  br label %loop.cond\n"
            << "loop.cond:\n"
            << "  %index = phi i64 [ 0, %initialized.check ], [ %next, %loop.next ]\n"
            << "  %more = icmp slt i64 %index, %left.len\n"
            << "  br i1 %more, label %loop.body, label %equal\n"
            << "loop.body:\n"
            << "  %offset.base = mul i64 %index, " << stride << "\n"
            << "  %offset = add i64 %offset.base, 8\n"
            << "  %left.slot = getelementptr inbounds i8, ptr %left, i64 %offset\n"
            << "  %right.slot = getelementptr inbounds i8, ptr %right, i64 %offset\n"
            << "  %left.value = load " << llvm_type(element) << ", ptr %left.slot, align 1\n"
            << "  %right.value = load " << llvm_type(element) << ", ptr %right.slot, align 1\n";
        out << equality_value_ir(element, "%left.value", "%right.value", "element.equal");
        out << "  br i1 %element.equal, label %loop.next, label %different\n"
            << "loop.next:\n"
            << "  %next = add i64 %index, 1\n"
            << "  br label %loop.cond\n";
    }

    out << "equal:\n"
        << "  ret i1 true\n"
        << "different:\n"
        << "  ret i1 false\n"
        << "}\n\n";
    return out.str();
}

std::string runtime_helpers(){return R"LLVM(
declare void @quidra_runtime_set_args(i32, ptr)
declare ptr @quidra_bigint_literal(ptr)
declare ptr @quidra_bigint_parse(ptr)
declare ptr @quidra_bigreal_literal(ptr)
declare ptr @quidra_bigreal_parse(ptr)
declare void @quidra_bigint_drop(ptr)
declare void @quidra_bigreal_drop(ptr)
declare ptr @quidra_bigint_text(ptr)
declare ptr @quidra_bigreal_text(ptr, i32)
declare ptr @quidra_bigint_neg(ptr, i64, i64)
declare ptr @quidra_bigint_abs(ptr, i64, i64)
declare ptr @quidra_bigint_binary(ptr, ptr, i32, i64, i64)
declare i32 @quidra_bigint_compare(ptr, ptr)
declare ptr @quidra_bigreal_neg(ptr, i64, i64)
declare ptr @quidra_bigreal_abs(ptr, i64, i64)
declare ptr @quidra_bigreal_binary(ptr, ptr, i32, i64, i64)
declare i32 @quidra_bigreal_compare(ptr, ptr, i64, i64)
declare ptr @quidra_bigreal_sqrt(ptr, i64, i64)
declare ptr @quidra_bigreal_math_unary(ptr, i32, i64, i64)
declare ptr @quidra_bigreal_pow(ptr, ptr, i64, i64)
declare ptr @quidra_bigreal_round(ptr, i32, i64, i64)
declare ptr @quidra_bigint_from_i64(i64)
declare ptr @quidra_bigint_from_u64(i64)
declare ptr @quidra_bigreal_from_i64(i64)
declare ptr @quidra_bigreal_from_u64(i64)
declare ptr @quidra_bigreal_from_float64(double)
declare ptr @quidra_bigreal_from_bigint(ptr)
declare ptr @quidra_bigreal_to_bigint(ptr, i64, i64)
declare i64 @quidra_bigint_to_i64(ptr, i32, i64, i64)
declare i64 @quidra_bigint_to_u64(ptr, i32, i64, i64)
declare double @quidra_bigreal_to_float64(ptr, i64, i64)
declare float @quidra_bigreal_to_float32(ptr, i64, i64)
declare double @quidra_bigint_to_float64(ptr, i64, i64)
declare float @quidra_bigint_to_float32(ptr, i64, i64)
declare ptr @quidra_string_index(ptr, i64, i64, i64)
declare i64 @quidra_string_length(ptr)
declare i1 @quidra_string_contains(ptr, ptr)
declare i1 @quidra_string_starts_with(ptr, ptr)
declare i1 @quidra_string_ends_with(ptr, ptr)
declare i64 @quidra_string_find(ptr, ptr)
declare ptr @quidra_string_slice(ptr, i64, i64)
declare ptr @quidra_string_trim(ptr)
declare ptr @quidra_string_split(ptr, ptr)
declare ptr @quidra_string_utf8(ptr)
declare ptr @quidra_string_codepoints(ptr)
declare ptr @quidra_string_join(ptr, ptr, i64, i64)
declare ptr @quidra_string_concat_many(ptr, i64)
declare ptr @quidra_string_concat2(ptr, ptr)
declare i1 @quidra_string_equal(ptr, ptr)
declare i1 @quidra_string_can_append_move(ptr)
declare ptr @quidra_string_append_move_many(ptr, ptr, i64)
declare ptr @quidra_string_repeat(i64, ptr)
declare ptr @quidra_bin_alloc(i64, i64)
declare ptr @quidra_bin_index(ptr, i64)
declare void @quidra_bin_set(ptr, i64, ptr)
declare ptr @quidra_bin_slice(ptr, i64, i64)
declare ptr @quidra_bin_parse(ptr)
declare ptr @quidra_bin_string(ptr)
declare ptr @quidra_bin_from_u64(i64, i32)
declare i64 @quidra_bin_to_u64(ptr, i32)
declare ptr @quidra_bin_from_array(ptr, i32, i32)
declare ptr @quidra_bin_to_array(ptr, i32, i32)
declare ptr @quidra_bin_clone(ptr)
declare i1 @quidra_bin_equal(ptr, ptr)
declare i64 @quidra_bin_byte_length(ptr)
declare ptr @quidra_cli_argument(i64)
declare ptr @quidra_cli_option(ptr)
declare i1 @quidra_cli_flag(ptr)
declare void @quidra_cli_finish()
declare i64 @quidra_cli_parse_int(ptr)
declare double @quidra_cli_parse_float(ptr)
declare ptr @quidra_cli_parse_bigint(ptr)
declare ptr @quidra_cli_parse_bigreal(ptr)
declare i1 @quidra_cli_parse_bool(ptr)
declare ptr @quidra_file_read_raw(ptr)
declare ptr @quidra_file_read_bin_raw(ptr)
declare i1 @quidra_file_write_raw(ptr, ptr)
declare i1 @quidra_file_write_bin_raw(ptr, ptr)
declare i32 @quidra_file_exists_raw(ptr)
declare i32 @quidra_file_is_directory_raw(ptr)
declare i1 @quidra_file_remove_raw(ptr)
declare i1 @quidra_file_copy_raw(ptr, ptr)
declare i1 @quidra_file_move_raw(ptr, ptr)
declare i1 @quidra_file_mkdir_raw(ptr)
declare ptr @quidra_file_list_raw(ptr, i1)
declare ptr @quidra_environment_get(ptr)
declare i1 @quidra_environment_has(ptr)
declare void @quidra_test_assert(i1)
declare double @quidra_time_now()
declare i1 @quidra_time_sleep(double)
declare i64 @quidra_random_int(ptr, i64, i64)
declare double @quidra_random_float(ptr)
declare i1 @quidra_random_bool(ptr)
declare ptr @quidra_process_run(ptr, ptr)
declare ptr @quidra_json_parse_raw(ptr)
declare void @quidra_json_drop(ptr)
declare ptr @quidra_json_last_error_copy()
declare ptr @quidra_json_kind(ptr)
declare i64 @quidra_json_size(ptr)
declare i1 @quidra_json_is_object(ptr)
declare i1 @quidra_json_is_array(ptr)
declare ptr @quidra_json_get(ptr, ptr)
declare ptr @quidra_json_at(ptr, i64)
declare ptr @quidra_json_text(ptr)
declare i1 @quidra_json_integer_ok(ptr)
declare i64 @quidra_json_integer(ptr)
declare i1 @quidra_json_number_ok(ptr)
declare double @quidra_json_number(ptr)
declare ptr @quidra_json_number_text(ptr)
declare i1 @quidra_json_boolean_ok(ptr)
declare i1 @quidra_json_boolean(ptr)
declare ptr @quidra_json_encode(ptr)
declare i1 @quidra_json_equal(ptr, ptr)
declare ptr @quidra_http_get(ptr)
declare ptr @quidra_http_last_error_copy()
declare ptr @quidra_http_header(ptr, ptr)
declare ptr @quidra_http_response_clone(ptr)
declare void @quidra_http_response_drop(ptr)
declare ptr @quidra_image_read(ptr, i32, i32, i32, i64, i64, i64, ptr)
declare i1 @quidra_image_write(ptr, ptr, i32, i64)
declare ptr @quidra_image_last_error_copy()
declare ptr @quidra_image_tensor_geometry(ptr, i32, i32, i64, i64, i64, i64, i64, i64)
declare ptr @quidra_image_tensor_grayscale(ptr, i64, i64)
declare ptr @quidra_image_tensor_threshold(ptr, i8, i8, i8, i64, i64)
declare ptr @quidra_image_tensor_blur(ptr, i64, i64, i64)
declare ptr @quidra_image_tensor_filter(ptr, ptr, i64, i64, i64, i64)
declare ptr @quidra_image_tensor_morphology(ptr, i32, i64, i1, i64, i64)
declare i32 @printf(ptr, ...)
declare i32 @puts(ptr nocapture nonnull readonly)
declare i64 @strlen(ptr nocapture nonnull readonly)
declare i32 @strcmp(ptr nocapture nonnull readonly, ptr nocapture nonnull readonly)
declare ptr @strchr(ptr nocapture nonnull readonly, i32)
declare void @free(ptr)
declare ptr @quidra_managed_alloc(i64)
declare void @quidra_managed_retain(ptr)
declare void @quidra_managed_release(ptr, ptr)
declare void @quidra_managed_pin(ptr)
declare void @quidra_managed_unpin(ptr)
declare void @quidra_init_create(ptr, i64, i64, i64, i32)
declare void @quidra_init_mark_range(ptr, i64)
declare void @quidra_init_check(ptr, i64, i64)
declare void @quidra_init_require_range(ptr, i64, i64, i64)
declare void @quidra_init_clone(ptr, ptr, i64, i64, i64)
declare i1 @quidra_array_can_append_move(ptr)
declare ptr @quidra_array_grow_move(ptr, i64)
declare ptr @quidra_array_sorted(ptr, i32, i64, i64, i64)
declare void @quidra_numeric_cast_element(ptr, ptr, i32, i32, i64, i64)
declare ptr @quidra_tensor_create(ptr, i32, i32, i1, i64, i64, i64)
declare ptr @quidra_tensor_to_gpu(ptr, i64, i64, i64)
declare ptr @quidra_tensor_to_cpu(ptr, i64, i64)
declare ptr @quidra_tensor_clone(ptr)
declare void @quidra_tensor_drop(ptr)
declare ptr @quidra_tensor_reshape(ptr, ptr, i64, i64)
declare ptr @quidra_tensor_transpose(ptr, i64, i64, i64, i64)
declare ptr @quidra_tensor_contiguous(ptr)
declare ptr @quidra_tensor_shape(ptr)
declare ptr @quidra_tensor_shape_fixed(ptr, i64)
declare i1 @quidra_tensor_is_contiguous(ptr)
declare ptr @quidra_tensor_item_ptr(ptr, i64, i64)
declare ptr @quidra_tensor_cast(ptr, i32, i64, i64)
declare void @quidra_tensor_rank_check(ptr, i64, i64, i64)
declare void @quidra_tensor_extent_check(ptr, i64, i64, i64, i64)
declare ptr @quidra_neural_track(ptr, i64, i64)
declare ptr @quidra_neural_parameter_track(ptr, i64, i64)
declare ptr @quidra_neural_untrack(ptr)
declare ptr @quidra_neural_cast(ptr, i32, i64, i64)
declare void @quidra_neural_rank_check(ptr, i64, i64, i64)
declare void @quidra_neural_extent_check(ptr, i64, i64, i64, i64)
declare ptr @quidra_neural_clone(ptr)
declare void @quidra_neural_drop(ptr)
declare ptr @quidra_neural_gradients_clone(ptr)
declare void @quidra_neural_gradients_drop(ptr)
declare ptr @quidra_neural_unary(ptr, i32, i64, i64)
declare ptr @quidra_neural_tensor_unary(ptr, i32, i64, i64)
declare ptr @quidra_neural_binary(ptr, ptr, i32, i64, i64)
declare ptr @quidra_neural_binary_scalar(ptr, double, i32, i1, i64, i64)
declare ptr @quidra_neural_grad(ptr, i64, i64)
declare i1 @quidra_neural_update_parameter(ptr, ptr, double, i64, i64)
declare ptr @quidra_neural_normalize(ptr, ptr, ptr, ptr, ptr, double, double, i1, i64, i64)
declare ptr @quidra_neural_random_mask(ptr, ptr, double, i64, i64)
declare ptr @quidra_neural_convolve2d(ptr, ptr, ptr, i64, i64, i64, i64)
declare ptr @quidra_neural_tensor_convolve2d(ptr, ptr, ptr, i64, i64, i64, i64)
declare ptr @quidra_neural_affine(ptr, ptr, ptr, i64, i64)
declare ptr @quidra_neural_tensor_affine(ptr, ptr, ptr, i64, i64)
declare i1 @quidra_neural_parameter_has_gradient(ptr, ptr, i64, i64)
declare i64 @quidra_neural_moment_begin(ptr, i64, i64, i64)
declare void @quidra_neural_moment_validate_parameter(ptr, ptr, ptr, ptr, i64, i64, i64)
declare i1 @quidra_neural_moment_update_parameter(ptr, ptr, ptr, ptr, i64, i64, i64, i64)
declare void @quidra_neural_moment_finish(ptr, i64, i64, i64)
declare void @quidra_neural_validate_step(ptr, i64, i64, i64)
declare ptr @quidra_neural_state_save_begin(ptr, ptr, i64, i64)
declare void @quidra_neural_state_write(ptr, ptr, i32, ptr, i64, i64)
declare void @quidra_neural_state_save_finish(ptr, i64, i64)
declare ptr @quidra_neural_state_load_begin(ptr, ptr, i64, i64)
declare void @quidra_neural_state_read(ptr, ptr, i32, ptr, i64, i64)
declare void @quidra_neural_state_load_finish(ptr, i64, i64)
declare i64 @quidra_math_trunc_int(double, i64, i64)
declare i64 @quidra_math_round_int(double, i64, i64)
declare i64 @quidra_math_floor_int(double, i64, i64)
declare i64 @quidra_math_ceil_int(double, i64, i64)
declare double @quidra_stats_mean(ptr, i64, i64)
declare ptr @quidra_stats_reduce_ptr(ptr, i32, i32, i64, i64)
declare ptr @quidra_linear_matmul(ptr, ptr, i64, i64)
declare i64 @quidra_linear_dot_integer(ptr, ptr, i32, i64, i64)
declare float @quidra_linear_dot_float32(ptr, ptr, i64, i64)
declare double @quidra_linear_dot_float64(ptr, ptr, i64, i64)
declare ptr @quidra_tensor_unary(ptr, i32, i64, i64)
declare ptr @quidra_tensor_binary(ptr, ptr, ptr, i32, i32, i64, i64)
declare ptr @quidra_tensor_index(ptr, ptr, i64, i64, i64)
declare void @quidra_tensor_set(ptr, ptr, i64, ptr, i64, i64)
declare ptr @quidra_format_float(double)
declare ptr @quidra_format_signed(i64, i32, i32, i32, i32)
declare ptr @quidra_format_unsigned(i64, i32, i32, i32, i32)
declare ptr @quidra_format_number(double, i32, i32, i32, i32)
declare ptr @memcpy(ptr, ptr, i64)
declare ptr @memmove(ptr, ptr, i64)
declare ptr @memset(ptr, i32, i64)
declare i32 @memcmp(ptr nocapture nonnull readonly, ptr nocapture nonnull readonly, i64)
declare i32 @snprintf(ptr, i64, ptr, ...)
declare i32 @fflush(ptr)
declare void @exit(i32)
declare void @_Exit(i32)
declare double @sin(double)
declare float @sinf(float)
declare double @cos(double)
declare float @cosf(float)
declare double @tan(double)
declare float @tanf(float)
declare double @log(double)
declare float @logf(float)
declare double @exp(double)
declare float @expf(float)
declare double @pow(double, double)
declare float @powf(float, float)
declare { i64, i1 } @llvm.sadd.with.overflow.i64(i64, i64)
declare { i64, i1 } @llvm.ssub.with.overflow.i64(i64, i64)
declare { i64, i1 } @llvm.smul.with.overflow.i64(i64, i64)
declare { i8, i1 } @llvm.sadd.with.overflow.i8(i8, i8)
declare { i16, i1 } @llvm.sadd.with.overflow.i16(i16, i16)
declare { i32, i1 } @llvm.sadd.with.overflow.i32(i32, i32)
declare { i8, i1 } @llvm.ssub.with.overflow.i8(i8, i8)
declare { i16, i1 } @llvm.ssub.with.overflow.i16(i16, i16)
declare { i32, i1 } @llvm.ssub.with.overflow.i32(i32, i32)
declare { i8, i1 } @llvm.smul.with.overflow.i8(i8, i8)
declare { i16, i1 } @llvm.smul.with.overflow.i16(i16, i16)
declare { i32, i1 } @llvm.smul.with.overflow.i32(i32, i32)
declare { i8, i1 } @llvm.uadd.with.overflow.i8(i8, i8)
declare { i16, i1 } @llvm.uadd.with.overflow.i16(i16, i16)
declare { i32, i1 } @llvm.uadd.with.overflow.i32(i32, i32)
declare { i64, i1 } @llvm.uadd.with.overflow.i64(i64, i64)
declare { i8, i1 } @llvm.usub.with.overflow.i8(i8, i8)
declare { i16, i1 } @llvm.usub.with.overflow.i16(i16, i16)
declare { i32, i1 } @llvm.usub.with.overflow.i32(i32, i32)
declare { i64, i1 } @llvm.usub.with.overflow.i64(i64, i64)
declare { i8, i1 } @llvm.umul.with.overflow.i8(i8, i8)
declare { i16, i1 } @llvm.umul.with.overflow.i16(i16, i16)
declare { i32, i1 } @llvm.umul.with.overflow.i32(i32, i32)
declare { i64, i1 } @llvm.umul.with.overflow.i64(i64, i64)
declare float @llvm.fabs.f32(float)
declare double @llvm.fabs.f64(double)
declare double @llvm.sqrt.f64(double)
declare float @llvm.sqrt.f32(float)
declare i8 @llvm.fptosi.sat.i8.f32(float)
declare i16 @llvm.fptosi.sat.i16.f32(float)
declare i32 @llvm.fptosi.sat.i32.f32(float)
declare i64 @llvm.fptosi.sat.i64.f32(float)
declare i8 @llvm.fptoui.sat.i8.f32(float)
declare i16 @llvm.fptoui.sat.i16.f32(float)
declare i32 @llvm.fptoui.sat.i32.f32(float)
declare i64 @llvm.fptoui.sat.i64.f32(float)
declare i8 @llvm.fptosi.sat.i8.f64(double)
declare i16 @llvm.fptosi.sat.i16.f64(double)
declare i32 @llvm.fptosi.sat.i32.f64(double)
declare i64 @llvm.fptosi.sat.i64.f64(double)
declare i8 @llvm.fptoui.sat.i8.f64(double)
declare i16 @llvm.fptoui.sat.i16.f64(double)
declare i32 @llvm.fptoui.sat.i32.f64(double)
declare i64 @llvm.fptoui.sat.i64.f64(double)

declare i1 @quidra_parse_signed(ptr, ptr)
declare i1 @quidra_parse_unsigned(ptr, ptr)
declare i1 @quidra_parse_float32(ptr, ptr)
declare i1 @quidra_parse_float64(ptr, ptr)
declare i32 @quidra_input_read(ptr)

@.quidra.source.line = internal global i64 0
@.quidra.source.column = internal global i64 0

define void @quidra_fail(ptr %msg) noreturn {
entry:
  call i32 @puts(ptr %msg)
  call i32 @fflush(ptr null)
  call void @_Exit(i32 101)
  unreachable
}

define void @quidra_fail_at(ptr %code, ptr %msg, i64 %line, i64 %column) noreturn {
entry:
  call i32 (ptr, ...) @printf(ptr @.fmt.runtime.error, ptr %code, i64 %line, i64 %column, ptr %msg)
  call i32 @fflush(ptr null)
  call void @_Exit(i32 101)
  unreachable
}

define void @quidra_fail_code(ptr %code, ptr %msg) noreturn {
entry:
  %line = load i64, ptr @.quidra.source.line
  %column = load i64, ptr @.quidra.source.column
  call void @quidra_fail_at(ptr %code, ptr %msg, i64 %line, i64 %column)
  unreachable
}

@.quidra.stack.depth = internal global i64 0

define void @quidra_stack_enter() {
entry:
  %depth = load i64, ptr @.quidra.stack.depth
  %next = add i64 %depth, 1
  %too.deep = icmp ugt i64 %next, 4096
  br i1 %too.deep, label %fail, label %ok
fail:
  call void @quidra_fail_code(ptr @.code.stack, ptr @.msg.stack)
  unreachable
ok:
  store i64 %next, ptr @.quidra.stack.depth
  ret void
}

define void @quidra_stack_leave() {
entry:
  %depth = load i64, ptr @.quidra.stack.depth
  %next = sub i64 %depth, 1
  store i64 %next, ptr @.quidra.stack.depth
  ret void
}

define ptr @quidra_alloc(i64 %bytes) {
entry:
  %p = call ptr @quidra_managed_alloc(i64 %bytes)
  ret ptr %p
}
define ptr @quidra_float_text(double %x) {
entry:
  %text = call ptr @quidra_format_float(double %x)
  ret ptr %text
}

define ptr @quidra_array_alloc(i64 %n, i64 %stride, i32 %initialized) {
entry:
  %neg = icmp slt i64 %n, 0
  %stride.bad = icmp ule i64 %stride, 0
  %product = call { i64, i1 } @llvm.umul.with.overflow.i64(i64 %n, i64 %stride)
  %data = extractvalue { i64, i1 } %product, 0
  %mul.overflow = extractvalue { i64, i1 } %product, 1
  %sum = call { i64, i1 } @llvm.uadd.with.overflow.i64(i64 %data, i64 8)
  %bytes = extractvalue { i64, i1 } %sum, 0
  %add.overflow = extractvalue { i64, i1 } %sum, 1
  %bad0 = or i1 %neg, %stride.bad
  %bad1 = or i1 %mul.overflow, %add.overflow
  %bad = or i1 %bad0, %bad1
  br i1 %bad, label %fail, label %ok
fail:
  call void @quidra_fail_at(ptr @.code.overflow, ptr @.msg.overflow, i64 0, i64 0)
  unreachable
ok:
  %p = call ptr @quidra_alloc(i64 %bytes)
  store i64 %n, ptr %p, align 1
  %payload = getelementptr inbounds i8, ptr %p, i64 8
  call ptr @memset(ptr %payload, i32 0, i64 %data)
  call void @quidra_init_create(ptr %p, i64 %n, i64 %stride, i64 8, i32 %initialized)
  ret ptr %p
}
define ptr @quidra_array_slot(ptr %array, i64 %index, i64 %stride, i64 %line, i64 %column) alwaysinline {
entry:
  %len = load i64, ptr %array, align 1
  %negative = icmp slt i64 %index, 0
  %past = icmp sge i64 %index, %len
  %bad = or i1 %negative, %past
  br i1 %bad, label %fail, label %ok
fail:
  call i32 (ptr, ...) @printf(ptr @.err.bounds, i64 %line, i64 %column, i64 %index, i64 %len)
  call i32 @fflush(ptr null)
  call void @_Exit(i32 101)
  unreachable
ok:
  %mul = mul i64 %index, %stride
  %off = add i64 %mul, 8
  %slot = getelementptr inbounds i8, ptr %array, i64 %off
  ret ptr %slot
}

define ptr @quidra_fixed_array_slot(ptr %array, i64 %index, i64 %len, i64 %stride, i64 %line, i64 %column) alwaysinline {
entry:
  %negative = icmp slt i64 %index, 0
  %past = icmp sge i64 %index, %len
  %bad = or i1 %negative, %past
  br i1 %bad, label %fail, label %ok
fail:
  call i32 (ptr, ...) @printf(ptr @.err.bounds, i64 %line, i64 %column, i64 %index, i64 %len)
  call i32 @fflush(ptr null)
  call void @_Exit(i32 101)
  unreachable
ok:
  %off = mul i64 %index, %stride
  %slot = getelementptr inbounds i8, ptr %array, i64 %off
  ret ptr %slot
}

define ptr @quidra_array_slot_proven(ptr %array, i64 %index, i64 %stride) alwaysinline {
entry:
  %mul = mul i64 %index, %stride
  %off = add i64 %mul, 8
  %slot = getelementptr inbounds i8, ptr %array, i64 %off
  ret ptr %slot
}

define ptr @quidra_fixed_array_slot_proven(ptr %array, i64 %index, i64 %stride) alwaysinline {
entry:
  %off = mul i64 %index, %stride
  %slot = getelementptr inbounds i8, ptr %array, i64 %off
  ret ptr %slot
}

define i64 @quidra_add(i64 %a, i64 %b) {
entry:
  %p = call { i64, i1 } @llvm.sadd.with.overflow.i64(i64 %a, i64 %b)
  %r = extractvalue { i64, i1 } %p, 0
  %o = extractvalue { i64, i1 } %p, 1
  br i1 %o, label %bad, label %ok
bad:
  call void @quidra_fail_at(ptr @.code.overflow, ptr @.msg.overflow, i64 0, i64 0)
  unreachable
ok:
  ret i64 %r
}

define i64 @quidra_abs(i64 %x) {
entry:
  %negative = icmp slt i64 %x, 0
  br i1 %negative, label %neg, label %ok
neg:
  %minimum = icmp eq i64 %x, -9223372036854775808
  br i1 %minimum, label %bad, label %negate
bad:
  call void @quidra_fail_at(ptr @.code.overflow, ptr @.msg.overflow, i64 0, i64 0)
  unreachable
negate:
  %r = sub i64 0, %x
  ret i64 %r
ok:
  ret i64 %x
}

define i64 @quidra_sub(i64 %a, i64 %b) {
entry:
  %p = call { i64, i1 } @llvm.ssub.with.overflow.i64(i64 %a, i64 %b)
  %r = extractvalue { i64, i1 } %p, 0
  %o = extractvalue { i64, i1 } %p, 1
  br i1 %o, label %bad, label %ok
bad:
  call void @quidra_fail_at(ptr @.code.overflow, ptr @.msg.overflow, i64 0, i64 0)
  unreachable
ok:
  ret i64 %r
}

define i64 @quidra_mul(i64 %a, i64 %b) {
entry:
  %p = call { i64, i1 } @llvm.smul.with.overflow.i64(i64 %a, i64 %b)
  %r = extractvalue { i64, i1 } %p, 0
  %o = extractvalue { i64, i1 } %p, 1
  br i1 %o, label %bad, label %ok
bad:
  call void @quidra_fail_at(ptr @.code.overflow, ptr @.msg.overflow, i64 0, i64 0)
  unreachable
ok:
  ret i64 %r
}

define i64 @quidra_div(i64 %a, i64 %b) {
entry:
  %zero = icmp eq i64 %b, 0
  br i1 %zero, label %divzero, label %overflow_check
divzero:
  call void @quidra_fail_at(ptr @.code.divzero, ptr @.msg.divzero, i64 0, i64 0)
  unreachable
overflow_check:
  %min = icmp eq i64 %a, -9223372036854775808
  %negone = icmp eq i64 %b, -1
  %overflow = and i1 %min, %negone
  br i1 %overflow, label %bad, label %ok
bad:
  call void @quidra_fail_at(ptr @.code.overflow, ptr @.msg.overflow, i64 0, i64 0)
  unreachable
ok:
  %r = sdiv i64 %a, %b
  ret i64 %r
}

define i64 @quidra_mod(i64 %a, i64 %b) {
entry:
  %zero = icmp eq i64 %b, 0
  br i1 %zero, label %divzero, label %overflow_check
divzero:
  call void @quidra_fail_at(ptr @.code.divzero, ptr @.msg.divzero, i64 0, i64 0)
  unreachable
overflow_check:
  %min = icmp eq i64 %a, -9223372036854775808
  %negone = icmp eq i64 %b, -1
  %overflow = and i1 %min, %negone
  br i1 %overflow, label %bad, label %ok
bad:
  call void @quidra_fail_at(ptr @.code.overflow, ptr @.msg.overflow, i64 0, i64 0)
  unreachable
ok:
  %r = srem i64 %a, %b
  ret i64 %r
}

)LLVM";}

} // namespace

std::string emit_llvm(const ir::Module& module, bool debug_info) {
    StringPool pool;
    std::unordered_map<std::string, FunctionType> sigs;
    std::unordered_map<std::string, ir::ClassLayout> layouts;
    std::unordered_map<std::string,std::string> external_symbols;
    for (const auto& c : module.classes) layouts[c.name] = c;
    for (const auto& f : module.functions) if(f.external_symbol) external_symbols[f.name]=*f.external_symbol;
    for (const auto& f : module.functions) {
        if (f.entrypoint) continue;
        FunctionType signature;
        signature.result = f.result;
        for (const auto& p : f.parameters) {
            signature.parameters.push_back(
                FunctionParameterType{p.name, p.type, p.writable, nullptr, p.is_const});
        }
        sigs[f.name] = std::move(signature);
    }
    const auto array_layout = collect_array_layout_policy(module);
    const auto recursive = recursive_functions(module);

    std::string primary_source;
    if(debug_info) {
        for(const auto& f:module.functions)
            if(f.entrypoint&&!f.source_file.empty()) { primary_source=f.source_file; break; }
        if(primary_source.empty())
            for(const auto& f:module.functions)
                if(!f.source_file.empty()) { primary_source=f.source_file; break; }
    }

    std::map<std::string,std::size_t> debug_files;
    std::size_t next_debug_metadata=7;
    if(debug_info&&!primary_source.empty()) {
        debug_files.emplace(primary_source,1);
        for(const auto& f:module.functions) {
            if(f.source_file.empty()||debug_files.contains(f.source_file)) continue;
            debug_files.emplace(f.source_file,next_debug_metadata++);
        }
    }

    std::vector<std::optional<std::size_t>> debug_subprograms(module.functions.size());
    std::vector<std::optional<std::size_t>> debug_locations(module.functions.size());
    if(debug_info&&!primary_source.empty()) {
        for(std::size_t i=0;i<module.functions.size();++i) {
            const auto& f=module.functions[i];
            if(!f.external_symbol&&!f.source_file.empty()) {
                debug_subprograms[i]=next_debug_metadata++;
                debug_locations[i]=next_debug_metadata++;
            }
        }
    }

    std::vector<std::string> funcs;
    for (std::size_t i=0;i<module.functions.size();++i) {
        const auto& f=module.functions[i];
        auto emitted=FunctionEmitter{
            f, sigs, external_symbols, pool, layouts, array_layout,
            recursive.contains(f.name), recursive, debug_subprograms[i]}.emit();
        if(debug_locations[i]) emitted=attach_debug_location(std::move(emitted),*debug_locations[i]);
        funcs.push_back(std::move(emitted));
    }
    const auto array_cast_pairs = collect_array_cast_pairs(module);
    std::map<std::string, Type> clone_types;
    collect_clone_types(module, clone_types, layouts);
    std::ostringstream out;
    out << "; Quidra 0.1 generated LLVM IR\n";
    if(debug_info&&!primary_source.empty()) {
        const auto path=std::filesystem::path(primary_source);
        out<<"source_filename = \""<<escape_metadata(path.filename().string())<<"\"\n";
    }
    out << runtime_helpers();
    out << "@.quidra.repl.replaying = internal global i1 false\n";
    out << "@.fmt.int = private unnamed_addr constant [6 x i8] c\"%lld\\0A\\00\"\n";
out<<"@.fmt.int.write = private unnamed_addr constant [5 x i8] c\"%lld\\00\"\n";
out<<"@.fmt.uint = private unnamed_addr constant [6 x i8] c\"%llu\\0A\\00\"\n";
out<<"@.fmt.uint.write = private unnamed_addr constant [5 x i8] c\"%llu\\00\"\n";
out<<"@.fmt.int.text = private unnamed_addr constant [5 x i8] c\"%lld\\00\"\n";
out<<"@.fmt.uint.text = private unnamed_addr constant [5 x i8] c\"%llu\\00\"\n";
out<<"@.fmt.string.write = private unnamed_addr constant [3 x i8] c\"%s\\00\"\n";
out<<"@.fmt.repl.string = private unnamed_addr constant [5 x i8] c\"\\22%s\\22\\00\"\n";
out<<"@.fmt.repl.error = private unnamed_addr constant [12 x i8] c\"error(\\22%s\\22)\\00\"\n";out<<"@.bool.true = private unnamed_addr constant [5 x i8] c\"true\\00\"\n@.bool.false = private unnamed_addr constant [6 x i8] c\"false\\00\"\n";out<<"@.err.bounds = private unnamed_addr constant [81 x i8] c\"Quidra runtime error[INDEX_BOUNDS] at %lld:%lld: index %lld outside length %lld\\0A\\00\"\n";
out<<"@.fmt.runtime.error = private unnamed_addr constant [43 x i8] c\"Quidra runtime error[%s] at %lld:%lld: %s\\0A\\00\"\n";
out<<"@.code.overflow = private unnamed_addr constant [17 x i8] c\"INTEGER_OVERFLOW\\00\"\n@.msg.overflow = private unnamed_addr constant [17 x i8] c\"integer overflow\\00\"\n";
out<<"@.code.divzero = private unnamed_addr constant [17 x i8] c\"DIVISION_BY_ZERO\\00\"\n@.msg.divzero = private unnamed_addr constant [17 x i8] c\"division by zero\\00\"\n";
out<<"@.code.shift = private unnamed_addr constant [12 x i8] c\"SHIFT_COUNT\\00\"\n@.msg.shift = private unnamed_addr constant [34 x i8] c\"shift count outside integer width\\00\"\n";
out<<"@.code.range.step = private unnamed_addr constant [16 x i8] c\"RANGE_STEP_ZERO\\00\"\n@.msg.range.step = private unnamed_addr constant [19 x i8] c\"range step is zero\\00\"\n";
out<<"@.code.stack = private unnamed_addr constant [17 x i8] c\"CALL_DEPTH_LIMIT\\00\"\n@.msg.stack = private unnamed_addr constant [17 x i8] c\"call depth limit\\00\"\n";
out<<"@.code.numeric.cast = private unnamed_addr constant [19 x i8] c\"NUMERIC_CAST_RANGE\\00\"\n@.msg.numeric.cast = private unnamed_addr constant [39 x i8] c\"numeric cast outside destination range\\00\"\n";
out<<"@.code.shape = private unnamed_addr constant [15 x i8] c\"SHAPE_MISMATCH\\00\"\n@.msg.shape = private unnamed_addr constant [25 x i8] c\"captured extent mismatch\\00\"\n";
out<<"@.err.parse = private unnamed_addr constant [21 x i8] c\"numeric parse failed\\00\"\n";
out<<"@.err.input = private unnamed_addr constant [13 x i8] c\"input failed\\00\"\n";
out<<"@.err.json.type = private unnamed_addr constant [33 x i8] c\"JSON value has incompatible kind\\00\"\n";
out<<"@.err.file = private unnamed_addr constant [22 x i8] c\"file operation failed\\00\"\n";
out<<"@.code.time.sleep = private unnamed_addr constant [23 x i8] c\"INVALID_SLEEP_DURATION\\00\"\n@.msg.time.sleep = private unnamed_addr constant [23 x i8] c\"invalid sleep duration\\00\"\n";
out<<"@.code.random.range = private unnamed_addr constant [21 x i8] c\"INVALID_RANDOM_RANGE\\00\"\n@.msg.random.range = private unnamed_addr constant [21 x i8] c\"invalid random range\\00\"\n";
for (const auto& [name, value] : pool.entries) {
    out << "@" << name << " = private unnamed_addr constant [" << (value.size() + 1)
        << " x i8] c\"" << escape_bytes(value) << "\"\n";
}
out << "\n";
for (const auto& [_, pair] : array_cast_pairs)
    out << emit_array_cast_helper(pair.source,pair.target,array_layout);
for (const auto& [_, type] : clone_types) out << emit_clone_helper(type, layouts, array_layout);
std::map<std::string, Type> drop_types;
collect_drop_types(module, drop_types, layouts);
for (const auto& [_, type] : drop_types) out << emit_drop_helper(type, layouts, array_layout);
std::map<std::string, Type> equality_types;
collect_equality_types(module, equality_types, layouts);
for (const auto& [_, type] : equality_types) out << emit_equality_helper(type, layouts, array_layout);
for (const auto& function : funcs) out << function;
if(debug_info&&!primary_source.empty()) {
    const auto primary_path=std::filesystem::path(primary_source);
    out<<"\n!llvm.dbg.cu = !{!0}\n"
       <<"!llvm.module.flags = !{!2, !3}\n"
       <<"!llvm.ident = !{!4}\n"
       <<"!0 = distinct !DICompileUnit(language: DW_LANG_C11, file: !1, producer: \"Quidra\", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug)\n"
       <<"!1 = !DIFile(filename: \""<<escape_metadata(primary_path.filename().string())
       <<"\", directory: \""<<escape_metadata(primary_path.parent_path().string())<<"\")\n"
       <<"!2 = !{i32 2, !\"Dwarf Version\", i32 4}\n"
       <<"!3 = !{i32 2, !\"Debug Info Version\", i32 3}\n"
       <<"!4 = !{!\"Quidra\"}\n"
       <<"!5 = !DISubroutineType(types: !6)\n"
       <<"!6 = !{}\n";

    for(const auto& [source,id]:debug_files) {
        if(id==1) continue;
        const auto path=std::filesystem::path(source);
        out<<"!"<<id<<" = !DIFile(filename: \""<<escape_metadata(path.filename().string())
           <<"\", directory: \""<<escape_metadata(path.parent_path().string())<<"\")\n";
    }

    for(std::size_t i=0;i<module.functions.size();++i) {
        if(!debug_subprograms[i]) continue;
        const auto& f=module.functions[i];
        const auto file=debug_files.at(f.source_file);
        std::string display=f.entrypoint?"<top-level>":f.name;
        constexpr std::string_view method_prefix="$method.";
        if(display.rfind(method_prefix,0)==0) display.erase(0,method_prefix.size());
        const auto linkage=f.entrypoint?"main":mangle(f.name);
        out<<"!"<<*debug_subprograms[i]
           <<" = distinct !DISubprogram(name: \""<<escape_metadata(display)
           <<"\", linkageName: \""<<escape_metadata(linkage)
           <<"\", scope: !"<<file<<", file: !"<<file
           <<", line: "<<std::max<std::uint32_t>(1,f.source_line)
           <<", type: !5, scopeLine: "<<std::max<std::uint32_t>(1,f.source_line)
           <<", spFlags: DISPFlagDefinition, unit: !0)\n";
        if(debug_locations[i]) {
            out<<"!"<<*debug_locations[i]
               <<" = !DILocation(line: "<<std::max<std::uint32_t>(1,f.source_line)
               <<", column: "<<std::max<std::uint32_t>(1,f.source_column)
               <<", scope: !"<<*debug_subprograms[i]<<")\n";
        }
    }
}
return out.str();
}

} // namespace quidra
