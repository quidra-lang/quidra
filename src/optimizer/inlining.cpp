#include "optimizer/inlining.hpp"
#include "optimizer/rule_registry.hpp"
#include "ir/mutable_operands.hpp"
#include "ir/inline_binding_origins.hpp"
#include "ir/facts/entry.hpp"
#include "ir/analysis/call_graph.hpp"
#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>
namespace quidra::optimizer {
namespace {
using namespace ir;
struct Refused : std::runtime_error {using std::runtime_error::runtime_error;};
std::size_t size(const Function& f){std::size_t n=0;for(const auto& b:f.blocks){if(b.instructions.size()>std::numeric_limits<std::size_t>::max()-n)throw Refused("instruction count overflow");n+=b.instructions.size();}return n;}
struct BudgetStop {};
struct Work { InliningReport* report; std::uint64_t maximum; void touch(){if(report->operations==maximum)throw BudgetStop{};++report->operations;} };
struct Inventory final:MutableOperandVisitor {
    Work work;explicit Inventory(Work w):work(w){}
    std::set<ValueId> definitions,uses;
    std::set<std::string> locals,labels;
    ValueId maximum{};
    void definition(ValueId& v) override {work.touch();if(!v||!definitions.insert(v).second)throw Refused("duplicate or zero SSA definition");maximum=std::max(maximum,v);}
    void use(ValueId& v) override {work.touch();if(v){uses.insert(v);maximum=std::max(maximum,v);}}
    void metadata_value(ValueId&) override {work.touch();}
    void local(std::string& n,LocalRole) override {work.touch();locals.insert(n);}
    void successor(std::string& n) override {work.touch();labels.insert(n);}
};
Inventory inventory(const Function& f,Work work){Inventory v{work};for(const auto& b:f.blocks)for(auto n:b.instructions)mutate_operands(n,v);return v;}
struct Rename final:MutableOperandVisitor {
    Work work;
    const std::map<ValueId,ValueId>& values;
    const std::map<std::string,std::string,std::less<>>& names,labels;
    Rename(Work w,const std::map<ValueId,ValueId>&v,const std::map<std::string,std::string,std::less<>>&n,const std::map<std::string,std::string,std::less<>>&l):work(w),values(v),names(n),labels(l){}
    void mapped(ValueId& v){work.touch();if(!v)return;const auto p=values.find(v);if(p==values.end())throw Refused("unmapped actual SSA operand");v=p->second;}
    void definition(ValueId& v) override{mapped(v);} void use(ValueId& v) override{mapped(v);}
    void metadata_value(ValueId& v) override{mapped(v);}
    void local(std::string& n,LocalRole) override{work.touch();const auto p=names.find(n);if(p==names.end())throw Refused("unmapped storage operand");n=p->second;}
    void successor(std::string& n) override{work.touch();const auto p=labels.find(n);if(p==labels.end())throw Refused("unmapped CFG successor");n=p->second;}
};
bool terminal(const Instruction& n){
    if(std::holds_alternative<Return>(n)||std::holds_alternative<ReturnVoid>(n)||std::holds_alternative<Jump>(n)||std::holds_alternative<Branch>(n)||std::holds_alternative<Exit>(n)||std::holds_alternative<InlineExit>(n)||std::holds_alternative<FailError>(n))return true;
    if(const auto*p=std::get_if<Call>(&n))return p->no_normal_return||p->result.kind==TypeKind::Never;
    if(const auto*p=std::get_if<IndirectCall>(&n))return p->result.kind==TypeKind::Never;
    return false;
}
using SiteKey=std::tuple<std::size_t,std::string,std::size_t>;
using HotSites=std::set<SiteKey>;
struct PendingSites {
    using Emitted=std::pair<std::string,std::uint32_t>;
    std::map<ir::analysis::InstrRef,std::optional<Emitted>,ir::analysis::InlineSiteLess> instructions;
    std::map<ir::analysis::InstrRef,Emitted,ir::analysis::InlineSiteLess> boundaries;
    static std::uint32_t index(std::size_t i) {
        if(i>std::numeric_limits<std::uint32_t>::max())throw Refused("inline origin instruction count overflow");
        return static_cast<std::uint32_t>(i);
    }
    void finish(const Function& fn,ir::analysis::InlineOriginMap& result)const {
        std::map<std::string,ir::analysis::BlockId,std::less<>> blocks;
        for(std::size_t b=0;b<fn.blocks.size();++b) {
            if(b>std::numeric_limits<ir::analysis::BlockId>::max() ||
               !blocks.emplace(fn.blocks[b].label,static_cast<ir::analysis::BlockId>(b)).second)
                throw Refused("inline origin block identity is ambiguous");
        }
        for(const auto& [old,target]:instructions) {
            if(target)result.instructions.emplace(old,ir::analysis::InstrRef{blocks.at(target->first),target->second});
            else result.instructions.emplace(old,std::nullopt);
        }
        for(const auto& [old,target]:boundaries)result.boundaries.emplace(old,ir::analysis::InstrRef{blocks.at(target.first),target.second});
    }
};
struct Expansion {Function function;HotSites hot;ir::analysis::InlineOriginTransfer origins;};
struct Engine {
    Module& module;const RuleRegistry& rules;InliningOptions options;InliningReport report;
    std::vector<std::size_t> caller_base;std::size_t module_base{},module_current{};
    std::map<std::string,std::size_t,std::less<>> functions;
    std::set<std::string> ambiguous,recursive,address_taken,bound;
    std::set<SiteKey> declined;
    HotSites hot;
    bool parallel_context{};
    bool tick(std::uint64_t n=1){if(n>options.maximum_operations-report.operations){++report.budget_fallbacks;return false;}report.operations+=n;return true;}
    void reject(std::size_t caller,const Call& call,std::string reason){report.unavailable.push_back({module.functions[caller].name,call.callee,std::move(reason)});}
    void setup(){
        if (!options.node_cleanup_modes) {
            const auto cleanup = ir::analysis::node_cleanup_functions(
                module, options.maximum_operations - report.operations);
            report.operations += cleanup.operations;
            if (!cleanup.available)
                throw Refused(cleanup.budget_exhausted
                    ? "node cleanup reachability budget exhausted"
                    : "node cleanup reachability unavailable");
            std::map<std::string, bool, std::less<>> modes;
            for (const auto& function : module.functions) {
                if (report.operations == options.maximum_operations)
                    throw Refused("node cleanup mode assembly budget exhausted");
                ++report.operations;
                modes.emplace(function.name, cleanup.functions.contains(function.name));
            }
            // Publish only a fully assembled authoritative map. An unavailable
            // query or partial assembly never becomes an empty success fallback.
            options.node_cleanup_modes = std::move(modes);
        }
        for(std::size_t i=0;i<module.functions.size();++i){const auto& f=module.functions[i];const auto count=size(f);caller_base.push_back(count);if(count>std::numeric_limits<std::size_t>::max()-module_base)throw Refused("module instruction count overflow");module_base+=count;if(!functions.emplace(f.name,i).second)ambiguous.insert(f.name);if(f.parallel_node)parallel_context=true;
            for(const auto& b:f.blocks)for(const auto& n:b.instructions){if(const auto*p=std::get_if<FunctionRef>(&n))address_taken.insert(p->function);if(std::holds_alternative<ParallelBlock>(n))parallel_context=true;}
        }module_current=module_base;
        // Caller coordinates are converted once to authored block identities.
        // Accepted expansion explicitly remaps them; vector indices are not
        // retained after CFG insertion.
        for(const auto& site:options.hot_sites) {
            if(!tick())return;
            const auto fn=functions.find(site.caller);
            if(fn==functions.end() || ambiguous.contains(site.caller))throw Refused("hot caller unavailable or ambiguous");
            const auto& caller=module.functions[fn->second];
            if(site.block>=caller.blocks.size() || site.instruction>=caller.blocks[site.block].instructions.size() ||
               !std::holds_alternative<Call>(caller.blocks[site.block].instructions[site.instruction]))
                throw Refused("hot call site unavailable");
            hot.emplace(fn->second,caller.blocks[site.block].label,site.instruction);
        }
        for(const auto& table:rules.normalized_tables()){
            const auto kind=table.normalized?table.normalized->kind:CompilerExtensionTableKind::Passthrough;
            if(kind!=CompilerExtensionTableKind::Kernel&&kind!=CompilerExtensionTableKind::Derivative&&!table.name.starts_with("kernel.")&&!table.name.starts_with("derivative."))continue;
            std::string operation;if(table.normalized){const auto p=table.normalized->fields.find("operation");if(p!=table.normalized->fields.end())if(const auto*s=std::get_if<std::string>(&p->second))operation=*s;}else if(const auto p=table.raw.find("operation");p!=table.raw.end())operation=p->second;
            if(operation.empty()){for(const auto& [name,_]:rules.bindings().call_operations)bound.insert(name);continue;}
            const auto reference=table.extension+":"+operation;
            const auto p=rules.bindings().operation_functions.find(reference);if(p!=rules.bindings().operation_functions.end())bound.insert(p->second.begin(),p->second.end());
            if(const auto binding=rules.declarations().operations.find(reference);binding!=rules.declarations().operations.end())for(const auto& f:module.functions)if(f.declaration&&std::any_of(binding->second.declarations.begin(),binding->second.declarations.end(),[&](const auto& d){return same_declaration(d,*f.declaration);}))bound.insert(f.name);
        }
        for(const auto& f:module.functions)if(f.validated_entry&&bound.contains(f.validated_entry->source_instance))bound.insert(f.name);
        for(const auto& [name,index]:functions){
            std::vector<std::size_t> pending{index};std::set<std::size_t> seen{index};
            for(std::size_t p=0;p<pending.size();++p){if(!tick())return;for(const auto& b:module.functions[pending[p]].blocks)for(const auto& n:b.instructions){const auto edge=[&](const std::string& callee){const auto q=functions.find(callee);if(q==functions.end())return;if(q->second==index)recursive.insert(name);if(seen.insert(q->second).second)pending.push_back(q->second);};if(const auto*c=std::get_if<Call>(&n))edge(c->callee);if(const auto*c=std::get_if<ParallelBlock>(&n))for(const auto& node:c->nodes)edge(node.function);}}
        }
    }
    std::string eligible(const Function& f,const Call& call,std::size_t ci,std::size_t bi,std::size_t ii)const{
        if(f.external_symbol||f.c_export_symbol||f.entrypoint||f.parallel_node||address_taken.contains(f.name))return "open callable ABI";
        if(recursive.contains(f.name))return "recursive call graph";
        if(bound.contains(f.name))return "kernel or derivative binding";
        if(f.blocks.empty())return "body unavailable";
        if(call.args.size()!=f.parameters.size()||call.result!=f.result)return "call signature mismatch";
        if((call.result.kind==TypeKind::Void||call.result.kind==TypeKind::Never)?call.out!=0:call.out==0)return "call result ABI unavailable";
        for(std::size_t i=0;i<f.parameters.size();++i){const auto& p=f.parameters[i];const auto&a=call.args[i];if(p.type.kind==TypeKind::Invalid||p.type.kind==TypeKind::Auto)return "parameter type unavailable";if(p.writable?!a.writable_address||!*a.writable_address||a.value!=0:!a.value||a.writable_address.has_value())return "argument ownership or address ABI mismatch";}
        if(options.node_cleanup_modes){if(!options.node_cleanup_modes->contains(f.name))return "cleanup mode unavailable";}else if(parallel_context)return "cleanup mode unavailable";
        const bool candidate=hot.contains({ci,module.functions[ci].blocks[bi].label,ii});
        if(size(f)>(candidate?options.hot_instruction_limit:options.ordinary_instruction_limit))return "callee instruction budget";
        return {};
    }
    Expansion expand(const Function& original,const Function& callee,std::size_t ci,std::size_t bi,std::size_t ii){
        Function f=original;const auto call=std::get<Call>(f.blocks[bi].instructions[ii]);
        ir::analysis::InlineOriginTransfer origins;
        origins.caller=original.name;origins.callee=callee.name;
        origins.caller_body=ir::analysis::binding_origin_body_fields(original);
        origins.callee_body=ir::analysis::binding_origin_body_fields(callee);
        if(bi>std::numeric_limits<ir::analysis::BlockId>::max())throw Refused("inline origin block count overflow");
        origins.call={static_cast<ir::analysis::BlockId>(bi),PendingSites::index(ii)};origins.removed_call=call;
        PendingSites retained,copied_sites;
        for(std::size_t b=0;b<original.blocks.size();++b) {
            if(b>std::numeric_limits<ir::analysis::BlockId>::max())throw Refused("inline origin block count overflow");
            const auto block=static_cast<ir::analysis::BlockId>(b);const auto& old=original.blocks[b];
            origins.retained.labels.emplace(old.label,old.label);
            for(std::size_t i=0;i<=old.instructions.size();++i) {
                const ir::analysis::InstrRef at{block,PendingSites::index(i)};
                if(b==bi && i>ii)continue; // Authored below while emitting the continuation.
                retained.boundaries.emplace(at,PendingSites::Emitted{old.label,PendingSites::index(i)});
                if(i<old.instructions.size()) {
                    if(b==bi && i==ii)retained.instructions.emplace(at,std::nullopt);
                    else retained.instructions.emplace(at,PendingSites::Emitted{old.label,PendingSites::index(i)});
                }
            }
        }
        const bool inherited_hot=hot.contains({ci,f.blocks[bi].label,ii});
        HotSites transferred_hot;
        for(const auto& site:hot) {
            if(!tick())throw BudgetStop{};
            transferred_hot.insert(site);
        }
        auto source=inventory(callee,{&report,options.maximum_operations});for(auto use:source.uses)if(!source.definitions.contains(use))throw Refused("callee SSA use has no definition");
        auto target=inventory(f,{&report,options.maximum_operations});
        for(const auto id:target.definitions)origins.retained.values.emplace(id,id);
        for(const auto id:target.uses)origins.retained.values.emplace(id,id);
        for(const auto& name:target.locals)origins.retained.locals.emplace(name,name);
        for(const auto& param:original.parameters)origins.retained.locals.emplace(param.name,param.name);
        for(const auto& old:original.inline_scopes)origins.retained.scopes.emplace(old.identity,old.identity);
        f.next_value=std::max(f.next_value,std::uint64_t(target.maximum)+1);
        std::map<ValueId,ValueId> values;for(auto id:source.definitions){if(!f.next_value||f.next_value>std::numeric_limits<ValueId>::max())throw Refused("SSA allocation exhausted");values.emplace(id,static_cast<ValueId>(f.next_value++));}
        std::uint64_t scope_id=0,statement_id=0;for(const auto&s:f.inline_scopes)scope_id=std::max(scope_id,s.identity);
        for(const auto& b:f.blocks)for(const auto& n:b.instructions)if(const auto*p=std::get_if<StatementBoundary>(&n))statement_id=std::max(statement_id,p->identity);
        auto next_scope=[&](){if(scope_id==std::numeric_limits<std::uint64_t>::max())throw Refused("scope identity exhausted");return ++scope_id;};
        auto next_statement=[&](){if(statement_id==std::numeric_limits<std::uint64_t>::max())throw Refused("statement identity exhausted");return ++statement_id;};
        const auto root_id=next_scope();std::map<std::uint64_t,std::uint64_t> scopes,statements;
        for(const auto&s:callee.inline_scopes){if(!s.identity||!scopes.emplace(s.identity,next_scope()).second)throw Refused("invalid nested scope identity");}
        for(const auto& b:callee.blocks)for(const auto& n:b.instructions)if(const auto*p=std::get_if<StatementBoundary>(&n)){if(!p->identity)throw Refused("zero statement identity");if(!statements.contains(p->identity))statements.emplace(p->identity,next_statement());}
        const auto prefix="$inl."+std::to_string(root_id)+".";
        std::map<std::string,std::string,std::less<>> names,labels;std::set<std::string> existing_labels;for(const auto&b:f.blocks)existing_labels.insert(b.label);
        for(const auto&p:callee.parameters)source.locals.insert(p.name);
        for(const auto& n:source.locals){auto fresh=prefix+n;if(target.locals.contains(fresh))throw Refused("inline local collision");names.emplace(n,std::move(fresh));}
        for(const auto& b:callee.blocks){const auto fresh=prefix+b.label;if(existing_labels.contains(fresh)||!labels.emplace(b.label,fresh).second)throw Refused("inline block collision");}
        for(const auto& l:source.labels)if(!labels.contains(l))throw Refused("callee CFG missing successor");
        const auto binding=prefix+"bind",continuation=prefix+"continue",result_name=prefix+"result";
        if(existing_labels.contains(binding)||existing_labels.contains(continuation)||target.locals.contains(result_name)||names.contains("result")||labels.contains("bind")||labels.contains("continue"))throw Refused("inline generated-name collision");
        InlineScope scope;scope.identity=root_id;scope.callee=callee.name;scope.source_file=callee.source_file;scope.position=call.position;
        scope.node_cleanup=options.node_cleanup_modes&&options.node_cleanup_modes->at(callee.name);
        if(callee.validated_entry&&callee.validated_entry->origin==facts::EntryFactOrigin::SpecializedCalls)scope.entry_obligation=callee.validated_entry;
        const auto depth=[&](std::uint64_t id){std::size_t d=0;for(std::size_t k=0;k<f.inline_scopes.size()&&id;++k){auto p=std::find_if(f.inline_scopes.begin(),f.inline_scopes.end(),[&](const auto&x){return x.identity==id;});if(p==f.inline_scopes.end())throw Refused("scope parent unavailable");id=p->parent;++d;}if(id)throw Refused("cyclic scope parents");return d;};
        const auto frame=[&](const std::string& label){std::uint64_t selected=0;std::size_t maximum=0;for(const auto& s:f.inline_scopes)if(std::find(s.blocks.begin(),s.blocks.end(),label)!=s.blocks.end()){const auto d=depth(s.identity);if(d>maximum){maximum=d;selected=s.identity;}}return selected;};
        scope.parent=frame(f.blocks[bi].label);
        if(call.out&&scope.parent){auto parent=std::find_if(f.inline_scopes.begin(),f.inline_scopes.end(),[&](const auto& s){return s.identity==scope.parent;});parent->slots.push_back({result_name,call.result,InlineSlotKind::OwnedLocal,"",false,false});}
        for(std::size_t b=0;b<=bi;++b){if(frame(f.blocks[b].label)!=scope.parent)continue;const auto end=b==bi?ii:f.blocks[b].instructions.size();for(std::size_t i=0;i<end;++i){const auto&n=f.blocks[b].instructions[i];if(const auto*p=std::get_if<SourceLocation>(&n))scope.caller_location=*p;if(const auto*p=std::get_if<StatementBoundary>(&n))scope.caller_statement=p->identity;}}
        scope.blocks.push_back(binding);for(const auto&b:callee.blocks)scope.blocks.push_back(labels.at(b.label));
        // Already inlined child markers belong to their original child
        // context. The complete map still renames every physical boundary.
        std::set<std::uint64_t> nested_statements;
        for(const auto& nested:callee.inline_scopes)
            for(const auto& statement:nested.statements)
                nested_statements.insert(statement.remapped);
        for(const auto&[old,fresh]:statements)
            if(!nested_statements.contains(old))scope.statements.push_back({old,fresh});
        std::map<std::string,std::size_t,std::less<>> slots;
        auto slot=[&](const std::string&old,const Type&type,InlineSlotKind kind,bool is_const=false,bool escapes=false){const auto name=names.at(old);if(auto p=slots.find(name);p!=slots.end()){auto&s=scope.slots[p->second];if(s.type!=type)throw Refused("inconsistent inline storage type");if(kind==InlineSlotKind::BorrowedLocal)s.kind=kind;return;}slots.emplace(name,scope.slots.size());scope.slots.push_back({name,type,kind,old,is_const,escapes});};
        for(const auto&p:callee.parameters)slot(p.name,p.type,p.writable?InlineSlotKind::RawAddress:p.borrowed?InlineSlotKind::BorrowedLocal:InlineSlotKind::OwnedLocal,p.is_const,p.target_escapes);
        for(const auto& b:callee.blocks)for(const auto&n:b.instructions){if(const auto*p=std::get_if<DeclareLocal>(&n))slot(p->name,p->type,InlineSlotKind::OwnedLocal);if(const auto*p=std::get_if<DeclareReference>(&n))slot(p->name,p->type,InlineSlotKind::PinnedReference,p->is_const);if(const auto*p=std::get_if<StoreLocal>(&n))slot(p->name,p->type,p->borrowed?InlineSlotKind::BorrowedLocal:InlineSlotKind::OwnedLocal);if(const auto*p=std::get_if<FiniteFlagUpdate>(&n))slot(p->flag,Type::simple(TypeKind::Bool),InlineSlotKind::OwnedLocal);if(const auto*p=std::get_if<InlineBindAddress>(&n))slot(p->name,p->type,InlineSlotKind::RawAddress);}
        for(const auto&[old,fresh]:names){(void)old;if(!slots.contains(fresh))throw Refused("inline storage declaration unavailable");}
        // Nested slots belong only to their innermost frame.
        for(const auto&s:callee.inline_scopes)for(const auto&ss:s.slots){const auto renamed=names.at(ss.name);std::erase_if(scope.slots,[&](const auto&x){return x.name==renamed;});}
        origins.scope=root_id;origins.parent=scope.parent;
        origins.copied.values=values;origins.copied.locals=names;origins.copied.scopes=scopes;
        origins.copied.statements=statements;origins.copied.labels=labels;
        Rename rename({&report,options.maximum_operations},values,names,labels);std::vector<Block> copied;Block bind{binding,{InlineScopeBegin{root_id}}};
        std::vector<PendingSites::Emitted> parameter_sites;
        for(std::size_t p=0;p<callee.parameters.size();++p){const auto&param=callee.parameters[p];const auto&arg=call.args[p];
            parameter_sites.emplace_back(binding,PendingSites::index(bind.instructions.size()));
            ir::analysis::InlineParameterBinding actual;actual.ir_parameter=p;actual.storage=names.at(param.name);actual.type=param.type;
            actual.writable=param.writable;actual.borrowed=param.borrowed;actual.value=arg.value;actual.address=arg.writable_address;
            origins.parameters.push_back(std::move(actual));
            if(param.writable)bind.instructions.push_back(InlineBindAddress{names.at(param.name),*arg.writable_address,param.type});else bind.instructions.push_back(StoreLocal{names.at(param.name),arg.value,param.type,param.borrowed});}
        bind.instructions.push_back(Jump{labels.at(callee.blocks[0].label)});copied.push_back(std::move(bind));
        struct PendingReturn {
            ir::analysis::InlineReturnBinding row;
            std::optional<PendingSites::Emitted> store,load;
            PendingSites::Emitted cleanup,end,jump;
        };
        std::vector<PendingReturn> returned;
        bool has_return=false;
        for(std::size_t cb=0;cb<callee.blocks.size();++cb){const auto& b=callee.blocks[cb];Block nb{labels.at(b.label),{}};if(b.instructions.empty())throw Refused("callee missing explicit terminator");
            if(cb>std::numeric_limits<ir::analysis::BlockId>::max())throw Refused("inline origin block count overflow");
            const auto old_block=static_cast<ir::analysis::BlockId>(cb);
            copied_sites.boundaries.emplace(ir::analysis::InstrRef{old_block,0},PendingSites::Emitted{nb.label,0});
            bool boundary_overflow=false;
            for(std::size_t i=0;i<b.instructions.size();++i){
                const ir::analysis::InstrRef old_site{old_block,PendingSites::index(i)};
                auto [end_boundary,inserted]=copied_sites.boundaries.emplace(
                    ir::analysis::InstrRef{old_site.block,PendingSites::index(i+1)},PendingSites::Emitted{nb.label,0});
                if(!inserted)throw Refused("inline origin boundary collides");
                struct Boundary {
                    std::uint32_t& index;const Block& block;bool& overflow;
                    ~Boundary()noexcept {
                        if(block.instructions.size()>std::numeric_limits<std::uint32_t>::max())overflow=true;
                        else index=static_cast<std::uint32_t>(block.instructions.size());
                    }
                } boundary{end_boundary->second.second,nb,boundary_overflow};
                const auto& original_ins=b.instructions[i];
                if(std::holds_alternative<Return>(original_ins) || std::holds_alternative<ReturnVoid>(original_ins) || std::holds_alternative<Exit>(original_ins))
                    copied_sites.instructions.emplace(old_site,std::nullopt);
                else copied_sites.instructions.emplace(old_site,PendingSites::Emitted{nb.label,PendingSites::index(nb.instructions.size())});
                auto n=b.instructions[i];if(i+1<b.instructions.size()&&terminal(n))throw Refused("callee instruction after terminator");mutate_operands(n,rename);
            if(auto*p=std::get_if<StatementBoundary>(&n))p->identity=statements.at(p->identity);
            auto scope_ref=[&](std::uint64_t& id){const auto p=scopes.find(id);if(p==scopes.end())throw Refused("unmapped nested scope");id=p->second;};
            if(auto*p=std::get_if<InlineScopeBegin>(&n))scope_ref(p->scope);
            if(auto*p=std::get_if<InlineScopeCleanup>(&n))scope_ref(p->scope);
            if(auto*p=std::get_if<InlineScopeEnd>(&n))scope_ref(p->scope);
            PendingReturn disposition;
            disposition.row.original=old_site;disposition.row.continuation=continuation;
            if(const auto*p=std::get_if<Return>(&n)) {
                if(p->type!=callee.result||call.no_normal_return)throw Refused("return ABI mismatch");
                const auto& authored=std::get<Return>(original_ins);
                disposition.row.original_value=authored.value;disposition.row.type=authored.type;
                disposition.row.value=p->value;
                if(p->type.kind==TypeKind::Void) {
                    if(call.out)throw Refused("void return ABI mismatch");
                    disposition.row.kind=ir::analysis::InlineReturnBinding::Kind::Void;
                } else {
                    disposition.row.kind=ir::analysis::InlineReturnBinding::Kind::Value;
                    disposition.row.storage=result_name;
                    const StoreLocal store{result_name,p->value,p->type};
                    disposition.row.store_borrowed=store.borrowed;
                    disposition.row.store_replace_without_release=store.replace_without_release;
                    disposition.store=PendingSites::Emitted{nb.label,PendingSites::index(nb.instructions.size())};
                    nb.instructions.push_back(store);
                }
                has_return=true;
            }
            else if(std::holds_alternative<ReturnVoid>(n)) {
                if(callee.result.kind!=TypeKind::Void||call.no_normal_return)throw Refused("void return ABI mismatch");
                disposition.row.kind=ir::analysis::InlineReturnBinding::Kind::Void;
                disposition.row.type=callee.result;has_return=true;
            }
            else if(const auto*p=std::get_if<Exit>(&n)){nb.instructions.push_back(InlineScopeCleanup{root_id});nb.instructions.push_back(InlineExit{root_id,p->status});continue;}
            else if(auto*p=std::get_if<InlineExit>(&n)){scope_ref(p->scope);nb.instructions.push_back(std::move(n));continue;}
            else {
                if(std::holds_alternative<Call>(n) && (inherited_hot || hot.contains({functions.at(callee.name),b.label,i})))
                    transferred_hot.emplace(ci,nb.label,nb.instructions.size());
                nb.instructions.push_back(std::move(n));continue;
            }
            disposition.cleanup={nb.label,PendingSites::index(nb.instructions.size())};
            nb.instructions.push_back(InlineScopeCleanup{root_id});
            disposition.end={nb.label,PendingSites::index(nb.instructions.size())};
            nb.instructions.push_back(InlineScopeEnd{root_id});
            disposition.jump={nb.label,PendingSites::index(nb.instructions.size())};
            nb.instructions.push_back(Jump{continuation});
            returned.push_back(std::move(disposition));
        }if(boundary_overflow)throw Refused("inline origin instruction count overflow");
        if(!terminal(b.instructions.back()))throw Refused("callee missing explicit terminator");copied.push_back(std::move(nb));}
        Block cont{continuation,{}};
        if(call.out&&has_return) {
            const LoadLocal load{call.out,result_name,call.result,owns_managed(call.result)};
            for(auto& disposition:returned) {
                disposition.load=PendingSites::Emitted{cont.label,PendingSites::index(cont.instructions.size())};
                disposition.row.output=load.out;disposition.row.load_move_out=load.move_out;
            }
            cont.instructions.push_back(load);
        }
        // Later calls of the split block now live in its continuation. The
        // optional result load is a real prefix instruction of that block.
        const auto continuation_prefix=cont.instructions.size();
        for(auto site=transferred_hot.begin();site!=transferred_hot.end();) {
            if(std::get<0>(*site)==ci && std::get<1>(*site)==f.blocks[bi].label && std::get<2>(*site)>=ii) {
                if(std::get<2>(*site)>ii)
                    transferred_hot.emplace(ci,continuation,continuation_prefix+std::get<2>(*site)-ii-1);
                site=transferred_hot.erase(site);
            } else ++site;
        }
        for(std::size_t i=ii+1;i<=original.blocks[bi].instructions.size();++i) {
            const ir::analysis::InstrRef old_site{static_cast<ir::analysis::BlockId>(bi),PendingSites::index(i)};
            const PendingSites::Emitted actual{cont.label,PendingSites::index(cont.instructions.size())};
            retained.boundaries.emplace(old_site,actual);
            if(i<original.blocks[bi].instructions.size()) {
                retained.instructions.emplace(old_site,actual);
                cont.instructions.push_back(original.blocks[bi].instructions[i]);
            }
        }
        if(!has_return&&call.out)throw Refused("nonreturning result has no value");
        auto& prefix_block=f.blocks[bi];prefix_block.instructions.erase(prefix_block.instructions.begin()+ii,prefix_block.instructions.end());if(call.out)prefix_block.instructions.push_back(DeclareLocal{result_name,call.result,"",0,0});prefix_block.instructions.push_back(Jump{binding});
        // Outer scopes cover the copied descendants and outer continuation.
        for(auto&s:f.inline_scopes)if(std::find(s.blocks.begin(),s.blocks.end(),prefix_block.label)!=s.blocks.end()){s.blocks.insert(s.blocks.end(),scope.blocks.begin(),scope.blocks.end());s.blocks.push_back(continuation);}
        for(const auto& original_scope:callee.inline_scopes){auto ns=original_scope;ns.identity=scopes.at(ns.identity);ns.parent=ns.parent?scopes.at(ns.parent):root_id;ns.caller_statement=ns.caller_statement?statements.at(ns.caller_statement):scope.caller_statement;for(auto&label:ns.blocks)label=labels.at(label);for(auto& ss:ns.slots)ss.name=names.at(ss.name);for(auto& st:ns.statements)st.remapped=statements.at(st.remapped);f.inline_scopes.push_back(std::move(ns));}
        f.inline_scopes.push_back(std::move(scope));copied.push_back(std::move(cont));f.blocks.insert(f.blocks.begin()+bi+1,std::make_move_iterator(copied.begin()),std::make_move_iterator(copied.end()));
        retained.finish(f,origins.retained);copied_sites.finish(f,origins.copied);
        const auto bind_block=std::find_if(f.blocks.begin(),f.blocks.end(),[&](const auto& b){return b.label==binding;});
        origins.begin={static_cast<ir::analysis::BlockId>(bind_block-f.blocks.begin()),0};
        for(std::size_t p=0;p<origins.parameters.size();++p)origins.parameters[p].at={origins.begin.block,parameter_sites[p].second};
        std::map<std::string,ir::analysis::BlockId,std::less<>> destination_blocks;
        for(std::size_t b=0;b<f.blocks.size();++b) {
            if(b>std::numeric_limits<ir::analysis::BlockId>::max() ||
               !destination_blocks.emplace(f.blocks[b].label,static_cast<ir::analysis::BlockId>(b)).second)
                throw Refused("inline return block identity is ambiguous");
        }
        const auto emitted=[&](const PendingSites::Emitted& at) {
            return ir::analysis::InstrRef{destination_blocks.at(at.first),at.second};
        };
        for(auto& disposition:returned) {
            if(disposition.store)disposition.row.store=emitted(*disposition.store);
            if(disposition.load)disposition.row.load=emitted(*disposition.load);
            disposition.row.cleanup=emitted(disposition.cleanup);
            disposition.row.end=emitted(disposition.end);disposition.row.jump=emitted(disposition.jump);
            origins.returns.push_back(std::move(disposition.row));
        }
        return {std::move(f),std::move(transferred_hot),std::move(origins)};
    }
    InliningReport run(){if(!options.maximum_operations){++report.budget_fallbacks;return std::move(report);}try{setup();}catch(const Refused& e){++report.budget_fallbacks;report.unavailable.push_back({"","",e.what()});return std::move(report);}if(report.operations>=options.maximum_operations)return std::move(report);
        for(;;){bool changed=false;for(std::size_t ci=0;ci<module.functions.size()&&!changed;++ci){auto& caller=module.functions[ci];if(caller.external_symbol)continue;for(std::size_t bi=0;bi<caller.blocks.size()&&!changed;++bi)for(std::size_t ii=0;ii<caller.blocks[bi].instructions.size()&&!changed;++ii){if(!tick())return std::move(report);const auto*p=std::get_if<Call>(&caller.blocks[bi].instructions[ii]);if(!p)continue;const auto key=std::tuple{ci,caller.blocks[bi].label,ii};if(declined.contains(key))continue;const auto call=*p;const auto found=functions.find(call.callee);if(found==functions.end()||ambiguous.contains(call.callee)){declined.insert(key);reject(ci,call,"callee unavailable or ambiguous");continue;}const auto reason=eligible(module.functions[found->second],call,ci,bi,ii);if(!reason.empty()){declined.insert(key);reject(ci,call,reason);continue;}
            try{if(!tick(size(caller))||!tick(size(module.functions[found->second])))return std::move(report);auto next=expand(caller,module.functions[found->second],ci,bi,ii);const auto old=size(caller),count=size(next.function);const auto growth=count>old?count-old:0;
                const auto used=module_current-module_base,limit=module_base/2;
                if((count>caller_base[ci]&&count-caller_base[ci]>caller_base[ci])||used>limit||growth>limit-used||growth>std::numeric_limits<std::size_t>::max()-module_current){++report.budget_fallbacks;declined.insert(key);reject(ci,call,"caller or module growth budget");continue;}
                auto origins=ir::analysis::transfer_inline_origins(module,caller,module.functions[found->second],next.function,next.origins);
                for(const auto& issue:origins.unavailable)report.unavailable.push_back({caller.name,call.callee,
                    "binding origin component unavailable ("+std::to_string(static_cast<unsigned>(issue.component))+":"+std::to_string(static_cast<unsigned>(issue.reason))+")"});
                caller=std::move(next.function);hot=std::move(next.hot);
                if(module.binding_origins)module.binding_origins=std::move(origins.snapshots);
                module_current+=growth;report.instruction_growth=module_current-module_base;++report.inlined_calls;changed=true;
            }catch(const BudgetStop&){++report.budget_fallbacks;return std::move(report);}catch(const Refused&e){declined.insert(key);reject(ci,call,e.what());}catch(const std::out_of_range&){declined.insert(key);reject(ci,call,"inline metadata reference unavailable");}catch(const std::length_error&){++report.budget_fallbacks;declined.insert(key);reject(ci,call,"inline storage size budget");}
        }}if(!changed)break;}return std::move(report);
    }
};
}
InliningReport BudgetedInliner::run(ir::Module& module,const RuleRegistry& registry,InliningOptions options){Engine e{module,registry,std::move(options),{},{},0,0,{},{},{},{},{},{},{},false};return e.run();}
} // namespace quidra::optimizer
