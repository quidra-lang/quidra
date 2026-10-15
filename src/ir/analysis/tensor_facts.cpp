#include "ir/analysis/tensor_facts.hpp"
#include "ir/analysis/inline_scopes.hpp"
#include "ir/analysis/alias.hpp"
#include "quidra/abi/tensor_index.hpp"
#include "quidra/ir/visit.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>
#include <type_traits>

namespace quidra::ir::analysis {
namespace {
using namespace ir;
using namespace ir::analysis;
using namespace ir::facts;
using Key = TensorFactKey;
struct Env {
    std::map<Key, TensorFacts> tensors;
    std::map<Key, ShapeExtent> scalars;
    PolicyFacts policies;
    std::set<ValueId> invalidated_snapshots;
    std::map<std::string,ValueId,std::less<>> inline_bindings;
    bool operator==(const Env&) const = default;
};
TensorFacts unknown(const Type& type) {
    TensorFacts result;
    if (type.kind == TypeKind::Tensor && type.first) {
        result.type = Type::tensor(*type.first); result.has_type = true;
    }
    return result;
}
void shape_from_type(TensorFacts& fact, Type type) {
    fact.type = std::move(type); fact.has_type = fact.type.kind == TypeKind::Tensor;
    fact.extents.clear();
    for (long long axis = 0; axis < fact.type.length; ++axis)
        fact.extents.push_back(tensor_extent(fact.type, static_cast<std::size_t>(axis)));
    if (fact.type.length >= 0) {
        fact.minimum_rank = static_cast<std::size_t>(fact.type.length);
        fact.rank_extent = ShapeExtent(fact.type.length);
        fact.head_extents.clear(); fact.tail_extents.clear();
    }
}
TensorFacts overlay_shape(TensorFacts value, const TensorFacts& proof) {
    if (!value.has_type || !proof.has_type) return value;
    value.type.length = proof.type.length;
    value.type.tensor_known_shape_prefix = proof.extents;
    value.extents = proof.extents; value.head_extents = proof.head_extents;
    value.tail_extents = proof.tail_extents; value.minimum_rank = proof.minimum_rank;
    value.rank_extent = proof.rank_extent;
    return value;
}
Type shape_type(const TensorFacts& value) {
    auto type = value.type; type.tensor_known_shape_prefix = type.length<0 ? value.head_extents : value.extents; return type;
}
template<class T> const T* scoped(const std::map<Key,T>& incoming,
    const std::map<Key,std::vector<std::pair<std::uint32_t,T>>>& changes, Key key, std::uint32_t index) {
    if (const auto found = changes.find(key); found != changes.end()) {
        const auto after = std::upper_bound(found->second.begin(), found->second.end(), index,
            [](auto i, const auto& item) { return i < item.first; });
        if (after != found->second.begin()) return &std::prev(after)->second;
    }
    const auto found = incoming.find(key); return found == incoming.end() ? nullptr : &found->second;
}
std::optional<std::int64_t> integer(const std::string& spelling) {
    std::int64_t value{};
    const auto [end,error] = std::from_chars(spelling.data(), spelling.data()+spelling.size(), value);
    return error == std::errc{} && end == spelling.data()+spelling.size() ? std::optional{value} : std::nullopt;
}
class Engine {
    const Function& fn_; const Cfg& cfg_; const LocalSsa& ssa_; const Slots& slots_;
    const ModuleSummaries& summaries_; const FactSources& sources_; const ParamFacts& params_;
    const AliasInfo* aliases_;
    std::map<std::string,std::uint32_t,std::less<>> readonly_borrowed_parameters_;
    std::map<std::string,InlineSlotKind,std::less<>> inline_slots_;
    TensorFactAnalysis out_;
    std::map<std::uint32_t,TensorFacts> parameter_tensors_;
    std::map<std::uint32_t,ShapeExtent> parameter_scalars_, phi_scalars_;
    std::map<ValueId,std::int64_t> integers_;
    std::map<ValueId,std::vector<ShapeExtent>> arrays_;
    std::map<ValueId,ValueId> shape_arrays_;
    struct ShapeRead { ValueId tensor{}; std::optional<std::size_t> axis; };
    std::map<ValueId,ShapeRead> shape_reads_;
    std::vector<std::optional<Env>> exits_;
    std::map<BlockId,std::set<BlockId>> loops_;
    struct Origin { std::optional<BlockId> block; bool merge{}; };
    std::map<std::int64_t,Origin> symbol_origins_;
    std::map<std::tuple<std::uint8_t,std::uint32_t,BlockId,std::size_t>,ShapeExtent> symbols_;
    std::int64_t next_symbol_{1};
    bool mutable_arrays_{}, changed_{};
    ShapeExtent symbol(std::uint8_t kind, std::uint32_t id, BlockId block, std::size_t axis, bool merge=false) {
        const auto key=std::tuple{kind,id,block,axis};
        if (const auto found=symbols_.find(key); found!=symbols_.end()) return found->second;
        if (next_symbol_==std::numeric_limits<std::int64_t>::max()) throw std::overflow_error("tensor fact symbols exhausted");
        auto value=ShapeExtent::symbol(next_symbol_++);
        symbol_origins_.emplace(value.value,Origin{kind==0 ? std::nullopt : std::optional{block},merge});
        symbols_.emplace(key,value); return value;
    }
    void reserve(const ShapeExtent& extent, bool external=false) {
        auto add=[&](std::int64_t id) {
            if(external) symbol_origins_.try_emplace(id,Origin{std::nullopt,false});
            if(id>=next_symbol_) {
                if(id==std::numeric_limits<std::int64_t>::max()) throw std::overflow_error("tensor fact symbols exhausted");
                next_symbol_=id+1;
            }
        };
        if(extent.kind==ShapeExtent::Kind::Symbol) add(extent.value);
        for(const auto id:extent.symbols) add(id);
        for(const auto& child:extent.operands) reserve(child,external);
        for(const auto& [atom,children]:extent.atom_operands) { (void)atom; for(const auto& child:children) reserve(child,external); }
    }
    void reserve_type(const Type& type) {
        for(const auto& extent:type.tensor_known_shape_prefix)reserve(extent);
        if(type.first)reserve_type(*type.first);
        for(const auto& child:type.parameters)reserve_type(child);
        for(const auto& child:type.cases)reserve_type(child);
    }
    bool admissible(const ShapeExtent& extent, BlockId header) const {
        const auto loop=loops_.find(header); if(loop==loops_.end()) return true;
        const auto allowed=[&](std::int64_t id) {
            const auto origin=symbol_origins_.find(id);
            if(origin==symbol_origins_.end()) return false;
            return !origin->second.block || !loop->second.contains(*origin->second.block) ||
                (origin->second.merge && *origin->second.block==header);
        };
        if(extent.kind==ShapeExtent::Kind::Symbol && !allowed(extent.value)) return false;
        return std::all_of(extent.symbols.begin(),extent.symbols.end(),allowed);
    }
    template<class Map,class T> void put(Map& map, typename Map::key_type key, T value) {
        const auto found=map.find(key);
        if(found==map.end() || !(found->second==value)) { map.insert_or_assign(key,std::move(value)); changed_=true; }
    }
    TensorFacts tensor(ValueId value, const Env& env) const {
        const auto found=out_.values.find(value); if(found==out_.values.end()) return {};
        auto result=found->second;
        if(const auto proof=env.tensors.find({Key::Value,value});proof!=env.tensors.end()) result=proof->second;
        if(const auto lineage=out_.lineage.find(value);lineage!=out_.lineage.end())
            if(const auto proof=env.tensors.find(lineage->second);proof!=env.tensors.end()) result=overlay_shape(result,proof->second);
        if(const auto proof=env.tensors.find({Key::Value,value});proof!=env.tensors.end()) result=overlay_shape(result,proof->second);
        return result;
    }
    ShapeExtent scalar(ValueId value, const Env& env) const {
        if(const auto proof=env.scalars.find({Key::Value,value});proof!=env.scalars.end()) return proof->second;
        if(const auto lineage=out_.lineage.find(value);lineage!=out_.lineage.end())
            if(const auto proof=env.scalars.find(lineage->second);proof!=env.scalars.end()) return proof->second;
        const auto found=out_.scalars.find(value); return found==out_.scalars.end() ? ShapeExtent{} : found->second;
    }
    static std::optional<Key> key(const ReachingDef& def) {
        if(!def.available) return {};
        if(def.kind==ReachingDef::Param) return Key{Key::Parameter,def.parameter};
        if(def.kind==ReachingDef::Store) return Key{Key::Value,def.value};
        if(def.kind==ReachingDef::Phi) return Key{Key::Phi,def.phi.value};
        return {};
    }
    std::optional<TensorFacts> from(const ReachingDef& def, const Env& env, const Type& type) const {
        if(!def.available || def.kind==ReachingDef::Undefined || def.kind==ReachingDef::Empty) return unknown(type);
        const auto k=key(def); if(!k) return unknown(type);
        if(const auto proof=env.tensors.find(*k);proof!=env.tensors.end()) return proof->second;
        if(k->kind==Key::Parameter) {
            const auto found=parameter_tensors_.find(k->id); return found==parameter_tensors_.end() ? std::optional{unknown(type)} : std::optional{found->second};
        }
        const auto& map=k->kind==Key::Phi ? out_.phis : out_.values;
        const auto found=map.find(k->id); return found==map.end() ? std::nullopt : std::optional{found->second};
    }
    std::optional<ShapeExtent> scalar_from(const ReachingDef& def, const Env& env) const {
        if(!def.available || def.kind==ReachingDef::Undefined || def.kind==ReachingDef::Empty) return ShapeExtent{};
        const auto k=key(def); if(!k) return ShapeExtent{};
        if(const auto proof=env.scalars.find(*k);proof!=env.scalars.end()) return proof->second;
        const auto& map=k->kind==Key::Parameter ? parameter_scalars_ : k->kind==Key::Phi ? phi_scalars_ : out_.scalars;
        const auto found=map.find(k->id); return found==map.end() ? std::nullopt : std::optional{found->second};
    }
    TensorFacts meet(std::span<const TensorFacts> values, BlockId block, Key k) {
        if(values.empty()) return {};
        auto result=values.front();
        std::vector<Type> types;
        for(const auto& value:values) {
            types.push_back(value.has_type ? shape_type(value) : Type::simple(TypeKind::Invalid));
            result.has_type &= value.has_type;
            if(result.device!=value.device) result.device={};
            if(result.contiguous!=value.contiguous) result.contiguous=Tri::Unknown;
            if(result.tracked!=value.tracked) result.tracked=Tri::Unknown;
            if(result.unique!=value.unique) result.unique=Tri::Unknown;
            if(result.fully_initialized!=value.fully_initialized) result.fully_initialized=Tri::Unknown;
            result.owns_storage &= value.owns_storage;
            result.minimum_rank=std::min(result.minimum_rank,value.minimum_rank);
            if(!same_extent(result.rank_extent,value.rank_extent)) result.rank_extent={};
        }
        if(!result.has_type) return {};
        const auto merge=[&](std::size_t axis) { return symbol(3+static_cast<std::uint8_t>(k.kind),k.id,block,axis,true); };
        auto type=merge_tensor_shapes(shape_type(result),types,merge,[&](const auto& extent){return admissible(extent,block);});
        result.type=std::move(type); result.extents=result.type.tensor_known_shape_prefix;
        if(result.type.length>=0) {
            result.minimum_rank=static_cast<std::size_t>(result.type.length); result.rank_extent=ShapeExtent(result.type.length);
            result.head_extents.clear(); result.tail_extents.clear();
        } else {
            auto pattern=[&](bool tail) {
                std::size_t count=std::numeric_limits<std::size_t>::max();
                for(const auto& value:values) count=std::min(count,tail?value.tail_extents.size():value.head_extents.size());
                std::vector<ShapeExtent> extents;
                for(std::size_t axis=0;axis<count;++axis) {
                    auto extent=tail?values.front().tail_extents[values.front().tail_extents.size()-count+axis]:values.front().head_extents[axis];
                    bool equal=extent.kind!=ShapeExtent::Kind::Unknown && admissible(extent,block);
                    for(const auto& value:values) {
                        const auto& current=tail?value.tail_extents[value.tail_extents.size()-count+axis]:value.head_extents[axis];
                        equal &= same_extent(extent,current) && admissible(current,block);
                    }
                    extents.push_back(equal?extent:merge((tail?std::numeric_limits<std::size_t>::max()/2:0)+axis));
                }
                return extents;
            };
            result.head_extents=pattern(false); result.tail_extents=pattern(true);
        }
        return result;
    }
    Env entry(BlockId block) {
        std::vector<const Env*> inputs; Env external; external.policies=params_.policies;
        for(const auto& [id,fact]:parameter_tensors_) external.tensors.emplace(Key{Key::Parameter,id},fact);
        if(block==0) inputs.push_back(&external);
        for(const auto pred:cfg_.pred[block]) if(cfg_.reachable[pred] && exits_[pred]) inputs.push_back(&*exits_[pred]);
        if(inputs.empty()) return {};
        auto env=*inputs.front();
        for(const auto* input:inputs)env.invalidated_snapshots.insert(input->invalidated_snapshots.begin(),input->invalidated_snapshots.end());
        for(auto it=env.tensors.begin();it!=env.tensors.end();) {
            std::vector<TensorFacts> values; bool all=true;
            for(const auto* input:inputs) { const auto found=input->tensors.find(it->first); if(found==input->tensors.end()){all=false;break;} values.push_back(found->second); }
            if(!all) it=env.tensors.erase(it); else {it->second=meet(values,block,it->first);++it;}
        }
        for(auto it=env.scalars.begin();it!=env.scalars.end();) {
            bool all=true;
            for(const auto* input:inputs) { const auto found=input->scalars.find(it->first); all &= found!=input->scalars.end() && same_extent(found->second,it->second) && admissible(found->second,block); }
            if(!all) it=env.scalars.erase(it); else ++it;
        }
        for(auto it=env.policies.begin();it!=env.policies.end();) {
            bool all=true;
            for(const auto* input:inputs) { const auto found=input->policies.find(it->first); all &= found!=input->policies.end() && found->second==it->second; }
            if(!all) it=env.policies.erase(it); else ++it;
        }
        for(auto it=env.inline_bindings.begin();it!=env.inline_bindings.end();) {
            bool all=true;for(const auto* input:inputs){const auto found=input->inline_bindings.find(it->first);all &= found!=input->inline_bindings.end() && found->second==it->second;}
            if(!all)it=env.inline_bindings.erase(it);else ++it;
        }
        return env;
    }
    void phi(BlockId block) {
        for(const auto& value:ssa_.phis) if(value.block==block) {
            const auto& type=slots_.type.at(value.local);
            std::vector<TensorFacts> tensors; std::vector<ShapeExtent> scalars;
            auto add=[&](const ReachingDef& def,const Env& env) {
                if(type.kind==TypeKind::Tensor) {if(auto fact=from(def,env,type)) tensors.push_back(*fact);}
                else if(is_integer_family_type(type)) {if(auto fact=scalar_from(def,env)) scalars.push_back(*fact);}
            };
            if(value.entry) add(*value.entry,{});
            for(const auto& [pred,def]:value.incoming) if(cfg_.reachable[pred] && exits_[pred]) add(def,*exits_[pred]);
            if(!tensors.empty()) {auto result=meet(tensors,block,{Key::Phi,value.id.value});result.owns_storage=false;result.unique=Tri::Unknown;put(out_.phis,value.id.value,std::move(result));}
            if(!scalars.empty()) {
                auto result=scalars.front();bool equal=result.kind!=ShapeExtent::Kind::Unknown;
                for(const auto& extent:scalars) equal &= same_extent(result,extent) && admissible(extent,block);
                if(!equal) result=symbol(6,value.id.value,block,0,true);
                put(phi_scalars_,value.id.value,std::move(result));
            }
        }
    }
    void refine(ValueId value, TensorFacts proof, Env& env, InstrRef at) {
        env.tensors.insert_or_assign(Key{Key::Value,value},proof);
        out_.scopes[at.block].changes[{Key::Value,value}].emplace_back(at.index+1,proof);
        if(const auto lineage=out_.lineage.find(value);lineage!=out_.lineage.end()) {
            env.tensors.insert_or_assign(lineage->second,proof);
            out_.scopes[at.block].changes[lineage->second].emplace_back(at.index+1,std::move(proof));
        }
    }
    ShapeExtent axis(ValueId value,std::size_t index,const Env& env,BlockId block) {
        const auto fact=tensor(value,env);
        if(index<fact.extents.size()) return fact.extents[index];
        if(index<fact.head_extents.size()) return fact.head_extents[index];
        const auto lineage=out_.lineage.find(value);
        const auto k=lineage==out_.lineage.end()?Key{Key::Value,value}:lineage->second;
        if(k.kind==Key::Parameter) return symbol(0,k.id,0,index+1);
        return symbol(10+static_cast<std::uint8_t>(k.kind),k.id,block,index);
    }
    void guard(const ShapedConstraintCheck& check,Env& env,InstrRef at) {
        if(check.kind!=TypeKind::Tensor) return;
        auto fact=tensor(check.value,env);if(!fact.has_type) return;
        std::vector<ShapeExtent> expected;
        for(std::size_t i=0;i<check.extents.size();++i) expected.push_back(check.extents[i]?scalar(*check.extents[i],env):ShapeExtent{});
        if(!check.ellipsis) {
            fact.type.length=static_cast<long long>(expected.size());fact.extents.resize(expected.size());
            for(std::size_t i=0;i<expected.size();++i) fact.extents[i]=expected[i].kind!=ShapeExtent::Kind::Unknown?expected[i]:axis(check.value,i,env,at.block);
            fact.type.tensor_known_shape_prefix=fact.extents;fact.minimum_rank=expected.size();fact.rank_extent=ShapeExtent(fact.type.length);
            fact.head_extents.clear();fact.tail_extents.clear();
        } else {
            if(*check.ellipsis>expected.size()) throw std::invalid_argument("tensor shape fact ellipsis is out of range");
            fact.minimum_rank=std::max(fact.minimum_rank,expected.size());
            if(fact.type.length>=0) {
                const auto rank=static_cast<std::size_t>(fact.type.length);
                if(rank<expected.size()) {fact=unknown(fact.type);fact.minimum_rank=expected.size();}
                else {
                    fact.extents.resize(rank);
                    for(std::size_t i=0;i<expected.size();++i) if(expected[i].kind!=ShapeExtent::Kind::Unknown) {
                        const auto physical=i<*check.ellipsis?i:rank-(expected.size()-i);fact.extents[physical]=expected[i];
                    }
                    fact.type.tensor_known_shape_prefix=fact.extents;
                }
            }
            if(fact.type.length<0) {
                fact.head_extents.assign(expected.begin(),expected.begin()+static_cast<std::ptrdiff_t>(*check.ellipsis));
                fact.tail_extents.assign(expected.begin()+static_cast<std::ptrdiff_t>(*check.ellipsis),expected.end());
            }
        }
        refine(check.value,std::move(fact),env,at);
    }
    void guard(const ExtentEqualCheck& check,Env& env,InstrRef at) {
        auto extent=scalar(check.expected,env); if(extent.kind==ShapeExtent::Kind::Unknown) extent=symbol(20,check.expected,at.block,0);
        env.scalars.insert_or_assign(Key{Key::Value,check.actual},extent);
        out_.scopes[at.block].scalar_changes[{Key::Value,check.actual}].emplace_back(at.index+1,extent);
        if(const auto lineage=out_.lineage.find(check.actual);lineage!=out_.lineage.end()) {
            env.scalars.insert_or_assign(lineage->second,extent);out_.scopes[at.block].scalar_changes[lineage->second].emplace_back(at.index+1,extent);
        }
        if(const auto read=shape_reads_.find(check.actual);read!=shape_reads_.end() && !env.invalidated_snapshots.contains(check.actual)) {
            auto fact=tensor(read->second.tensor,env);if(!fact.has_type) return;
            if(read->second.axis) {
                const auto index=*read->second.axis;
                if(fact.type.length>=0 && index<static_cast<std::size_t>(fact.type.length)) {fact.extents.resize(static_cast<std::size_t>(fact.type.length));fact.extents[index]=extent;fact.type.tensor_known_shape_prefix=fact.extents;}
                else {fact.head_extents.resize(std::max(fact.head_extents.size(),index+1));fact.head_extents[index]=extent;fact.minimum_rank=std::max(fact.minimum_rank,index+1);}
            } else if(const auto rank=extent.known()) {
                fact.type.length=*rank;fact.minimum_rank=static_cast<std::size_t>(*rank);fact.rank_extent=extent;
                fact.extents.resize(static_cast<std::size_t>(*rank));
                for(std::size_t i=0;i<fact.extents.size();++i) {
                    if(i<fact.head_extents.size())fact.extents[i]=fact.head_extents[i];
                    else if(fact.extents.size()>=fact.tail_extents.size() && i>=fact.extents.size()-fact.tail_extents.size())fact.extents[i]=fact.tail_extents[i-(fact.extents.size()-fact.tail_extents.size())];
                    else fact.extents[i]=axis(read->second.tensor,i,env,at.block);
                }
                fact.type.tensor_known_shape_prefix=fact.extents;fact.head_extents.clear();fact.tail_extents.clear();
            } else fact.rank_extent=extent;
            refine(read->second.tensor,std::move(fact),env,at);
        }
    }
    // Descriptor snapshots are flow-sensitive. A shape-only lineage is
    // insufficient to retain tracking, layout or device across a call.
    void invalidate(Env& env,InstrRef at,const std::vector<ValueId>* targets=nullptr) {
        std::set<StorageClassId> classes;
        bool selective=targets && aliases_ && !targets->empty();
        if(selective)for(const auto value:*targets) {
            const auto* storage=aliases_->find(value);
            if(!storage || !storage->storage_available || storage->classes.empty()){selective=false;break;}
            classes.insert(storage->classes.begin(),storage->classes.end());
        }
        auto affected=[&](Key k) {
            if(!selective)return true;
            StorageProjection projection;
            if(k.kind==Key::Value) {
                const auto* value=aliases_->find(k.id);
                if(!value)return true;
                projection={value->classes,value->storage_available};
            } else {
                ReachingDef def;def.available=true;
                if(k.kind==Key::Parameter){def.kind=ReachingDef::Param;def.parameter=k.id;}
                else {def.kind=ReachingDef::Phi;def.phi.value=k.id;}
                projection=aliases_->storage_of(def);
            }
            return !projection.storage_available || std::any_of(projection.classes.begin(),projection.classes.end(),[&](auto id){return classes.contains(id);});
        };
        for(auto& [k,fact]:env.tensors)if(affected(k)) {
            fact=unknown(fact.type);
            out_.scopes[at.block].changes[k].emplace_back(at.index+1,fact);
        }
        // Numeric values from old TensorShape snapshots are valid scalars,
        // but comparing them cannot validate a subsequently changed tensor.
        for(const auto& [value,tensor]:shape_arrays_) {
            (void)tensor;const auto definition=out_.definitions.find(value);
            if(definition!=out_.definitions.end() && out_.dominance.dominates(definition->second,at))env.invalidated_snapshots.insert(value);
        }
        for(const auto& [value,read]:shape_reads_) {
            (void)read;const auto definition=out_.definitions.find(value);
            if(definition!=out_.definitions.end() && out_.dominance.dominates(definition->second,at))env.invalidated_snapshots.insert(value);
        }
    }
    void call_descriptors(const Call& call,Env& env,InstrRef at) {
        const auto* summary=summaries_.find(call.callee);
        if(!summary || !summary->semantic || !summary->effects_complete || !summary->descriptor_complete ||
           (summary->effects & effect_bit::Opaque)) {invalidate(env,at);return;}
        const auto& semantic=*summary->semantic;
        if(semantic.mutates_ambient || !semantic.reference_shared_writes.empty() || !semantic.reference_shared_write_origins.empty() ||
           std::any_of(call.args.begin(),call.args.end(),[](const auto& arg){return arg.writable_address.has_value();})) {
            invalidate(env,at);return;
        }
        std::vector<ValueId> targets;
        if(summary->semantic_parameter_for_ir_parameter.size()!=call.args.size()) {invalidate(env,at);return;}
        for(std::size_t i=0;i<call.args.size();++i) {
            const auto mapped=summary->semantic_parameter_for_ir_parameter[i];
            if(!mapped){if(summary->descriptor_effects){invalidate(env,at);return;}continue;}
            const auto parameter=*mapped;
            const bool exposed=parameter<semantic.exposed_writes.size() && !semantic.exposed_writes[parameter].empty();
            const bool shared=parameter<semantic.mutates_shared_of_parameter.size() && semantic.mutates_shared_of_parameter[parameter];
            const bool address=parameter<semantic.const_parameter_address_observed.size() && semantic.const_parameter_address_observed[parameter];
            if(summary->descriptor_effects || exposed || shared || address || (semantic.mutates_receiver && parameter==0))targets.push_back(call.args[i].value);
        }
        if(!targets.empty())invalidate(env,at,&targets);
        else if(summary->descriptor_effects || semantic.mutates_receiver)invalidate(env,at);
    }
    void call_policy(const Call& call,Env& env) const {
        bool writes=true;
        if(const auto contract=sources_.policies.writes_policy.find(call.callee);contract!=sources_.policies.writes_policy.end()) writes=contract->second;
        else if(const auto* summary=summaries_.find(call.callee);summary && summary->semantic && summary->effects_complete &&
                !(summary->effects & effect_bit::Opaque)) writes=summary->semantic->mutates_ambient;
        if(const auto* summary=summaries_.find(call.callee);summary && (summary->effects & effect_bit::WritesPolicy)) writes=true;
        if(writes) env.policies.clear();
        if(const auto setter=sources_.policies.setters.find(call.callee);setter!=sources_.policies.setters.end())
            for(const auto& assignment:setter->second) env.policies[assignment.identity]=assignment.value;
    }
    void instruction(const Instruction& instruction,Env& env,InstrRef at);
public:
    Engine(const Function& fn,const Cfg& cfg,const DomTree& dom,const LocalSsa& ssa,const Slots& slots,
           const ModuleSummaries& summaries,const FactSources& sources,const ParamFacts& params,const AliasInfo* aliases)
        :fn_(fn),cfg_(cfg),ssa_(ssa),slots_(slots),summaries_(summaries),sources_(sources),params_(params),aliases_(aliases),exits_(fn.blocks.size()) {
        if(cfg.reachable.size()!=fn.blocks.size()) throw std::invalid_argument("tensor facts CFG dimensions disagree");
        out_.dominance=dom;out_.reachable=cfg.reachable;out_.scopes.resize(fn.blocks.size());
        reserve_type(fn.result);
        for(const auto& parameter:fn.parameters)reserve_type(parameter.type);
        for(const auto& block:fn.blocks)for(const auto& instruction:block.instructions)std::visit([&](const auto& node){
            if constexpr(requires{node.type;})reserve_type(node.type);
            if constexpr(requires{node.result.kind;})reserve_type(node.result);
            if constexpr(requires{node.result_type;})reserve_type(node.result_type);
            if constexpr(requires{node.target_type;})reserve_type(node.target_type);
            if constexpr(requires{node.source_type;})reserve_type(node.source_type);
            if constexpr(requires{node.field_type;})reserve_type(node.field_type);
        },instruction);
        for(const auto& [index,fact]:params.tensors){(void)index;for(const auto& extent:fact.extents)reserve(extent,true);for(const auto& extent:fact.head_extents)reserve(extent,true);for(const auto& extent:fact.tail_extents)reserve(extent,true);reserve(fact.rank_extent,true);}
        for(const auto& [index,extent]:params.scalars){(void)index;reserve(extent,true);}
        for(const auto& scope:fn.inline_scopes)for(const auto& slot:scope.slots)inline_slots_.emplace(slot.name,slot.kind);
        struct BindingWrites final : OperandVisitor {
            std::set<std::string> writes;
            void local(std::string_view name,LocalRole role) override {
                if(role==LocalRole::Write || role==LocalRole::ReadWrite || role==LocalRole::ReferenceWrite || role==LocalRole::Address)
                    writes.emplace(name);
            }
        } binding_writes;
        for(const auto& block:fn.blocks)for(const auto& instruction:block.instructions)visit_operands(instruction,binding_writes);
        for(std::size_t i=0;i<fn.parameters.size();++i) {
            if(i>std::numeric_limits<std::uint32_t>::max()) throw std::overflow_error("tensor fact parameter index exhausted");
            const auto& parameter=fn.parameters[i];
            if(const auto f=slots_.facts.find(parameter.name);f!=slots_.facts.end()) {
                const auto& facts=f->second;
                if(parameter.borrowed && !parameter.writable && !facts.writable_parameter && !facts.borrowed_local &&
                   !facts.address_taken && !facts.reference_bound && !facts.reference_binding && !facts.opaque_access &&
                   !binding_writes.writes.contains(parameter.name))
                    readonly_borrowed_parameters_.emplace(parameter.name,static_cast<std::uint32_t>(i));
            }
            const auto index=static_cast<std::uint32_t>(i);const auto& type=fn.parameters[i].type;
            if(type.kind==TypeKind::Tensor) {auto fact=unknown(type);fact.rank_extent=symbol(0,index,0,0);if(const auto found=params.tensors.find(i);found!=params.tensors.end())fact=found->second;parameter_tensors_[index]=std::move(fact);}
            if(is_integer_family_type(type)) {const auto found=params.scalars.find(i);parameter_scalars_[index]=found==params.scalars.end()?symbol(0,index,0,0):found->second;}
        }
        for(BlockId block=0;block<fn.blocks.size();++block) {
            for(std::uint32_t index=0;index<fn.blocks[block].instructions.size();++index) std::visit([&](const auto& node){
                using T=std::decay_t<decltype(node)>;
                if constexpr(requires{node.out;}) if(node.out) if(!out_.definitions.emplace(node.out,InstrRef{block,index}).second) throw std::invalid_argument("tensor fact value has duplicate definitions");
                if constexpr(std::is_same_v<T,ArraySet> || std::is_same_v<T,ArraySliceSet> || std::is_same_v<T,StoreAddress> || std::is_same_v<T,StoreReference>) mutable_arrays_=true;
                if constexpr(std::is_same_v<T,Call>) for(const auto& arg:node.args) if(arg.writable_address) mutable_arrays_=true;
            },fn.blocks[block].instructions[index]);
            for(const auto succ:cfg.succ[block]) if(dom.dominates(succ,block)) loops_[succ].insert(block);
        }
        for(auto& [header,body]:loops_) {
            std::vector<BlockId> pending(body.begin(),body.end());body.insert(header);
            while(!pending.empty()) {const auto block=pending.back();pending.pop_back();for(const auto pred:cfg.pred[block])if(cfg.reachable[pred] && body.insert(pred).second && pred!=header)pending.push_back(pred);}
        }
    }
    TensorFactAnalysis run() {
        bool again=true;
        while(again) {
            changed_=false;
            for(const auto block:cfg_.rpo) {
                auto env=entry(block);phi(block);
                for(const auto& value:ssa_.phis)if(value.block==block)if(const auto fact=out_.phis.find(value.id.value);fact!=out_.phis.end())
                    env.tensors.insert_or_assign(Key{Key::Phi,value.id.value},fact->second);
                auto& scope=out_.scopes[block];scope.incoming=env.tensors;scope.scalar_incoming=env.scalars;
                scope.changes.clear();scope.scalar_changes.clear();scope.policies_before.clear();
                for(std::uint32_t index=0;index<fn_.blocks[block].instructions.size();++index) {
                    scope.policies_before.push_back(env.policies);instruction(fn_.blocks[block].instructions[index],env,{block,index});
                    // A definition on a later loop iteration creates its own
                    // current descriptor snapshot, rather than reviving an
                    // earlier iteration's killed facts.
                    std::visit([&](const auto& node){if constexpr(requires{node.out;})if(node.out)
                        if(const auto fact=out_.values.find(node.out);fact!=out_.values.end()) {
                            env.tensors.insert_or_assign(Key{Key::Value,node.out},fact->second);
                            scope.changes[{Key::Value,node.out}].emplace_back(index+1,fact->second);
                        }},fn_.blocks[block].instructions[index]);
                }
                scope.policies_before.push_back(env.policies);
                if(!exits_[block] || *exits_[block]!=env){exits_[block]=std::move(env);changed_=true;}
            }
            again=changed_;
        }
        return std::move(out_);
    }
};
void Engine::instruction(const Instruction& instruction,Env& env,InstrRef at) {
    const InlineScope* boundary=nullptr;bool cleanup=false;
    if(const auto* n=std::get_if<InlineScopeBegin>(&instruction))boundary=&inline_scope(fn_,n->scope);
    if(const auto* n=std::get_if<InlineScopeCleanup>(&instruction)){boundary=&inline_scope(fn_,n->scope);cleanup=true;}
    if(boundary){
        for(const auto& slot:boundary->slots)env.inline_bindings.erase(slot.name);
        if(cleanup && inline_cleanup_unknown(fn_,*boundary)){invalidate(env,at);env.policies.clear();}
        return;
    }
    bool unknown_drop=false;
    std::visit([&](const auto& n){using T=std::decay_t<decltype(n)>;
        if constexpr(std::is_same_v<T,Release> || std::is_same_v<T,StoreAddress> || std::is_same_v<T,StoreReference>)unknown_drop=unknown_cleanup_type(n.type);
        else if constexpr(std::is_same_v<T,StoreLocal>)unknown_drop=!n.borrowed && !n.replace_without_release && unknown_cleanup_type(n.type);
        else if constexpr(std::is_same_v<T,FieldSet>)unknown_drop=!n.replace_without_release && unknown_cleanup_type(n.field_type);
        else if constexpr(std::is_same_v<T,ArraySet>)unknown_drop=unknown_cleanup_type(n.element_type);
        else if constexpr(std::is_same_v<T,ArraySliceSet>)unknown_drop=unknown_cleanup_type(n.array_type);
        else if constexpr(std::is_same_v<T,Unpin>)unknown_drop=unknown_cleanup_type(cleanup_operand_type(fn_,n.value));
        else if constexpr(std::is_same_v<T,BindReference>){const auto t=slots_.type.find(n.name);unknown_drop=t==slots_.type.end() || unknown_cleanup_type(t->second);}
        else if constexpr(std::is_same_v<T,BindFieldReference> || std::is_same_v<T,ParallelBlock>)unknown_drop=true;
    },instruction);
    if(unknown_drop){invalidate(env,at);env.policies.clear();}
    if(const auto* node=std::get_if<StoreLocal>(&instruction)){
        if(const auto slot=inline_slots_.find(node->name);slot!=inline_slots_.end() && slot->second!=InlineSlotKind::RawAddress && slot->second!=InlineSlotKind::PinnedReference)
            env.inline_bindings.insert_or_assign(node->name,node->value);
        return;
    }
    if(const auto* node=std::get_if<InlineBindAddress>(&instruction)){env.inline_bindings.erase(node->name);return;}
    if(const auto* node=std::get_if<ShapedConstraintCheck>(&instruction)){guard(*node,env,at);return;}
    if(const auto* node=std::get_if<ExtentEqualCheck>(&instruction)){guard(*node,env,at);return;}
    if(const auto* node=std::get_if<ConstantInt>(&instruction)) {
        if(const auto value=integer(node->value)) {integers_[node->out]=*value;put(out_.scalars,node->out,ShapeExtent(*value));}
        else put(out_.scalars,node->out,symbol(20,node->out,at.block,0));
        return;
    }
    if(const auto* node=std::get_if<ConstantExact>(&instruction)) {
        // Bare integer literals lower to ConstantExact. Only an integral,
        // representable value supplies an extent; real/fractional and larger
        // exact constants retain unknown facts.
        if(node->type.kind==TypeKind::Int || node->type.kind==TypeKind::Nat)
            if(const auto value=integer(node->spelling);value && (node->type.kind!=TypeKind::Nat || *value>=0)) {
                integers_[node->out]=*value;put(out_.scalars,node->out,ShapeExtent(*value));
            }
        return;
    }
    if(const auto* node=std::get_if<LoadLocal>(&instruction)) {
        const auto source=ssa_.load_source.find(node->out);
        auto def=source==ssa_.load_source.end()?ReachingDef{}:source->second;
        if(!def.available)if(const auto binding=env.inline_bindings.find(node->name);binding!=env.inline_bindings.end()){
            def.kind=ReachingDef::Store;def.value=binding->second;def.available=true;
        }
        // Formal value provenance is independent of storage promotion. An
        // immutable borrowed parameter binding still names its exact input.
        if(!def.available)if(const auto p=readonly_borrowed_parameters_.find(node->name);p!=readonly_borrowed_parameters_.end()) {
            def.kind=ReachingDef::Param;def.parameter=p->second;def.available=true;
        }
        if(const auto k=key(def)) put(out_.lineage,node->out,*k);
        if(node->type.kind==TypeKind::Tensor) {
            if(auto fact=from(def,env,node->type)) {fact->owns_storage=false;fact->unique=Tri::Unknown;put(out_.values,node->out,std::move(*fact));}
        } else if(is_integer_family_type(node->type)) {
            if(auto extent=scalar_from(def,env)) put(out_.scalars,node->out,std::move(*extent));
        }
        return;
    }
    if(const auto* node=std::get_if<Clone>(&instruction)) {
        if(node->type.kind==TypeKind::Tensor && out_.values.contains(node->value)) {
            auto fact=tensor(node->value,env);fact.owns_storage=false;fact.unique=Tri::False;
            put(out_.values,node->out,std::move(fact));
            const auto source=out_.lineage.find(node->value);put(out_.lineage,node->out,source==out_.lineage.end()?Key{Key::Value,node->value}:source->second);
        }
        if(is_integer_family_type(node->type)) put(out_.scalars,node->out,scalar(node->value,env));
        if(const auto values=arrays_.find(node->value);values!=arrays_.end()) put(arrays_,node->out,values->second);
        if(const auto values=shape_arrays_.find(node->value);values!=shape_arrays_.end()) {put(shape_arrays_,node->out,values->second);if(env.invalidated_snapshots.contains(node->value))env.invalidated_snapshots.insert(node->out);else env.invalidated_snapshots.erase(node->out);}
        return;
    }
    if(const auto* node=std::get_if<NumericConvert>(&instruction)) {
        if(is_integer_family_type(node->source_type) && is_integer_family_type(node->target_type)) {
            put(out_.scalars,node->out,scalar(node->value,env));
            if(const auto value=integers_.find(node->value);value!=integers_.end()) integers_[node->out]=value->second;
            if(const auto read=shape_reads_.find(node->value);read!=shape_reads_.end()) {shape_reads_[node->out]=read->second;if(env.invalidated_snapshots.contains(node->value))env.invalidated_snapshots.insert(node->out);else env.invalidated_snapshots.erase(node->out);}
        }
        return;
    }
    if(const auto* node=std::get_if<ArrayNumericCast>(&instruction)) {
        if(node->source_type.first && node->target_type.first && is_integer_family_type(*node->source_type.first) && is_integer_family_type(*node->target_type.first)) {
            if(const auto values=arrays_.find(node->array);values!=arrays_.end()) put(arrays_,node->out,values->second);
            if(const auto values=shape_arrays_.find(node->array);values!=shape_arrays_.end()) {put(shape_arrays_,node->out,values->second);if(env.invalidated_snapshots.contains(node->array))env.invalidated_snapshots.insert(node->out);else env.invalidated_snapshots.erase(node->out);}
        }
        return;
    }
    if(const auto* node=std::get_if<ArrayMake>(&instruction)) {
        if(!mutable_arrays_ && node->type.first && is_integer_family_type(*node->type.first)) {
            std::vector<ShapeExtent> values;for(const auto value:node->elements)values.push_back(scalar(value,env));
            put(arrays_,node->out,std::move(values));
        }
        return;
    }
    if(const auto* node=std::get_if<TensorShape>(&instruction)) {
        if(!mutable_arrays_) {put(shape_arrays_,node->out,node->tensor);env.invalidated_snapshots.erase(node->out);}
        return;
    }
    if(const auto* node=std::get_if<ArrayGet>(&instruction)) {
        if(!is_integer_family_type(node->element_type))return;
        const auto index=integers_.find(node->index);
        ShapeExtent extent;
        if(index!=integers_.end() && index->second>=0 && static_cast<std::uint64_t>(index->second)<std::numeric_limits<std::size_t>::max()) {
            const auto physical=static_cast<std::size_t>(index->second);
            if(const auto shape=shape_arrays_.find(node->array);shape!=shape_arrays_.end()) {
                extent=axis(shape->second,physical,env,at.block);shape_reads_[node->out]={shape->second,physical};
                if(env.invalidated_snapshots.contains(node->array))env.invalidated_snapshots.insert(node->out);else env.invalidated_snapshots.erase(node->out);
                auto fact=tensor(shape->second,env);
                if(fact.has_type && !env.invalidated_snapshots.contains(node->out)){fact.minimum_rank=std::max(fact.minimum_rank,physical+1);if(fact.type.length<0){fact.head_extents.resize(std::max(fact.head_extents.size(),physical+1));fact.head_extents[physical]=extent;}refine(shape->second,std::move(fact),env,at);}
            } else if(const auto values=arrays_.find(node->array);values!=arrays_.end() && physical<values->second.size()) extent=values->second[physical];
        }
        if(extent.kind==ShapeExtent::Kind::Unknown) extent=symbol(20,node->out,at.block,0);
        put(out_.scalars,node->out,std::move(extent));return;
    }
    if(const auto* node=std::get_if<ArrayLength>(&instruction)) {
        ShapeExtent extent;
        if(const auto shape=shape_arrays_.find(node->array);shape!=shape_arrays_.end()) {
            extent=tensor(shape->second,env).rank_extent;shape_reads_[node->out]={shape->second,{}};
            if(env.invalidated_snapshots.contains(node->array))env.invalidated_snapshots.insert(node->out);else env.invalidated_snapshots.erase(node->out);
        } else if(const auto values=arrays_.find(node->array);values!=arrays_.end()) extent=ShapeExtent(static_cast<long long>(values->second.size()));
        if(extent.kind==ShapeExtent::Kind::Unknown)extent=symbol(20,node->out,at.block,0);
        put(out_.scalars,node->out,std::move(extent));return;
    }
    if(const auto* node=std::get_if<Binary>(&instruction);node && node->result_type.kind!=TypeKind::Tensor) {
        if(is_integer_family_type(node->result_type)) put(out_.scalars,node->out,extent_arithmetic(node->op,scalar(node->left,env),scalar(node->right,env)));
        return;
    }
    if(const auto* node=std::get_if<Unary>(&instruction);node && node->type.kind!=TypeKind::Tensor) {
        if(is_integer_family_type(node->type)) put(out_.scalars,node->out,node->op=="-"?extent_arithmetic("-",ShapeExtent(0),scalar(node->operand,env)):scalar(node->operand,env));
        return;
    }
    if(const auto* node=std::get_if<Call>(&instruction)) {
        call_descriptors(*node,env,at);call_policy(*node,env);
        if(node->result.kind!=TypeKind::Tensor)return;
        auto fact=unknown(node->result);const auto* summary=summaries_.find(node->callee);
        if(summary && summary->result_complete && summary->result==ResultAlias::ParamDerived && summary->result_parameter && *summary->result_parameter<node->args.size()) {
            const auto& arg=node->args[*summary->result_parameter];
            if(!arg.writable_address && summary->descriptor_complete && !summary->descriptor_effects) fact=tensor(arg.value,env);
            fact.owns_storage=false;fact.unique=Tri::Unknown;
        } else if(summary && summary->result_complete && summary->result==ResultAlias::Fresh) fact.unique=Tri::True;
        TensorShapeTransfer transfer;transfer.operation=TensorShapeOp::CallOrigins;transfer.result=fact.type;
        transfer.fresh=[&](std::size_t axis){return symbol(21,node->out,at.block,axis);};
        if(const auto* source=sources_.find(node->callee)) {
            transfer.result_origins=source->result_extents;
            for(std::size_t i=0;i<source->semantic_parameter_for_ir_parameter.size();++i)if(const auto index=source->semantic_parameter_for_ir_parameter[i]) {
                if(*index==std::numeric_limits<std::size_t>::max())throw std::overflow_error("tensor fact semantic index exhausted");
                transfer.inputs.resize(std::max(transfer.inputs.size(),*index+1));transfer.scalar_inputs.resize(transfer.inputs.size());
                if(i<node->args.size() && !node->args[i].writable_address) {transfer.inputs[*index]=shape_type(tensor(node->args[i].value,env));transfer.scalar_inputs[*index]=scalar(node->args[i].value,env);}
            }
        }
        shape_from_type(fact,transfer_tensor_shape(transfer));put(out_.values,node->out,std::move(fact));return;
    }
    if(std::holds_alternative<IndirectCall>(instruction)){invalidate(env,at);env.policies.clear();}
    // Metadata operation selection; every shape computation uses the common
    // transfer table above the checker/CFG boundary.
    ValueId output{},input{};Type type;TensorShapeTransfer transfer;TensorFacts fact;
    bool matched=false,fresh_output=false;
    std::visit([&](const auto& node){
        using T=std::decay_t<decltype(node)>;
        if constexpr(std::is_same_v<T,TensorCreate>) {
            matched=true;output=node.out;type=node.type;fact.contiguous=Tri::True;fact.tracked=Tri::False;
            fact.unique=Tri::True;fact.owns_storage=true;fact.fully_initialized=node.fill_mode==tensor_fill_mode::uninitialized?Tri::False:Tri::True;
            fact.device={node.gpu?BackendKind::AnyGpu:BackendKind::Cpu,node.gpu?std::nullopt:std::optional<std::int64_t>{-1}};
            if(node.gpu)if(const auto value=integers_.find(*node.gpu);value!=integers_.end() && value->second>=0 && value->second<=std::numeric_limits<int>::max())fact.device.index=value->second;
            transfer.operation=TensorShapeOp::ExplicitShape;if(const auto shape=arrays_.find(node.shape);shape!=arrays_.end())transfer.explicit_shape=shape->second;
        } else if constexpr(std::is_same_v<T,TensorTransfer> || std::is_same_v<T,TensorReshape> || std::is_same_v<T,TensorTranspose> || std::is_same_v<T,TensorContiguous> || std::is_same_v<T,TensorGather> || std::is_same_v<T,TensorScatter> || std::is_same_v<T,TensorTrack> || std::is_same_v<T,TensorCast> || std::is_same_v<T,TensorIndex>) {
            if constexpr(std::is_same_v<T,TensorCast>) if(node.result_type.kind!=TypeKind::Tensor)return;
            matched=true;output=node.out;input=node.tensor;fact=tensor(input,env);fact.owns_storage=false;fact.unique=Tri::Unknown;
            if constexpr(std::is_same_v<T,TensorCast>)type=node.target_type;else type=node.type;
            if constexpr(std::is_same_v<T,TensorTransfer>) {
                fact.device={node.gpu?BackendKind::AnyGpu:BackendKind::Cpu,node.gpu?std::nullopt:std::optional<std::int64_t>{-1}};
                if(node.gpu)if(const auto value=integers_.find(*node.gpu);value!=integers_.end() && value->second>=0 && value->second<=std::numeric_limits<int>::max())fact.device.index=value->second;
                fact.contiguous=Tri::True;fact.owns_storage=true;
            }
            if constexpr(std::is_same_v<T,TensorContiguous>)fact.contiguous=Tri::True;
            if constexpr(std::is_same_v<T,TensorTrack>)fact.tracked=node.mode==tensor_track_mode::untrack?Tri::False:Tri::True;
            if constexpr(std::is_same_v<T,TensorCast>) {fact.contiguous=Tri::True;fact.tracked=Tri::False;fact.unique=Tri::True;fact.owns_storage=true;fact.fully_initialized=Tri::True;}
            if constexpr(std::is_same_v<T,TensorGather> || std::is_same_v<T,TensorScatter> || std::is_same_v<T,TensorReshape>) {
                transfer.operation=TensorShapeOp::ExplicitShape;
                if(const auto shape=arrays_.find(node.shape);shape!=arrays_.end())transfer.explicit_shape=shape->second;
                else type=unknown(type).type;
                if constexpr(!std::is_same_v<T,TensorReshape>){fact.contiguous=Tri::True;fact.unique=Tri::True;fact.owns_storage=true;fact.fully_initialized=Tri::True;}
            }
            if constexpr(std::is_same_v<T,TensorTranspose>) {
                transfer.operation=TensorShapeOp::Transpose;fact.contiguous=Tri::Unknown;fact.head_extents.clear();fact.tail_extents.clear();
                const auto a=integers_.find(node.axis0),b=integers_.find(node.axis1);
                if(a!=integers_.end() && b!=integers_.end()) {transfer.axes=std::pair{a->second,b->second};if(a->second==b->second)fact.contiguous=tensor(input,env).contiguous;}
            }
            if constexpr(std::is_same_v<T,TensorIndex>) {
                const bool boolean_mask =
                    node.items.size() == 1 &&
                    node.items.front().kind == abi::tensor_index_spec::mask;
                fact.head_extents.clear();
                fact.tail_extents.clear();
                if (boolean_mask) {
                    // Mask compaction has a runtime-dependent output length,
                    // unrelated to any source-axis extent. In particular,
                    // preserving the input's first axis would be an unsound
                    // static shape proof for a rank-one result.
                    transfer.operation = TensorShapeOp::ExplicitShape;
                    transfer.explicit_shape = std::vector<ShapeExtent>{ShapeExtent{}};
                    fact.contiguous = Tri::True;
                    fact.unique = Tri::True;
                    fact.owns_storage = true;
                } else {
                    transfer.operation=TensorShapeOp::Index;
                    fact.contiguous=Tri::Unknown;
                    for(const auto& item:node.items)
                        transfer.indices.push_back({
                            item.kind==abi::tensor_index_spec::index
                                ? TensorShapeTransfer::IndexAxis::Drop
                                : (item.start || item.stop || item.step)
                                    ? TensorShapeTransfer::IndexAxis::Slice
                                    : TensorShapeTransfer::IndexAxis::Preserve});
                }
            }
        } else if constexpr(std::is_same_v<T,TensorBinary> || std::is_same_v<T,TensorCompare> || std::is_same_v<T,Binary>) {
            if constexpr(std::is_same_v<T,Binary>) {if(node.result_type.kind!=TypeKind::Tensor)return;}
            matched=true;output=node.out;type=node.result_type;
            const auto left=tensor(node.left,env),right=tensor(node.right,env);fact=left.has_type?left:right;
            transfer.inputs={shape_type(left),shape_type(right)};
            transfer.operation=std::is_same_v<T,TensorCompare>?TensorShapeOp::Preserve:TensorShapeOp::Broadcast;
            fact.contiguous=Tri::True;fact.unique=Tri::True;fact.owns_storage=false;fact.fully_initialized=Tri::True;
            if(left.has_type && right.has_type && left.device!=right.device)fact.device={};
            if constexpr(std::is_same_v<T,TensorCompare>)fact.tracked=Tri::False;
            else if(left.tracked==Tri::True || right.tracked==Tri::True)fact.tracked=Tri::True;
            else if((!left.has_type || left.tracked==Tri::False) && (!right.has_type || right.tracked==Tri::False))fact.tracked=Tri::False;
            else fact.tracked=Tri::Unknown;
            if(left.has_type && right.has_type){fact.head_extents.clear();fact.tail_extents.clear();}
            fresh_output=true;
        } else if constexpr(std::is_same_v<T,Unary>) {
            if(node.type.kind!=TypeKind::Tensor)return;
            matched=true;output=node.out;input=node.operand;type=node.type;fact=tensor(input,env);
            fact.contiguous=Tri::True;fact.unique=Tri::True;fact.owns_storage=false;fact.fully_initialized=Tri::True;fresh_output=true;
        } else if constexpr(std::is_same_v<T,FieldGet> || std::is_same_v<T,LoadReference> || std::is_same_v<T,LoadAddress>) {
            if constexpr(std::is_same_v<T,FieldGet>)type=node.field_type;else type=node.type;
            if(type.kind!=TypeKind::Tensor)return;
            matched=true;output=node.out;fact=unknown(type);type=fact.type;
        }
    },instruction);
    if(!matched)return;
    (void)fresh_output;
    if(input && !out_.values.contains(input))return;
    if(input)transfer.inputs={shape_type(tensor(input,env))};
    transfer.result=type;transfer.fresh=[&](std::size_t axis){return symbol(21,output,at.block,axis);};
    shape_from_type(fact,transfer_tensor_shape(transfer));
    for(std::size_t axis=0;axis<fact.extents.size();++axis)if(fact.extents[axis].kind==ShapeExtent::Kind::Unknown)fact.extents[axis]=symbol(21,output,at.block,axis);
    fact.type.tensor_known_shape_prefix=fact.extents;
    put(out_.values,output,std::move(fact));
}
} // namespace
TensorFacts TensorFactAnalysis::at(ir::analysis::InstrRef at,ir::ValueId value) const {
    if(at.block>=scopes.size() || !reachable[at.block] || at.index>=scopes[at.block].policies_before.size())return {};
    const auto definition=definitions.find(value);
    if(definition==definitions.end() || !dominance.dominates(definition->second,at) || definition->second==at)return {};
    const auto found=values.find(value);if(found==values.end())return {};
    auto result=found->second;const auto& scope=scopes[at.block];
    if(const auto* proof=scoped(scope.incoming,scope.changes,{Key::Value,value},at.index))result=*proof;
    if(const auto source=lineage.find(value);source!=lineage.end())if(const auto* proof=scoped(scope.incoming,scope.changes,source->second,at.index))result=overlay_shape(result,*proof);
    if(const auto* proof=scoped(scope.incoming,scope.changes,{Key::Value,value},at.index))result=overlay_shape(result,*proof);
    return result;
}
ShapeExtent TensorFactAnalysis::scalar_at(ir::analysis::InstrRef at,ir::ValueId value) const {
    if(at.block>=scopes.size() || !reachable[at.block] || at.index>=scopes[at.block].policies_before.size())return {};
    const auto definition=definitions.find(value);
    if(definition==definitions.end() || !dominance.dominates(definition->second,at) || definition->second==at)return {};
    const auto& scope=scopes[at.block];
    if(const auto* proof=scoped(scope.scalar_incoming,scope.scalar_changes,{Key::Value,value},at.index))return *proof;
    if(const auto source=lineage.find(value);source!=lineage.end())if(const auto* proof=scoped(scope.scalar_incoming,scope.scalar_changes,source->second,at.index))return *proof;
    const auto found=scalars.find(value);return found==scalars.end()?ShapeExtent{}:found->second;
}
const PolicyFacts* TensorFactAnalysis::policy_at(ir::analysis::InstrRef at) const noexcept {
    return at.block<scopes.size() && reachable[at.block] && at.index<scopes[at.block].policies_before.size()?&scopes[at.block].policies_before[at.index]:nullptr;
}
TensorFactAnalysis compute_tensor_facts(const ir::Function& function,const ir::analysis::Cfg& cfg,
    const ir::analysis::DomTree& dom,const ir::analysis::LocalSsa& ssa,const ir::analysis::Slots& slots,
    const ir::analysis::ModuleSummaries& summaries,const ir::facts::FactSources& sources,const ParamFacts& params,const AliasInfo* aliases) {
    return Engine(function,cfg,dom,ssa,slots,summaries,sources,params,aliases).run();
}
} // namespace quidra::ir::analysis
