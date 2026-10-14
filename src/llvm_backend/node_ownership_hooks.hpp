#pragma once
#include "quidra/ir/module.hpp"
#include "quidra/types.hpp"
#include <string>
namespace quidra::llvm_backend {
// Every hook runs during LLVM generation, not at runtime. Implementations
// reserve stable cells and emit non-fallible publication/transfer stores.
class NodeOwnershipHooks {
public:
    virtual ~NodeOwnershipHooks()=default;
    virtual bool active()const{return true;}
    virtual void cleanup()=0;
    virtual void cleanup_scope(std::uint64_t){cleanup();}
    virtual void pin(const ir::Instruction& instruction,ir::ValueId target)=0;
    virtual void unpin(const ir::Instruction& instruction,ir::ValueId target)=0;
    virtual void acquire(ir::ValueId value,const Type& type,const std::string& raw)=0;
    virtual void acquire_custom(ir::ValueId value,const Type& type,const std::string& raw,const std::string& release)=0;
    virtual void acquire_custom_temporary(const std::string& raw,const Type& type,const std::string& release)=0;
    virtual void tuple_projection(ir::ValueId value,ir::ValueId tuple,std::size_t index,const Type& type,bool take)=0;
    virtual void storage_origin(ir::ValueId value,const std::string& address,const Type& type)=0;
    virtual void alias_value(ir::ValueId value,ir::ValueId source,const Type& type)=0;
    virtual void borrowed_value(ir::ValueId value)=0;
    virtual void acquire_temporary(const std::string& raw,const Type& type)=0;
    virtual void transfer(ir::ValueId value,const Type& type)=0;
    virtual void transfer_temporary(const std::string& raw,const Type& type)=0;
    virtual void bind_borrowed_local(const std::string& name,ir::ValueId value,const Type& type)=0;
    virtual void load_borrowed_local(ir::ValueId value,const std::string& name,const Type& type)=0;
};
}
