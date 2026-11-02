#ifndef PURPLE_PARSER_H
#define PURPLE_PARSER_H

#include "lexer.h"
#include "ast.h"

typedef struct {
    Lexer *lexer;
    Token current;
    Token previous;
    bool had_error;
    bool panic_mode;
} Parser;

void parser_init(Parser *P, Lexer *L);
ASTNode *parse_program(Parser *P);
void parser_free(Parser *P);

#endif
