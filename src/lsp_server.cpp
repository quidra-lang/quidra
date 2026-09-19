#include "lsp_server.hpp"

#include "quidra/compiler.hpp"
#include "quidra/diagnostic.hpp"
#include "quidra/formatter.hpp"
#include "quidra/language.hpp"
#include "quidra/lexer.hpp"
#include "quidra/parser.hpp"
#include "quidra/types.hpp"
#include "quidra/version.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace quidra::cli {
namespace {

struct Json {
    enum class Kind { Null, Bool, Number, String, Object, Array };
    Kind kind{Kind::Null};
    bool boolean{};
    std::string text;
    std::map<std::string,Json> object;
    std::vector<Json> array;
};

void append_utf8(std::string& output,std::uint32_t value) {
    if(value<=0x7fU) output.push_back(static_cast<char>(value));
    else if(value<=0x7ffU) {
        output.push_back(static_cast<char>(0xc0U|(value>>6U)));
        output.push_back(static_cast<char>(0x80U|(value&0x3fU)));
    } else if(value<=0xffffU) {
        output.push_back(static_cast<char>(0xe0U|(value>>12U)));
        output.push_back(static_cast<char>(0x80U|((value>>6U)&0x3fU)));
        output.push_back(static_cast<char>(0x80U|(value&0x3fU)));
    } else {
        output.push_back(static_cast<char>(0xf0U|(value>>18U)));
        output.push_back(static_cast<char>(0x80U|((value>>12U)&0x3fU)));
        output.push_back(static_cast<char>(0x80U|((value>>6U)&0x3fU)));
        output.push_back(static_cast<char>(0x80U|(value&0x3fU)));
    }
}

class JsonParser {
public:
    explicit JsonParser(std::string_view input):input_(input){}
    Json parse() {
        auto result=value(0);
        spaces();
        if(index_!=input_.size()) fail("trailing data");
        return result;
    }
private:
    std::string_view input_;
    std::size_t index_{};

