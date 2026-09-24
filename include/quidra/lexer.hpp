#pragma once
#include "quidra/token.hpp"
#include <string_view>
#include <vector>

namespace quidra {

class Lexer {
public:
    explicit Lexer(std::string_view source) : source_(source) {}
    std::vector<Token> scan();
private:
    std::string_view source_;
    std::size_t index_{};
    SourcePos pos_{};

    bool eof() const { return index_ >= source_.size(); }
    char peek(std::size_t lookahead = 0) const;
    char advance();
    bool match(char c);
    void skip_space_and_comments();
    Token make(TokenKind kind, std::size_t start_index, SourcePos start, std::string text = {});
    Token identifier();
    Token number();
    Token string();
    [[noreturn]] void error(std::string code, std::string message, SourcePos start) const;
};

} // namespace quidra
