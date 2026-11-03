#include "quidra/lexer.hpp"
#include "constant_numeric_eval.hpp"
#include <cmath>
#include <cstdio>
int main(){
    int failures=0;
    const auto tokens=quidra::Lexer("inf -inf infinity inf_value").scan();
    const auto expect=[&](bool ok,const char* msg){if(!ok){std::fprintf(stderr,"%s\n",msg);++failures;}};
    expect(tokens.at(0).kind==quidra::TokenKind::RealLiteral,"inf must lex as a real-family literal");
    expect(tokens.at(2).kind==quidra::TokenKind::RealLiteral,"-inf must use unary minus + real literal");
    expect(tokens.at(3).kind==quidra::TokenKind::Identifier,"infinity remains an identifier");
    expect(tokens.at(4).kind==quidra::TokenKind::Identifier,"inf_value remains an identifier");
    for(auto kind:{quidra::TypeKind::Real16,quidra::TypeKind::Real16b,quidra::TypeKind::Real32,quidra::TypeKind::Real64}){
        const auto type=quidra::Type::simple(kind);
        const auto positive=quidra::constant_eval::real_literal_value("inf",type);
        const auto negative=quidra::constant_eval::real_literal_value("-inf",type);
        expect(positive&&std::isinf(*positive)&&*positive>0,"positive inf must materialize");
        expect(negative&&std::isinf(*negative)&&*negative<0,"negative inf must materialize");
        expect(!quidra::constant_eval::real_literal_value("nan",type),"NaN is not a real literal");
    }
    return failures?1:0;
}