    [[noreturn]] void fail(const char* message) const {
        throw std::runtime_error(std::string("LSP JSON: ")+message);
    }
    void spaces() {
        while(index_<input_.size()&&std::isspace(static_cast<unsigned char>(input_[index_]))) ++index_;
    }
    bool take(char c) {
        spaces();
        if(index_>=input_.size()||input_[index_]!=c) return false;
        ++index_;
        return true;
    }
    void expect(char c) { if(!take(c)) fail("unexpected token"); }
    unsigned hex4() {
        if(index_+4>input_.size()) fail("truncated unicode escape");
        unsigned result=0;
        for(unsigned i=0;i<4;++i) {
            const char c=input_[index_++];
            result<<=4U;
            if(c>='0'&&c<='9') result|=static_cast<unsigned>(c-'0');
            else if(c>='a'&&c<='f') result|=10U+static_cast<unsigned>(c-'a');
            else if(c>='A'&&c<='F') result|=10U+static_cast<unsigned>(c-'A');
            else fail("invalid unicode escape");
        }
        return result;
    }
    std::string string() {
        spaces();
        if(index_>=input_.size()||input_[index_]!='"') fail("expected string");
        ++index_;
        std::string result;
        while(index_<input_.size()) {
            const unsigned char raw=static_cast<unsigned char>(input_[index_++]);
            if(raw=='"') return result;
            if(raw<0x20U) fail("control character in string");
            if(raw!='\\') {
                result.push_back(static_cast<char>(raw));
                continue;
            }
            if(index_>=input_.size()) fail("truncated escape");
            const char escaped=input_[index_++];
            switch(escaped) {
                case '"': result.push_back('"'); break;
                case '\\': result.push_back('\\'); break;
                case '/': result.push_back('/'); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                case 'u': {
                    std::uint32_t point=hex4();
                    if(point>=0xd800U&&point<=0xdbffU) {
                        if(index_+2>input_.size()||input_[index_]!='\\'||input_[index_+1]!='u')
                            fail("missing low surrogate");
                        index_+=2;
                        const auto low=hex4();
                        if(low<0xdc00U||low>0xdfffU) fail("invalid low surrogate");
                        point=0x10000U+((point-0xd800U)<<10U)+(low-0xdc00U);
                    } else if(point>=0xdc00U&&point<=0xdfffU) {
                        fail("unpaired low surrogate");
                    }
                    append_utf8(result,point);
                    break;
                }
                default: fail("invalid escape");
            }
        }
        fail("unterminated string");
    }
    Json value(std::size_t depth) {
        if(depth>128) fail("nesting too deep");
        spaces();
        if(index_>=input_.size()) fail("unexpected end");
        const char c=input_[index_];
        if(c=='"') {
            Json result; result.kind=Json::Kind::String; result.text=string(); return result;
        }
        if(c=='{') return object(depth+1);
        if(c=='[') return array(depth+1);
        if(input_.substr(index_,4)=="null") { index_+=4; return Json{}; }
        if(input_.substr(index_,4)=="true") {
            index_+=4; Json result; result.kind=Json::Kind::Bool; result.boolean=true; return result;
        }
        if(input_.substr(index_,5)=="false") {
            index_+=5; Json result; result.kind=Json::Kind::Bool; return result;
        }
        if(c=='-'||(c>='0'&&c<='9')) {
            const auto start=index_;
            if(c=='-') ++index_;
            if(index_>=input_.size()) fail("invalid number");
            if(input_[index_]=='0') ++index_;
            else {
                if(input_[index_]<'1'||input_[index_]>'9') fail("invalid number");
                while(index_<input_.size()&&std::isdigit(static_cast<unsigned char>(input_[index_]))) ++index_;
            }
            if(index_<input_.size()&&input_[index_]=='.') {
                ++index_;
                if(index_>=input_.size()||!std::isdigit(static_cast<unsigned char>(input_[index_])))
                    fail("invalid fraction");
                while(index_<input_.size()&&std::isdigit(static_cast<unsigned char>(input_[index_]))) ++index_;
            }
            if(index_<input_.size()&&(input_[index_]=='e'||input_[index_]=='E')) {
                ++index_;
                if(index_<input_.size()&&(input_[index_]=='+'||input_[index_]=='-')) ++index_;
                if(index_>=input_.size()||!std::isdigit(static_cast<unsigned char>(input_[index_])))
                    fail("invalid exponent");
                while(index_<input_.size()&&std::isdigit(static_cast<unsigned char>(input_[index_]))) ++index_;
            }
            Json result; result.kind=Json::Kind::Number;
            result.text=std::string(input_.substr(start,index_-start));
            return result;
        }
        fail("invalid value");
    }
    Json object(std::size_t depth) {
        expect('{');
        Json result; result.kind=Json::Kind::Object;
        if(take('}')) return result;
        for(;;) {
            auto key=string();
            expect(':');
            result.object[std::move(key)]=value(depth);
            if(take('}')) return result;
            expect(',');
        }
    }
    Json array(std::size_t depth) {
        expect('[');
        Json result; result.kind=Json::Kind::Array;
        if(take(']')) return result;
        for(;;) {
            result.array.push_back(value(depth));
            if(take(']')) return result;
            expect(',');
        }
    }
};

const Json* member(const Json& value,std::string_view name) {
    if(value.kind!=Json::Kind::Object) return nullptr;
    const auto found=value.object.find(std::string(name));
    return found==value.object.end()?nullptr:&found->second;
}
std::optional<std::string> string_member(const Json& value,std::string_view name) {
    const auto* found=member(value,name);
    if(!found||found->kind!=Json::Kind::String) return std::nullopt;
    return found->text;
}
std::optional<bool> bool_member(const Json& value,std::string_view name) {
    const auto* found=member(value,name);
    if(!found||found->kind!=Json::Kind::Bool) return std::nullopt;
    return found->boolean;
}
const Json& required(const Json& value,std::string_view name,Json::Kind kind) {
    const auto* found=member(value,name);
    if(!found||found->kind!=kind)
        throw std::runtime_error("invalid LSP field '"+std::string(name)+"'");
    return *found;
}
std::string required_string(const Json& value,std::string_view name) {
    const auto found=string_member(value,name);
    if(!found) throw std::runtime_error("invalid LSP string '"+std::string(name)+"'");
    return *found;
}

std::string escape(std::string_view input) {
    std::ostringstream output;
    const char* hex="0123456789abcdef";
    for(const unsigned char c:input) {
        switch(c) {
            case '"': output<<"\\\""; break;
            case '\\': output<<"\\\\"; break;
            case '\b': output<<"\\b"; break;
            case '\f': output<<"\\f"; break;
            case '\n': output<<"\\n"; break;
            case '\r': output<<"\\r"; break;
            case '\t': output<<"\\t"; break;
            default:
                if(c<0x20U) output<<"\\u00"<<hex[(c>>4U)&0xfU]<<hex[c&0xfU];
                else output<<static_cast<char>(c);
        }
    }
    return output.str();
}
std::string id_text(const Json* id) {
    if(!id||id->kind==Json::Kind::Null) return "null";
    if(id->kind==Json::Kind::Number) return id->text;
    if(id->kind==Json::Kind::String) return "\""+escape(id->text)+"\"";
    throw std::runtime_error("invalid LSP id");
}

std::string trim(std::string text) {
    while(!text.empty()&&std::isspace(static_cast<unsigned char>(text.front()))) text.erase(text.begin());
    while(!text.empty()&&std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
    return text;
}
bool read_message(std::string& body) {
    std::string line;
    std::optional<std::size_t> length;
    while(std::getline(std::cin,line)) {
        if(!line.empty()&&line.back()=='\r') line.pop_back();
        if(line.empty()) break;
        constexpr std::string_view prefix="Content-Length:";
        if(line.rfind(prefix,0)==0) {
            const auto raw=trim(line.substr(prefix.size()));
            std::size_t consumed=0;
            const auto parsed=std::stoull(raw,&consumed);
            if(consumed!=raw.size()||parsed>64ULL*1024ULL*1024ULL)
                throw std::runtime_error("invalid LSP Content-Length");
            length=static_cast<std::size_t>(parsed);
        }
    }
    if(!length) {
        if(std::cin.eof()) return false;
        throw std::runtime_error("missing LSP Content-Length");
    }
    body.resize(*length);
    std::cin.read(body.data(),static_cast<std::streamsize>(*length));
    if(static_cast<std::size_t>(std::cin.gcount())!=*length)
        throw std::runtime_error("truncated LSP message");
    return true;
}
void write_message(std::string_view body) {
    std::cout<<"Content-Length: "<<body.size()<<"\r\n\r\n"<<body<<std::flush;
}
void respond(const Json* id,std::string_view result) {
    write_message("{\"jsonrpc\":\"2.0\",\"id\":"+id_text(id)+",\"result\":"+std::string(result)+"}");
}
void fail_response(const Json* id,int code,std::string_view message) {
    write_message("{\"jsonrpc\":\"2.0\",\"id\":"+id_text(id)+
        ",\"error\":{\"code\":"+std::to_string(code)+",\"message\":\""+escape(message)+"\"}}");
}
void notify(std::string_view method,std::string_view params) {
    write_message("{\"jsonrpc\":\"2.0\",\"method\":\""+escape(method)+"\",\"params\":"+std::string(params)+"}");
}

std::optional<fs::path> uri_path(std::string_view uri) {
    constexpr std::string_view prefix="file://";
    if(uri.rfind(prefix,0)!=0) return std::nullopt;
    std::string encoded(uri.substr(prefix.size()));
    if(encoded.rfind("localhost/",0)==0) encoded.erase(0,9);
    std::string decoded;
    decoded.reserve(encoded.size());
    auto digit=[](char c)->int {
        if(c>='0'&&c<='9') return c-'0';
        if(c>='a'&&c<='f') return 10+c-'a';
        if(c>='A'&&c<='F') return 10+c-'A';
        return -1;
    };
    for(std::size_t i=0;i<encoded.size();++i) {
        if(encoded[i]!='%') { decoded.push_back(encoded[i]); continue; }
        if(i+2>=encoded.size()) return std::nullopt;
        const int high=digit(encoded[i+1]),low=digit(encoded[i+2]);
        if(high<0||low<0) return std::nullopt;
        decoded.push_back(static_cast<char>((high<<4)|low));
        i+=2;
    }
#ifdef _WIN32
    if(decoded.size()>=3&&decoded[0]=='/'&&
       std::isalpha(static_cast<unsigned char>(decoded[1]))&&decoded[2]==':')
        decoded.erase(decoded.begin());
#endif
    return fs::path(decoded);
}
std::string read_file(const fs::path& path) {
    std::ifstream input(path,std::ios::binary);
    if(!input) throw std::runtime_error("cannot read LSP document: "+path.string());
    std::ostringstream output; output<<input.rdbuf(); return output.str();
}
struct LspPosition {
    std::size_t line{};
    std::size_t character{};
};

std::size_t utf8_width(unsigned char lead) {
    if(lead<0x80U) return 1;
    if((lead&0xe0U)==0xc0U) return 2;
    if((lead&0xf0U)==0xe0U) return 3;
    if((lead&0xf8U)==0xf0U) return 4;
    return 1;
}

std::uint32_t utf8_code_point(std::string_view source,std::size_t offset,std::size_t width) {
    const auto lead=static_cast<unsigned char>(source[offset]);
    if(width==1||offset+width>source.size()) return lead;
    std::uint32_t value=lead&static_cast<unsigned char>((1U<<(7U-static_cast<unsigned>(width)))-1U);
    for(std::size_t index=1;index<width;++index) {
        const auto tail=static_cast<unsigned char>(source[offset+index]);
        if((tail&0xc0U)!=0x80U) return lead;
        value=(value<<6U)|(tail&0x3fU);
    }
    return value;
}

LspPosition lsp_position(std::string_view source,std::size_t raw_offset) {
    const auto limit=std::min(raw_offset,source.size());
    LspPosition position;
    std::size_t offset=0;
    while(offset<limit) {
        const auto lead=static_cast<unsigned char>(source[offset]);
        if(lead=='\n') {
            ++position.line;
            position.character=0;
            ++offset;
            continue;
        }
        const auto width=utf8_width(lead);
        if(width>limit-offset) {
            ++position.character;
            ++offset;
            continue;
        }
        const auto code_point=utf8_code_point(source,offset,width);
        position.character+=code_point>0xffffU?2U:1U;
        offset+=width;
    }
    return position;
}

std::string diagnostic_text(const Diagnostic& diagnostic,std::string_view source) {
    const auto start=lsp_position(source,diagnostic.span.start.offset);
    const auto end=lsp_position(source,diagnostic.span.end.offset);
    std::ostringstream output;
    output<<"{\"range\":{\"start\":{\"line\":"<<start.line
          <<",\"character\":"<<start.character
          <<"},\"end\":{\"line\":"<<end.line
          <<",\"character\":"<<end.character
          <<"}},\"severity\":1,\"code\":\""<<escape(diagnostic.code)
          <<"\",\"source\":\"quidra\",\"message\":\""<<escape(diagnostic.message)<<"\"}";
    return output.str();
}


std::size_t required_index(const Json& value,std::string_view name) {
    const auto& number=required(value,name,Json::Kind::Number);
    std::size_t consumed=0;
    unsigned long long parsed=0;
    try {
        parsed=std::stoull(number.text,&consumed);
    } catch(...) {
        throw std::runtime_error("invalid LSP integer '"+std::string(name)+"'");
    }
    if(consumed!=number.text.size())
        throw std::runtime_error("invalid LSP integer '"+std::string(name)+"'");
    return static_cast<std::size_t>(parsed);
}

LspPosition request_position(const Json& message) {
    const auto& params=required(message,"params",Json::Kind::Object);
    const auto& position=required(params,"position",Json::Kind::Object);
    return LspPosition{required_index(position,"line"),required_index(position,"character")};
}

std::size_t raw_offset(std::string_view source,LspPosition target) {
    std::size_t offset=0;
    std::size_t line=0;
    while(offset<source.size()&&line<target.line) {
        if(source[offset++]=='\n') ++line;
    }
    if(line!=target.line) return source.size();

    std::size_t character=0;
    while(offset<source.size()&&source[offset]!='\n'&&character<target.character) {
        const auto width=utf8_width(static_cast<unsigned char>(source[offset]));
        const auto available=std::min(width,source.size()-offset);
        const auto point=utf8_code_point(source,offset,available);
        const auto units=point>0xffffU?2U:1U;
        if(character+units>target.character) break;
        character+=units;
        offset+=available;
    }
    return offset;
}

bool contains_offset(SourceSpan span,std::size_t offset) {
    if(span.start.offset==span.end.offset) return offset==span.start.offset;
    return offset>=span.start.offset&&offset<span.end.offset;
}

std::string range_json(std::string_view source,SourceSpan span) {
    const auto start=lsp_position(source,span.start.offset);
    const auto end=lsp_position(source,span.end.offset);
    std::ostringstream out;
    out<<"{\"start\":{\"line\":"<<start.line<<",\"character\":"<<start.character
       <<"},\"end\":{\"line\":"<<end.line<<",\"character\":"<<end.character<<"}}";
    return out.str();
}

std::string source_type_name(const TypeName& type,std::string_view source) {
    const auto start=std::min(type.span.start.offset,source.size());
    const auto end=std::min(type.span.end.offset,source.size());
    if(end>start) {
        auto text=trim(std::string(source.substr(start,end-start)));
        if(!text.empty()) return text;
    }
    return type.name.empty()?"auto":type.name;
}

std::string parameter_label(const FunctionParameterType& parameter) {
    std::string result;
    if(parameter.is_const) result+="const ";
    result+=type_name(parameter.type);
    result+=parameter.writable?" &":" ";
    result+=parameter.name;
    return result;
}

std::string signature_label(std::string_view name,const FunctionType& function) {
    std::string result=type_name(function.result)+" "+std::string(name)+"(";
    for(std::size_t i=0;i<function.parameters.size();++i) {
        if(i) result+=", ";
        result+=parameter_label(function.parameters[i]);
    }
    result+=")";
    return result;
}

void consider_expression(const Expr& expression,std::size_t offset,const Expr*& best);

void consider_statement(const Stmt& statement,std::size_t offset,const Expr*& best) {
    if(!contains_offset(statement.span,offset)) return;
    std::visit([&](const auto& node) {
        using T=std::decay_t<decltype(node)>;
        if constexpr(std::is_same_v<T,BindingStmt>) {
            if(node.value) consider_expression(*node.value,offset,best);
        } else if constexpr(std::is_same_v<T,AssignStmt>) {
            consider_expression(*node.target,offset,best);
            consider_expression(*node.value,offset,best);
        } else if constexpr(std::is_same_v<T,ReturnStmt>) {
            if(node.value) consider_expression(*node.value,offset,best);
        } else if constexpr(std::is_same_v<T,ExprStmt>) {
            consider_expression(*node.value,offset,best);
        } else if constexpr(std::is_same_v<T,IfStmt>) {
            consider_expression(*node.condition,offset,best);
            for(const auto& child:node.then_body) consider_statement(*child,offset,best);
            for(const auto& child:node.else_body) consider_statement(*child,offset,best);
        } else if constexpr(std::is_same_v<T,WhileStmt>) {
            consider_expression(*node.condition,offset,best);
            for(const auto& child:node.body) consider_statement(*child,offset,best);
        } else if constexpr(std::is_same_v<T,ForStmt>) {
            consider_expression(*node.iterable,offset,best);
            for(const auto& child:node.body) consider_statement(*child,offset,best);
        } else if constexpr(std::is_same_v<T,MatchStmt>) {
            consider_expression(*node.value,offset,best);
            for(const auto& current:node.cases)
                for(const auto& child:current.body) consider_statement(*child,offset,best);
        }
    },statement.data);
}

void consider_expression(const Expr& expression,std::size_t offset,const Expr*& best) {
    if(!contains_offset(expression.span,offset)) return;
    if(!best||
       expression.span.end.offset-expression.span.start.offset<
       best->span.end.offset-best->span.start.offset) {
        best=&expression;
    }
    std::visit([&](const auto& node) {
        using T=std::decay_t<decltype(node)>;
        if constexpr(std::is_same_v<T,UnaryExpr>) {
            consider_expression(*node.operand,offset,best);
        } else if constexpr(std::is_same_v<T,TryExpr>) {
            consider_expression(*node.value,offset,best);
        } else if constexpr(std::is_same_v<T,ArrayExpr>) {
            for(const auto& element:node.elements) consider_expression(*element,offset,best);
        } else if constexpr(std::is_same_v<T,IndexExpr>) {
            consider_expression(*node.base,offset,best);
            for(const auto& item:node.items) {
                if(item.index) consider_expression(*item.index,offset,best);
                if(item.start) consider_expression(*item.start,offset,best);
                if(item.stop) consider_expression(*item.stop,offset,best);
                if(item.step) consider_expression(*item.step,offset,best);
            }
        } else if constexpr(std::is_same_v<T,MemberExpr>) {
            consider_expression(*node.base,offset,best);
        } else if constexpr(std::is_same_v<T,MethodCallExpr>) {
            consider_expression(*node.receiver,offset,best);
            for(const auto& arg:node.args) consider_expression(*arg.value,offset,best);
        } else if constexpr(std::is_same_v<T,StringTemplateExpr>) {
            for(const auto& part:node.expressions) consider_expression(*part,offset,best);
        } else if constexpr(std::is_same_v<T,BinaryExpr>) {
            consider_expression(*node.left,offset,best);
            consider_expression(*node.right,offset,best);
        } else if constexpr(std::is_same_v<T,CallExpr>) {
            for(const auto& arg:node.args) consider_expression(*arg.value,offset,best);
        }
    },expression.data);
}

const Expr* expression_at(const Program& program,std::size_t offset) {
    const Expr* best=nullptr;
    for(const auto& declaration:program.classes) {
        for(const auto& field:declaration.fields)
            if(field.default_value) consider_expression(*field.default_value,offset,best);
        for(const auto& method:declaration.methods) {
            for(const auto& parameter:method.parameters)
                if(parameter.default_value) consider_expression(*parameter.default_value,offset,best);
            for(const auto& statement:method.body) consider_statement(*statement,offset,best);
        }
    }
    for(const auto& function:program.functions) {
        for(const auto& parameter:function.parameters)
            if(parameter.default_value) consider_expression(*parameter.default_value,offset,best);
        for(const auto& statement:function.body) consider_statement(*statement,offset,best);
    }
    for(const auto& statement:program.statements) consider_statement(*statement,offset,best);
    return best;
}


void consider_call(const Expr& expression,std::size_t offset,const Expr*& best) {
    if(!contains_offset(expression.span,offset)) return;
    if(std::holds_alternative<CallExpr>(expression.data)||
       std::holds_alternative<MethodCallExpr>(expression.data)) {
        if(!best||
           expression.span.end.offset-expression.span.start.offset<
           best->span.end.offset-best->span.start.offset) {
            best=&expression;
        }
    }
    std::visit([&](const auto& node) {
        using T=std::decay_t<decltype(node)>;
        if constexpr(std::is_same_v<T,UnaryExpr>) consider_call(*node.operand,offset,best);
        else if constexpr(std::is_same_v<T,TryExpr>) consider_call(*node.value,offset,best);
        else if constexpr(std::is_same_v<T,ArrayExpr>) {
            for(const auto& element:node.elements) consider_call(*element,offset,best);
        } else if constexpr(std::is_same_v<T,IndexExpr>) {
            consider_call(*node.base,offset,best);
            for(const auto& item:node.items) {
                if(item.index) consider_call(*item.index,offset,best);
                if(item.start) consider_call(*item.start,offset,best);
                if(item.stop) consider_call(*item.stop,offset,best);
                if(item.step) consider_call(*item.step,offset,best);
            }
        } else if constexpr(std::is_same_v<T,MemberExpr>) consider_call(*node.base,offset,best);
        else if constexpr(std::is_same_v<T,MethodCallExpr>) {
            consider_call(*node.receiver,offset,best);
            for(const auto& arg:node.args) consider_call(*arg.value,offset,best);
        } else if constexpr(std::is_same_v<T,StringTemplateExpr>) {
            for(const auto& part:node.expressions) consider_call(*part,offset,best);
        } else if constexpr(std::is_same_v<T,BinaryExpr>) {
            consider_call(*node.left,offset,best);
            consider_call(*node.right,offset,best);
        } else if constexpr(std::is_same_v<T,CallExpr>) {
            for(const auto& arg:node.args) consider_call(*arg.value,offset,best);
        }
    },expression.data);
}

void consider_call_statement(const Stmt& statement,std::size_t offset,const Expr*& best) {
    if(!contains_offset(statement.span,offset)) return;
    std::visit([&](const auto& node) {
        using T=std::decay_t<decltype(node)>;
        if constexpr(std::is_same_v<T,BindingStmt>) {
            if(node.value) consider_call(*node.value,offset,best);
        } else if constexpr(std::is_same_v<T,AssignStmt>) {
            consider_call(*node.target,offset,best); consider_call(*node.value,offset,best);
        } else if constexpr(std::is_same_v<T,ReturnStmt>) {
            if(node.value) consider_call(*node.value,offset,best);
        } else if constexpr(std::is_same_v<T,ExprStmt>) consider_call(*node.value,offset,best);
        else if constexpr(std::is_same_v<T,IfStmt>) {
            consider_call(*node.condition,offset,best);
            for(const auto& child:node.then_body) consider_call_statement(*child,offset,best);
            for(const auto& child:node.else_body) consider_call_statement(*child,offset,best);
        } else if constexpr(std::is_same_v<T,WhileStmt>) {
            consider_call(*node.condition,offset,best);
            for(const auto& child:node.body) consider_call_statement(*child,offset,best);
        } else if constexpr(std::is_same_v<T,ForStmt>) {
            consider_call(*node.iterable,offset,best);
            for(const auto& child:node.body) consider_call_statement(*child,offset,best);
        } else if constexpr(std::is_same_v<T,MatchStmt>) {
            consider_call(*node.value,offset,best);
            for(const auto& current:node.cases)
                for(const auto& child:current.body) consider_call_statement(*child,offset,best);
        }
    },statement.data);
}

const Expr* call_at(const Program& program,std::size_t offset) {
    const Expr* best=nullptr;
    for(const auto& declaration:program.classes) {
        for(const auto& field:declaration.fields)
            if(field.default_value) consider_call(*field.default_value,offset,best);
        for(const auto& method:declaration.methods) {
            for(const auto& parameter:method.parameters)
                if(parameter.default_value) consider_call(*parameter.default_value,offset,best);
            for(const auto& statement:method.body) consider_call_statement(*statement,offset,best);
        }
    }
    for(const auto& function:program.functions) {
        for(const auto& parameter:function.parameters)
            if(parameter.default_value) consider_call(*parameter.default_value,offset,best);
        for(const auto& statement:function.body) consider_call_statement(*statement,offset,best);
    }
    for(const auto& statement:program.statements) consider_call_statement(*statement,offset,best);
    return best;
}

bool body_contains(const std::vector<StmtPtr>& body,std::size_t offset) {
    if(body.empty()) return false;
    return offset>=body.front()->span.start.offset&&offset<body.back()->span.end.offset;
}

std::optional<SourceSpan> identifier_span(
    const std::vector<Token>& tokens,SourceSpan within,std::string_view name,
    std::size_t minimum_offset=0) {
    for(const auto& token:tokens) {
        if(token.kind!=TokenKind::Identifier||token.text!=name) continue;
        if(token.span.start.offset<within.start.offset||
           token.span.end.offset>within.end.offset||
           token.span.start.offset<minimum_offset) continue;
        return token.span;
    }
    return std::nullopt;
}

std::optional<SourceSpan> local_definition(
    const std::vector<StmtPtr>& body,std::string_view name,std::size_t offset,
    const std::vector<Token>& tokens) {
    std::optional<SourceSpan> result;
    for(const auto& statement:body) {
        if(statement->span.start.offset>offset) break;
        std::visit([&](const auto& node) {
            using T=std::decay_t<decltype(node)>;
            if constexpr(std::is_same_v<T,BindingStmt>) {
                if(node.name==name) {
                    const auto found=identifier_span(
                        tokens,statement->span,name,node.declared_type.span.end.offset);
                    if(found&&found->start.offset<=offset) result=found;
                }
            } else if constexpr(std::is_same_v<T,IfStmt>) {
                if(body_contains(node.then_body,offset)) {
                    if(auto nested=local_definition(node.then_body,name,offset,tokens)) result=nested;
                } else if(body_contains(node.else_body,offset)) {
                    if(auto nested=local_definition(node.else_body,name,offset,tokens)) result=nested;
                }
            } else if constexpr(std::is_same_v<T,WhileStmt>) {
                if(body_contains(node.body,offset))
                    if(auto nested=local_definition(node.body,name,offset,tokens)) result=nested;
            } else if constexpr(std::is_same_v<T,ForStmt>) {
                if(body_contains(node.body,offset)) {
                    if(node.name==name) {
                        if(auto found=identifier_span(tokens,statement->span,name))
                            result=found;
                    }
                    if(auto nested=local_definition(node.body,name,offset,tokens)) result=nested;
                }
            } else if constexpr(std::is_same_v<T,MatchStmt>) {
                for(const auto& current:node.cases) {
                    if(!body_contains(current.body,offset)) continue;
                    if(current.binder&&*current.binder==name) {
                        if(auto found=identifier_span(tokens,current.span,name)) result=found;
                    }
                    if(auto nested=local_definition(current.body,name,offset,tokens)) result=nested;
                    break;
                }
            }
        },statement->data);
    }
    return result;
}

Program root_program(std::string_view source) {
    return Parser(Lexer(source).scan()).parse();
}

std::optional<SourceSpan> definition_span(
    const Program& program,std::string_view name,std::size_t offset,
    const std::vector<Token>& tokens) {
    for(const auto& declaration:program.classes) {
        if(!contains_offset(declaration.span,offset)) continue;
        for(const auto& method:declaration.methods) {
            if(!contains_offset(method.span,offset)) continue;
            for(const auto& parameter:method.parameters) {
                if(parameter.name==name&&parameter.span.start.offset<=offset) return parameter.span;
            }
            if(auto local=local_definition(method.body,name,offset,tokens)) return local;
            for(const auto& field:declaration.fields) {
                if(field.name==name) {
                    if(auto found=identifier_span(tokens,field.span,name)) return found;
                }
            }
        }
    }

    for(const auto& function:program.functions) {
        if(!contains_offset(function.span,offset)) continue;
        for(const auto& parameter:function.parameters) {
            if(parameter.name==name&&parameter.span.start.offset<=offset) return parameter.span;
        }
        if(auto local=local_definition(function.body,name,offset,tokens)) return local;
    }

    if(auto local=local_definition(program.statements,name,offset,tokens)) return local;

    for(const auto& function:program.functions) {
        if(function.name==name) {
            if(auto found=identifier_span(tokens,function.span,name,function.return_type.span.end.offset))
                return found;
        }
    }
    for(const auto& declaration:program.classes) {
        if(declaration.name==name) {
            if(auto found=identifier_span(tokens,declaration.span,name)) return found;
        }
    }
    return std::nullopt;
}

struct CompletionSymbol {
    std::string name;
    int kind{};
    std::string detail;
};

void add_local_completions(
    std::vector<CompletionSymbol>& out,const std::vector<StmtPtr>& body,
    std::size_t offset,std::string_view source) {
    for(const auto& statement:body) {
        if(statement->span.start.offset>offset) break;
        std::visit([&](const auto& node) {
            using T=std::decay_t<decltype(node)>;
            if constexpr(std::is_same_v<T,BindingStmt>) {
                out.push_back({node.name,6,source_type_name(node.declared_type,source)});
            } else if constexpr(std::is_same_v<T,IfStmt>) {
                if(body_contains(node.then_body,offset))
                    add_local_completions(out,node.then_body,offset,source);
                else if(body_contains(node.else_body,offset))
                    add_local_completions(out,node.else_body,offset,source);
            } else if constexpr(std::is_same_v<T,WhileStmt>) {
                if(body_contains(node.body,offset))
                    add_local_completions(out,node.body,offset,source);
            } else if constexpr(std::is_same_v<T,ForStmt>) {
                if(body_contains(node.body,offset)) {
                    out.push_back({node.name,6,"loop binding"});
                    add_local_completions(out,node.body,offset,source);
                }
            } else if constexpr(std::is_same_v<T,MatchStmt>) {
                for(const auto& current:node.cases) {
                    if(!body_contains(current.body,offset)) continue;
                    if(current.binder) out.push_back({*current.binder,6,"match binding"});
                    add_local_completions(out,current.body,offset,source);
                    break;
                }
            }
        },statement->data);
    }
}

std::vector<CompletionSymbol> completions_at(
    const Program& program,std::string_view source,std::size_t offset) {
    std::vector<CompletionSymbol> result;
    for(const auto& builtin:builtin_callables)
        result.push_back({std::string(builtin.name),3,"built-in"});
    for(const auto& function:program.functions)
        result.push_back({function.name,3,"function"});
    for(const auto& declaration:program.classes)
        result.push_back({declaration.name,7,"class"});
    for(const auto& import:program.imports)
        result.push_back({import.alias,9,"module"});

    bool inside_callable=false;
    for(const auto& declaration:program.classes) {
        if(!contains_offset(declaration.span,offset)) continue;
        for(const auto& method:declaration.methods) {
            if(!contains_offset(method.span,offset)) continue;
            inside_callable=true;
            for(const auto& field:declaration.fields)
                result.push_back({field.name,5,source_type_name(field.type,source)});
            for(const auto& parameter:method.parameters)
                result.push_back({parameter.name,6,source_type_name(parameter.type,source)});
            add_local_completions(result,method.body,offset,source);
        }
    }
    if(!inside_callable) {
        for(const auto& function:program.functions) {
            if(!contains_offset(function.span,offset)) continue;
            inside_callable=true;
            for(const auto& parameter:function.parameters)
                result.push_back({parameter.name,6,source_type_name(parameter.type,source)});
            add_local_completions(result,function.body,offset,source);
            break;
        }
    }
    if(!inside_callable) add_local_completions(result,program.statements,offset,source);

    std::unordered_set<std::string> seen;
    std::vector<CompletionSymbol> unique;
    for(auto& item:result) {
        if(seen.insert(item.name).second) unique.push_back(std::move(item));
    }
    std::sort(unique.begin(),unique.end(),[](const auto& left,const auto& right) {
        return left.name<right.name;
    });
    return unique;
}


bool same_span(SourceSpan left,SourceSpan right) {
    return left.start.offset==right.start.offset&&left.end.offset==right.end.offset;
}

std::optional<Token> identifier_token_at(std::string_view source,std::size_t offset) {
    for(const auto& token:Lexer(source).scan()) {
        if(token.kind==TokenKind::Identifier&&contains_offset(token.span,offset)) return token;
    }
    return std::nullopt;
}

std::optional<SourceSpan> resolved_definition_span(
    const Program& program,std::string_view name,std::size_t offset,
    const std::vector<Token>& tokens) {
    if(auto found=definition_span(program,name,offset,tokens)) return found;

    const auto* expression=expression_at(program,offset);
    if(!expression) return std::nullopt;
    const bool method=std::holds_alternative<MethodCallExpr>(expression->data);
    const bool member_access=std::holds_alternative<MemberExpr>(expression->data);
    if(!method&&!member_access) return std::nullopt;

    std::optional<SourceSpan> candidate;
    std::size_t matches=0;
    for(const auto& declaration:program.classes) {
        if(method) {
            for(const auto& item:declaration.methods) {
                if(item.name!=name) continue;
                if(auto span=identifier_span(tokens,item.span,name,item.return_type.span.end.offset)) {
                    candidate=span;
                    ++matches;
                }
            }
        } else {
            for(const auto& field:declaration.fields) {
                if(field.name!=name) continue;
                if(auto span=identifier_span(tokens,field.span,name)) {
                    candidate=span;
                    ++matches;
                }
            }
        }
    }
    return matches==1?candidate:std::nullopt;
}

std::optional<SourceSpan> semantic_member_definition_span(
    const CheckedProgram& checked,const Expr* expression,std::string_view name,
    const std::vector<Token>& tokens) {
    if(!expression) return std::nullopt;

    if(std::holds_alternative<MethodCallExpr>(expression->data)) {
        const auto method=checked.method_calls.find(expression);
        if(method==checked.method_calls.end()) return std::nullopt;
        for(const auto& declaration:checked.program.classes) {
            const auto info=checked.classes.find(declaration.name);
            if(info==checked.classes.end()) continue;
            const auto target=info->second.methods.find(std::string(name));
            if(target==info->second.methods.end()||
               target->second!=method->second.internal_name) continue;
            for(const auto& item:declaration.methods) {
                if(item.name!=name) continue;
                if(auto span=identifier_span(
                       tokens,item.span,name,item.return_type.span.end.offset))
                    return span;
            }
        }
    }

    if(std::holds_alternative<MemberExpr>(expression->data)) {
        const auto field=checked.field_accesses.find(expression);
        if(field==checked.field_accesses.end()) return std::nullopt;
        for(const auto& declaration:checked.program.classes) {
            if(declaration.name!=field->second.owner) continue;
            for(const auto& item:declaration.fields) {
                if(item.name!=name) continue;
                if(auto span=identifier_span(tokens,item.span,name)) return span;
            }
        }
    }
    return std::nullopt;
}

std::optional<SourceSpan> resolved_definition_span(
    const CheckedProgram& checked,std::string_view source,std::string_view name,
    std::size_t offset,const std::vector<Token>& tokens) {
    const auto* expression=expression_at(checked.program,offset);
    if(auto semantic=semantic_member_definition_span(
           checked,expression,name,tokens))
        return semantic;
    return resolved_definition_span(checked.program,name,offset,tokens);
}

std::optional<SourceSpan> resolved_definition_span(
    const CheckedProgram& checked,std::string_view source,std::string_view name,std::size_t offset) {
    const auto tokens=Lexer(source).scan();
    return resolved_definition_span(checked,source,name,offset,tokens);
}

std::vector<SourceSpan> reference_spans(
    const CheckedProgram& checked,std::string_view source,std::string_view name,SourceSpan target) {
    const auto tokens=Lexer(source).scan();
    std::vector<SourceSpan> result;
    for(const auto& token:tokens) {
        if(token.kind!=TokenKind::Identifier||token.text!=name) continue;
        const auto resolved=resolved_definition_span(
            checked,source,name,token.span.start.offset,tokens);
        if(resolved&&same_span(*resolved,target)) result.push_back(token.span);
    }
    return result;
}

bool valid_rename_identifier(std::string_view name) {
    if(name.empty()) return false;
    std::size_t identifiers=0;
    for(const auto& token:Lexer(name).scan()) {
        if(token.kind==TokenKind::Eof||token.kind==TokenKind::Newline) continue;
        if(token.kind!=TokenKind::Identifier||token.text!=name) return false;
        ++identifiers;
    }
    if(identifiers!=1) return false;
    if(builtin_scalar_type(name)) return false;
    for(const auto& builtin:builtin_callables)
        if(builtin.name==name) return false;
    return true;
}

bool is_keyword_token(TokenKind kind) {
    const auto value=static_cast<int>(kind);
    return value>=static_cast<int>(TokenKind::KwClass)&&
           value<=static_cast<int>(TokenKind::KwBitXor);
}

bool is_operator_token(TokenKind kind) {
    switch(kind) {
        case TokenKind::Pipe:
        case TokenKind::Ampersand:
        case TokenKind::Assign:
        case TokenKind::PlusAssign:
        case TokenKind::MinusAssign:
        case TokenKind::StarAssign:
        case TokenKind::SlashAssign:
        case TokenKind::PercentAssign:
        case TokenKind::Plus:
        case TokenKind::Minus:
        case TokenKind::Star:
        case TokenKind::Slash:
        case TokenKind::Percent:
        case TokenKind::EqEq:
        case TokenKind::NotEq:
        case TokenKind::Less:
        case TokenKind::LessEq:
        case TokenKind::Greater:
        case TokenKind::GreaterEq:
            return true;
        default:
            return false;
    }
}

int declaration_semantic_type(
    const Program& program,const Token& token,
    const std::vector<Token>& tokens) {
    for(const auto& import:program.imports)
        if(import.alias==token.text&&contains_offset(import.span,token.span.start.offset)) return 0;
    for(const auto& declaration:program.classes) {
        if(declaration.name==token.text) {
            if(auto span=identifier_span(tokens,declaration.span,token.text);
               span&&same_span(*span,token.span)) return 2;
        }
        for(const auto& field:declaration.fields) {
            if(field.name==token.text&&contains_offset(field.span,token.span.start.offset)) return 7;
        }
        for(const auto& method:declaration.methods) {
            if(method.name==token.text) {
                if(auto span=identifier_span(tokens,method.span,token.text,method.return_type.span.end.offset);
                   span&&same_span(*span,token.span)) return 4;
            }
            for(const auto& parameter:method.parameters)
                if(parameter.name==token.text&&contains_offset(parameter.span,token.span.start.offset))
                    return 5;
        }
    }
    for(const auto& function:program.functions) {
        if(function.name==token.text) {
            if(auto span=identifier_span(tokens,function.span,token.text,function.return_type.span.end.offset);
               span&&same_span(*span,token.span)) return 3;
        }
        for(const auto& parameter:function.parameters)
            if(parameter.name==token.text&&contains_offset(parameter.span,token.span.start.offset))
                return 5;
    }
    return 6;
}

int identifier_semantic_type(
    const Program& program,const Token& token,
    const std::vector<Token>& tokens) {
    if(builtin_scalar_type(token.text)||
       token.text=="auto"||token.text=="void"||token.text=="none"||
       token.text=="error"||token.text=="never") return 1;

    if(const auto* expression=expression_at(program,token.span.start.offset)) {
        if(const auto* call=std::get_if<MethodCallExpr>(&expression->data);
           call&&call->method==token.text) return 4;
        if(const auto* member=std::get_if<MemberExpr>(&expression->data);
           member&&member->name==token.text) return 7;
        if(const auto* call=std::get_if<CallExpr>(&expression->data);
           call&&call->callee==token.text) return 3;
    }

    const auto target=resolved_definition_span(
        program,token.text,token.span.start.offset,tokens);
    if(!target) {
        for(const auto& import:program.imports) if(import.alias==token.text) return 0;
        for(const auto& declaration:program.classes) if(declaration.name==token.text) return 2;
        for(const auto& function:program.functions) if(function.name==token.text) return 3;
        return 6;
    }

    Token definition_token=token;
    for(const auto& candidate:tokens) {
        if(candidate.kind==TokenKind::Identifier&&candidate.text==token.text&&
           contains_offset(*target,candidate.span.start.offset)) {
            definition_token=candidate;
            break;
        }
    }
    return declaration_semantic_type(program,definition_token,tokens);
}

std::string semantic_tokens_json(const Program& program,std::string_view source) {
    const auto tokens=Lexer(source).scan();
    std::vector<unsigned long long> data;
    std::size_t previous_line=0;
    std::size_t previous_start=0;
    bool first=true;
    for(const auto& token:tokens) {
        std::optional<int> type;
        if(is_keyword_token(token.kind)) type=8;
        else if(token.kind==TokenKind::String) type=9;
        else if(token.kind==TokenKind::Integer||token.kind==TokenKind::Float) type=10;
        else if(is_operator_token(token.kind)) type=11;
        else if(token.kind==TokenKind::Identifier)
            type=identifier_semantic_type(program,token,tokens);
        if(!type) continue;

        const auto start=lsp_position(source,token.span.start.offset);
        const auto end=lsp_position(source,token.span.end.offset);
        if(start.line!=end.line||end.character<=start.character) continue;
        const auto delta_line=first?start.line:start.line-previous_line;
        const auto delta_start=first||delta_line?start.character:start.character-previous_start;
        data.push_back(delta_line);
        data.push_back(delta_start);
        data.push_back(end.character-start.character);
        data.push_back(static_cast<unsigned long long>(*type));
        data.push_back(0);
        previous_line=start.line;
        previous_start=start.character;
        first=false;
    }
    std::ostringstream out;
    out<<"{\"data\":[";
    for(std::size_t i=0;i<data.size();++i) {
        if(i) out<<",";
        out<<data[i];
    }
    out<<"]}";
    return out.str();
}


class Server {
public:
    int run() {
        std::string body;
        while(read_message(body)) {
            Json message;
            try { message=JsonParser(body).parse(); }
            catch(const std::exception& error) {
                write_message("{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32700,\"message\":\""+
                              escape(error.what())+"\"}}");
                continue;
            }
            const auto* id=member(message,"id");
            try {
                const auto method=string_member(message,"method");
                if(!method) {
                    if(id) fail_response(id,-32600,"request method is missing");
                    continue;
                }
                if(*method=="initialize") initialize(message,id);
                else if(*method=="initialized") {}
                else if(*method=="shutdown") { shutdown_=true; respond(id,"null"); }
                else if(*method=="exit") return shutdown_?0:1;
                else if(*method=="textDocument/didOpen") open(message);
                else if(*method=="textDocument/didChange") change(message);
                else if(*method=="textDocument/didClose") close(message);
                else if(*method=="textDocument/didSave") save(message);
                else if(*method=="textDocument/formatting") formatting(message,id);
                else if(*method=="textDocument/hover") hover(message,id);
                else if(*method=="textDocument/definition") definition(message,id);
                else if(*method=="textDocument/completion") completion(message,id);
                else if(*method=="textDocument/signatureHelp") signature_help(message,id);
                else if(*method=="textDocument/references") references(message,id);
                else if(*method=="textDocument/rename") rename(message,id);
                else if(*method=="textDocument/semanticTokens/full") semantic_tokens(message,id);
                else if(id) fail_response(id,-32601,"method not supported");
            } catch(const std::exception& error) {
                if(id) fail_response(id,-32603,error.what());
                else std::cerr<<"quidra lsp: "<<error.what()<<"\n";
            }
        }
        return shutdown_?0:1;
    }
private:
    fs::path workspace_{fs::current_path()};
    std::unordered_map<std::string,std::string> documents_;
    bool shutdown_{};

    void initialize(const Json& message,const Json* id) {
        if(const auto* params=member(message,"params");params&&params->kind==Json::Kind::Object) {
            if(const auto root_uri=string_member(*params,"rootUri")) {
                if(const auto path=uri_path(*root_uri)) workspace_=*path;
            } else if(const auto root_path=string_member(*params,"rootPath")) {
                workspace_=*root_path;
            }
        }
        respond(id,
            "{\"capabilities\":{\"positionEncoding\":\"utf-16\","
            "\"textDocumentSync\":{\"openClose\":true,\"change\":1},"
            "\"documentFormattingProvider\":true,"
            "\"hoverProvider\":true,\"definitionProvider\":true,"
            "\"completionProvider\":{\"triggerCharacters\":[\".\"]},"
            "\"signatureHelpProvider\":{\"triggerCharacters\":[\"(\",\",\"]},"
            "\"referencesProvider\":true,\"renameProvider\":true,"
            "\"semanticTokensProvider\":{\"legend\":{\"tokenTypes\":["
            "\"namespace\",\"type\",\"class\",\"function\",\"method\",\"parameter\","
            "\"variable\",\"property\",\"keyword\",\"string\",\"number\",\"operator\"],"
            "\"tokenModifiers\":[]},\"full\":true}},"
            "\"serverInfo\":{\"name\":\"Quidra\",\"version\":\""+escape(compiler_version)+"\"}}");
    }

    void diagnostics(std::string_view uri,std::string_view source) {
        std::vector<Diagnostic> items;
        try {
            if(const auto path=uri_path(uri)) (void)check_file_source(*path,source,{},workspace_);
            else (void)check(source);
        } catch(const CompileErrors& errors) {
            items=errors.diagnostics();
        } catch(const CompileError& error) {
            items.push_back(error.diagnostic());
        } catch(const std::exception& error) {
            Diagnostic diagnostic;
            diagnostic.code="LSP_CHECK";
            diagnostic.message=error.what();
            items.push_back(std::move(diagnostic));
        }
        std::ostringstream params;
        params<<"{\"uri\":\""<<escape(uri)<<"\",\"diagnostics\":[";
        for(std::size_t i=0;i<items.size();++i) {
            if(i) params<<',';
            params<<diagnostic_text(items[i],source);
        }
        params<<"]}";
        notify("textDocument/publishDiagnostics",params.str());
    }

    void open(const Json& message) {
        const auto& params=required(message,"params",Json::Kind::Object);
        const auto& document=required(params,"textDocument",Json::Kind::Object);
        const auto uri=required_string(document,"uri");
        const auto source=required_string(document,"text");
        documents_[uri]=source;
        diagnostics(uri,source);
    }
    void change(const Json& message) {
        const auto& params=required(message,"params",Json::Kind::Object);
        const auto& document=required(params,"textDocument",Json::Kind::Object);
        const auto uri=required_string(document,"uri");
        const auto& changes=required(params,"contentChanges",Json::Kind::Array);
        if(changes.array.empty()) return;
        const auto source=required_string(changes.array.back(),"text");
        documents_[uri]=source;
        diagnostics(uri,source);
    }
    void close(const Json& message) {
        const auto& params=required(message,"params",Json::Kind::Object);
        const auto& document=required(params,"textDocument",Json::Kind::Object);
        const auto uri=required_string(document,"uri");
        documents_.erase(uri);
        notify("textDocument/publishDiagnostics",
               "{\"uri\":\""+escape(uri)+"\",\"diagnostics\":[]}");
    }
    void save(const Json& message) {
        const auto& params=required(message,"params",Json::Kind::Object);
        const auto& document=required(params,"textDocument",Json::Kind::Object);
        const auto uri=required_string(document,"uri");
        if(const auto source=string_member(params,"text")) documents_[uri]=*source;
        if(const auto found=documents_.find(uri);found!=documents_.end()) diagnostics(uri,found->second);
    }
    void formatting(const Json& message,const Json* id) {
        const auto& params=required(message,"params",Json::Kind::Object);
        const auto& document=required(params,"textDocument",Json::Kind::Object);
        const auto uri=required_string(document,"uri");
        std::string source;
        if(const auto found=documents_.find(uri);found!=documents_.end()) source=found->second;
        else {
            const auto path=uri_path(uri);
            if(!path) throw std::runtime_error("formatting requires a file URI or open document");
            source=read_file(*path);
        }
        const auto formatted=format_source(source);
        if(formatted==source) { respond(id,"[]"); return; }
        const auto end=lsp_position(source,source.size());
        std::ostringstream result;
        result<<"[{\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":"
              <<end.line<<",\"character\":"<<end.character<<"}},\"newText\":\""<<escape(formatted)<<"\"}]";
        respond(id,result.str());
    }

    std::string document_source(std::string_view uri) const {
        if(const auto found=documents_.find(std::string(uri));found!=documents_.end())
            return found->second;
        const auto path=uri_path(uri);
        if(!path) throw std::runtime_error("semantic request requires a file URI or open document");
        return read_file(*path);
    }

    CheckedProgram semantic_check(std::string_view uri,std::string_view source) const {
        if(const auto path=uri_path(uri))
            return check_file_source(*path,source,{},workspace_);
        return check(source);
    }

    void hover(const Json& message,const Json* id) {
        const auto& params=required(message,"params",Json::Kind::Object);
        const auto& document=required(params,"textDocument",Json::Kind::Object);
        const auto uri=required_string(document,"uri");
        const auto source=document_source(uri);
        const auto offset=raw_offset(source,request_position(message));
        try {
            const auto checked=semantic_check(uri,source);
            const auto* expression=expression_at(checked.program,offset);
            if(!expression) { respond(id,"null"); return; }

            std::string value;
            if(const auto* call=std::get_if<CallExpr>(&expression->data)) {
                const auto resolution=checked.call_resolutions.find(expression);
                if(resolution!=checked.call_resolutions.end()&&
                   resolution->second.kind==CallKind::Function) {
                    if(const auto function=checked.functions.find(resolution->second.target);
                       function!=checked.functions.end()) {
                        value=signature_label(call->callee,function->second);
                    }
                }
            } else if(const auto* method_call=std::get_if<MethodCallExpr>(&expression->data)) {
                if(const auto method=checked.method_calls.find(expression);
                   method!=checked.method_calls.end()) {
                    if(const auto function=checked.functions.find(method->second.internal_name);
                       function!=checked.functions.end()) {
                        value=signature_label(method_call->method,function->second);
                    }
                }
            }
            if(value.empty()) {
                const auto found=checked.expr_types.find(expression);
                if(found==checked.expr_types.end()) { respond(id,"null"); return; }
                value=type_name(found->second);
            }
            respond(id,"{\"contents\":{\"kind\":\"plaintext\",\"value\":\""+
                       escape(value)+"\"},\"range\":"+range_json(source,expression->span)+"}");
        } catch(const CompileError&) {
            respond(id,"null");
        } catch(const CompileErrors&) {
            respond(id,"null");
        }
    }

    void definition(const Json& message,const Json* id) {
        const auto& params=required(message,"params",Json::Kind::Object);
        const auto& document=required(params,"textDocument",Json::Kind::Object);
        const auto uri=required_string(document,"uri");
        const auto source=document_source(uri);
        const auto offset=raw_offset(source,request_position(message));
        try {
            const auto checked=semantic_check(uri,source);
            const auto* expression=expression_at(checked.program,offset);
            if(!expression) { respond(id,"null"); return; }

            std::string name;
            if(const auto* name_node=std::get_if<NameExpr>(&expression->data)) name=name_node->name;
            else if(const auto* call_node=std::get_if<CallExpr>(&expression->data)) name=call_node->callee;
            else if(const auto* member_node=std::get_if<MemberExpr>(&expression->data)) name=member_node->name;
            else if(const auto* method_node=std::get_if<MethodCallExpr>(&expression->data)) name=method_node->method;
            if(name.empty()) { respond(id,"null"); return; }

            const auto tokens=Lexer(source).scan();
            auto span=resolved_definition_span(checked,source,name,offset,tokens);
            if(!span) { respond(id,"null"); return; }
            respond(id,"{\"uri\":\""+escape(uri)+"\",\"range\":"+
                       range_json(source,*span)+"}");
        } catch(const CompileError&) {
            respond(id,"null");
        } catch(const CompileErrors&) {
            respond(id,"null");
        }
    }

    void completion(const Json& message,const Json* id) {
        const auto& params=required(message,"params",Json::Kind::Object);
        const auto& document=required(params,"textDocument",Json::Kind::Object);
        const auto uri=required_string(document,"uri");
        const auto source=document_source(uri);
        const auto offset=raw_offset(source,request_position(message));
        try {
            const auto program=root_program(source);
            const auto items=completions_at(program,source,offset);
            std::ostringstream result;
            result<<"[";
            for(std::size_t i=0;i<items.size();++i) {
                if(i) result<<",";
                result<<"{\"label\":\""<<escape(items[i].name)<<"\",\"kind\":"
                      <<items[i].kind;
                if(!items[i].detail.empty())
                    result<<",\"detail\":\""<<escape(items[i].detail)<<"\"";
                result<<"}";
            }
            result<<"]";
            respond(id,result.str());
        } catch(const CompileError&) {
            respond(id,"[]");
        } catch(const CompileErrors&) {
            respond(id,"[]");
        }
    }

    void signature_help(const Json& message,const Json* id) {
        const auto& params=required(message,"params",Json::Kind::Object);
        const auto& document=required(params,"textDocument",Json::Kind::Object);
        const auto uri=required_string(document,"uri");
        const auto source=document_source(uri);
        const auto offset=raw_offset(source,request_position(message));
        try {
            const auto checked=semantic_check(uri,source);
            const auto* expression=call_at(checked.program,offset);
            if(!expression) { respond(id,"null"); return; }

            const FunctionType* function=nullptr;
            std::string name;
            std::size_t active=0;
            const std::vector<CallArg>* arguments=nullptr;
            if(const auto* call=std::get_if<CallExpr>(&expression->data)) {
                name=call->callee;
                arguments=&call->args;
                const auto resolution=checked.call_resolutions.find(expression);
                if(resolution!=checked.call_resolutions.end()&&
                   resolution->second.kind==CallKind::Function) {
                    const auto found=checked.functions.find(resolution->second.target);
                    if(found!=checked.functions.end()) function=&found->second;
                }
            } else if(const auto* method_call=std::get_if<MethodCallExpr>(&expression->data)) {
                name=method_call->method;
                arguments=&method_call->args;
                const auto resolution=checked.method_calls.find(expression);
                if(resolution!=checked.method_calls.end()) {
                    const auto found=checked.functions.find(resolution->second.internal_name);
                    if(found!=checked.functions.end()) function=&found->second;
                }
            }
            if(!function||!arguments) { respond(id,"null"); return; }
            for(std::size_t i=0;i<arguments->size();++i) {
                if(offset>=(*arguments)[i].span.start.offset) active=i;
                if(contains_offset((*arguments)[i].span,offset)) { active=i; break; }
            }
            if(function->parameters.empty()) active=0;
            else active=std::min(active,function->parameters.size()-1);

            const auto label=signature_label(name,*function);
            std::ostringstream result;
            result<<"{\"signatures\":[{\"label\":\""<<escape(label)
                  <<"\",\"parameters\":[";
            for(std::size_t i=0;i<function->parameters.size();++i) {
                if(i) result<<",";
                result<<"{\"label\":\""<<escape(parameter_label(function->parameters[i]))<<"\"}";
            }
            result<<"]}],\"activeSignature\":0,\"activeParameter\":"<<active<<"}";
            respond(id,result.str());
        } catch(const CompileError&) {
            respond(id,"null");
        } catch(const CompileErrors&) {
            respond(id,"null");
        }
    }

    void references(const Json& message,const Json* id) {
        const auto& params=required(message,"params",Json::Kind::Object);
        const auto& document=required(params,"textDocument",Json::Kind::Object);
        const auto uri=required_string(document,"uri");
        const auto source=document_source(uri);
        const auto offset=raw_offset(source,request_position(message));
        try {
            const auto checked=semantic_check(uri,source);
            const auto token=identifier_token_at(source,offset);
            if(!token) { respond(id,"[]"); return; }
            const auto target=resolved_definition_span(
                checked,source,token->text,token->span.start.offset);
            if(!target) { respond(id,"[]"); return; }

            bool include_declaration=true;
            if(const auto* context=member(params,"context");context&&context->kind==Json::Kind::Object) {
                if(const auto requested=bool_member(*context,"includeDeclaration"))
                    include_declaration=*requested;
            }
            const auto spans=reference_spans(checked,source,token->text,*target);
            std::ostringstream result;
            result<<"[";
            bool first=true;
            for(const auto& span:spans) {
                if(!include_declaration&&contains_offset(*target,span.start.offset)) continue;
                if(!first) result<<",";
                first=false;
                result<<"{\"uri\":\""<<escape(uri)<<"\",\"range\":"
                      <<range_json(source,span)<<"}";
            }
            result<<"]";
            respond(id,result.str());
        } catch(const CompileError&) {
            respond(id,"[]");
        } catch(const CompileErrors&) {
            respond(id,"[]");
        }
    }

    void rename(const Json& message,const Json* id) {
        const auto& params=required(message,"params",Json::Kind::Object);
        const auto& document=required(params,"textDocument",Json::Kind::Object);
        const auto uri=required_string(document,"uri");
        const auto new_name=required_string(params,"newName");
        if(!valid_rename_identifier(new_name))
            throw std::runtime_error("rename target must be a non-reserved Quidra identifier");

        const auto source=document_source(uri);
        const auto offset=raw_offset(source,request_position(message));
        try {
            const auto checked=semantic_check(uri,source);
            const auto token=identifier_token_at(source,offset);
            if(!token) { respond(id,"null"); return; }
            const auto target=resolved_definition_span(
                checked,source,token->text,token->span.start.offset);
            if(!target) { respond(id,"null"); return; }

            if(new_name!=token->text) {
                for(const auto& candidate:Lexer(source).scan()) {
                    if(candidate.kind!=TokenKind::Identifier||candidate.text!=new_name) continue;
                    const auto other=resolved_definition_span(
                        checked,source,new_name,candidate.span.start.offset);
                    if(other&&!same_span(*other,*target))
                        throw std::runtime_error(
                            "rename target conflicts with an existing visible definition");
                }
            }

            const auto spans=reference_spans(checked,source,token->text,*target);
            std::ostringstream result;
            result<<"{\"changes\":{\""<<escape(uri)<<"\":[";
            for(std::size_t i=0;i<spans.size();++i) {
                if(i) result<<",";
                result<<"{\"range\":"<<range_json(source,spans[i])
                      <<",\"newText\":\""<<escape(new_name)<<"\"}";
            }
            result<<"]}}";
            respond(id,result.str());
        } catch(const CompileError&) {
            respond(id,"null");
        } catch(const CompileErrors&) {
            respond(id,"null");
        }
    }

    void semantic_tokens(const Json& message,const Json* id) {
        const auto& params=required(message,"params",Json::Kind::Object);
        const auto& document=required(params,"textDocument",Json::Kind::Object);
        const auto uri=required_string(document,"uri");
        const auto source=document_source(uri);
        try {
            const auto program=root_program(source);
            respond(id,semantic_tokens_json(program,source));
        } catch(const CompileError&) {
            respond(id,"{\"data\":[]}");
        } catch(const CompileErrors&) {
            respond(id,"{\"data\":[]}");
        }
    }
};

} // namespace

int run_lsp() {
    try { return Server{}.run(); }
    catch(const std::exception& error) {
        std::cerr<<"quidra lsp: "<<error.what()<<"\n";
        return 1;
    }
}

} // namespace quidra::cli
