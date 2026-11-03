#include "ir/inline_binding_origins.hpp"
#include "optimizer/inlining.hpp"
#include "optimizer/rule_registry.hpp"
#include "quidra/compiler.hpp"
#include "quidra/lowering.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
using namespace quidra;
using namespace quidra::ir;
using namespace quidra::ir::analysis;
using namespace quidra::optimizer;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
std::string named(const Module& module,std::string_view source) {
    for(const auto& fn:module.functions)if(fn.declaration && fn.declaration->kind==DeclarationIdentity::Kind::Function && fn.declaration->source_name==source)return fn.name;
    throw std::runtime_error("fixture source declaration missing");
}
void continuity() {
    std::string program=R"(class Item(int value)
class Model(Item first, Item second)
void walk_items(Model input)
    for auto item = reflect.each<Item>(input)[]
        print(item.value)
    return
void relay(Model input)
    walk_items(input)
    for auto item = reflect.each<Item>(input)[]
        print(item.value)
    return
void drive(Model input)
    relay(input)
    relay(input)
    return
Model model = Model(Item(1), Item(2))
drive(model)
)";
    // Authored caller statements supply headroom for the fixed per-caller
    // growth bound, separately from the unrelated module reserve below.
    const auto padding=[](std::string_view name,std::size_t count,std::string_view indent) {
        std::string text=std::string(indent)+"int32 "+std::string(name)+" = 0\n";
        for(std::size_t i=0;i<count;++i)text+=std::string(indent)+std::string(name)+" = "+std::string(name)+" + 1\n";
        return text;
    };
    for(const auto& [header,count]:std::vector<std::pair<std::string,std::size_t>>{{"void relay(Model input)\n",24},{"void drive(Model input)\n",80}})
        program.insert(program.find(header)+header.size(),padding("padding",count,"    "));
    program.insert(program.find("Model model ="),padding("entry_padding",80,""));
    auto module=lower(check(program));
    const auto scan=named(module,"walk_items"),relay=named(module,"relay");
    const auto original=module.binding_origins->functions.at(scan).source;
    const auto original_paths=original.traversals.at(0).paths;
    // Expand the inner helper before its caller so the real producer copies an
    // existing scoped row, then relocate it again for a second inline site.
    const auto first=std::find_if(module.functions.begin(),module.functions.end(),[&](const auto& f){return f.name==relay;});
    std::iter_swap(module.functions.begin(),first);
    // Unrelated valid IR provides a module growth reserve; it grants no source
    // declaration, effects or origin proof to any function.
    Function reserve;reserve.name="fixture.reserve";
    Block unused{"entry",{}};
    for(ValueId id=1;id<=500;++id)unused.instructions.push_back(ConstantBool{id,true});
    unused.instructions.push_back(ReturnVoid{});reserve.blocks.push_back(std::move(unused));
    reserve.next_value=501;module.functions.push_back(std::move(reserve));
    const auto before_owner=module.binding_origins;
    const auto before=binding_origin_body_fields(module.functions.front());
    auto registry=RuleRegistry::build(module);
    InliningOptions refused;refused.maximum_operations=0;
    require(!BudgetedInliner::run(module,registry,refused).changed() && module.binding_origins==before_owner &&
            binding_origin_body_fields(module.functions.front())==before,
            "refused expansion published a body or origin receipt");
    InliningOptions options;options.ordinary_instruction_limit=400;
    const auto report=BudgetedInliner::run(module,registry,options);
    require(report.inlined_calls>=2,"actual nested inlining did not execute");
    const auto read=read_binding_origins(module);
    require(read.inputs.functions.contains(relay) && read.inputs.functions.at(relay).availability.traversals &&
            read.inputs.functions.at(relay).traversals.at(0).paths==original_paths,
            "retained caller traversal disappeared after its call block split");
    bool copied=false,nested=false,carrier=false,typed_returns=false;
    for(const auto& [host,rows]:read.inputs.scopes) {
        const auto fn=std::find_if(module.functions.begin(),module.functions.end(),[&](const auto& f){return f.name==host;});
        require(fn!=module.functions.end(),"scoped source host is missing");
        if(!fn->declaration) {
            carrier=true;
            require(!module.binding_origins->functions.at(host).root_source_available && !read.inputs.functions.contains(host),
                    "entry carrier fabricated an original caller declaration");
        }
        for(const auto& [id,row]:rows) {
            require(id==row.scope && valid_scoped_binding_source(module,*fn,row),"scoped bind receipt disagrees with actual emitted IR");
            require(row.returns_complete && row.result_type.kind!=TypeKind::Invalid,
                    "accepted scope did not capture the original result ABI/all returns");
            for(const auto& returned:row.returns) {
                typed_returns=true;
                if(returned.kind==InlineReturnBinding::Kind::Void)
                    require(!returned.store && !returned.load && !returned.output &&
                            bool(returned.original_value)==bool(returned.value),
                            "void return fabricated value storage or output");
                else require(returned.store && returned.load && returned.storage && returned.value && returned.output &&
                             !returned.store_borrowed && !returned.store_replace_without_release &&
                             returned.load_move_out==owns_managed(row.result_type),
                             "value return lost exact storage/ownership transfer");
                if(returned.kind==InlineReturnBinding::Kind::Void) {
                    auto invalid_void=row;
                    invalid_void.returns.front().output=1;
                    require(!valid_scoped_binding_source(module,*fn,invalid_void),
                            "void return fabricated an output SSA value");
                    invalid_void=row;invalid_void.returns.front().storage="forbidden_void_result";
                    require(!valid_scoped_binding_source(module,*fn,invalid_void),
                            "void return fabricated result storage");
                }
                auto invalid=row;
                ++invalid.returns.front().end.index;
                require(!valid_scoped_binding_source(module,*fn,invalid),
                        "reader accepted a result disposition that skips the actual scope end");
            }
            if(row.source_function!=scan)continue;
            copied=true;nested=nested||row.parent!=0;
            require(row.semantic.has_value() && row.parameters.size()==1 && row.source.availability.traversals &&
                    row.source.traversals.at(0).paths==original_paths && row.source.declaration.source_name=="walk_items",
                    "copied traversal lost original typed semantic context");
            const auto& binding=row.parameters.at(0);
            const auto& path=row.source.local_storage_paths.at(binding.storage);
            require(binding.semantic_parameter==0 && path.root==semantics::StorageRoot::parameter(0) &&
                    row.source.traversals.at(0).root!=original.traversals.at(0).root,
                    "callee formal namespace or actual SSA substitution was flattened");
            require(!row.source.traversals.at(0).output,"each traversal fabricated an array output");
        }
    }
    require(copied && nested && carrier && typed_returns,"owned copied/nested/entry-host return/origin continuity was not produced");
}
}
int main(){try{continuity();std::cout<<"transactional inline binding origins: ok\n";return 0;}
catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
