#include "llvm_backend/debug_module.hpp"
#include "llvm_backend/authored_display_name.hpp"

#include "quidra/abi/symbols.hpp"
#include "quidra/member_function_names.hpp"
#include "llvm_text/escape.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <sstream>
#include <string_view>

namespace quidra::llvm_backend {

namespace {

std::string_view primary_source_file(const ir::Module& module){
    for(const auto& f:module.functions)
        if(f.entrypoint&&!f.source_file.empty()) return f.source_file;
    for(const auto& f:module.functions)
        if(!f.source_file.empty()) return f.source_file;
    return {};
}

} // namespace

DebugModule::DebugModule(const ir::Module& module)
    :module_(module),primary_source_(primary_source_file(module)),debug_subprograms_(module.functions.size()),
     debug_locations_(module.functions.size()){
    if(!primary_source_.empty()) {
        debug_files_.emplace(primary_source_,1);
        for(const auto& f:module_.functions) {
            if(f.source_file.empty()||debug_files_.contains(f.source_file)) continue;
            debug_files_.emplace(f.source_file,next_debug_metadata_++);
        }
    }

    if(!primary_source_.empty()) {
        for(std::size_t i=0;i<module_.functions.size();++i) {
            const auto& f=module_.functions[i];
            if(!f.external_symbol&&!f.source_file.empty()) {
                debug_subprograms_[i]=next_debug_metadata_++;
                debug_locations_[i]=next_debug_metadata_++;
                subprogram_locations_.emplace(*debug_subprograms_[i],*debug_locations_[i]);
            }
        }
    }
}

std::size_t DebugModule::file_id(std::size_t function)const{
    return debug_subprograms_[function]?debug_files_.at(module_.functions[function].source_file):0;
}

void DebugModule::emit_source_filename(const ir::Module& source,llvm_text::LlvmModule& module){
    const auto primary=primary_source_file(source);
    const auto filename=primary.empty()?std::string("<memory>")
                                        :std::filesystem::path(primary).filename().string();
    module.append(llvm_text::LlvmModule::Section::header,
                  "source_filename = \""+llvm_text::escape_metadata(filename)+"\"\n");
}

void DebugModule::emit_metadata(llvm_text::LlvmModule& module)const{
    if(!primary_source_.empty()) {
        std::ostringstream out;
        const auto primary_path=std::filesystem::path(primary_source_);
        out<<"\n!llvm.dbg.cu = !{!0}\n"
           <<"!llvm.module.flags = !{!2, !3}\n"
           <<"!llvm.ident = !{!4}\n"
           <<"!0 = distinct !DICompileUnit(language: DW_LANG_C11, file: !1, producer: \"Quidra\", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug)\n"
           <<"!1 = !DIFile(filename: \""<<llvm_text::escape_metadata(primary_path.filename().string())
           <<"\", directory: \""<<llvm_text::escape_metadata(primary_path.parent_path().string())<<"\")\n"
           <<"!2 = !{i32 2, !\"Dwarf Version\", i32 4}\n"
           <<"!3 = !{i32 2, !\"Debug Info Version\", i32 3}\n"
           <<"!4 = !{!\"Quidra\"}\n"
           <<"!5 = !DISubroutineType(types: !6)\n"
           <<"!6 = !{}\n";

        for(const auto& [source,id]:debug_files_) {
            if(id==1) continue;
            const auto path=std::filesystem::path(source);
            out<<"!"<<id<<" = !DIFile(filename: \""<<llvm_text::escape_metadata(path.filename().string())
               <<"\", directory: \""<<llvm_text::escape_metadata(path.parent_path().string())<<"\")\n";
        }

        for(std::size_t i=0;i<module_.functions.size();++i) {
            if(!debug_subprograms_[i]) continue;
            const auto& f=module_.functions[i];
            const auto file=debug_files_.at(f.source_file);
            std::string display=f.entrypoint?"<top-level>":(f.source_function.empty()?f.name:f.source_function);
            using member_function_name::method_prefix;
            using member_function_name::constructor_prefix;
            if(display.rfind(method_prefix,0)==0) display.erase(0,method_prefix.size());
            else if(display.rfind(constructor_prefix,0)==0){
                // "$construct.Point.0" is shown as "Point.construct".
                display.erase(0,constructor_prefix.size());
                const auto dot=display.rfind('.');
                if(dot!=std::string::npos) display=display.substr(0,dot)+".construct";
            }
            if(const auto source=authored_specialization_display_name(f))
                display=source->name+(source->constructor?".construct":"");
            const auto linkage=f.entrypoint?std::string(abi::symbol_namespace::entry):abi::user_symbol(f.name);
            out<<"!"<<*debug_subprograms_[i]
               <<" = distinct !DISubprogram(name: \""<<llvm_text::escape_metadata(display)
               <<"\", linkageName: \""<<llvm_text::escape_metadata(linkage)
               <<"\", scope: !"<<file<<", file: !"<<file
               <<", line: "<<std::max<std::uint32_t>(1,f.source_line)
               <<", type: !5, scopeLine: "<<std::max<std::uint32_t>(1,f.source_line)
               <<", spFlags: DISPFlagDefinition, unit: !0)\n";
            if(debug_locations_[i]) {
                out<<"!"<<*debug_locations_[i]
                   <<" = !DILocation(line: "<<std::max<std::uint32_t>(1,f.source_line)
                   <<", column: "<<std::max<std::uint32_t>(1,f.source_column)
                   <<", scope: !"<<*debug_subprograms_[i]<<")\n";
            }
        }
        for(const auto& location:debug_statement_locations_) {
            out<<"!"<<location.id
               <<" = !DILocation(line: "<<location.line
               <<", column: "<<location.column
               <<", scope: !"<<location.scope<<")\n";
        }
        for(const auto& variable:debug_variables_) {
            out<<"!"<<variable.type_id
               <<" = !DIBasicType(name: \""<<llvm_text::escape_metadata(variable.type.name)
               <<"\", size: "<<variable.type.bits
               <<", encoding: "<<variable.type.encoding<<")\n";
            out<<"!"<<variable.id
               <<" = !DILocalVariable(name: \""<<llvm_text::escape_metadata(variable.name)<<"\"";
            if(variable.argument) out<<", arg: "<<variable.argument;
            out<<", scope: !"<<variable.scope
               <<", file: !"<<variable.file
               <<", line: "<<variable.line
               <<", type: !"<<variable.type_id<<")\n";
        }
        module.append(llvm_text::LlvmModule::Section::metadata, out.view());
    }
}

} // namespace quidra::llvm_backend
