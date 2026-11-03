#include "llvm_backend/parallel_node_emission.hpp"
#include "llvm_backend/call_site_metadata.hpp"
#include "llvm_backend/type_helpers.hpp"
#include "llvm_backend/runtime_abi.hpp"
#include "quidra/abi/symbols.hpp"
#include <sstream>
#include <stdexcept>
namespace quidra::llvm_backend {
std::string emit_parallel_node_wrapper(const ir::Function& function,CallSiteMetadata* metadata) {
    if(!function.parallel_node)return {};
    const auto& node=*function.parallel_node;
    std::ostringstream out;
    const auto body=abi::user_symbol(function.name),entry=abi::user_symbol(node.wrapper);
    std::optional<std::pair<std::size_t,std::size_t>> location;
    if(metadata)location=metadata->parallel_entry(function);
    // The table module must be able to name an indirectly entered wrapper.
    // Hidden linkage keeps it out of the public exports.
    out<<"define hidden i8 @"<<entry<<"(ptr %frame)";
    if(location)out<<" !dbg !"<<location->first;
    out<<" {\nentry:\n";
    for(std::size_t i=0;i<function.parameters.size();++i) {
        if(!function.parameters[i].writable)throw std::logic_error("parallel capture is not storage");
        out<<"  %slot"<<i<<" = getelementptr ptr, ptr %frame, i64 "<<i+1<<"\n"
           <<"  %capture"<<i<<" = load ptr, ptr %slot"<<i<<"\n";
    }
    out<<"  ";if(node.fallible)out<<"%result = ";
    out<<"call "<<(node.fallible?"ptr":"void")<<" @"<<body<<"(";
    for(std::size_t i=0;i<function.parameters.size();++i){if(i)out<<", ";out<<"ptr %capture"<<i;}
    out<<")";
    if(location)out<<", !dbg !"<<location->second;
    out<<"\n";
    if(node.fallible) {
        const auto type=Type::union_of({Type::simple(TypeKind::Void),Type::simple(TypeKind::Error)});
        const auto drop=drop_name(type);
        out<<"  %tag = load i64, ptr %result\n  %error = icmp eq i64 %tag, "<<case_index(type,Type::simple(TypeKind::Error))
           <<"\n  br i1 %error, label %failed, label %succeeded\nfailed:\n"
           <<"  store ptr %result, ptr %frame\n  ret i8 1\nsucceeded:\n"
           <<"  call void @quidra_managed_release(ptr %result, ptr "<<drop<<")\n  ret i8 0\n}\n\n"
           <<"define internal void @"<<entry<<".drop(ptr %frame) {\nentry:\n"
           <<"  %result = load ptr, ptr %frame\n  store ptr null, ptr %frame\n"
           <<"  call void @quidra_managed_release(ptr %result, ptr "<<drop<<")\n  ret void\n}\n\n";
    }else out<<"  ret i8 0\n}\n\n";
    return out.str();
}
std::string emit_parallel_node_tables(const ir::Module& module,StringPool& pool) {
    std::ostringstream out;bool present=false;
    for(const auto& function:module.functions)for(const auto& block:function.blocks)for(const auto& instruction:block.instructions) {
        const auto* parallel=std::get_if<ir::ParallelBlock>(&instruction);if(!parallel)continue;
        present=true;
        const auto table=abi::user_symbol(parallel->table_name);
        for(std::size_t i=0;i<parallel->nodes.size();++i){
            const auto& predecessors=parallel->nodes[i].predecessors;
            out<<"@"<<table<<".pred."<<i<<" = private constant ["<<predecessors.size()<<" x i32] [";
            for(std::size_t j=0;j<predecessors.size();++j){if(j)out<<", ";out<<"i32 "<<predecessors[j];}
            out<<"]\n";
        }
        out<<"@"<<table<<".pointers = private constant ["<<parallel->nodes.size()*5<<" x ptr] [";
        for(std::size_t i=0;i<parallel->nodes.size();++i){
            const auto& node=parallel->nodes[i];if(i)out<<", ";
            const auto wrapper=abi::user_symbol(node.function+".entry");
            const auto display=pool.intern(parallel->iterations?"<parallel iteration>()":"<node body>()");
            out<<"ptr @"<<wrapper<<", ptr @"<<wrapper<<", ptr @"<<table<<".pred."<<i
               <<", ptr "<<(node.fallible?"@"+wrapper+".drop":"null")<<", ptr @"<<display;
        }
        out<<"]\n@"<<table<<".numbers = private constant ["<<parallel->nodes.size()*4<<" x i64] [";
        std::size_t file_index=0;bool user=false;
        for(std::size_t i=0;i<module.user_sources.size();++i)if(module.user_sources[i].absolute_path==function.source_file){file_index=i;user=true;break;}
        for(std::size_t i=0;i<parallel->nodes.size();++i){
            const auto& node=parallel->nodes[i];if(i)out<<", ";
            const auto line=user?(static_cast<unsigned long long>(file_index)<<32)|node.position.line:0;
            out<<"i64 "<<node.predecessors.size()<<", i64 "<<(node.worker_safe?1:0)<<", i64 "<<line<<", i64 "<<(user?node.position.column:0);
        }
        out<<"]\n";
    }
    if(present)out<<R"(
define internal void @.quidra.parallel.state(ptr %values, i1 %restore) {
entry:
  %p1 = getelementptr i64, ptr %values, i64 1
  %p2 = getelementptr i64, ptr %values, i64 2
  br i1 %restore, label %load, label %save
save:
  %d = load i64, ptr @.quidra.stack.depth
  %l = load i64, ptr @.quidra.source.line
  %c = load i64, ptr @.quidra.source.column
  store i64 %d, ptr %values
  store i64 %l, ptr %p1
  store i64 %c, ptr %p2
  store i64 0, ptr @.quidra.stack.depth
  store i64 0, ptr @.quidra.source.line
  store i64 0, ptr @.quidra.source.column
  ret void
load:
  %rd = load i64, ptr %values
  %rl = load i64, ptr %p1
  %rc = load i64, ptr %p2
  store i64 %rd, ptr @.quidra.stack.depth
  store i64 %rl, ptr @.quidra.source.line
  store i64 %rc, ptr @.quidra.source.column
  ret void
}
)";
    return out.str();
}
}
