#ifndef PURPLE_CODEGEN_H
#define PURPLE_CODEGEN_H

#include "ast.h"
#include <stdio.h>
#include <stdbool.h>

#define PURPLE_MAX_LOCALS   256
#define PURPLE_MAX_STRUCTS  64
#define PURPLE_MAX_FIELDS   32

typedef struct {
    char *name;
    PurpleType type;
    char *type_name;      /* if TY_STRUCT */
    int offset;           /* byte offset inside the parent struct */
    int size;             /* size in bytes */
} StructField;

typedef struct {
    char *name;
    StructField fields[PURPLE_MAX_FIELDS];
    int field_count;
    int size;             /* total size in bytes, 8-byte aligned */
} StructDef;

typedef struct {
    char *name;
    int offset;           /* rbp-relative offset of the base of this local */
    PurpleType type;
    char *type_name;      /* for TY_STRUCT: which struct */
    int size;             /* size of this local in bytes */
} LocalVar;

typedef struct {
    FILE *out;
    int label_counter;
    int str_counter;
    ASTNode *current_function;
    LocalVar locals[PURPLE_MAX_LOCALS];
    int local_count;
    int frame_size;
    bool had_error;

    /* Milestone 1.7 struct registry */
    StructDef structs[PURPLE_MAX_STRUCTS];
    int struct_count;
    ASTNode *program_root;   /* so we can find struct defs */
} CodeGen;

void codegen_init(CodeGen *CG, FILE *out);
void codegen_program(CodeGen *CG, ASTNode *prog);
void codegen_free(CodeGen *CG);

#endif
