#pragma once

#include "ir/function_builder.hpp"
#include "quidra/ir/visit.hpp"
#include "lowering/ownership_expansion.hpp"
#include "semantics/effect_summary.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace quidra::lowering {

inline bool contains_reference_fields(const ir::Module& module, const Type& type,
                                      std::set<std::string>& seen) {
    if (type.kind == TypeKind::Tuple || type.kind == TypeKind::Union) {
        for (const auto& child : (type.kind==TypeKind::Tuple?type.parameters:type.cases))
            if (contains_reference_fields(module,child,seen)) return true;
    }
    if (type.kind == TypeKind::Array && type.first)
        return contains_reference_fields(module,*type.first,seen);
    if (type.kind != TypeKind::Class || !seen.insert(type.class_name).second) return false;
    const auto layout = std::find_if(module.classes.begin(),module.classes.end(),[&](const auto& value){return value.name==type.class_name;});
    if (layout == module.classes.end()) return false;
    if (!layout->reference_fields.empty()) return true;
    for (const auto& child : layout->fields)
        if (contains_reference_fields(module,child,seen)) return true;
    return false;
}
inline bool contains_reference_fields(const ir::Module& module,const Type& type) {
    std::set<std::string> seen;
    return contains_reference_fields(module,type,seen);
}

