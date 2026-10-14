#include "optimizer/helper_specialization.hpp"
#include "ir/helper_binding_origins.hpp"
#include "ir/analysis/analysis_cache.hpp"
#include "platform/sha256.hpp"
#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>

namespace quidra::optimizer {
namespace {
using namespace ir;
using namespace ir::facts;
using namespace ir::analysis;
struct Exhausted {};
struct Work {
    std::uint64_t used{}, limit{};
    void take(std::uint64_t n = 1) {
        if (n > limit - used) throw Exhausted{};
        used += n;
    }
};
void part(std::string& out, std::string_view s) { out += ':' + std::to_string(s.size()) + ':'; out += s; }
std::string type_key(const Type& t) {
    std::string out = std::to_string(static_cast<unsigned>(t.kind));
    part(out, t.class_name); part(out, t.union_name);
    if (t.kind == TypeKind::Array || t.kind == TypeKind::Tensor) out += ':' + std::to_string(t.length);
    if (t.first) part(out, type_key(*t.first));
    for (const auto& v : t.parameters) part(out, type_key(v));
    for (const auto& v : t.cases) part(out, type_key(v));
    for (const auto& v : t.case_names) part(out, v);
    return out;
}
std::string instance_key(const Function& f) {
    if (!f.declaration) return {};
    auto out = declaration_key(*f.declaration);
    for (const auto& v : f.declaration->generic_args) part(out, v);
    part(out, type_key(f.result));
    for (const auto& p : f.parameters) {
        part(out, p.name); part(out, type_key(p.type));
        out += ':' + std::to_string(p.writable) + std::to_string(p.borrowed) + std::to_string(p.is_const) + std::to_string(p.target_escapes);
    }
    out += f.shape_wrapper ? ":wrapper" : ":body";
    return out;
}
TensorFacts unknown(const Type& type) {
    TensorFacts f;
    if (type.kind == TypeKind::Tensor && type.first) { f.has_type = true; f.type = Type::tensor(*type.first); }
    return f;
}
ShapeExtent numeric(const ShapeExtent& value) { return value.known() ? ShapeExtent(*value.known()) : ShapeExtent{}; }
TensorFacts formal(TensorFacts f) {
    if (!f.has_type || f.type.kind != TypeKind::Tensor || !f.type.first) return {};
    f.type = Type::tensor(*f.type.first, f.type.length);
    for (auto& v : f.extents) v = numeric(v);
    for (auto& v : f.head_extents) v = numeric(v);
    for (auto& v : f.tail_extents) v = numeric(v);
    f.rank_extent = numeric(f.rank_extent);
    if (f.type.length < 0 && f.rank_extent.known()) f.type.length = *f.rank_extent.known();
    if (f.type.length >= 0) {
        f.minimum_rank = static_cast<std::size_t>(f.type.length);
        f.rank_extent = ShapeExtent(f.type.length);
        f.extents.resize(f.minimum_rank);
        f.head_extents.clear(); f.tail_extents.clear();
    }
    f.type.tensor_known_shape_prefix = f.type.length >= 0 ? f.extents : f.head_extents;
    f.unique = Tri::Unknown; f.owns_storage = false; f.fully_initialized = Tri::Unknown;
    return f;
}
TensorFacts meet(TensorFacts a, const TensorFacts& b) {
    if (!a.has_type || !b.has_type || !a.type.first || !b.type.first || *a.type.first != *b.type.first) return {};
    if (a.type.length != b.type.length) { a.type.length = -1; a.extents.clear(); }
    const auto axes = [](auto& dst, const auto& src) {
        dst.resize(std::min(dst.size(), src.size()));
        for (std::size_t i = 0; i < dst.size(); ++i) if (!same_extent(dst[i], src[i])) dst[i] = {};
    };
    axes(a.extents, b.extents); axes(a.head_extents, b.head_extents); axes(a.tail_extents, b.tail_extents);
    a.minimum_rank = std::min(a.minimum_rank, b.minimum_rank);
    if (!same_extent(a.rank_extent, b.rank_extent)) a.rank_extent = {};
    if (a.device.kind != b.device.kind) {
        const auto gpu = [](BackendKind k) { return k != BackendKind::Unknown && k != BackendKind::Cpu; };
        a.device.kind = gpu(a.device.kind) && gpu(b.device.kind) ? BackendKind::AnyGpu : BackendKind::Unknown;
    }
    if (a.device.index != b.device.index || a.device.kind == BackendKind::Unknown) a.device.index.reset();
    if (a.contiguous != b.contiguous) a.contiguous = Tri::Unknown;
    if (a.tracked != b.tracked) a.tracked = Tri::Unknown;
    return formal(std::move(a));
}
ParamFacts params(const ValidatedEntryFacts& e) { return {e.tensors, e.scalars, e.policies}; }
ValidatedEntryFacts combine(const ValidatedEntryFacts& a, const ValidatedEntryFacts& b) {
    auto out = a;
    for (auto& [i, f] : out.tensors) { const auto it = b.tensors.find(i); f = it == b.tensors.end() ? TensorFacts{} : meet(std::move(f), it->second); }
    for (auto it = out.scalars.begin(); it != out.scalars.end();) {
        const auto v = b.scalars.find(it->first);
        if (v == b.scalars.end() || !same_extent(it->second, v->second)) it = out.scalars.erase(it); else ++it;
    }
    for (auto it = out.policies.begin(); it != out.policies.end();) {
        const auto v = b.policies.find(it->first);
        if (v == b.policies.end() || v->second != it->second) it = out.policies.erase(it); else ++it;
    }
    return out;
}
std::string signature(const ValidatedEntryFacts& e, bool geometry) {
    std::string out = e.source_instance;
    for (const auto& [i, f] : e.tensors) {
        out += ":p" + std::to_string(i);
        part(out, f.has_type && f.type.first ? type_key(*f.type.first) : "?");
        out += ":r" + std::to_string(f.has_type ? f.type.length : -1);
        out += ":d" + std::to_string(static_cast<unsigned>(f.device.kind));
        out += ":i" + (f.device.index ? std::to_string(*f.device.index) : "?");
        out += ":c" + std::to_string(static_cast<unsigned>(f.contiguous)) + ":t" + std::to_string(static_cast<unsigned>(f.tracked));
        if (geometry) {
            out += ":min" + std::to_string(f.minimum_rank);
            for (const auto* extents : {&f.extents, &f.head_extents, &f.tail_extents}) { out += '['; for (const auto& v : *extents) part(out, canonical_extent_key(v)); out += ']'; }
        }
    }
    if (geometry) {
        for (const auto& [i, v] : e.scalars) { out += ":s" + std::to_string(i); part(out, canonical_extent_key(v)); }
        for (const auto& [k, v] : e.policies) { part(out, k); part(out, v); }
    }
    return out;
}
bool equal(const EntryFactsMap& a, const EntryFactsMap& b) {
    if (a.size() != b.size()) return false;
    for (const auto& [name, e] : a) { const auto it = b.find(name); if (it == b.end() || signature(*e, true) != signature(*it->second, true)) return false; }
    return true;
}
bool useful(const ValidatedEntryFacts& e) {
    for (const auto& [i, f] : e.tensors) {
        (void)i;
        if (f.type.length >= 0 || f.minimum_rank || f.device.kind != BackendKind::Unknown || f.contiguous != Tri::Unknown || f.tracked != Tri::Unknown) return true;
    }
    return !e.scalars.empty() || !e.policies.empty();
}
std::size_t size(const Function& f) {
    std::size_t n{};
    for (const auto& b : f.blocks) { if (b.instructions.size() > std::numeric_limits<std::size_t>::max() - n) throw std::overflow_error("specialization instruction count exhausted"); n += b.instructions.size(); }
    return n;
}
bool hot(const Function& f) {
    for (const auto& b : f.blocks) for (const auto& i : b.instructions) if (std::visit([](const auto& n) {
        using T = std::decay_t<decltype(n)>;
        if constexpr (InstructionTraits<T>::domain == Domain::tensor || InstructionTraits<T>::domain == Domain::autograd) return true;
        if constexpr (std::is_same_v<T, Unary>) return n.type.kind == TypeKind::Tensor;
        if constexpr (std::is_same_v<T, Binary>) return n.result_type.kind == TypeKind::Tensor;
        return false;
    }, i)) return true;
    return false;
}
struct Site { std::size_t caller{}, target{}; InstrRef at; ValidatedEntryFacts entry; };
struct Engine {
    Module& module; AnalysisCache& cache; const EntryFactsMap& prior; SpecializationOptions options;
    HelperSpecializationReport report; Work work;
    std::map<std::string, std::size_t, std::less<>> index;
    std::set<std::string, std::less<>> ambiguous, open;
    std::vector<std::string> instances;
    std::vector<Site> sites;
    EntryFactsMap entries;
    Engine(Module& m, AnalysisCache& c, const EntryFactsMap& p, SpecializationOptions o) : module(m), cache(c), prior(p), options(o), work{0,o.maximum_operations} {}
    bool clone(const std::string& name) const { const auto e = prior.find(name); return e != prior.end() && e->second && e->second->origin == EntryFactOrigin::SpecializedCalls; }
    void unavailable(const Function& f, std::string reason) { report.unavailable.push_back({f.name,std::move(reason)}); }
    bool eligible(std::size_t i) const {
        const auto& f = module.functions[i];
        return f.declaration && !f.declaration->external && !f.external_symbol && !f.entrypoint && !f.dispatch_stub && !f.blocks.empty() &&
            !clone(f.name) && !ambiguous.contains(f.name) && !instances[i].empty();
    }
    void catalog() {
        std::map<std::string,std::vector<std::size_t>,std::less<>> families;
        for (std::size_t i=0;i<module.functions.size();++i) {
            work.take(); const auto& f=module.functions[i];
            if (!index.emplace(f.name,i).second) ambiguous.insert(f.name);
            instances.push_back(instance_key(f));
            if (!clone(f.name) && !instances.back().empty()) families[instances.back()].push_back(i);
            if (f.entrypoint || f.external_symbol || f.c_export_symbol || f.parallel_node) open.insert(f.name);
            for (const auto& b:f.blocks) for (const auto& ins:b.instructions) {
                work.take();
                if (const auto* n=std::get_if<FunctionRef>(&ins)) open.insert(n->function);
                if (const auto* n=std::get_if<ParallelBlock>(&ins)) for (const auto& node:n->nodes) open.insert(node.function);
            }
        }
        for (const auto& [key, rows]:families) if (rows.size()>1) for (auto i:rows) { instances[i].clear(); unavailable(module.functions[i],"ambiguous exact declaration/concrete ABI instance"); }
        for (const auto& [name,e]:prior) if (e && e->origin==EntryFactOrigin::SpecializedCalls && index.contains(name)) entries.emplace(name,e);
    }
    bool read_only(const Call& n) const {return helper_descriptor_read_only(module,n);}
    bool barrier(const Instruction& i, ValueId value, const AliasInfo& aliases) const {
        if (const auto* n=std::get_if<Call>(&i)) {
            if (read_only(*n)) return false;
            if (n->args.empty()) return true;
            for (const auto& a:n->args) if (a.writable_address || aliases.may_alias(value,a.value)) return true;
            // Missing descriptor/effect contracts may reach values without
            // an explicit tensor argument; semantic path rows alone do not
            // certify native graph/descriptor stability.
            const auto* summary=module.summaries?module.summaries->find(n->callee):nullptr;
            return !summary || !summary->semantic || !summary->descriptor_complete || !summary->effects_complete ||
                (summary->effects&effect_bit::Opaque) || summary->semantic->mutates_ambient;
        }
        return std::holds_alternative<IndirectCall>(i) || std::holds_alternative<ParallelBlock>(i) || std::holds_alternative<StoreAddress>(i) ||
            std::holds_alternative<StoreReference>(i) || std::holds_alternative<FieldSet>(i) || std::holds_alternative<ArraySet>(i) ||
            std::holds_alternative<ArraySliceSet>(i) || std::holds_alternative<TensorSet>(i) || std::holds_alternative<TensorBackward>(i);
    }
    bool stable(std::size_t caller, InstrRef at, ValueId value, const TensorFactAnalysis& facts) {
        const auto& f=module.functions[caller]; const auto& cfg=cache.cfg(caller); const auto& aliases=cache.aliases(caller);
        std::optional<InstrRef> fresh;
        std::set<ValueId> lineage;
        for (auto v=value;v && lineage.insert(v).second;) {
            work.take();const auto d=facts.definitions.find(v);if(d==facts.definitions.end())break;
            const auto& ins=f.blocks[d->second.block].instructions[d->second.index];
            if(std::holds_alternative<TensorCreate>(ins)){fresh=d->second;break;}
            ValueId input{};
            std::visit([&](const auto& n){using T=std::decay_t<decltype(n)>;
                if constexpr(std::is_same_v<T,Clone>) input=n.value;
                else if constexpr(std::is_same_v<T,TensorTrack> || std::is_same_v<T,TensorTransfer> || std::is_same_v<T,TensorReshape> || std::is_same_v<T,TensorTranspose> || std::is_same_v<T,TensorContiguous> || std::is_same_v<T,TensorCast> || std::is_same_v<T,TensorIndex>) input=n.tensor;
            },ins);
            if(!input)if(const auto l=facts.lineage.find(v);l!=facts.lineage.end()&&l->second.kind==TensorFactKey::Value)input=l->second.id;
            v=input;
        }
        std::set<BlockId> ancestors;std::vector<BlockId> pending=cfg.pred[at.block];
        while(!pending.empty()){work.take();auto b=pending.back();pending.pop_back();if(!cfg.reachable[b]||!ancestors.insert(b).second)continue;for(auto p:cfg.pred[b])pending.push_back(p);}
        for(BlockId b=0;b<f.blocks.size();++b) {
            if (b!=at.block && !ancestors.contains(b)) continue;
            for(std::uint32_t j=0;j<f.blocks[b].instructions.size();++j) {
                work.take();const InstrRef where{b,j};
                if (b==at.block && j>=at.index && !ancestors.contains(b)) continue;
                if (where==at) continue;
                if (fresh && cache.dominators(caller).dominates(where,*fresh)) continue;
                if (barrier(f.blocks[b].instructions[j],value,aliases)) return false;
            }
        }
        return true;
    }
    TensorFacts argument(std::size_t caller, InstrRef at, const CallArgument& a, const ir::Parameter& p, const TensorFactAnalysis& facts) {
        if (p.writable || a.writable_address || !a.value) return unknown(p.type);
        auto f=formal(facts.at(at,a.value));
        if (!f.has_type || !f.type.first || !p.type.first || *f.type.first!=*p.type.first) return unknown(p.type);
        if (!stable(caller,at,a.value,facts)) return unknown(p.type);
        return f;
    }
    TensorFactAnalysis analyze(std::size_t i, const EntryFactsMap& input) {
        const auto& f=module.functions[i];
        const auto e=input.find(f.name);
        if (e==input.end()) return cache.tensor_facts(i);
        const ModuleSummaries no_summaries; const FactSources no_sources;
        return compute_tensor_facts(f,cache.cfg(i),cache.dominators(i),cache.local_ssa(i),cache.slots(i),
            module.summaries?*module.summaries:no_summaries,module.fact_sources?*module.fact_sources:no_sources,params(*e->second));
    }
    EntryFactsMap collect(const EntryFactsMap& input) {
        sites.clear(); EntryFactsMap next;
        for(const auto& [name,e]:prior)if(e && e->origin==EntryFactOrigin::SpecializedCalls && index.contains(name))next.emplace(name,e);
        std::map<std::size_t,ValidatedEntryFacts> meets;
        std::set<std::size_t> bad;
        for(std::size_t caller=0;caller<module.functions.size();++caller) {
            work.take(size(module.functions[caller]));
            const auto facts=analyze(caller,input); const auto& f=module.functions[caller];const auto& cfg=cache.cfg(caller);
            for(BlockId b=0;b<f.blocks.size();++b) if(cfg.reachable[b]) for(std::uint32_t j=0;j<f.blocks[b].instructions.size();++j) {
                work.take();const auto* c=std::get_if<Call>(&f.blocks[b].instructions[j]);if(!c)continue;
                const auto ti=index.find(c->callee);if(ti==index.end()||!eligible(ti->second))continue;
                const auto target=ti->second;const auto& callee=module.functions[target];
                if(c->args.size()!=callee.parameters.size() || c->result!=callee.result) {bad.insert(target);unavailable(callee,"call arity/result contract unavailable");continue;}
                ValidatedEntryFacts entry;entry.declaration=*callee.declaration;entry.source_instance=instances[target];
                for(std::size_t p=0;p<c->args.size();++p) {
                    work.take();const auto& parameter=callee.parameters[p];
                    if(parameter.type.kind==TypeKind::Tensor)entry.tensors.emplace(p,argument(caller,{b,j},c->args[p],parameter,facts));
                    else if(!parameter.writable&&!c->args[p].writable_address&&is_integer_family_type(parameter.type)) {auto v=numeric(facts.scalar_at({b,j},c->args[p].value));if(v.known())entry.scalars.emplace(p,std::move(v));}
                }
                if(const auto* policies=facts.policy_at({b,j}))entry.policies=*policies;
                const auto it=meets.find(target);if(it==meets.end())meets.emplace(target,entry);else it->second=combine(it->second,entry);
                sites.push_back({caller,target,{b,j},std::move(entry)});
            }
        }
        for(auto& [i,e]:meets)if(!bad.contains(i)&&!open.contains(module.functions[i].name)) {e.fact_key=signature(e,true);next.emplace(module.functions[i].name,std::make_shared<const ValidatedEntryFacts>(std::move(e)));}
        return next;
    }
    std::shared_ptr<const HelperTrackingProof> fold(Function& fn,std::shared_ptr<const ValidatedEntryFacts> entry) {
        auto proof=prove_helper_tracking_folds(module,fn,std::move(entry),[&]{work.take();});
        for(const auto& replacement:proof->replacements())
            fn.blocks[replacement.at.block].instructions[replacement.at.index]=ConstantBool{replacement.out,false};
        return proof;
    }
    HelperSpecializationReport run() {
        if(!options.exact_binding){report.unavailable.push_back({{},"helper specialization requires exact binding"});return report;}
        try {
            catalog();bool converged=false;
            for(std::size_t round=0;round<options.maximum_rounds;++round){auto next=collect(entries);if(equal(entries,next)){entries=std::move(next);converged=true;break;}entries=std::move(next);}
            if(!converged)throw Exhausted{};
            std::size_t total{},base{};
            for(const auto& f:module.functions){const auto n=size(f);if(n>std::numeric_limits<std::size_t>::max()-total)throw Exhausted{};total+=n;if(!clone(f.name))base+=n;}
            const auto growth=base/2;if(growth>std::numeric_limits<std::size_t>::max()-base)throw Exhausted{};const auto maximum=base+growth;
            struct Edit {std::size_t caller;InstrRef at;std::string target;};std::vector<Edit> edits;
            std::vector<Function> added;
            std::vector<HelperCloneOriginReceipt> clone_origins;
            std::vector<HelperCallRetargetReceipt> call_origins;
            std::map<std::size_t,std::shared_ptr<const std::string>> caller_bodies;
            std::map<std::string,std::size_t,std::less<>> counts;
            std::map<std::string,std::string,std::less<>> reuse;
            for(const auto& [name,e]:prior)if(e&&e->origin==EntryFactOrigin::SpecializedCalls&&index.contains(name)){++counts[e->source_instance];reuse.emplace(e->fact_key,name);}
            std::map<std::pair<std::string,std::string>,std::vector<std::size_t>> groups;
            for(std::size_t i=0;i<sites.size();++i)groups[{instances[sites[i].target],signature(sites[i].entry,false)}].push_back(i);
            for(const auto& [group,rows]:groups){work.take();const auto& first=sites[rows.front()];const auto& source=module.functions[first.target];
                if(!useful(first.entry))continue;
                auto entry=first.entry;for(auto row:rows)entry=combine(entry,sites[row].entry);entry.origin=EntryFactOrigin::SpecializedCalls;entry.fact_key=signature(entry,true);
                const auto common=entries.find(source.name);
                if(common!=entries.end()&&signature(*common->second,true)==signature(entry,true))continue;
                std::string name;
                if(const auto old=reuse.find(entry.fact_key);old!=reuse.end())name=old->second;
                else {
                    const auto n=size(source);
                    if((n>options.small_instruction_limit&&!hot(source)) || counts[entry.source_instance]>=std::min<std::size_t>(4,options.maximum_clones) || n>maximum || total>maximum-n){++report.budget_fallbacks;continue;}
                    name="$spec."+platform::sha256_hex(entry.fact_key);
                    if(index.contains(name)){unavailable(source,"content-named clone collides with an unrelated function");continue;}
                    auto copy=source;copy.name=name;copy.entrypoint=false;copy.c_export_symbol.reset();copy.declaration->specialization_of=declaration_key(entry.declaration);
                    auto owned_entry=std::make_shared<const ValidatedEntryFacts>(entry);
                    work.take(n);const auto proof=fold(copy,owned_entry);report.folded_tracking+=proof->replacements().size();
                    clone_origins.push_back({name,source.name,proof});
                    added.push_back(std::move(copy));total+=n;++counts[entry.source_instance];reuse.emplace(entry.fact_key,name);
                    entries[name]=std::move(owned_entry);report.clones.push_back({name,source.name});
                }
                for(auto row:rows) {
                    const auto& site=sites[row];const auto& caller=module.functions[site.caller];
                    const auto before=std::get<Call>(caller.blocks[site.at.block].instructions[site.at.index]);
                    auto after=before;after.callee=name;
                    auto [body,inserted]=caller_bodies.try_emplace(site.caller);
                    if(inserted) {
                        work.take(size(caller));
                        body->second=std::make_shared<const std::string>(binding_origin_body_fields(caller));
                    }
                    call_origins.push_back({caller.name,source.name,name,body->second,instances[site.target],entries.at(name),site.at,before,std::move(after)});
                    edits.push_back({site.caller,site.at,name});
                }
            }
            // Original meet records are revalidatable metadata. Do not fold
            // the shared source body: later call-producing rewrites may weaken
            // the meet, while a clone keeps its explicit caller obligation.
            report.entries=entries;
            for(const auto& [name,e]:prior)if(e && !entries.contains(name))report.removed_entries.push_back(name);
            for(const auto& [name,e]:entries){const auto it=prior.find(name);if(it==prior.end()||!it->second||it->second->origin!=e->origin||signature(*it->second,true)!=e->fact_key)++report.updated_entries;}
            // Build owned origin receipts against the unchanged preimages.
            // The returned owner is published by PassManager after semantic
            // clone import; this prevents duplicate destination metadata.
            auto origin_result=prepare_helper_origins(module,added,clone_origins,call_origins);
            report.origin_owner=std::move(origin_result.snapshots);
            for(const auto& issue:origin_result.unavailable)report.unavailable.push_back({issue.lookup,
                "helper origin component unavailable ("+std::to_string(static_cast<unsigned>(issue.component))+":"+std::to_string(static_cast<unsigned>(issue.reason))+")"});
            // Preserve IR proofs only when the actual post-transaction source
            // and clone remain exactly equal, including all concrete ABI.
            for(auto& origin:report.clones) {
                const auto src=index.find(origin.source);
                const auto target=std::find_if(added.begin(),added.end(),[&](const auto& fn){return fn.name==origin.clone;});
                if(src==index.end() || target==added.end())continue;
                auto projected=module.functions[src->second];
                for(const auto& edit:edits)if(edit.caller==src->second)
                    std::get<Call>(projected.blocks[edit.at.block].instructions[edit.at.index]).callee=edit.target;
                origin.unchanged_body=helper_unchanged_body(projected,*target);
            }
            // Allocate before mutation. No source/reference retained across the
            // vector append; any analysis/allocation failure leaves input IR.
            if(added.size()>module.functions.max_size()-module.functions.size())throw std::length_error("specialization function count exhausted");
            module.functions.reserve(module.functions.size()+added.size());
            for(auto& edit:edits)std::get<Call>(module.functions[edit.caller].blocks[edit.at.block].instructions[edit.at.index]).callee.swap(edit.target);
            for(auto& f:added)module.functions.push_back(std::move(f));
            report.generated_clones=report.clones.size();report.rewritten_calls=edits.size();
        } catch(const Exhausted&) {
            report={};report.budget_fallbacks=1;report.unavailable.push_back({{},"helper specialization operation/round budget exhausted"});
        }
        report.operations=work.used;
        std::sort(report.unavailable.begin(),report.unavailable.end(),[](const auto& a,const auto& b){return std::tie(a.function,a.reason)<std::tie(b.function,b.reason);});
        report.unavailable.erase(std::unique(report.unavailable.begin(),report.unavailable.end(),[](const auto& a,const auto& b){return a.function==b.function&&a.reason==b.reason;}),report.unavailable.end());
        return std::move(report);
    }
};
} // namespace
HelperSpecializationReport HelperSpecialization::run(Module& module, AnalysisCache& cache, const EntryFactsMap& prior, SpecializationOptions options) {
    return Engine(module,cache,prior,options).run();
}
} // namespace quidra::optimizer
