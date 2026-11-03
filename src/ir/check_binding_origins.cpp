#include "ir/check_binding_origins.hpp"
#include "ir/summaries.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

namespace quidra::ir::analysis {
namespace {
struct Invalid : std::runtime_error {using std::runtime_error::runtime_error;};
struct Limit {};
// Full authored fields: neither location nor shape-symbol normalization is
// appropriate for an edit receipt tied to one exact original Function.
class RawFields {
public:
    RawFields(const std::function<void()>& tick,std::size_t limit,
        const std::function<void(std::size_t)>& bytes):tick_(tick),charge_bytes_(bytes),limit_(limit) {}
    std::string finish() {return std::move(bytes_);}
    template<class T> void put(const T& value) {
        using V=std::remove_cvref_t<T>;
        tick_();
        if(depth_>=256)throw Limit{};
        struct Depth {std::size_t& n;explicit Depth(std::size_t& v):n(v){++n;}~Depth(){--n;}} depth(depth_);
        using namespace canonical_detail;
        if constexpr(std::is_same_v<V,bool>)word(value?1:0);
        else if constexpr(std::is_enum_v<V>)put(static_cast<std::underlying_type_t<V>>(value));
        else if constexpr(std::is_integral_v<V>) {static_assert(sizeof(V)<=8);word(static_cast<std::uint64_t>(value));}
        else if constexpr(std::is_same_v<V,float>)word(std::bit_cast<std::uint32_t>(value));
        else if constexpr(std::is_same_v<V,double>)word(std::bit_cast<std::uint64_t>(value));
        else if constexpr(std::is_same_v<V,std::string> || std::is_same_v<V,std::string_view>) {word(value.size());append(value);}
        else if constexpr(std::is_same_v<V,ShapeCoefficient>)put(value.text());
        else if constexpr(IsOptional<V>::value) {word(value?1:0);if(value)put(*value);}
        else if constexpr(IsVariant<V>::value) {
            if(value.valueless_by_exception())throw Invalid("valueless check origin preimage");
            word(value.index());std::visit([&](const auto& n){put(n);},value);
        } else if constexpr(IsShared<V>::value) {
            word(value?1:0);
            if(value) {
                if(std::find(active_.begin(),active_.end(),value.get())!=active_.end())throw Invalid("cyclic check origin preimage");
                active_.push_back(value.get());put(*value);active_.pop_back();
            }
        } else if constexpr(IsVector<V>::value || IsArray<V>::value || IsSet<V>::value) {
            word(value.size());
            for(const auto& item:value) {
                if constexpr(std::is_same_v<typename V::value_type,bool>)put(static_cast<bool>(item));
                else put(item);
            }
        } else if constexpr(IsMap<V>::value) {
            word(value.size());for(const auto& [key,item]:value){put(key);put(item);}
        } else if constexpr(IsPair<V>::value) {put(value.first);put(value.second);}
        else if constexpr(std::is_aggregate_v<V> || std::is_same_v<V,Binary> || std::is_same_v<V,ShapeExtent>) {
            word(fields::aggregate_arity<V>());fields::for_each_field(value,[&](auto,const auto& field){put(field);});
        } else static_assert(unsupported<V>,"raw check origin preimage needs an authored field adapter");
    }
private:
    const std::function<void()>& tick_;
    const std::function<void(std::size_t)>& charge_bytes_;
    std::size_t limit_,depth_{};
    std::string bytes_;
    std::vector<const void*> active_;
    void append(std::string_view text) {
        if(bytes_.size()>limit_ || text.size()>limit_-bytes_.size())throw Limit{};
        if(charge_bytes_)charge_bytes_(text.size());
        bytes_.append(text);
    }
    void word(std::uint64_t value) {
        if(bytes_.size()>limit_ || 8>limit_-bytes_.size())throw Limit{};
        if(charge_bytes_)charge_bytes_(8);
        for(unsigned shift=0;shift<64;shift+=8)bytes_.push_back(static_cast<char>(value>>shift));
    }
};
struct Validate {
    const std::function<void()>& tick;
    std::size_t limit;
    const std::function<void(std::size_t)>& bytes;
    template<class T> std::string raw(const T& value)const {RawFields fields(tick,limit,bytes);fields.put(value);return fields.finish();}
    void require(bool valid,const char* message)const {tick();if(!valid)throw Invalid(message);}
};
bool erasable(const Instruction& ins) {
    if(const auto* shape=std::get_if<ShapedConstraintCheck>(&ins))
        return shape->kind==TypeKind::Tensor && (!shape->ellipsis || *shape->ellipsis<=shape->extents.size());
    return std::holds_alternative<ExtentEqualCheck>(ins) ||
        std::holds_alternative<InitializedCheck>(ins) || std::holds_alternative<ForStepCheck>(ins);
}
void flag(Instruction& ins,CheckOriginAction action,const Validate& check) {
    bool accepted=false;
    std::visit([&](auto& n) {
        using T=std::remove_cvref_t<decltype(n)>;
        const auto set=[&](bool& field){check.require(!field,"check receipt repeats an already proven flag");field=true;accepted=true;};
        if constexpr(std::is_same_v<T,ArrayGet> || std::is_same_v<T,ArraySet>) {
            if(action==CheckOriginAction::BoundsFlag)set(n.bounds_proven);
            else if(action==CheckOriginAction::InitializationFlag)set(n.initialization_proven);
        } else if constexpr(std::is_same_v<T,Binary>) {
            if(action==CheckOriginAction::OverflowFlag)set(n.overflow_proven);
            else if(action==CheckOriginAction::InlineFlag)set(n.inline_proven);
        } else if constexpr(std::is_same_v<T,Unary> || std::is_same_v<T,NumericConvert>) {
            if(action==CheckOriginAction::InlineFlag)set(n.inline_proven);
        } else if constexpr(std::is_same_v<T,TensorBinary>) {
            if(action==CheckOriginAction::BroadcastFlag)set(n.broadcast_proven);
        } else if constexpr(std::is_same_v<T,TensorCompare>) {
            if(action==CheckOriginAction::ShapeFlag)set(n.shape_proven);
        }
    },ins);
    check.require(accepted,"check receipt sets an unsupported flag");
}
void validate_edits(const Function& before,const Function& after,const CheckOriginTransfer& transfer,const Validate& check) {
    check.require(transfer.function==before.name && after.name==before.name,"check receipt function mismatch");
    check.require(before.blocks.size()==after.blocks.size(),"check edit changed block count");
    auto old_metadata=before,new_metadata=after;
    for(auto& block:old_metadata.blocks)block.instructions.clear();
    for(auto& block:new_metadata.blocks)block.instructions.clear();
    if(transfer.invalidate_tensor_regions) {
        check.require(after.tensor_regions.empty(),"invalidated tensor regions were not cleared");
        old_metadata.tensor_regions.clear();
    }
    check.require(check.raw(old_metadata)==check.raw(new_metadata),"check edit changed Function metadata or ABI");
    std::map<InstrRef,std::set<CheckOriginAction>,InlineSiteLess> edits;
    for(const auto& edit:transfer.edits) {
        check.require(edit.at.block<before.blocks.size() && edit.at.index<before.blocks[edit.at.block].instructions.size(),"check edit site is out of range");
        check.require(edits[edit.at].insert(edit.action).second,"duplicate check edit disposition");
    }
    std::size_t instructions=0,boundaries=0;
    bool shifted=false;
    for(std::size_t b=0;b<before.blocks.size();++b) {
        check.require(b<=std::numeric_limits<BlockId>::max() && before.blocks[b].instructions.size()<=std::numeric_limits<std::uint32_t>::max(),"check source dimensions exceed typed site range");
        const auto block=static_cast<BlockId>(b);
        std::uint32_t next=0;
        for(std::size_t i=0;i<=before.blocks[b].instructions.size();++i) {
            const InstrRef original{block,static_cast<std::uint32_t>(i)};
            const auto boundary=transfer.boundaries.find(original);
            check.require(boundary!=transfer.boundaries.end() && boundary->second==InstrRef{block,next},"check boundary receipt is incomplete or reordered");
            ++boundaries;
            if(i==before.blocks[b].instructions.size())break;
            const auto target=transfer.instructions.find(original);
            check.require(target!=transfer.instructions.end(),"check instruction disposition is missing");
            ++instructions;
            const auto changed=edits.find(original);
            const bool erased=changed!=edits.end() && changed->second.contains(CheckOriginAction::Erase);
            if(erased) {
                check.require(changed->second.size()==1 && !target->second && erasable(before.blocks[b].instructions[i]),"check receipt erases a noncheck or mixes dispositions");
                shifted=true;
                continue;
            }
            check.require(target->second && *target->second==InstrRef{block,next} && next<after.blocks[b].instructions.size(),"check retained site is missing or reordered");
            auto expected=before.blocks[b].instructions[i];
            if(changed!=edits.end())for(const auto action:changed->second)flag(expected,action,check);
            check.require(check.raw(expected)==check.raw(after.blocks[b].instructions[next]),"check replacement changes unauthorized authored fields");
            ++next;
        }
        check.require(next==after.blocks[b].instructions.size(),"check edit inserts unreceipted instructions");
    }
    check.require(instructions==transfer.instructions.size() && boundaries==transfer.boundaries.size(),"check receipt contains extra sites");
    check.require(!shifted || before.tensor_regions.empty() || transfer.invalidate_tensor_regions,"check erase retains stale region indices");
}
InstrRef remap(InstrRef at,bool boundary,const CheckOriginTransfer& transfer,const Validate& check) {
    if(boundary) {
        const auto found=transfer.boundaries.find(at);
        check.require(found!=transfer.boundaries.end(),"source boundary is not mapped");return found->second;
    }
    const auto found=transfer.instructions.find(at);
    check.require(found!=transfer.instructions.end() && found->second.has_value(),"concrete source instruction was erased or unmapped");
    return *found->second;
}
void remap(FunctionBindingSource& source,const CheckOriginTransfer& transfer,const Validate& check) {
    for(auto& collection:source.collections)collection.at=remap(collection.at,true,transfer,check);
    for(auto& traversal:source.traversals)traversal.at=remap(traversal.at,true,transfer,check);
    for(auto& event:source.rebind_events)event.at=remap(event.at,false,transfer,check);
}
void remap(ScopedBindingSource& source,const CheckOriginTransfer& transfer,const Validate& check) {
    remap(source.source,transfer,check);
    source.begin=remap(source.begin,false,transfer,check);
    for(auto& parameter:source.parameters)parameter.at=remap(parameter.at,false,transfer,check);
    for(auto& returned:source.returns) {
        if(returned.store)returned.store=remap(*returned.store,false,transfer,check);
        if(returned.load)returned.load=remap(*returned.load,false,transfer,check);
        returned.cleanup=remap(returned.cleanup,false,transfer,check);
        returned.end=remap(returned.end,false,transfer,check);
        returned.jump=remap(returned.jump,false,transfer,check);
        // original, original_value and type remain in the owned callee ABI.
    }
}
} // namespace
CheckOriginResult transfer_check_origins(const Module& module,const Function& before,
    const Function& after,const CheckOriginTransfer& transfer,const std::function<void()>& tick,std::size_t maximum,
    const std::function<void(std::size_t)>& bytes) {
    CheckOriginResult result;result.snapshots=module.binding_origins;
    const Validate check{tick,maximum,bytes};
    const auto unavailable=[&](BindingSourceComponent component,BindingGap reason){result.unavailable.push_back({component,reason,before.name});};
    try {
        const Function* actual=nullptr;
        for(const auto& fn:module.functions) {
            tick();if(fn.name!=before.name)continue;
            check.require(!actual,"check source function is ambiguous");actual=&fn;
        }
        check.require(actual && check.raw(*actual)==check.raw(before),"check source is not the actual module preimage");
        validate_edits(before,after,transfer,check);
        if(!module.binding_origins) {
            unavailable(BindingSourceComponent::Body,BindingGap::MissingOrigin);result.accepted=true;return result;
        }
        // Charge the complete owned source copy, including every nested row.
        (void)check.raw(*module.binding_origins);
        auto snapshots=std::make_shared<BindingOriginSnapshots>(*module.binding_origins);
        const auto source=snapshots->functions.find(before.name);
        if(source==snapshots->functions.end()) {
            unavailable(BindingSourceComponent::Body,BindingGap::MissingOrigin);result.accepted=true;return result;
        }
        (void)check.raw(module.classes);
        if(snapshots->class_fields!=canonical_encode_value(module.classes).bytes || source->second.body_fields!=binding_origin_body_fields(before)) {
            snapshots->functions.erase(source);unavailable(BindingSourceComponent::Body,BindingGap::StaleOrigin);
            result.snapshots=std::move(snapshots);result.accepted=true;return result;
        }
        auto& output=source->second;
        if(output.root_source_available) {
            const auto* summary=module.summaries?module.summaries->find(before.name):nullptr;
            if(!before.declaration || !same_declaration(*before.declaration,output.source.declaration) ||
               before.declaration->generic_args!=output.source.declaration.generic_args || !summary ||
               output.source.semantic_parameter_for_ir_parameter!=summary->semantic_parameter_for_ir_parameter ||
               output.source.semantic_parameter_for_ir_parameter.size()!=before.parameters.size()) {
                snapshots->functions.erase(source);unavailable(BindingSourceComponent::Declaration,BindingGap::MissingOrigin);
                result.snapshots=std::move(snapshots);result.accepted=true;return result;
            }
            remap(output.source,transfer,check);
        }
        for(auto row=output.scopes.begin();row!=output.scopes.end();) {
            tick();
            if(row->first!=row->second.scope || !valid_scoped_binding_source(module,before,row->second)) {
                unavailable(BindingSourceComponent::Body,BindingGap::StaleOrigin);row=output.scopes.erase(row);continue;
            }
            remap(row->second,transfer,check);
            check.require(valid_scoped_binding_source(module,after,row->second),"remapped scope disagrees with actual replacement body");
            ++row;
        }
        output.body_fields=binding_origin_body_fields(after);
        output.unavailable.insert(output.unavailable.end(),result.unavailable.begin(),result.unavailable.end());
        result.snapshots=std::move(snapshots);result.accepted=true;
    } catch(const Limit&) {
        result.failure=CheckOriginFailure::Budget;
    } catch(const Invalid&) {
        result.failure=CheckOriginFailure::InvalidReceipt;
        unavailable(BindingSourceComponent::Body,BindingGap::StaleOrigin);
    } catch(const std::invalid_argument&) {
        result.failure=CheckOriginFailure::InvalidReceipt;
        unavailable(BindingSourceComponent::Body,BindingGap::StaleOrigin);
    }
    return result;
}
} // namespace quidra::ir::analysis
