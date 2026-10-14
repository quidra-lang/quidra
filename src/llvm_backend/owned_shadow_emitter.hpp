#pragma once
#include "llvm_backend/call_acceptance_emitter.hpp"
#include "llvm_backend/temporary_names.hpp"
#include "llvm_text/llvm_builder.hpp"
#include "quidra/ir/module.hpp"
#include <map>
#include <string>
#include <vector>
namespace quidra::llvm_backend {
struct OwnedShadowSlot {
    std::string name;
    Type type;
    bool pin{};
    std::uint64_t offset{};
    std::string drop_symbol;
    std::string clear_symbol;
    std::string custom_release;
    std::uint64_t scope{};
};
// A shadow is an independent cleanup obligation, never a refcount inferred
// from a pointer. Slots are reserved before entry and published immediately
// after an owning operation, before its next fallible operation.
class OwnedShadowEmitter {
public:
    static constexpr std::uint64_t header_bytes=16;
    OwnedShadowEmitter(llvm_text::LlvmBuilder& builder,TemporaryNames& names,
                       const std::unordered_map<std::string,ir::ClassLayout>& layouts)
        :builder_(builder),names_(names),layouts_(layouts){}
    void reserve(const std::string& name,const Type& type,bool pin=false,const std::string& custom_release={},std::uint64_t scope=0);
    bool empty()const{return slots_.empty();}
    void prepare(const std::string& function);
    void entry(const std::string& function,std::uint64_t scope=0);
    std::string entry_code(const std::string& function,std::uint64_t scope=0)const;
    std::string scope_allocations()const;
    bool has_scope(std::uint64_t scope)const{return scope_bytes_.contains(scope);}
    static std::string suffix(std::uint64_t scope){return scope?".inline."+std::to_string(scope):"";}
    std::string frame(std::uint64_t scope)const{return "%quidra.shadow.frame"+suffix(scope);}
    std::string base(std::uint64_t scope)const{return "%quidra.shadow.base"+suffix(scope);}
    std::string cleanup_symbol(std::uint64_t scope)const{return prefix_+suffix(scope)+".cleanup";}
    void publish(const std::string& name,const std::string& value);
    // Clear only after destination installation, or immediately before a
    // release/return. Neither this operation nor its clear callback drops.
    void consume(const std::string& name);
    void cleanup(std::uint64_t scope=0);
    CallArgumentOwnerLocation argument(const std::string& name)const;
    // An alias link contains the address of the actual owning shadow cell,
    // not the referent value. Null links denote borrowed external storage.
    void link(const std::string& link_address,const std::string& owner);
    void link_slot(const std::string& link_address,const std::string& owner_address,const std::string& clear_symbol,const std::string& value_address={});
    CallArgumentOwnerLocation argument_link(const std::string& link_address);
    std::string value_address(const std::string& name)const{return payload(name);}
    std::string link_value_address(const std::string& link_address);
    void copy_link(const std::string& destination,const std::string& source);
    void clear_link(const std::string& address);
    std::string consume_link(const std::string& link_address);
    std::string helpers()const;
    const OwnedShadowSlot& slot(const std::string& name)const{return slots_.at(indices_.at(name));}
    std::string cell(const std::string& name)const;
private:
    std::string payload(const std::string& name)const;
    std::string header(const std::string& name)const;
    std::string empty_value(const OwnedShadowSlot&)const;
    llvm_text::LlvmBuilder& builder_;
    TemporaryNames& names_;
    const std::unordered_map<std::string,ir::ClassLayout>& layouts_;
    std::map<std::string,std::size_t> indices_;
    std::vector<OwnedShadowSlot> slots_;
    std::string prefix_;
    bool entry_emitted_{};
    std::map<std::uint64_t,std::uint64_t> scope_bytes_;
};
}
