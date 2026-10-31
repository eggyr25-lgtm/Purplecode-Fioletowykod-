#ifndef PURPLE_ELFEMIT_H
#define PURPLE_ELFEMIT_H

#include "ast.h"
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

/* Direct ELF64 emitter for a useful subset of Purplecode.
 * Produces a runnable Linux x86-64 ELF with no intermediate .s or .c file.
 * Supports: int/bool/string locals, arithmetic, comparisons, if/while,
 * Data.ShellPrint / Data.ShellPrint.Int, and return.
 * Uses Linux syscalls (write=1, exit=60) so no external C runtime is required
 * for this subset.
 */

typedef struct {
    uint8_t *code;
    size_t code_len;
    size_t code_cap;

    uint8_t *rodata;
    size_t rodata_len;
    size_t rodata_cap;

    /* simple string table for .rodata offsets */
    struct {
        char *s;
        uint32_t off;
    } *strs;
    int str_count;
    int str_cap;

    /* locals: name -> stack offset (negative from rbp) */
    struct {
        char *name;
        int offset; /* bytes below rbp; used only when spilled */
        PurpleType type;
        int first_use; /* linearized AST position */
        int last_use;
        int reg;       /* x86-64 register encoding, -1 means stack spill */
    } locals[256];
    int local_count;
    int frame_size;
    bool dump_regalloc;

    int label_counter;
    bool had_error;

    /* pending fixups for forward jumps */
    struct {
        size_t patch_at;   /* offset in code where rel32 lives */
        int label_id;
        int is_jz;         /* 1 = jz, 0 = jmp */
    } *fixups;
    int fixup_count;
    int fixup_cap;

    /* label positions */
    struct {
        int id;
        size_t pos;
    } *labels;
    int label_count;
    int label_cap;
} ElfEmit;

void elfemit_init(ElfEmit *E);
void elfemit_free(ElfEmit *E);
int  elfemit_program(ElfEmit *E, ASTNode *prog);
int  elfemit_write_elf(ElfEmit *E, const char *path);
void elfemit_dump_regalloc(const ElfEmit *E, FILE *out);

#endif
