#pragma once
#include "llvm_backend/node_owned_type.hpp"
#include "llvm_backend/owned_value_plan.hpp"
#include "llvm_backend/type_lowering.hpp"
#include "quidra/standard_classes.hpp"
#include <optional>
#include <unordered_map>
namespace quidra::llvm_backend {
struct NodeResultContract {
    Type type;
    ResultOwner owner;
    OwnershipOrigin origin;
    // These results contain multiple allocations, owning operands, or an
    // externally unknown consumption contract. Domain hooks must certify
    // publication and handoffs; a post-instruction store alone is not safe.
    bool needs_domain_hooks{};
};
inline NodeResultContract result_contract(const Type& type,ResultOwner owner,
                                         OwnershipOrigin origin={},bool hooks=false) {
    if(!node_owns_value(type))owner=ResultOwner::unmanaged;
    return {type,owner,std::move(origin),hooks};
}
// Explicit ownership contracts of the operations audited here. An absent
// contract is a compiler diagnostic in node-cleanup functions, not permission
// to guess that a pointer is an owning value.
inline std::optional<NodeResultContract> node_result_contract(const ir::Instruction& instruction) {
    return std::visit([](const auto& n)->std::optional<NodeResultContract> {
        using N=std::decay_t<decltype(n)>;
        if constexpr(std::is_same_v<N,ir::ConstantInt> || std::is_same_v<N,ir::ConstantExact>)
            return result_contract(n.type,ResultOwner::owned);
        else if constexpr(std::is_same_v<N,ir::ConstantFloat>)
            return result_contract(n.type,ResultOwner::borrowed);
        else if constexpr(std::is_same_v<N,ir::ExactConstant>)
            return result_contract(n.type,ResultOwner::borrowed);
        else if constexpr(std::is_same_v<N,ir::ComplexPart>)
            return result_contract(complex_part_type(n.type),ResultOwner::borrowed,{OwnerOrigin::field,n.value,{}});
        else if constexpr(std::is_same_v<N,ir::ToString>) {
            if(n.source_type.kind==TypeKind::Bool)return result_contract(Type::simple(TypeKind::String),ResultOwner::borrowed);
            if(n.source_type.kind==TypeKind::String)return result_contract(Type::simple(TypeKind::String),ResultOwner::alias,{OwnerOrigin::value,n.value,{}});
            return result_contract(Type::simple(TypeKind::String),ResultOwner::owned,{},true);
        }
        else if constexpr(std::is_same_v<N,ir::ErrorCode>)
            return result_contract(Type::simple(TypeKind::String),ResultOwner::borrowed);
        else if constexpr(std::is_same_v<N,ir::JsonKind>)
            return result_contract(Type::simple(TypeKind::String),ResultOwner::borrowed);
        else if constexpr(std::is_same_v<N,ir::ConstantBool>)
            return result_contract(Type::simple(TypeKind::Bool),ResultOwner::unmanaged);
        else if constexpr(std::is_same_v<N,ir::ConstantString>)
            return result_contract(Type::simple(TypeKind::String),ResultOwner::borrowed);
        else if constexpr(std::is_same_v<N,ir::Clone> || std::is_same_v<N,ir::Retain>)
            return result_contract(n.type,ResultOwner::owned);
        else if constexpr(std::is_same_v<N,ir::LoadLocal>) {
            if(n.move_out)return result_contract(n.type,ResultOwner::owned,{},true);
            return result_contract(n.type,ResultOwner::alias,{OwnerOrigin::local,{},n.name});
        }
        else if constexpr(std::is_same_v<N,ir::TupleMake>)
            return result_contract(n.type,ResultOwner::owned,{},true);
        else if constexpr(std::is_same_v<N,ir::TupleGet>)
            return result_contract(n.element_type,n.take?ResultOwner::owned:ResultOwner::borrowed,{},n.take);
        else if constexpr(std::is_same_v<N,ir::StringQuote>)
            return result_contract(Type::simple(TypeKind::String),ResultOwner::owned,{},true);
        else if constexpr(std::is_same_v<N,ir::LoadReference>)
            return result_contract(n.type,ResultOwner::borrowed,{OwnerOrigin::reference,{},n.name});
        else if constexpr(std::is_same_v<N,ir::LoadAddress>)
            return result_contract(n.type,ResultOwner::borrowed);
        else if constexpr(std::is_same_v<N,ir::FieldGet>)
            return result_contract(n.field_type,ResultOwner::borrowed,{OwnerOrigin::field,n.object,{}});
        else if constexpr(std::is_same_v<N,ir::ArrayGet>)
            return result_contract(n.element_type,ResultOwner::borrowed,{OwnerOrigin::element,n.array,{}});
        else if constexpr(std::is_same_v<N,ir::VariantPayload>)
            return result_contract(n.payload_type,ResultOwner::borrowed,{OwnerOrigin::variant_payload,n.container,{}});
        else if constexpr(std::is_same_v<N,ir::ArrayNumericCast>)
            return result_contract(n.result_type,ResultOwner::owned,{},true);
        else if constexpr(std::is_same_v<N,ir::ArrayMake> || std::is_same_v<N,ir::ArrayAlloc> ||
                          std::is_same_v<N,ir::ClassMake>)
            return result_contract(n.type,ResultOwner::owned,{},true);
        else if constexpr(std::is_same_v<N,ir::VariantMake>)
            return result_contract(n.container_type,ResultOwner::owned,{},true);
        else if constexpr(std::is_same_v<N,ir::ArraySlice>)
            return result_contract(Type::array(*n.array_type.first),ResultOwner::owned,{},true);
        else if constexpr(std::is_same_v<N,ir::ArrayGrowMove> || std::is_same_v<N,ir::ArraySorted>)
            return result_contract(n.array_type,ResultOwner::owned,{},true);
        else if constexpr(std::is_same_v<N,ir::ArraySortIndices>)
            return result_contract(Type::array(Type::simple(TypeKind::Int64)),ResultOwner::owned,{},true);
        else if constexpr(std::is_same_v<N,ir::NumericConvert>)
            return result_contract(n.target_type,ResultOwner::owned,{},node_owns_value(n.source_type)||node_owns_value(n.target_type));
        else if constexpr(std::is_same_v<N,ir::Call> || std::is_same_v<N,ir::IndirectCall>)
            return result_contract(n.result,ResultOwner::owned,{},true);
        else if constexpr(std::is_same_v<N,ir::FunctionRef>)
            return result_contract(n.type,ResultOwner::borrowed);
        else if constexpr(std::is_same_v<N,ir::TimeNow>)
            return result_contract(Type::class_type(standard_class::time_instant),ResultOwner::owned);
        else if constexpr(std::is_same_v<N,ir::TimeSince> || std::is_same_v<N,ir::TimeSeconds>)
            return result_contract(Type::class_type(standard_class::time_duration),ResultOwner::owned);
        else return {};
    },instruction);
}
struct NodeCallContract {
    std::vector<std::size_t> owning_arguments;
    bool acceptance_required{};
};
inline NodeCallContract node_call_contract(const ir::Call& call,const ir::Function& target) {
    if(call.args.size()!=target.parameters.size())throw std::logic_error("node call parameter count mismatch");
    NodeCallContract contract;
    for(std::size_t i=0;i<target.parameters.size();++i) {
        const auto& p=target.parameters[i];
        if(p.writable || p.borrowed || !node_owns_value(p.type))continue;
        // The existing native ABI explicitly lends these buffers/tensors.
        // Other managed extern parameters have no acceptance protocol.
        if(target.external_symbol) {
            if(p.type.kind==TypeKind::String || p.type.kind==TypeKind::Bin || p.type.kind==TypeKind::Tensor)continue;
            throw std::logic_error("parallel extern managed argument lacks a consumption contract");
        }
        contract.owning_arguments.push_back(i);
    }
    contract.acceptance_required=!contract.owning_arguments.empty();
    return contract;
}
class NodeResultContracts {
public:
    void reserve(const ir::Function& function,std::uint64_t first_offset=0) {
        plan_.reserve(function,first_offset);needs_hooks_.clear();
        for(const auto& [id,record]:plan_.values()) {
            if(const auto contract=node_result_contract(*record.producer)) {
                plan_.resolve(id,contract->type,contract->owner,contract->origin);
                if(contract->needs_domain_hooks && contract->owner!=ResultOwner::unmanaged)
                    needs_hooks_.emplace(id,false);
            }
        }
    }
    // A domain supplies missing result contracts and certifies its own
    // intermediate publication/consumption ordering explicitly.
    void resolve(ir::ValueId id,const NodeResultContract& contract) {
        plan_.resolve(id,contract.type,contract.owner,contract.origin);
        if(contract.needs_domain_hooks && contract.owner!=ResultOwner::unmanaged)needs_hooks_.try_emplace(id,false);
    }
    void certify_domain(ir::ValueId id) {
        const auto found=needs_hooks_.find(id);
        if(found==needs_hooks_.end())throw std::logic_error("unexpected node domain ownership certification");
        found->second=true;
    }
    void require_complete()const {
        plan_.require_resolved();
        for(const auto& [_,safe]:needs_hooks_)if(!safe)
            throw std::logic_error("parallel managed result lacks early publication and handoff hooks");
    }
    const OwnedValuePlan& plan()const{return plan_;}
private:
    OwnedValuePlan plan_;
    std::map<ir::ValueId,bool> needs_hooks_;
};
}