// Address provenance is materialized only for bindings, stores and calls
// that require a root. Reads retain the ordinary address-only representation.
inline void place_reference_storage(ir::Module& module, const CheckedProgram& checked,
                                    ir::LoweringOptions options) {
    if (!checked.effects) return;
    const auto token_type = Type::simple(TypeKind::Int64);
    const bool reference_fields = std::any_of(module.classes.begin(),module.classes.end(),[](const auto& layout){return !layout.reference_fields.empty();});
    struct Signature { std::size_t count{}; std::vector<std::size_t> roots; };
    std::map<std::string,Signature> signatures;
    for (auto& function : module.functions) {
        auto& signature = signatures[function.name];
        signature.count = function.parameters.size();
        const auto* summary = checked.effects->find(function.source_function.empty()?function.name:function.source_function);
        if (!summary) continue;
        for (std::size_t i=0;i<signature.count;++i) {
            auto& parameter = function.parameters[i];
            parameter.target_escapes = i<summary->reference_target_escapes.size() && summary->reference_target_escapes[i];
            bool root = i<summary->reference_root_parameters.size() && summary->reference_root_parameters[i];
            root |= parameter.target_escapes;
            root |= reference_fields && i<summary->exposed_writes.size() && !summary->exposed_writes[i].empty() && contains_reference_fields(module,parameter.type);
            if (root && !function.external_symbol) signature.roots.push_back(i);
        }
        for (const auto index : signature.roots)
            function.parameters.push_back(ir::Parameter{"$root."+function.parameters[index].name,token_type});
    }
    for (auto& function : module.functions) {
        const auto* summary = checked.effects->find(function.source_function.empty()?function.name:function.source_function);
        if (!summary || function.blocks.empty()) continue;
        const auto& signature = signatures.at(function.name);
        std::map<std::string,std::string> parameter_roots;
        for (const auto index : signature.roots) parameter_roots.emplace(function.parameters[index].name,"$root."+function.parameters[index].name);
        std::set<std::string> names = summary->escaping_locals;
        if (options.box_every_address_taken_local)
            for (const auto& block : function.blocks) for (const auto& instruction : block.instructions)
                if (const auto* address=std::get_if<ir::AddressLocal>(&instruction)) names.insert(address->name);
        struct Box { std::string local; Type type; ir::ValueId object{}, serial{}; };
        std::map<std::string,Box> boxes;
        std::uint64_t next=function.next_value;
        std::map<ir::ValueId,const ir::Instruction*> definitions;
        struct Definitions final : ir::OperandVisitor {
            explicit Definitions(std::map<ir::ValueId,const ir::Instruction*>& values) : values(values) {}
            void definition(ir::ValueId value) override { values.emplace(value,instruction); }
            std::map<ir::ValueId,const ir::Instruction*>& values;
            const ir::Instruction* instruction{};
        } definition_visitor(definitions);
        for (const auto& block : function.blocks) for (const auto& instruction : block.instructions) {
            definition_visitor.instruction=&instruction;
            ir::visit_operands(instruction,definition_visitor);
            std::visit([&](const auto& node) {
                if constexpr (requires{node.source_name;node.name;node.type;})
                    if (names.contains(node.source_name) || names.contains(node.name))
                        boxes.try_emplace(node.name,Box{"$box."+node.name,node.type});
            },instruction);
        }
        std::map<ir::ValueId,Type> value_types;
        for(const auto& [value,definition]:definitions)std::visit([&](const auto& n) {
            if constexpr(requires{n.type;})value_types.emplace(value,n.type);
            else if constexpr(requires{n.field_type;})value_types.emplace(value,n.field_type);
            else if constexpr(requires{n.element_type;})value_types.emplace(value,n.element_type);
            else if constexpr(requires{n.payload_type;})value_types.emplace(value,n.payload_type);
            else if constexpr(requires{n.result;})value_types.emplace(value,n.result);
        },*definition);
        const auto field_name=[&](ir::ValueId object,std::size_t index) {
            const auto type=value_types.find(object);
            if(type!=value_types.end() && type->second.kind==TypeKind::Class)
                if(const auto layout=std::find_if(module.classes.begin(),module.classes.end(),[&](const auto& layout){return layout.name==type->second.class_name;});layout!=module.classes.end() && index<layout->field_names.size())return layout->field_names[index];
            return std::string("field");
        };
        std::map<ir::ValueId,std::string> value_paths;
        std::set<ir::ValueId> visiting_paths;
        const auto source_path=[&](const auto& self,ir::ValueId value)->std::string {
            if(const auto cached=value_paths.find(value);cached!=value_paths.end())return cached->second;
            if(!visiting_paths.insert(value).second)return "target";
            std::string path="target";
            if(const auto definition=definitions.find(value);definition!=definitions.end())std::visit([&](const auto& n) {
                using N=std::decay_t<decltype(n)>;
                if constexpr(std::is_same_v<N,ir::LoadLocal> || std::is_same_v<N,ir::AddressLocal> || std::is_same_v<N,ir::LoadReference> || std::is_same_v<N,ir::ReferenceAddress>)path=n.name=="$this"?"this":n.name;
                else if constexpr(std::is_same_v<N,ir::FieldGet> || std::is_same_v<N,ir::AddressField> || std::is_same_v<N,ir::FieldReferenceAddress>)path=self(self,n.object)+"."+field_name(n.object,n.index);
                else if constexpr(std::is_same_v<N,ir::LoadAddress>)path=self(self,n.address);
                else if constexpr(std::is_same_v<N,ir::AddressTupleElement>)path=self(self,n.address)+"."+std::to_string(n.index);
                else if constexpr(std::is_same_v<N,ir::ArrayGet> || std::is_same_v<N,ir::AddressElement>) {
                    std::string index="?";
                    if(const auto input=definitions.find(n.index);input!=definitions.end())if(const auto* integer=std::get_if<ir::ConstantInt>(input->second))index=integer->value;
                    path=self(self,n.array)+"["+index+"]";
                } else if constexpr(std::is_same_v<N,ir::TupleGet>)path=self(self,n.tuple)+"."+std::to_string(n.index);
                else if constexpr(std::is_same_v<N,ir::VariantPayload>)path=self(self,n.container);
            },*definition->second);
            visiting_paths.erase(value);value_paths.emplace(value,path);return path;
        };
        // Resolve the original SSA graph before block replacement invalidates
        // the definition pointers, including values used by later blocks.
        for(const auto& [value,_]:definitions)(void)source_path(source_path,value);
        const auto path=[&](ir::ValueId value){const auto found=value_paths.find(value);return found==value_paths.end()?std::string("target"):found->second;};
        for (std::size_t i=0;i<signature.count;++i) {
            const auto& parameter=function.parameters[i];
            if (!parameter.writable && names.contains(parameter.name)) boxes.try_emplace(parameter.name,Box{"$box."+parameter.name,parameter.type});
        }
        if (boxes.empty() && !reference_fields) continue;
        std::set<ir::ValueId> needed;
        std::set<std::string> reference_roots;
        const auto mark=[&](const auto& self,ir::ValueId value)->void {
            if (!value || !needed.insert(value).second) return;
            const auto found=definitions.find(value);if(found==definitions.end())return;
            std::visit([&](const auto& n) {
                using N=std::decay_t<decltype(n)>;
                if constexpr(std::is_same_v<N,ir::FieldGet> || std::is_same_v<N,ir::AddressField>) self(self,n.object);
                else if constexpr(std::is_same_v<N,ir::FieldReferenceAddress>) {}
                else if constexpr(std::is_same_v<N,ir::LoadAddress> || std::is_same_v<N,ir::AddressTupleElement>) self(self,n.address);
                else if constexpr(std::is_same_v<N,ir::ArrayGet> || std::is_same_v<N,ir::AddressElement> || std::is_same_v<N,ir::ArrayGrowMove>) self(self,n.array);
                else if constexpr(std::is_same_v<N,ir::TupleGet>) self(self,n.tuple);
                else if constexpr(std::is_same_v<N,ir::VariantPayload>) self(self,n.container);
                else if constexpr(std::is_same_v<N,ir::ReferenceAddress> || std::is_same_v<N,ir::LoadReference>) reference_roots.insert(n.name);
            },*found->second);
        };
        for (const auto& block:function.blocks) for(const auto& instruction:block.instructions) {
            if (const auto* bind=std::get_if<ir::BindFieldReference>(&instruction)) {mark(mark,bind->object);mark(mark,bind->address);}
            else if (const auto* call=std::get_if<ir::Call>(&instruction);call && signatures.contains(call->callee)) {
                for(auto index:signatures.at(call->callee).roots) if(index<call->args.size()) mark(mark,call->args[index].writable_address.value_or(call->args[index].value));
            } else if (const auto* store=std::get_if<ir::FieldSet>(&instruction);store && contains_reference_fields(module,store->field_type)) mark(mark,store->object);
            else if (const auto* store=std::get_if<ir::ArraySet>(&instruction);store && contains_reference_fields(module,store->element_type)) mark(mark,store->array);
            else if (const auto* store=std::get_if<ir::ArraySliceSet>(&instruction);store && contains_reference_fields(module,store->array_type)) mark(mark,store->array);
            else if (const auto* store=std::get_if<ir::StoreAddress>(&instruction);store && contains_reference_fields(module,store->type)) mark(mark,store->address);
            else if (const auto* store=std::get_if<ir::StoreReference>(&instruction);store && contains_reference_fields(module,store->type)) reference_roots.insert(store->name);
        }
        // A reference's root follows every candidate and every rebind, including
        // restores and null clears emitted for structured traversal scopes.
        for (bool changed=true;changed;) {
            const auto count=needed.size()+reference_roots.size();
            for (const auto& block:function.blocks) for(const auto& instruction:block.instructions)
                if(const auto* bind=std::get_if<ir::BindReference>(&instruction);bind && reference_roots.contains(bind->name)) mark(mark,bind->address);
            changed=count!=needed.size()+reference_roots.size();
        }
        if(boxes.empty() && signature.roots.empty() && needed.empty() && reference_roots.empty())continue;
        ir::Function entry;entry.blocks.push_back(ir::Block{"boxes",{}});
        ir::FunctionBuilder builder(entry,next,0,0);builder.enter(entry.blocks.front());
        const auto zero=builder.const_int(0);
        std::map<ir::ValueId,ir::ValueId> roots;
        for(auto& [name,box]:boxes) {
            const auto type=Type::class_type("$box<"+type_name(box.type)+">");
            if(std::none_of(module.classes.begin(),module.classes.end(),[&](const auto& layout){return layout.name==type.class_name;})) {
                ir::ClassLayout layout{type.class_name,{"value"},{box.type},false,{}};
                if(reference_fields){layout.field_names.emplace_back("$serial");layout.fields.push_back(token_type);}
                module.classes.push_back(std::move(layout));
            }
            box.serial=zero;
            if(reference_fields){box.serial=builder.fresh();builder.emit(ir::RootSerial{box.serial});}
            box.object=builder.fresh();roots[box.object]=box.serial;
            builder.emit(ir::DeclareLocal{box.local,type,{},0,0});
            std::vector<std::optional<ir::ValueId>> fields{std::nullopt};if(reference_fields)fields.push_back(box.serial);
            builder.emit(ir::ClassMake{box.object,type,std::move(fields)});
            builder.emit(ir::StoreLocal{box.local,box.object,type});
            const auto parameter=std::find_if(function.parameters.begin(),function.parameters.begin()+signature.count,[&](const auto& p){return p.name==name && !p.writable;});
            if(parameter!=function.parameters.begin()+signature.count) {
                const auto argument=builder.fresh();builder.emit(ir::LoadLocal{argument,name,box.type});
                const auto owned=copy_owned(builder,argument,box.type);builder.emit(ir::FieldSet{box.object,0,owned,box.type});
            }
        }
        for(const auto& name:reference_roots) {
            if(parameter_roots.contains(name))continue;
            builder.emit(ir::DeclareLocal{"$root."+name,token_type,{},0,0});
            builder.emit(ir::StoreLocal{"$root."+name,zero,token_type});
        }
        builder.finish();
        next=entry.next_value;
        const auto root=[&](ir::ValueId value){const auto found=roots.find(value);return found==roots.end()?zero:found->second;};
        for(auto& block:function.blocks) {
            ir::Function rewritten;rewritten.blocks.push_back(ir::Block{block.label,{}});
            ir::FunctionBuilder out(rewritten,next,0,0);out.enter(rewritten.blocks.front());
            ir::SourceLocation site;
            const auto slot_root=[&](const std::string& name) {
                if(boxes.contains(name))return boxes.at(name).serial;
                if(parameter_roots.contains(name) || reference_roots.contains(name)) {
                    const auto token=out.fresh();out.emit(ir::LoadLocal{token,"$root."+name,token_type});return token;
                }
                return zero;
            };
            const auto check_store=[&](ir::ValueId value,const Type& type,ir::ValueId destination,std::string path) {
                if(destination!=zero && contains_reference_fields(module,type))
                    out.emit(ir::StoredTargetsCheck{value,type,destination,std::move(path),site.position});
            };
            for(const auto& instruction:block.instructions) {
                if(const auto* location=std::get_if<ir::SourceLocation>(&instruction))site=*location;
                if(std::holds_alternative<ir::Return>(instruction) || std::holds_alternative<ir::ReturnVoid>(instruction)) {
                    for(auto box=boxes.rbegin();box!=boxes.rend();++box) {
                        const auto type=Type::class_type("$box<"+type_name(box->second.type)+">");
                        const auto owned=out.fresh();out.emit(ir::LoadLocal{owned,box->second.local,type,true});
                        release_owned(out,owned,type);
                    }
                }
                if(const auto* bind=std::get_if<ir::BindFieldReference>(&instruction)) {
                    const auto holder=root(bind->object),target=root(bind->address);
                    if(holder!=zero && target!=zero)out.emit(ir::SelfReferenceCheck{holder,target,path(bind->object)+"."+field_name(bind->object,bind->index),path(bind->address),site.position});
                    auto stored=*bind;stored.root=target;out.emit(stored);continue;
                }
                if(const auto* call=std::get_if<ir::Call>(&instruction);call && signatures.contains(call->callee)) {
                    auto extended=*call;
                    for(const auto index:signatures.at(call->callee).roots)
                        extended.args.push_back(ir::CallArgument{root(call->args.at(index).writable_address.value_or(call->args.at(index).value)),std::nullopt});
                    out.emit(std::move(extended));continue;
                }
                if(const auto* store=std::get_if<ir::StoreLocal>(&instruction)) {
                    check_store(store->value,store->type,boxes.contains(store->name)?boxes.at(store->name).serial:zero,store->name);
                    if(boxes.contains(store->name)) {
                        const auto& box=boxes.at(store->name);const auto value=store->borrowed?copy_owned(out,store->value,store->type):store->value;
                        out.emit(ir::FieldSet{box.object,0,value,store->type,store->replace_without_release});continue;
                    }
                } else if(const auto* store=std::get_if<ir::FieldSet>(&instruction)) check_store(store->value,store->field_type,root(store->object),path(store->object)+"."+field_name(store->object,store->index));
                else if(const auto* store=std::get_if<ir::ArraySet>(&instruction)) check_store(store->value,store->element_type,root(store->array),path(store->array)+"[?]");
                else if(const auto* store=std::get_if<ir::ArraySliceSet>(&instruction)) check_store(store->value,store->array_type,root(store->array),path(store->array)+"[:]");
                else if(const auto* store=std::get_if<ir::StoreAddress>(&instruction)) check_store(store->value,store->type,root(store->address),path(store->address));
                else if(const auto* store=std::get_if<ir::StoreReference>(&instruction)) check_store(store->value,store->type,slot_root(store->name),store->name);
                if(const auto* load=std::get_if<ir::LoadLocal>(&instruction);load && boxes.contains(load->name)) {
                    const auto& box=boxes.at(load->name);
                    if(load->move_out && owns_managed(load->type)) {
                        const auto borrowed=out.fresh();out.emit(ir::FieldGet{borrowed,box.object,0,load->type});
                        const auto owned=copy_owned(out,borrowed,load->type);const auto local="$box.move."+std::to_string(load->out);
                        out.emit(ir::DeclareLocal{local,load->type,{},0,0});out.emit(ir::StoreLocal{local,owned,load->type});out.emit(ir::LoadLocal{load->out,local,load->type,true});
                    } else out.emit(ir::FieldGet{load->out,box.object,0,load->type});
                    roots[load->out]=box.serial;continue;
                }
                if(const auto* address=std::get_if<ir::AddressLocal>(&instruction);address && boxes.contains(address->name)) {
                    out.emit(ir::AddressField{address->out,boxes.at(address->name).object,0});roots[address->out]=boxes.at(address->name).serial;continue;
                }
                out.emit(instruction);
                if(const auto* bind=std::get_if<ir::BindReference>(&instruction);bind && reference_roots.contains(bind->name) && !parameter_roots.contains(bind->name))
                    out.emit(ir::StoreLocal{"$root."+bind->name,root(bind->address),token_type});
                std::visit([&](const auto& n) {
                    using N=std::decay_t<decltype(n)>;
                    if constexpr(requires{n.out;}) {
                        if(!needed.contains(n.out))return;
                        if constexpr(std::is_same_v<N,ir::LoadLocal> || std::is_same_v<N,ir::AddressLocal> || std::is_same_v<N,ir::LoadReference> || std::is_same_v<N,ir::ReferenceAddress>) roots[n.out]=slot_root(n.name);
                        else if constexpr(std::is_same_v<N,ir::FieldReferenceAddress>) {const auto token=out.fresh();out.emit(ir::FieldReferenceRoot{token,n.object,n.index});roots[n.out]=token;}
                        else if constexpr(std::is_same_v<N,ir::FieldGet> || std::is_same_v<N,ir::AddressField>) roots[n.out]=root(n.object);
                        else if constexpr(std::is_same_v<N,ir::LoadAddress> || std::is_same_v<N,ir::AddressTupleElement>) roots[n.out]=root(n.address);
                        else if constexpr(std::is_same_v<N,ir::ArrayGet> || std::is_same_v<N,ir::AddressElement> || std::is_same_v<N,ir::ArrayGrowMove>) roots[n.out]=root(n.array);
                        else if constexpr(std::is_same_v<N,ir::TupleGet>) roots[n.out]=root(n.tuple);
                        else if constexpr(std::is_same_v<N,ir::VariantPayload>) roots[n.out]=root(n.container);
                    }
                },instruction);
            }
            out.finish();
            next=rewritten.next_value;
            block.instructions=std::move(rewritten.blocks.front().instructions);
        }
        auto& instructions=function.blocks.front().instructions;
        instructions.insert(instructions.begin(),entry.blocks.front().instructions.begin(),entry.blocks.front().instructions.end());
        function.next_value=next;
    }
}

} // namespace quidra::lowering
