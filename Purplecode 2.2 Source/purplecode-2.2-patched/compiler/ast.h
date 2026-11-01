#ifndef PURPLE_AST_H
#define PURPLE_AST_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    NODE_PROGRAM,
    NODE_FN,
    NODE_BLOCK,
    NODE_EXPR_STMT,
    NODE_RETURN,
    NODE_RAW_ASSIGN,
    NODE_VAR_DECL,
    NODE_ASSIGN,
    NODE_IF,
    NODE_WHILE,
    NODE_HIER_CALL,
    NODE_STRING,
    NODE_INT,
    NODE_BOOL,
    NODE_HEX_ADDR,
    NODE_IDENT,
    NODE_BINARY,
    NODE_UNARY,
    NODE_PARAM,
    NODE_ARG_LIST,
    /* Milestone 1.7 – structs */
    NODE_STRUCT_DEF,      /* name = struct name, body = list of NODE_FIELD */
    NODE_FIELD,           /* name = field name, var_type = field type, int_val = offset (filled by codegen) */
    NODE_FIELD_ACCESS,    /* left = base expr, name = field name (or chained) */
    NODE_MEMBER_ASSIGN    /* left = field access, right = value expr */
} NodeType;

typedef enum {
    TY_NONE = 0,
    TY_INT,
    TY_BOOL,
    TY_STRING,
    TY_STRUCT           /* var_type == TY_STRUCT; name or extra info identifies which */
} PurpleType;

typedef struct ASTNode ASTNode;

struct ASTNode {
    NodeType type;
    ASTNode *next;
    char *name;             /* identifier, field name, struct name, operator, etc. */
    char *str_val;
    int64_t int_val;        /* integer literal, or field offset, or struct size */
    uint64_t addr_val;
    ASTNode *left;
    ASTNode *right;
    ASTNode *body;
    ASTNode *args;
    int flags;
    PurpleType var_type;
    char *type_name;        /* for TY_STRUCT: the struct tag name */
};

ASTNode *ast_new(NodeType t);
void ast_free(ASTNode *n);
void ast_print(ASTNode *n, int indent);

#endif
