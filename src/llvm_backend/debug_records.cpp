#include "llvm_backend/debug_records.hpp"

#include <string_view>

namespace quidra::llvm_backend {

std::string attach_debug_location(std::string text,std::size_t location) {
    auto position=text.find('\n');
    if(position==std::string::npos) return text;
    position+=1;
    bool saw_label=false;
    while(position<text.size()) {
        const auto end=text.find('\n',position);
        if(end==std::string::npos) break;
        const std::string_view line(text.data()+position,end-position);
        if(!saw_label) {
            if(!line.empty()&&line.back()==':') saw_label=true;
        } else if(line.size()>=2&&line[0]==' '&&line[1]==' '&&
                  line.find_first_not_of(' ')!=std::string_view::npos) {
            text.insert(end,", !dbg !"+std::to_string(location));
            break;
        }
        position=end+1;
    }
    return text;
}

std::optional<DebugBasicType> debug_basic_type(const Type& type) {
    if(const auto* numeric=numeric_kind_info(type.kind); numeric&&numeric->width!=0) {
        const char* encoding=numeric->family==NumericFamily::Real?"DW_ATE_float":
                             numeric->is_signed?"DW_ATE_signed":"DW_ATE_unsigned";
        return DebugBasicType{std::string(numeric->spelling),static_cast<std::size_t>(numeric->width),encoding};
    }
    // int and nat are stored as one i64 word (abi::bare_integer_layout): the
    // debugger shows the word.
    if(const auto* numeric=numeric_kind_info(type.kind); numeric&&is_bare_integer(type))
        return DebugBasicType{std::string(numeric->spelling),64,"DW_ATE_signed"};
    if(type.kind==TypeKind::Bool) return DebugBasicType{"bool",1,"DW_ATE_boolean"};
    return std::nullopt;
}

bool attach_debug_location_range(
    std::string& text,std::size_t begin,std::size_t end,std::size_t location) {
    auto position=begin;
    while(position<end&&position<text.size()) {
        auto line_end=text.find('\n',position);
        if(line_end==std::string::npos||line_end>end) line_end=end;
        const std::string_view line(text.data()+position,line_end-position);
        if(line.size()>=2&&line[0]==' '&&line[1]==' ') {
            const auto first=line.find_first_not_of(' ');
            if(first!=std::string_view::npos) {
                const auto trimmed=line.substr(first);
                const bool continuation=
                    trimmed=="]"||trimmed.back()=='['||
                    (trimmed.rfind("i64 ",0)==0&&trimmed.find("label %")!=std::string_view::npos);
                if(!continuation&&trimmed.find("!dbg !")==std::string_view::npos) {
                    text.insert(line_end,", !dbg !"+std::to_string(location));
                    return true;
                }
            }
        }
        if(line_end>=end) break;
        position=line_end+1;
    }
    return false;
}

} // namespace quidra::llvm_backend
