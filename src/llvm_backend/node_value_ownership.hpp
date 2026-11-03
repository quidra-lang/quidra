#pragma once
#include "llvm_backend/node_ownership_hooks.hpp"
#include "llvm_backend/node_pin_plan.hpp"
#include "llvm_backend/owned_shadow_emitter.hpp"
#include "llvm_backend/symbol_table.hpp"
#include <map>
#include <set>
#include <vector>
namespace quidra::llvm_backend {
class NodeValueOwnership final:public NodeOwnershipHooks {
public:
    static constexpr const char* cleanup_marker="  ; quidra.node.ownership.cleanup\n";
    static constexpr const char* entry_marker="  ; quidra.node.ownership.entry\n";
    NodeValueOwnership(const ir::Function& function,SymbolTable& symbols,
                       llvm_text::LlvmBuilder& builder,llvm_text::FunctionText& function_text,TemporaryNames& names,
                       const std::unordered_map<std::string,ir::ClassLayout>& layouts,
                       const InlineScopePlan& scopes,bool root_cleanup);
    bool active()const override{return scopes_.enabled(symbols_.selected_scope(),root_cleanup_);}
    void acquire(ir::ValueId value,const Type& type,const std::string& raw)override;
    void acquire_custom(ir::ValueId value,const Type& type,const std::string& raw,const std::string& release)override;
    void acquire_custom_temporary(const std::string& raw,const Type& type,const std::string& release)override;
    void tuple_projection(ir::ValueId value,ir::ValueId tuple,std::size_t index,const Type& type,bool take)override;
    void storage_origin(ir::ValueId value,const std::string& address,const Type& type)override;
    void alias_value(ir::ValueId value,ir::ValueId source,const Type& type)override;
    void borrowed_value(ir::ValueId value)override{borrowed_.insert(value);unproven_origins_.insert(value);}
    void acquire_temporary(const std::string& raw,const Type& type)override;
    void transfer(ir::ValueId value,const Type& type)override;
    void transfer_temporary(const std::string& raw,const Type& type)override;
    void bind_borrowed_local(const std::string& name,ir::ValueId value,const Type& type)override;
    void load_borrowed_local(ir::ValueId value,const std::string& name,const Type& type)override;
    void pin(const ir::Instruction& instruction,ir::ValueId target)override;
    void unpin(const ir::Instruction& instruction,ir::ValueId target)override;
    CallArgumentOwnerLocation argument(ir::ValueId value,const Type& type);
    void certify_result(const ir::Instruction& instruction);
    void cleanup()override;
    void cleanup_scope(std::uint64_t scope)override;
    static std::string scope_entry_marker(std::uint64_t scope);
    static std::string scope_cleanup_marker(std::uint64_t scope);
    std::string finish_cleanup(std::uint64_t scope=0)const;
    std::string finish_entry(std::uint64_t scope=0)const;
    std::string helpers()const;
private:
    std::string value(ir::ValueId id)const{return symbols_.value(id);}
    std::string alias_cell(ir::ValueId id);
    std::string local_link(const std::string& name);
    void record(const std::string& raw,const Type& type,bool pin=false,std::optional<std::uint64_t> scope=std::nullopt);
    const ir::Function& function_;
    const InlineScopePlan& scopes_;
    bool root_cleanup_{};
    SymbolTable& symbols_;
    llvm_text::LlvmBuilder& builder_;
    llvm_text::FunctionText& function_text_;
    TemporaryNames& names_;
    OwnedShadowEmitter shadow_;
    std::map<std::string,Type> owning_;
    std::set<ir::ValueId> borrowed_;
    std::set<ir::ValueId> unproven_origins_;
    std::set<std::string> unproven_holders_;
    std::map<ir::ValueId,std::string> aliases_;
    std::map<std::string,std::string> local_links_;
    NodePinPlan pin_plan_;
    std::vector<std::pair<std::string,std::uint64_t>> link_slots_;
    std::map<std::string,Type> local_clears_;
};
}
