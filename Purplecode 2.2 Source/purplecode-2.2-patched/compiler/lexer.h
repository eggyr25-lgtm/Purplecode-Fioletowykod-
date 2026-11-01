#ifndef PURPLE_LEXER_H
#define PURPLE_LEXER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef enum {
    TOK_EOF = 0,
    TOK_IDENT,
    TOK_STRING,
    TOK_INT,
    TOK_HEX_ADDR,     // $100H
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_COMMA,
    TOK_GT,           // >
    TOK_EQ,
    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_PERCENT,
    TOK_EQEQ, TOK_NE, TOK_LT, TOK_LE, TOK_GE,
    TOK_BAND, TOK_BOR, TOK_BXOR, TOK_SHL, TOK_SHR, TOK_LAND, TOK_LOR, TOK_TILDE, TOK_BANG,
    TOK_SEMI,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_LBRACK,
    TOK_RBRACK,
    TOK_FN,
    TOK_RETURN,
    TOK_LET,
    TOK_COLON,
    TOK_TYPE_INT,
    TOK_TYPE_BOOL,
    TOK_TYPE_STRING,
    TOK_TRUE,
    TOK_FALSE,
    TOK_IF,
    TOK_ELSE,
    TOK_WHILE,
    TOK_CLASS,
    TOK_STRUCT,         /* Milestone 1.7 */
    TOK_RAW,
    TOK_IMPORT,
    TOK_ERROR
} TokenType;

typedef struct {
    TokenType type;
    char *lexeme;         // owned
    int64_t int_val;
    uint64_t addr_val;
    int line;
    int col;
} Token;

typedef struct {
    const char *src;
    size_t len;
    size_t pos;
    int line;
    int col;
    Token current;
    Token lookahead;
    bool has_lookahead;
} Lexer;

void lexer_init(Lexer *L, const char *src);
void lexer_free(Lexer *L);
Token lexer_next(Lexer *L);
Token lexer_peek(Lexer *L);
const char *token_type_name(TokenType t);

#endif
