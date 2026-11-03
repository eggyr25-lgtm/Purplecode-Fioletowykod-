#define _POSIX_C_SOURCE 200809L
#include "elfemit.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

/* Minimal ELF64 structures (little-endian x86-64). */
#define EI_NIDENT 16
typedef struct {
    unsigned char e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

#define ET_EXEC 2
#define EM_X86_64 62
#define EV_CURRENT 1
#define PT_LOAD 1
#define PF_X 1
#define PF_W 2
#define PF_R 4

#define LOAD_ADDR 0x400000ULL
#define BSS_ADDR  0x20000000ULL   /* zeroed read/write data page(s) for runtime globals */
#define BSS_SIZE  0x100000ULL

static void error(ElfEmit *E, const char *fmt, ...) {
    E->had_error = true;
    if (E->quiet) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "Purplecode GoodNative Emitter error: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static void ensure_code(ElfEmit *E, size_t need) {
    if (E->code_len + need <= E->code_cap) return;
    size_t ncap = E->code_cap ? E->code_cap * 2 : 4096;
    while (ncap < E->code_len + need) ncap *= 2;
    uint8_t *n = realloc(E->code, ncap);
    if (!n) { error(E, "out of memory"); return; }
    E->code = n;
    E->code_cap = ncap;
}

static void emit_bytes(ElfEmit *E, const void *p, size_t n) {
    ensure_code(E, n);
    if (E->had_error) return;
    memcpy(E->code + E->code_len, p, n);
    E->code_len += n;
}

static void emit_u8(ElfEmit *E, uint8_t v) { emit_bytes(E, &v, 1); }
static void emit_u32(ElfEmit *E, uint32_t v) { emit_bytes(E, &v, 4); }
static void emit_u64(ElfEmit *E, uint64_t v) { emit_bytes(E, &v, 8); }

/* x86-64 helpers (very small subset). */
static void emit_rex_w(ElfEmit *E) { emit_u8(E, 0x48); }

static void syscall_instr(ElfEmit *E);

static void mov_rax_imm64(ElfEmit *E, uint64_t imm) {
    emit_u8(E, 0x48); emit_u8(E, 0xB8); /* movabs rax, imm64 */
    emit_u64(E, imm);
}

/* movabs rN, imm64; register encodings are architectural x86-64 numbers. */
static void mov_reg_imm64(ElfEmit *E, int reg, uint64_t imm) {
    emit_u8(E, (uint8_t)(0x48 | (reg >= 8 ? 0x01 : 0x00)));
    emit_u8(E, (uint8_t)(0xB8 + (reg & 7)));
    emit_u64(E, imm);
}
static void pop_reg(ElfEmit *E, int reg) {
    if (reg >= 8) emit_u8(E, 0x41);
    emit_u8(E, (uint8_t)(0x58 + (reg & 7)));
}
static void syscall0(ElfEmit *E, uint64_t number) {
    mov_rax_imm64(E, number);
    syscall_instr(E);
}

static void mov_rdi_rax(ElfEmit *E) {
    emit_u8(E, 0x48); emit_u8(E, 0x89); emit_u8(E, 0xC7); /* mov rdi, rax */
}

static void mov_rsi_rax(ElfEmit *E) {
    emit_u8(E, 0x48); emit_u8(E, 0x89); emit_u8(E, 0xC6);
}

static void mov_rdx_rax(ElfEmit *E) {
    emit_u8(E, 0x48); emit_u8(E, 0x89); emit_u8(E, 0xC2);
}

static void mov_rax_rdi(ElfEmit *E) {
    emit_u8(E, 0x48); emit_u8(E, 0x89); emit_u8(E, 0xF8);
}

static void push_rbp(ElfEmit *E) { emit_u8(E, 0x55); }
static void mov_rbp_rsp(ElfEmit *E) {
    emit_u8(E, 0x48); emit_u8(E, 0x89); emit_u8(E, 0xE5);
}
static void sub_rsp_imm32(ElfEmit *E, int32_t imm) {
    emit_u8(E, 0x48); emit_u8(E, 0x81); emit_u8(E, 0xEC);
    emit_u32(E, (uint32_t)imm);
}
static void leave(ElfEmit *E) { emit_u8(E, 0xC9); }
static void ret(ElfEmit *E) { emit_u8(E, 0xC3); }

static void syscall_instr(ElfEmit *E) {
    emit_u8(E, 0x0F); emit_u8(E, 0x05);
}

/* mov [rbp + off], rax   off is signed 8 or 32 */
static void store_local_rax(ElfEmit *E, int off) {
    if (off >= -128 && off <= 127) {
        emit_u8(E, 0x48); emit_u8(E, 0x89); emit_u8(E, 0x45);
        emit_u8(E, (uint8_t)(int8_t)off);
    } else {
        emit_u8(E, 0x48); emit_u8(E, 0x89); emit_u8(E, 0x85);
        emit_u32(E, (uint32_t)off);
    }
}

/* mov rax, [rbp + off] */
static void load_local_rax(ElfEmit *E, int off) {
    if (off >= -128 && off <= 127) {
        emit_u8(E, 0x48); emit_u8(E, 0x8B); emit_u8(E, 0x45);
        emit_u8(E, (uint8_t)(int8_t)off);
    } else {
        emit_u8(E, 0x48); emit_u8(E, 0x8B); emit_u8(E, 0x85);
        emit_u32(E, (uint32_t)off);
    }
}

/* Move between RAX and a general-purpose register. Register numbers use
 * the architectural encoding (RBX=3, R12=12 ...). */
static void mov_reg_rax(ElfEmit *E, int reg) { /* mov reg, rax */
    uint8_t rex = (uint8_t)(0x48 | (reg >= 8 ? 0x01 : 0x00));
    emit_u8(E, rex); emit_u8(E, 0x89);
    emit_u8(E, (uint8_t)(0xC0 | (reg & 7)));
}
static void mov_rax_reg(ElfEmit *E, int reg) { /* mov rax, reg */
    uint8_t rex = (uint8_t)(0x48 | (reg >= 8 ? 0x04 : 0x00));
    emit_u8(E, rex); emit_u8(E, 0x89);
    emit_u8(E, (uint8_t)(0xC0 | ((reg & 7) << 3)));
}

static void test_rax_rax(ElfEmit *E) {
    emit_u8(E, 0x48); emit_u8(E, 0x85); emit_u8(E, 0xC0);
}

static void cmp_rax_rcx(ElfEmit *E) {
    emit_u8(E, 0x48); emit_u8(E, 0x39); emit_u8(E, 0xC8);
}

/* push rax / pop rcx etc. */
static void push_rax(ElfEmit *E) { emit_u8(E, 0x50); }
static void pop_rcx(ElfEmit *E) { emit_u8(E, 0x59); }
static void pop_rax(ElfEmit *E) { emit_u8(E, 0x58); }

static void add_rax_rcx(ElfEmit *E) {
    emit_u8(E, 0x48); emit_u8(E, 0x01); emit_u8(E, 0xC8);
}
static void sub_rax_rcx(ElfEmit *E) {
    emit_u8(E, 0x48); emit_u8(E, 0x29); emit_u8(E, 0xC8);
}
static void imul_rax_rcx(ElfEmit *E) {
    emit_u8(E, 0x48); emit_u8(E, 0x0F); emit_u8(E, 0xAF); emit_u8(E, 0xC1);
}

/* setcc al; movzx rax, al */
static void setcc_rax(ElfEmit *E, uint8_t cc) {
    emit_u8(E, 0x0F); emit_u8(E, 0x90 | cc); emit_u8(E, 0xC0); /* setcc al */
    emit_u8(E, 0x48); emit_u8(E, 0x0F); emit_u8(E, 0xB6); emit_u8(E, 0xC0); /* movzx rax, al */
}

/* Labels / fixups */
static int new_label(ElfEmit *E) {
    return E->label_counter++;
}

static void define_label(ElfEmit *E, int id) {
    if (E->label_count >= E->label_cap) {
        E->label_cap = E->label_cap ? E->label_cap * 2 : 32;
        E->labels = realloc(E->labels, E->label_cap * sizeof *E->labels);
    }
    E->labels[E->label_count].id = id;
    E->labels[E->label_count].pos = E->code_len;
    E->label_count++;
}

static void emit_jmp_rel32(ElfEmit *E, int label_id, int is_jz) {
    if (is_jz == 1) {
        emit_u8(E, 0x0F); emit_u8(E, 0x84); /* jz rel32 */
    } else if (is_jz == 4) {
        emit_u8(E, 0x0F); emit_u8(E, 0x88); /* js rel32 */
    } else if (is_jz == 2) {
        emit_u8(E, 0x0F); emit_u8(E, 0x85); /* jnz rel32 */
    } else if (is_jz == 3) {
        emit_u8(E, 0x0F); emit_u8(E, 0x8D); /* jge rel32 */
    } else if (is_jz == 5) {
        emit_u8(E, 0xE8); /* call rel32 */
    } else if (is_jz == 6) {
        emit_u8(E, 0x0F); emit_u8(E, 0x83); /* jae rel32 */
    } else {
        emit_u8(E, 0xE9); /* jmp rel32 */
    }
    size_t patch = E->code_len;
    emit_u32(E, 0); /* placeholder */

    if (E->fixup_count >= E->fixup_cap) {
        E->fixup_cap = E->fixup_cap ? E->fixup_cap * 2 : 32;
        E->fixups = realloc(E->fixups, E->fixup_cap * sizeof *E->fixups);
    }
    E->fixups[E->fixup_count].patch_at = patch;
    E->fixups[E->fixup_count].label_id = label_id;
    E->fixups[E->fixup_count].is_jz = is_jz;
    E->fixup_count++;
}

static void resolve_fixups(ElfEmit *E) {
    for (int i = 0; i < E->fixup_count; i++) {
        if (E->fixups[i].label_id < 0) continue; /* string absolute, patched in write_elf */
        size_t target = 0;
        int found = 0;
        for (int j = 0; j < E->label_count; j++) {
            if (E->labels[j].id == E->fixups[i].label_id) {
                target = E->labels[j].pos;
                found = 1;
                break;
            }
        }
        if (!found) {
            error(E, "unresolved label %d", E->fixups[i].label_id);
            continue;
        }
        int32_t rel = (int32_t)(target - (E->fixups[i].patch_at + 4));
        memcpy(E->code + E->fixups[i].patch_at, &rel, 4);
        /* is_jz 0=jmp, 1=jz, 2=jnz — opcode already emitted correctly */
        (void)E->fixups[i].is_jz;
    }
}

/* ----- strings in .rodata ----- */
/* Process the same escapes the GNU assembler does for .asciz in the hosted backend. */
static char *unescape_literal(const char *s) {
    size_t n = strlen(s);
    char *o = malloc(n + 1); size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '\\' || i + 1 >= n) { o[j++] = s[i]; continue; }
        char e = s[++i];
        switch (e) {
        case 'n': o[j++] = '\n'; break;   case 't': o[j++] = '\t'; break;
        case 'r': o[j++] = '\r'; break;   case 'b': o[j++] = '\b'; break;
        case 'f': o[j++] = '\f'; break;   case '\\': o[j++] = '\\'; break;
        case '"': o[j++] = '"'; break;
        case 'x': { int v = 0, k = 0; while (k < 2 && i + 1 < n && isxdigit((unsigned char)s[i + 1])) { char h = s[++i]; v = v * 16 + (h <= '9' ? h - '0' : (h | 32) - 'a' + 10); k++; } o[j++] = (char)v; break; }
        default:
            if (e >= '0' && e <= '7') { int v = e - '0', k = 1; while (k < 3 && i + 1 < n && s[i + 1] >= '0' && s[i + 1] <= '7') { v = v * 8 + (s[++i] - '0'); k++; } o[j++] = (char)v; }
            else { o[j++] = '\\'; o[j++] = e; }
        }
    }
    o[j] = 0;
    return o;
}

static uint32_t add_string(ElfEmit *E, const char *s) {
    for (int i = 0; i < E->str_count; i++) {
        if (strcmp(E->strs[i].s, s) == 0) return E->strs[i].off;
    }
    if (E->str_count >= E->str_cap) {
        E->str_cap = E->str_cap ? E->str_cap * 2 : 16;
        E->strs = realloc(E->strs, E->str_cap * sizeof *E->strs);
    }
    size_t len = strlen(s) + 1;
    if (E->rodata_len + len > E->rodata_cap) {
        size_t ncap = E->rodata_cap ? E->rodata_cap * 2 : 512;
        while (ncap < E->rodata_len + len) ncap *= 2;
        E->rodata = realloc(E->rodata, ncap);
        E->rodata_cap = ncap;
    }
    uint32_t off = (uint32_t)E->rodata_len;
    memcpy(E->rodata + E->rodata_len, s, len);
    E->rodata_len += len;
    E->strs[E->str_count].s = strdup(s);
    E->strs[E->str_count].off = off;
    E->str_count++;
    return off;
}

#define B(...) do { const uint8_t _b[] = { __VA_ARGS__ }; emit_bytes(E, _b, sizeof _b); } while (0)
static void emit_call_label(ElfEmit *E, int label) { emit_jmp_rel32(E, label, 5); }

/* Struct table (same layout rules as the hosted backend: 8-byte fields, nested structs inline) */
static EEStruct *find_struct(ElfEmit *E, const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < E->struct_count; i++)
        if (strcmp(E->structs[i].name, name) == 0) return &E->structs[i];
    return NULL;
}
static void build_structs(ElfEmit *E, ASTNode *prog) {
    int n = 0;
    for (ASTNode *s = prog->body; s; s = s->next) if (s->type == NODE_STRUCT_DEF && s->name) n++;
    E->structs = calloc(n ? n : 1, sizeof *E->structs);
    for (ASTNode *s = prog->body; s; s = s->next) {
        if (s->type != NODE_STRUCT_DEF || !s->name) continue;
        EEStruct *sd = &E->structs[E->struct_count++];
        sd->name = strdup(s->name);
        for (ASTNode *f = s->body; f; f = f->next) {
            if (f->type != NODE_FIELD || !f->name || sd->nfields >= 64) continue;
            EEField *ef = &sd->f[sd->nfields++];
            ef->name = strdup(f->name); ef->type = f->var_type;
            ef->tname = f->type_name ? strdup(f->type_name) : NULL;
        }
    }
    for (int pass = 0; pass < 4; pass++)
        for (int i = 0; i < E->struct_count; i++) {
            EEStruct *sd = &E->structs[i]; int off = 0;
            for (int k = 0; k < sd->nfields; k++) {
                EEField *f = &sd->f[k]; int sz = 8;
                if (f->type == TY_STRUCT) { EEStruct *in = find_struct(E, f->tname); if (in) sz = in->size; }
                f->off = off; off += sz;
            }
            sd->size = off > 0 ? off : 8;
        }
}

/* Locals */
static int find_local(ElfEmit *E, const char *name) {
    if (!name) return -1;
    for (int i = 0; i < E->local_count; i++)
        if (E->locals[i].name && strcmp(E->locals[i].name, name) == 0) return i;
    return -1;
}

static int add_local_idx(ElfEmit *E, const char *name, PurpleType ty);
static int add_local_idx(ElfEmit *E, const char *name, PurpleType ty) {
    int ex = find_local(E, name);
    if (ex >= 0) return ex;
    if (E->local_count >= 256) { error(E, "too many locals"); return -1; }
    int i = E->local_count++;
    E->locals[i].name = strdup(name);
    E->locals[i].type = ty;
    E->locals[i].offset = 0;
    E->locals[i].first_use = -1;
    E->locals[i].last_use = -1;
    E->locals[i].reg = -1;
    E->locals[i].sname = NULL;
    E->locals[i].slab = 0;
    return i;
}

static int find_func(ElfEmit *E, const char *name) {
    if (!name) return -1;
    for (int i = 0; i < E->func_count; i++) if (!strcmp(E->funcs[i].name, name)) return i;
    return -1;
}
static bool decl_is_string(ASTNode *n, const char *name) {
    if (!n) return false;
    if ((n->type == NODE_VAR_DECL || n->type == NODE_PARAM) && n->name && !strcmp(n->name, name) && n->var_type == TY_STRING) return true;
    if (n->type == NODE_BLOCK) { for (ASTNode *s = n->body; s; s = s->next) if (decl_is_string(s, name)) return true; return false; }
    return decl_is_string(n->body, name) || decl_is_string(n->left, name) || decl_is_string(n->right, name) || decl_is_string(n->next, name);
}
static bool returns_string_walk(ASTNode *fn, ASTNode *n) {
    if (!n) return false;
    if (n->type == NODE_RETURN && n->left) {
        if (n->left->type == NODE_STRING) return true;
        if (n->left->type == NODE_IDENT && n->left->name && (decl_is_string(fn->body, n->left->name) || decl_is_string(fn->args, n->left->name))) return true;
    }
    if (n->type == NODE_BLOCK) { for (ASTNode *s = n->body; s; s = s->next) if (returns_string_walk(fn, s)) return true; return false; }
    return returns_string_walk(fn, n->body) || returns_string_walk(fn, n->right);
}
static bool call_returns_string(ElfEmit *E, ASTNode *call) {
    if (!call || call->type != NODE_HIER_CALL || !call->name) return false;
    int f = find_func(E, call->name);
    return f >= 0 && returns_string_walk(E->funcs[f].fn, E->funcs[f].fn->body);
}

static void collect_locals(ElfEmit *E, ASTNode *n) {
    if (!n) return;
    if ((n->type == NODE_VAR_DECL || n->type == NODE_PARAM) && n->name) {
        PurpleType vt0 = n->var_type;
        if (!vt0 && n->type == NODE_VAR_DECL && n->right) {
            if (n->right->type == NODE_STRING || call_returns_string(E, n->right)) vt0 = TY_STRING;
        }
        int li = add_local_idx(E, n->name, vt0 ? vt0 : TY_INT);
        const char *sn = NULL;
        if (n->var_type == TY_STRUCT && n->type_name && find_struct(E, n->type_name)) sn = n->type_name;
        else if (n->type == NODE_VAR_DECL && n->right && n->right->type == NODE_HIER_CALL && n->right->name &&
                 (!strcmp(n->right->name, "Struct.New") || !strcmp(n->right->name, "Struct.Alloc")) &&
                 n->right->args && n->right->args->type == NODE_IDENT && find_struct(E, n->right->args->name))
            sn = n->right->args->name;
        else if (n->type == NODE_VAR_DECL && n->right && n->right->type == NODE_HIER_CALL && n->right->name && n->right->args) {
            const char *hn = n->right->name; ASTNode *ta = NULL;
            if (!strcmp(hn, "StructArray.New") || !strcmp(hn, "StructArray.Alloc")) ta = n->right->args;
            else if (!strcmp(hn, "StructArray.At") && n->right->args->next) ta = n->right->args->next->next;
            if (ta && ta->type == NODE_IDENT && find_struct(E, ta->name)) sn = ta->name;
        }
        if (li >= 0 && sn) {
            if (!E->locals[li].sname) E->locals[li].sname = strdup(sn);
            E->locals[li].type = TY_STRUCT;
            if (n->type == NODE_VAR_DECL && !n->right && !E->locals[li].slab)
                E->locals[li].slab = (find_struct(E, sn)->size + 15) & ~15;
        }
    }
    if (n->type == NODE_BLOCK) {
        for (ASTNode *s = n->body; s; s = s->next) collect_locals(E, s);
        return;
    }
    if (n->body) collect_locals(E, n->body);
    if (n->left) collect_locals(E, n->left);
    if (n->right) collect_locals(E, n->right);
    if (n->args) collect_locals(E, n->args);
    if (n->next) collect_locals(E, n->next);
}

/* Linearize the AST conservatively to obtain live intervals. Branches and
 * loops deliberately keep intervals live across the whole lexical region;
 * this may spill more than an SSA allocator, but cannot reuse a register
 * while a variable can still be read later in generated code. */
static void mark_local_use(ElfEmit *E, const char *name, int pos) {
    int i = find_local(E, name);
    if (i < 0) return;
    if (E->locals[i].first_use < 0) E->locals[i].first_use = pos;
    E->locals[i].last_use = pos;
}
static void analyze_intervals(ElfEmit *E, ASTNode *n, int *pos) {
    if (!n) return;
    int here = (*pos)++;
    if (n->type == NODE_WHILE) {
        /* A variable defined before a loop and used inside it stays live for the
         * whole loop, otherwise its register could be reused by a later local. */
        if (n->left) analyze_intervals(E, n->left, pos);
        if (n->body) analyze_intervals(E, n->body, pos);
        int end = (*pos)++;
        for (int i = 0; i < E->local_count; i++)
            if (E->locals[i].first_use >= 0 && E->locals[i].first_use < here && E->locals[i].last_use >= here)
                E->locals[i].last_use = end;
        if (n->next) analyze_intervals(E, n->next, pos);
        return;
    }
    if (n->type == NODE_IDENT) mark_local_use(E, n->name, here);
    else if (n->type == NODE_VAR_DECL && n->name) mark_local_use(E, n->name, here);
    else if (n->type == NODE_ASSIGN) {
        const char *name = n->name;
        if (!name && n->left && n->left->type == NODE_IDENT) name = n->left->name;
        mark_local_use(E, name, here);
    }
    if (n->body) analyze_intervals(E, n->body, pos);
    if (n->left) analyze_intervals(E, n->left, pos);
    if (n->right) analyze_intervals(E, n->right, pos);
    if (n->args) analyze_intervals(E, n->args, pos);
    if (n->next) analyze_intervals(E, n->next, pos);
}

#define SAVED_REG_BYTES 40   /* rbx, r12-r15 are saved by every function */
static const int alloc_regs[] = { 3, 12, 13, 14, 15 }; /* RBX, R12-R15 */
#define ALLOC_REG_COUNT ((int)(sizeof alloc_regs / sizeof alloc_regs[0]))

static void assign_local_offsets(ElfEmit *E) {
    int order[256];
    for (int i = 0; i < E->local_count; ++i) {
        order[i] = i;
        if (E->locals[i].first_use < 0) E->locals[i].first_use = E->locals[i].last_use = i;
    }
    /* Stable insertion sort by first live position. */
    for (int i = 1; i < E->local_count; ++i) {
        int x = order[i], j = i;
        while (j > 0 && E->locals[order[j-1]].first_use > E->locals[x].first_use) {
            order[j] = order[j-1]; --j;
        }
        order[j] = x;
    }
    int active[ALLOC_REG_COUNT];
    for (int r = 0; r < ALLOC_REG_COUNT; ++r) active[r] = -1;
    int spill_count = 0;
    for (int oi = 0; oi < E->local_count; ++oi) {
        int idx = order[oi];
        int start = E->locals[idx].first_use;
        for (int r = 0; r < ALLOC_REG_COUNT; ++r) {
            int old = active[r];
            if (old >= 0 && E->locals[old].last_use < start) active[r] = -1;
        }
        int free_reg = -1;
        for (int r = 0; r < ALLOC_REG_COUNT; ++r) if (active[r] < 0) { free_reg = r; break; }
        if (free_reg >= 0) {
            E->locals[idx].reg = alloc_regs[free_reg];
            active[free_reg] = idx;
        } else {
            E->locals[idx].reg = -1;
            E->locals[idx].offset = -(SAVED_REG_BYTES + (++spill_count) * 8);
        }
    }
    int cur = spill_count * 8;
    for (int i = 0; i < E->local_count; i++)
        if (E->locals[i].slab > 0) { cur += E->locals[i].slab; E->locals[i].slab_off = -(SAVED_REG_BYTES + cur); }
    E->frame_size = ((cur + 15) / 16) * 16;
    if (E->frame_size < 16) E->frame_size = 16;
}

static void load_local(ElfEmit *E, int idx) {
    if (E->locals[idx].reg >= 0) mov_rax_reg(E, E->locals[idx].reg);
    else load_local_rax(E, E->locals[idx].offset);
}
static void store_local(ElfEmit *E, int idx) {
    if (E->locals[idx].reg >= 0) mov_reg_rax(E, E->locals[idx].reg);
    else store_local_rax(E, E->locals[idx].offset);
}

void elfemit_dump_regalloc(const ElfEmit *E, FILE *out) {
    static const char *reg_names[16] = { [3] = "rbx", [12] = "r12", [13] = "r13", [14] = "r14", [15] = "r15" };
    fprintf(out, "Purplecode linear-scan register allocation (%d locals, %d-byte spill frame):\n", E->local_count, E->frame_size);
    for (int i = 0; i < E->local_count; ++i) {
        const char *where = E->locals[i].reg >= 0 ? reg_names[E->locals[i].reg] : "stack spill";
        fprintf(out, "  %-20s [%d..%d] -> %s", E->locals[i].name ? E->locals[i].name : "?", E->locals[i].first_use, E->locals[i].last_use, where);
        if (E->locals[i].reg < 0) fprintf(out, " (rbp%d)", E->locals[i].offset);
        fputc('\n', out);
    }
}

/* Forward */
static void gen_expr(ElfEmit *E, ASTNode *n);
static void gen_stmt(ElfEmit *E, ASTNode *n);
static void gen_print_string(ElfEmit *E, ASTNode *arg);
static void gen_print_int(ElfEmit *E, ASTNode *arg);

static bool resolve_field(ElfEmit *E, const char *dotted, int *base_idx, int *off, PurpleType *ty, const char **tname) {
    const char *dot = dotted ? strchr(dotted, '.') : NULL;
    if (!dot) return false;
    char base[128]; size_t bl = (size_t)(dot - dotted);
    if (bl >= sizeof base) return false;
    memcpy(base, dotted, bl); base[bl] = 0;
    int bi = find_local(E, base);
    if (bi < 0 || !E->locals[bi].sname) return false;
    const char *cur = E->locals[bi].sname; int o = 0; PurpleType t = TY_STRUCT; const char *p = dot + 1;
    while (*p) {
        EEStruct *sd = find_struct(E, cur);
        if (!sd) return false;
        const char *nd = strchr(p, '.'); size_t fl = nd ? (size_t)(nd - p) : strlen(p);
        char fname[128]; if (fl >= sizeof fname) return false;
        memcpy(fname, p, fl); fname[fl] = 0;
        EEField *f = NULL;
        for (int k = 0; k < sd->nfields; k++) if (!strcmp(sd->f[k].name, fname)) { f = &sd->f[k]; break; }
        if (!f) return false;
        o += f->off; t = f->type; cur = f->tname;
        if (nd && t != TY_STRUCT) return false;
        p = nd ? nd + 1 : p + fl;
    }
    *base_idx = bi; *off = o; *ty = t; *tname = cur;
    return true;
}

static bool expr_is_string(ElfEmit *E, ASTNode *n) {
    if (!n) return false;
    if (n->type == NODE_STRING) return true;
    if (n->type == NODE_IDENT) {
        int fb, fo; PurpleType ft; const char *ftn;
        if (n->name && !strcmp(n->name, "$LastHexResult")) return true;
        if (n->name && strchr(n->name, '.') && resolve_field(E, n->name, &fb, &fo, &ft, &ftn)) return ft == TY_STRING;
        int idx = find_local(E, n->name);
        return idx >= 0 && E->locals[idx].type == TY_STRING;
    }
    if (n->type == NODE_HIER_CALL && call_returns_string(E, n)) return true;
    return n->var_type == TY_STRING;
}

/* Runtime-library style builtins implemented directly on Linux syscalls. */
static bool builtin_ext(ElfEmit *E, ASTNode *n, const char *name, ASTNode *arg) {
    int nargs = 0; for (ASTNode *a = arg; a; a = a->next) nargs++;
    if (!strcmp(name, "Memory.Alloc") && nargs == 1) { gen_expr(E, arg); emit_call_label(E, E->rt_alloc); return true; }
    if ((!strcmp(name, "Memory.Free") && (nargs == 1 || nargs == 2)) ||
        ((!strcmp(name, "Struct.Free") || !strcmp(name, "Struct.Destroy") || !strcmp(name, "Array.Free") || !strcmp(name, "Array.Destroy")) && nargs == 1)) {
        gen_expr(E, arg); emit_call_label(E, E->rt_free); mov_rax_imm64(E, 0); return true;
    }
    if ((!strcmp(name, "Struct.New") || !strcmp(name, "Struct.Alloc")) && nargs == 1) {
        EEStruct *sd = (arg->type == NODE_IDENT) ? find_struct(E, arg->name) : NULL;
        if (sd) mov_rax_imm64(E, (uint64_t)sd->size); else gen_expr(E, arg);
        emit_call_label(E, E->rt_alloc); return true;
    }
    if ((!strcmp(name, "Array.New") || !strcmp(name, "Array.Alloc")) && nargs == 1) { gen_expr(E, arg); emit_call_label(E, E->rt_array_new); return true; }
    if ((!strcmp(name, "Array.Len") || !strcmp(name, "Array.Length")) && nargs == 1) {
        gen_expr(E, arg); int z = new_label(E);
        B(0x48, 0x85, 0xC0); emit_jmp_rel32(E, z, 1); B(0x48, 0x8B, 0x00); define_label(E, z); return true;
    }
    if (!strcmp(name, "Array.Data") && nargs == 1) {
        gen_expr(E, arg); int z = new_label(E);
        B(0x48, 0x85, 0xC0); emit_jmp_rel32(E, z, 1); B(0x48, 0x83, 0xC0, 0x10); define_label(E, z); return true;
    }
    if (!strcmp(name, "Array.Get") && nargs == 2) {
        gen_expr(E, arg); push_rax(E); gen_expr(E, arg->next); B(0x48, 0x89, 0xC1); pop_rax(E); /* rax=a rcx=i */
        int zero = new_label(E), done = new_label(E);
        B(0x48, 0x85, 0xC0); emit_jmp_rel32(E, zero, 1);
        B(0x48, 0x85, 0xC9); emit_jmp_rel32(E, zero, 4);
        B(0x48, 0x3B, 0x08); emit_jmp_rel32(E, zero, 3);            /* cmp rcx,[rax]; jge zero */
        B(0x48, 0x8B, 0x44, 0xC8, 0x10); emit_jmp_rel32(E, done, 0); /* mov rax,[rax+rcx*8+16] */
        define_label(E, zero); B(0x31, 0xC0); define_label(E, done); return true;
    }
    if (!strcmp(name, "Array.Set") && nargs == 3) {
        gen_expr(E, arg); push_rax(E); gen_expr(E, arg->next); push_rax(E); gen_expr(E, arg->next->next);
        B(0x48, 0x89, 0xC2); pop_rcx(E); pop_rax(E);                 /* rdx=v rcx=i rax=a */
        int bad = new_label(E), done = new_label(E);
        B(0x48, 0x85, 0xC0); emit_jmp_rel32(E, bad, 1);
        B(0x48, 0x85, 0xC9); emit_jmp_rel32(E, bad, 4);
        B(0x48, 0x3B, 0x08); emit_jmp_rel32(E, bad, 3);
        B(0x48, 0x89, 0x54, 0xC8, 0x10);                             /* mov [rax+rcx*8+16],rdx */
        B(0x31, 0xC0); emit_jmp_rel32(E, done, 0);
        define_label(E, bad); mov_rax_imm64(E, (uint64_t)-1); define_label(E, done); return true;
    }
    if (!strcmp(name, "Rt.Global") && nargs == 0) { mov_rax_imm64(E, BSS_ADDR); return true; }
    if (!strncmp(name, "Memory.Read", 11) && nargs == 1) {
        int w = atoi(name + 11);
        if (w == 8) { gen_expr(E, arg); B(0x0F, 0xB6, 0x00); return true; }
        if (w == 16) { gen_expr(E, arg); B(0x0F, 0xB7, 0x00); return true; }
        if (w == 32) { gen_expr(E, arg); B(0x8B, 0x00); return true; }
        if (w == 64) { gen_expr(E, arg); B(0x48, 0x8B, 0x00); return true; }
    }
    if (!strncmp(name, "Memory.Write", 12) && nargs == 2) {
        int w = atoi(name + 12);
        if (w == 8 || w == 16 || w == 32 || w == 64) {
            gen_expr(E, arg); push_rax(E); gen_expr(E, arg->next); B(0x48, 0x89, 0xC2); pop_rax(E);
            if (w == 8) B(0x88, 0x10); else if (w == 16) B(0x66, 0x89, 0x10); else if (w == 32) B(0x89, 0x10); else B(0x48, 0x89, 0x10);
            mov_rax_imm64(E, 0);
            return true;
        }
    }
    if ((!strcmp(name, "StructArray.New") || !strcmp(name, "StructArray.Alloc")) && (nargs == 1 || nargs == 2)) {
        EEStruct *sd = (arg->type == NODE_IDENT) ? find_struct(E, arg->name) : NULL;
        int es = sd ? sd->size : 8;
        if (arg->next) { gen_expr(E, arg->next); B(0x48, 0x69, 0xC0); emit_u32(E, (uint32_t)es); }  /* imul rax,rax,es */
        else mov_rax_imm64(E, (uint64_t)es);
        emit_call_label(E, E->rt_alloc); return true;
    }
    if (!strcmp(name, "StructArray.At") && nargs == 3) {
        EEStruct *sd = (arg->next->next->type == NODE_IDENT) ? find_struct(E, arg->next->next->name) : NULL;
        int es = sd ? sd->size : 8;
        gen_expr(E, arg); push_rax(E); gen_expr(E, arg->next);
        B(0x48, 0x69, 0xC0); emit_u32(E, (uint32_t)es); pop_rcx(E); B(0x48, 0x01, 0xC8);   /* imul; add rax,rcx */
        return true;
    }
    if ((!strcmp(name, "StructArray.Free") || !strcmp(name, "StructArray.Destroy")) && nargs == 1) {
        gen_expr(E, arg); emit_call_label(E, E->rt_free); mov_rax_imm64(E, 0); return true;
    }
    (void)n;
    return false;
}

static void gen_expr(ElfEmit *E, ASTNode *n) {
    if (!n || E->had_error) return;
    switch (n->type) {
    case NODE_INT:
        mov_rax_imm64(E, (uint64_t)n->int_val);
        break;
    case NODE_BOOL:
        mov_rax_imm64(E, n->int_val ? 1 : 0);
        break;
    case NODE_HEX_ADDR:
        mov_rax_imm64(E, n->addr_val);
        break;
    case NODE_STRING: {
        char *lit = unescape_literal(n->str_val ? n->str_val : "");
        uint32_t off = add_string(E, lit);
        free(lit);
        /* materialize absolute address later via patch; for now use placeholder
           and fix after we know LOAD_ADDR + rodata base.
           Simpler approach: emit lea with rip-relative once we know layout.
           For v1 we put rodata right after code and compute. */
        /* temporary: store offset; final absolute address fixed in write step
           if needed. For syscall write we need pointer. We'll resolve at
           emission of the call by keeping a list of rip-relative patches.
           Simpler v1: put string address as absolute LOAD_ADDR + code_size + off
           and require no ASLR (we set ELF non-PIE). */
        uint64_t abs = LOAD_ADDR + 0x1000 + off; /* text at 0x401000 typically; adjust in writer */
        (void)abs;
        /* We'll fix properly in write_elf by recording string uses.
           For now emit movabs with a sentinel and patch later. */
        emit_u8(E, 0x48); emit_u8(E, 0xB8);
        /* record patch site */
        size_t patch = E->code_len;
        emit_u64(E, 0xDEADBEEFDEADBEEFULL);
        /* reuse fixup table with special negative label meaning string off */
        if (E->fixup_count >= E->fixup_cap) {
            E->fixup_cap = E->fixup_cap ? E->fixup_cap * 2 : 32;
            E->fixups = realloc(E->fixups, E->fixup_cap * sizeof *E->fixups);
        }
        E->fixups[E->fixup_count].patch_at = patch;
        E->fixups[E->fixup_count].label_id = -((int)off + 1); /* negative => string */
        E->fixups[E->fixup_count].is_jz = 0;
        E->fixup_count++;
        break;
    }
    case NODE_IDENT: {
        int fb, fo; PurpleType ft; const char *ftn;
        if (n->name && !strcmp(n->name, "$LastHexResult")) {
            mov_rax_imm64(E, BSS_ADDR + 128); B(0x48, 0x8B, 0x00);
            break;
        }
        if (n->name && strchr(n->name, '.') && resolve_field(E, n->name, &fb, &fo, &ft, &ftn)) {
            load_local(E, fb);
            if (ft == TY_STRUCT) { if (fo) { B(0x48, 0x05); emit_u32(E, (uint32_t)fo); } }
            else { B(0x48, 0x8B, 0x80); emit_u32(E, (uint32_t)fo); }  /* mov rax,[rax+off] */
            break;
        }
        int idx = find_local(E, n->name);
        if (idx < 0) {
            error(E, "undefined variable '%s'", n->name ? n->name : "?");
            return;
        }
        load_local(E, idx);
        break;
    }
    case NODE_UNARY: {
        gen_expr(E, n->left);
        const char *op = n->name ? n->name : "";
        if (!strcmp(op, "-")) { emit_u8(E, 0x48); emit_u8(E, 0xF7); emit_u8(E, 0xD8); } /* neg rax */
        else if (!strcmp(op, "~")) { emit_u8(E,0x48); emit_u8(E,0xF7); emit_u8(E,0xD0); } /* not rax */
        else if (!strcmp(op, "!")) { test_rax_rax(E); emit_u8(E,0x0F); emit_u8(E,0x94); emit_u8(E,0xC0); emit_u8(E,0x48); emit_u8(E,0x0F); emit_u8(E,0xB6); emit_u8(E,0xC0); }
        else if (strcmp(op, "+") != 0) error(E, "unsupported unary op '%s' in direct ELF mode", op);
        break;
    }
    case NODE_BINARY: {
        /* eval left, push, eval right into rcx, op */
        gen_expr(E, n->left);
        push_rax(E);
        gen_expr(E, n->right);
        /* rax = right, stack = left */
        /* swap: pop rcx (left), keep rax (right) or vice-versa depending on op */
        /* standard: left in rcx, right in rax after pop? Let's do:
           after gen right: rax=right
           pop rcx  -> rcx=left
        */
        pop_rcx(E); /* rcx = left, rax = right */

        const char *op = n->name ? n->name : "";
        if (!strcmp(op, "+")) {
            /* left + right: rcx + rax -> rax */
            add_rax_rcx(E); /* rax = rax + rcx = right + left */
        } else if (!strcmp(op, "-")) {
            /* left - right: rcx - rax */
            /* xchg or: mov rdx,rax; mov rax,rcx; sub rax,rdx */
            emit_u8(E, 0x48); emit_u8(E, 0x91); /* xchg rax, rcx  -> rax=left, rcx=right */
            sub_rax_rcx(E);
        } else if (!strcmp(op, "*")) {
            imul_rax_rcx(E); /* rax = rax * rcx */
        } else if (!strcmp(op, "/") || !strcmp(op, "%")) {
            /* Signed division: RAX=left, RCX=right, RDX:RAX dividend. */
            emit_u8(E, 0x48); emit_u8(E, 0x91); /* xchg rax,rcx */
            emit_u8(E, 0x48); emit_u8(E, 0x99); /* cqo */
            emit_u8(E, 0x48); emit_u8(E, 0xF7); emit_u8(E, 0xF9); /* idiv rcx */
            if (!strcmp(op, "%")) { emit_u8(E, 0x48); emit_u8(E, 0x89); emit_u8(E, 0xD0); } /* rax=rdx */
        } else if (!strcmp(op, "&") || !strcmp(op, "|") || !strcmp(op, "^")) {
            if (!strcmp(op, "&")) { emit_u8(E,0x48); emit_u8(E,0x21); emit_u8(E,0xC8); }
            else if (!strcmp(op, "|")) { emit_u8(E,0x48); emit_u8(E,0x09); emit_u8(E,0xC8); }
            else { emit_u8(E,0x48); emit_u8(E,0x31); emit_u8(E,0xC8); }
        } else if (!strcmp(op, "<<") || !strcmp(op, ">>")) {
            emit_u8(E,0x48); emit_u8(E,0x91); /* rax=left, rcx=right */
            emit_u8(E,0x48); emit_u8(E,0xD3); emit_u8(E,!strcmp(op,"<<")?0xE0:0xF8);
        } else if (!strcmp(op, "&&") || !strcmp(op, "||")) {
            test_rax_rax(E); emit_u8(E,0x0F); emit_u8(E,0x95); emit_u8(E,0xC0); emit_u8(E,0x48); emit_u8(E,0x0F); emit_u8(E,0xB6); emit_u8(E,0xC0);
            emit_u8(E,0x48); emit_u8(E,0x85); emit_u8(E,0xC9); emit_u8(E,0x0F); emit_u8(E,0x95); emit_u8(E,0xC1); emit_u8(E,0x48); emit_u8(E,0x0F); emit_u8(E,0xB6); emit_u8(E,0xC9);
            emit_u8(E,0x48); emit_u8(E,!strcmp(op,"&&")?0x21:0x09); emit_u8(E,0xC8);
        } else if (!strcmp(op, "==") || !strcmp(op, "!=") ||
                   !strcmp(op, "<") || !strcmp(op, ">") ||
                   !strcmp(op, "<=") || !strcmp(op, ">=")) {
            /* cmp left, right  (rcx, rax) */
            cmp_rax_rcx(E); /* cmp rax, rcx  => flags for right ? left — wrong direction */
            /* We need cmp left, right = cmp rcx, rax */
            emit_u8(E, 0x48); emit_u8(E, 0x39); emit_u8(E, 0xC1); /* cmp rcx, rax */
            uint8_t cc = 0x4; /* sete */
            if (!strcmp(op, "==")) cc = 0x4;      /* sete */
            else if (!strcmp(op, "!=")) cc = 0x5; /* setne */
            else if (!strcmp(op, "<")) cc = 0xC;  /* setl */
            else if (!strcmp(op, ">")) cc = 0xF;  /* setg */
            else if (!strcmp(op, "<=")) cc = 0xE; /* setle */
            else if (!strcmp(op, ">=")) cc = 0xD; /* setge */
            setcc_rax(E, cc);
        } else {
            error(E, "unsupported binary op '%s' in direct ELF mode", op);
        }
        break;
    }
    case NODE_HIER_CALL: {
        char namebuf[160];
        const char *name = n->name ? n->name : "";
        if (!strncmp(name, "System.Memory.", 14)) { snprintf(namebuf, sizeof namebuf, "Memory.%s", name + 14); name = namebuf; }
        else if (!strcmp(name, "System.Kernel.Call")) name = "Kernel.Call";
        ASTNode *arg = n->args;
        {   /* method call: local.method(args) -> Struct.method(local, args) */
            const char *d0 = strchr(name, '.');
            if (d0 && !strchr(d0 + 1, '.') && find_func(E, name) < 0) {
                char base[128]; size_t bl = (size_t)(d0 - name);
                int bi = -1;
                if (bl < sizeof base) { memcpy(base, name, bl); base[bl] = 0; bi = find_local(E, base); }
                if (bi >= 0 && E->locals[bi].sname) {
                    char mname[256]; snprintf(mname, sizeof mname, "%s.%s", E->locals[bi].sname, d0 + 1);
                    int fi = find_func(E, mname);
                    if (fi >= 0) {
                        int want = 0, got = 1;
                        for (ASTNode *p = E->funcs[fi].fn->args; p; p = p->next) want++;
                        for (ASTNode *a = arg; a; a = a->next) got++;
                        if (want != got) { error(E, "method '%s' expects %d argument(s) including the receiver, got %d", mname, want, got); return; }
                        load_local(E, bi); push_rax(E);
                        for (ASTNode *a = arg; a; a = a->next) { gen_expr(E, a); push_rax(E); }
                        emit_call_label(E, E->funcs[fi].label);
                        B(0x48, 0x81, 0xC4); emit_u32(E, (uint32_t)(got * 8));
                        break;
                    }
                }
            }
        }
        if (1) {
            int fi = find_func(E, name);
            if (fi >= 0) {
                int want = 0, got = 0;
                for (ASTNode *p = E->funcs[fi].fn->args; p; p = p->next) want++;
                for (ASTNode *a = arg; a; a = a->next) got++;
                int pad = 0;
                if (got < want && !strncmp(name, "GUI.", 4)) pad = want - got;   /* GUI calls accept omitted trailing args as 0 */
                if (want != got + pad) { error(E, "function '%s' expects %d argument(s), got %d", name, want, got); return; }
                for (ASTNode *a = arg; a; a = a->next) { gen_expr(E, a); push_rax(E); }
                for (int pz = 0; pz < pad; pz++) { mov_rax_imm64(E, 0); push_rax(E); }
                got += pad;
                emit_call_label(E, E->funcs[fi].label);
                if (got) { B(0x48, 0x81, 0xC4); emit_u32(E, (uint32_t)(got * 8)); }  /* add rsp, 8*n */
                break;
            }
            if (!strchr(name, '.')) { error(E, "call to undefined function '%s'", name); return; }
        }
        if (arg || strcmp(name, "System.Process.ID") != 0) { if (builtin_ext(E, n, name, arg)) break; }
        if (!arg && !strcmp(name, "System.Process.ID")) {
            syscall0(E, 39); /* getpid */
            break;
        }
        if (!arg && !strcmp(name, "System.Time.Unix")) {
            mov_rax_imm64(E, 201); mov_reg_imm64(E, 7, 0); syscall_instr(E);
            break;
        }
        if (!arg) {
            error(E, "%s requires an argument", name);
            return;
        }
        if (!strcmp(name, "Data.ShellPrint")) {
            /* ShellPrint is overloaded: literals/expressions print as decimal;
               strings print as text. This avoids treating an integer as a pointer. */
            if (expr_is_string(E, arg))
                gen_print_string(E, arg);
            else
                gen_print_int(E, arg);
        } else if (!strcmp(name, "Data.ShellPrint.Int")) {
            gen_print_int(E, arg);
        } else if (!strcmp(name, "System.Process.ID") && !arg->next) {
            syscall0(E, 39); /* getpid */
        } else if (!strcmp(name, "System.Time.Unix") && !arg->next) {
            /* Linux x86-64 time(time_t *), NULL requests the value in RAX. */
            mov_rax_imm64(E, 201); mov_reg_imm64(E, 7, 0); syscall_instr(E);
        } else if (!strcmp(name, "LinuxAPI.Call") || !strcmp(name, "Kernel.Call")) {
            /* Linux syscall ABI: number, then up to six machine-word arguments.
               Evaluate and stack arguments first, then pop into ABI registers. */
            ASTNode *a = arg; int count = 0;
            for (; a; a = a->next) { if (count == 7) break; gen_expr(E, a); push_rax(E); count++; }
            if (a) { error(E, "%s accepts a syscall number and at most six arguments in direct ELF mode", name); break; }
            if (count < 1) { error(E, "%s requires a syscall number", name); break; }
            if (count >= 7) pop_reg(E, 9);  /* arg6 -> r9 */
            if (count >= 6) pop_reg(E, 8);  /* arg5 -> r8 */
            if (count >= 5) pop_reg(E, 10); /* arg4 -> r10 */
            if (count >= 4) pop_reg(E, 2);  /* arg3 -> rdx */
            if (count >= 3) pop_reg(E, 6);  /* arg2 -> rsi */
            if (count >= 2) pop_reg(E, 7);  /* arg1 -> rdi */
            pop_reg(E, 0);                 /* syscall number -> rax */
            syscall_instr(E);
        } else if (!strcmp(name, "FS.Exists") && !arg->next) {
            gen_expr(E, arg); mov_reg_rax(E, 7); /* rdi=path */
            mov_reg_imm64(E, 6, 0); /* rsi=F_OK */
            syscall0(E, 21); /* access(path, F_OK) */
            test_rax_rax(E);
            emit_u8(E, 0x0F); emit_u8(E, 0x94); emit_u8(E, 0xC0); /* sete al */
            emit_u8(E, 0x48); emit_u8(E, 0x0F); emit_u8(E, 0xB6); emit_u8(E, 0xC0);
        } else if (!strcmp(name, "FS.Size") && !arg->next) {
            /* open(path,O_RDONLY), lseek(fd,0,SEEK_END), close(fd). */
            gen_expr(E, arg); mov_reg_rax(E, 7); mov_reg_imm64(E, 6, 0); mov_reg_imm64(E, 2, 0);
            syscall0(E, 2); /* open */
            emit_u8(E, 0x48); emit_u8(E, 0x85); emit_u8(E, 0xC0);
            int bad = new_label(E), done = new_label(E);
            emit_jmp_rel32(E, bad, 4);
            push_rax(E); mov_reg_rax(E, 7); mov_reg_imm64(E, 6, 0); mov_reg_imm64(E, 2, 2);
            syscall0(E, 8); /* lseek */
            mov_reg_rax(E, 9); pop_reg(E, 7); syscall0(E, 3); mov_rax_reg(E, 9);
            emit_jmp_rel32(E, done, 0);
            define_label(E, bad); mov_rax_imm64(E, (uint64_t)-1);
            define_label(E, done);
        } else if (!strcmp(name, "Memory.Alloc") && !arg->next) {
            /* Linux mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0). */
            gen_expr(E, arg);
            mov_reg_rax(E, 6);                 /* rsi = size */
            mov_reg_imm64(E, 7, 0);           /* rdi = address hint */
            mov_reg_imm64(E, 2, 3);           /* rdx = PROT_READ|PROT_WRITE */
            mov_reg_imm64(E, 10, 0x22);       /* r10 = MAP_PRIVATE|MAP_ANONYMOUS */
            mov_reg_imm64(E, 8, (uint64_t)-1);/* r8 = fd */
            mov_reg_imm64(E, 9, 0);           /* r9 = offset */
            syscall0(E, 9);
        } else if (!strcmp(name, "Memory.Free") && arg->next && !arg->next->next) {
            /* Memory.Free(address, size) -> munmap(address, size). */
            gen_expr(E, arg); push_rax(E);
            gen_expr(E, arg->next); mov_reg_rax(E, 6);
            pop_reg(E, 7); syscall0(E, 11);
        } else if (!strcmp(name, "FS.Data.Write") && arg->next && !arg->next->next) {
            /* FS.Data.Write(data, path): openat + write + close, no libc. */
            gen_expr(E, arg); push_rax(E);             /* data pointer remains at [rsp+8] after fd push */
            gen_expr(E, arg->next);                    /* path pointer in rax */
            mov_reg_rax(E, 6);                         /* rsi = path */
            mov_reg_imm64(E, 7, (uint64_t)-100);       /* AT_FDCWD */
            mov_reg_imm64(E, 2, 577);                  /* O_WRONLY|O_CREAT|O_TRUNC */
            mov_reg_imm64(E, 10, 0644);                /* mode */
            syscall0(E, 257);                           /* openat */
            push_rax(E);                               /* stack: fd, data */
            /* rsi = data from [rsp+8], with the fd kept safely on the stack */
            emit_u8(E, 0x48); emit_u8(E, 0x8B); emit_u8(E, 0x74); emit_u8(E, 0x24); emit_u8(E, 0x08);
            emit_u8(E, 0x48); emit_u8(E, 0x31); emit_u8(E, 0xD2); /* xor rdx,rdx */
            int sl_loop = new_label(E), sl_done = new_label(E);
            define_label(E, sl_loop);
            emit_u8(E, 0x8A); emit_u8(E, 0x04); emit_u8(E, 0x16); /* mov al,[rsi+rdx] */
            emit_u8(E, 0x84); emit_u8(E, 0xC0);
            emit_jmp_rel32(E, sl_done, 1);
            emit_u8(E, 0x48); emit_u8(E, 0xFF); emit_u8(E, 0xC2);
            emit_jmp_rel32(E, sl_loop, 0);
            define_label(E, sl_done);
            emit_u8(E, 0x48); emit_u8(E, 0x8B); emit_u8(E, 0x3C); emit_u8(E, 0x24); /* rdi=[rsp] (fd) */
            mov_rax_imm64(E, 1); syscall_instr(E);      /* write(fd,data,len) */
            mov_reg_rax(E, 8);                         /* preserve write result */
            pop_reg(E, 7);                             /* fd -> rdi */
            pop_rax(E);                                /* discard data pointer */
            mov_rax_imm64(E, 3); syscall_instr(E);      /* close(fd) */
            mov_rax_reg(E, 8);                         /* return write result */
        } else if (!strcmp(name, "System.Exit") && !arg->next) {
            gen_expr(E, arg); mov_reg_rax(E, 7); syscall0(E, 60);
        } else {
            error(E, "the GoodNative Emitter 2.4 does not support call '%s' yet (see docs/NATIVE-BACKENDS.md)", name);
        }
        break;
    }
    default:
        error(E, "unsupported expression kind %d in direct ELF mode", (int)n->type);
        break;
    }
}




/* The Int path above became messy. Let's simplify the whole file with a cleaner
 * implementation focused on getting hello.purple working first, then extend. */

void elfemit_init(ElfEmit *E) {
    memset(E, 0, sizeof *E);
}

void elfemit_free(ElfEmit *E) {
    free(E->code);
    free(E->rodata);
    for (int i = 0; i < E->str_count; i++) free(E->strs[i].s);
    free(E->strs);
    free(E->fixups);
    free(E->labels);
    for (int i = 0; i < E->local_count; i++) { free(E->locals[i].name); free(E->locals[i].sname); }
    for (int i = 0; i < E->struct_count; i++) {
        for (int k = 0; k < E->structs[i].nfields; k++) { free(E->structs[i].f[k].name); free(E->structs[i].f[k].tname); }
        free(E->structs[i].name);
    }
    free(E->structs);
    memset(E, 0, sizeof *E);
}

/* Because the first draft of gen_expr for Int became too tangled, we rewrite
 * a clean, tested path focused on hello + basic arithmetic/control, and a
 * simple integer-to-decimal that is known correct. */

static void gen_print_string(ElfEmit *E, ASTNode *arg) {
    gen_expr(E, arg); /* rax = ptr */
    mov_rsi_rax(E);
    mov_rax_imm64(E, 1);
    mov_rdi_rax(E);
    /* strlen */
    emit_u8(E, 0x48); emit_u8(E, 0x31); emit_u8(E, 0xD2); /* xor rdx,rdx */
    int lab_loop = new_label(E);
    int lab_done = new_label(E);
    define_label(E, lab_loop);
    emit_u8(E, 0x8A); emit_u8(E, 0x04); emit_u8(E, 0x16); /* mov al,[rsi+rdx] */
    emit_u8(E, 0x84); emit_u8(E, 0xC0);
    emit_jmp_rel32(E, lab_done, 1);
    emit_u8(E, 0x48); emit_u8(E, 0xFF); emit_u8(E, 0xC2); /* inc rdx */
    emit_jmp_rel32(E, lab_loop, 0);
    define_label(E, lab_done);
    mov_rax_imm64(E, 1);
    syscall_instr(E);
    /* newline */
    uint32_t nl = add_string(E, "\n");
    mov_rax_imm64(E, 1); mov_rdi_rax(E);
    emit_u8(E, 0x48); emit_u8(E, 0xB8);
    size_t patch = E->code_len;
    emit_u64(E, 0);
    if (E->fixup_count >= E->fixup_cap) {
        E->fixup_cap = E->fixup_cap ? E->fixup_cap * 2 : 32;
        E->fixups = realloc(E->fixups, E->fixup_cap * sizeof *E->fixups);
    }
    E->fixups[E->fixup_count].patch_at = patch;
    E->fixups[E->fixup_count].label_id = -((int)nl + 1);
    E->fixups[E->fixup_count].is_jz = 0;
    E->fixup_count++;
    mov_rsi_rax(E);
    mov_rax_imm64(E, 1); mov_rdx_rax(E);
    mov_rax_imm64(E, 1);
    syscall_instr(E);
}

static void gen_print_int(ElfEmit *E, ASTNode *arg) {
    /* Decimal print for non-negative ints used by the examples. */
    gen_expr(E, arg); /* rax = signed 64-bit value */
    sub_rsp_imm32(E, 32);
    /* Keep a sign flag in r9. Negating INT64_MIN intentionally leaves the
       0x8000000000000000 bit pattern, which DIV treats as unsigned magnitude. */
    emit_u8(E, 0x45); emit_u8(E, 0x31); emit_u8(E, 0xC9); /* xor r9d,r9d */
    emit_u8(E, 0x48); emit_u8(E, 0x85); emit_u8(E, 0xC0); /* test rax,rax */
    emit_u8(E, 0x0F); emit_u8(E, 0x89); /* jns positive */
    size_t sign_patch = E->code_len; emit_u32(E, 0);
    emit_u8(E, 0x49); emit_u8(E, 0xB9); emit_u64(E, 1); /* mov r9,1 */
    emit_u8(E, 0x48); emit_u8(E, 0xF7); emit_u8(E, 0xD8); /* neg rax */
    emit_u8(E, 0x49); emit_u8(E, 0x89); emit_u8(E, 0xC0); /* r8=abs(value) */
    size_t positive_pos = E->code_len;
    int32_t sign_rel = (int32_t)(positive_pos - (sign_patch + 4));
    memcpy(E->code + sign_patch, &sign_rel, 4);
    emit_u8(E, 0x49); emit_u8(E, 0x89); emit_u8(E, 0xC0); /* r8 = magnitude (also covers positive values) */
    emit_u8(E, 0x48); emit_u8(E, 0x89); emit_u8(E, 0xE6); /* rsi = rsp */
    emit_u8(E, 0x48); emit_u8(E, 0x83); emit_u8(E, 0xC6); emit_u8(E, 31);
    emit_u8(E, 0xC6); emit_u8(E, 0x06); emit_u8(E, 0); /* *rsi = 0 */

    int Lloop = new_label(E);
    int Ldone = new_label(E);
    int Lzero = new_label(E);

    emit_u8(E, 0x4D); emit_u8(E, 0x85); emit_u8(E, 0xC0); /* test r8,r8 */
    emit_jmp_rel32(E, Lzero, 1); /* jz zero */

    define_label(E, Lloop);
    emit_u8(E, 0x4C); emit_u8(E, 0x89); emit_u8(E, 0xC0); /* rax = r8 */
    emit_u8(E, 0x48); emit_u8(E, 0x31); emit_u8(E, 0xD2); /* rdx = 0 */
    emit_u8(E, 0x48); emit_u8(E, 0xB9); emit_u64(E, 10);  /* rcx = 10 */
    emit_u8(E, 0x48); emit_u8(E, 0xF7); emit_u8(E, 0xF1); /* div rcx */
    emit_u8(E, 0x49); emit_u8(E, 0x89); emit_u8(E, 0xC0); /* r8 = quot */
    emit_u8(E, 0x48); emit_u8(E, 0x83); emit_u8(E, 0xC2); emit_u8(E, '0');
    emit_u8(E, 0x48); emit_u8(E, 0xFF); emit_u8(E, 0xCE); /* dec rsi */
    emit_u8(E, 0x88); emit_u8(E, 0x16);                   /* [rsi]=dl */
    emit_u8(E, 0x4D); emit_u8(E, 0x85); emit_u8(E, 0xC0);
    /* jnz Lloop */
    emit_u8(E, 0x0F); emit_u8(E, 0x85);
    size_t jnz_patch = E->code_len;
    emit_u32(E, 0);
    if (E->fixup_count >= E->fixup_cap) {
        E->fixup_cap = E->fixup_cap ? E->fixup_cap * 2 : 32;
        E->fixups = realloc(E->fixups, E->fixup_cap * sizeof *E->fixups);
    }
    E->fixups[E->fixup_count].patch_at = jnz_patch;
    E->fixups[E->fixup_count].label_id = Lloop;
    E->fixups[E->fixup_count].is_jz = 2;
    E->fixup_count++;
    emit_jmp_rel32(E, Ldone, 0);

    define_label(E, Lzero);
    emit_u8(E, 0x48); emit_u8(E, 0xFF); emit_u8(E, 0xCE);
    emit_u8(E, 0xC6); emit_u8(E, 0x06); emit_u8(E, '0');

    define_label(E, Ldone);
    /* Prefix '-' after digit conversion so INT64_MIN is handled without
       signed overflow or a signed divide. */
    emit_u8(E, 0x4D); emit_u8(E, 0x85); emit_u8(E, 0xC9); /* test r9,r9 */
    emit_u8(E, 0x0F); emit_u8(E, 0x84); size_t nosign_patch=E->code_len; emit_u32(E,0);
    emit_u8(E, 0x48); emit_u8(E, 0xFF); emit_u8(E, 0xCE); /* dec rsi */
    emit_u8(E, 0xC6); emit_u8(E, 0x06); emit_u8(E, '-');
    size_t nosign_pos=E->code_len; int32_t nosign_rel=(int32_t)(nosign_pos-(nosign_patch+4)); memcpy(E->code+nosign_patch,&nosign_rel,4);
    emit_u8(E, 0x48); emit_u8(E, 0x89); emit_u8(E, 0xE2); /* rdx = rsp */
    emit_u8(E, 0x48); emit_u8(E, 0x83); emit_u8(E, 0xC2); emit_u8(E, 31);
    emit_u8(E, 0x48); emit_u8(E, 0x29); emit_u8(E, 0xF2); /* len = end-rsi */

    mov_rax_imm64(E, 1); mov_rdi_rax(E);
    mov_rax_imm64(E, 1);
    syscall_instr(E);
    emit_u8(E, 0x48); emit_u8(E, 0x81); emit_u8(E, 0xC4); emit_u32(E, 32);

    uint32_t nl = add_string(E, "\n");
    mov_rax_imm64(E, 1); mov_rdi_rax(E);
    emit_u8(E, 0x48); emit_u8(E, 0xB8);
    size_t patch = E->code_len;
    emit_u64(E, 0);
    if (E->fixup_count >= E->fixup_cap) {
        E->fixup_cap = E->fixup_cap ? E->fixup_cap * 2 : 32;
        E->fixups = realloc(E->fixups, E->fixup_cap * sizeof *E->fixups);
    }
    E->fixups[E->fixup_count].patch_at = patch;
    E->fixups[E->fixup_count].label_id = -((int)nl + 1);
    E->fixups[E->fixup_count].is_jz = 0;
    E->fixup_count++;
    mov_rsi_rax(E);
    mov_rax_imm64(E, 1); mov_rdx_rax(E);
    mov_rax_imm64(E, 1);
    syscall_instr(E);
}

static void gen_stmt(ElfEmit *E, ASTNode *n) {
    if (!n || E->had_error) return;
    switch (n->type) {
    case NODE_EXPR_STMT:
        gen_expr(E, n->left);
        break;
    case NODE_VAR_DECL: {
        int idx = find_local(E, n->name);
        if (n->right) {
            gen_expr(E, n->right);
            if (idx >= 0) store_local(E, idx);
        } else if (idx >= 0) {
            if (E->locals[idx].slab > 0) {
                int32_t so = E->locals[idx].slab_off;
                B(0x48, 0x8D, 0xBD); emit_u32(E, (uint32_t)so);                 /* lea rdi,[rbp+so] */
                B(0xB9); emit_u32(E, (uint32_t)(E->locals[idx].slab / 8));      /* mov ecx,qwords */
                B(0x31, 0xC0); B(0xF3, 0x48, 0xAB);                              /* xor eax,eax; rep stosq */
                B(0x48, 0x8D, 0x85); emit_u32(E, (uint32_t)so);                 /* lea rax,[rbp+so] */
            } else {
                mov_rax_imm64(E, 0);
            }
            store_local(E, idx);
        }
        break;
    }
    case NODE_ASSIGN: {
        gen_expr(E, n->right);
        const char *vname = n->name;
        if (!vname && n->left && n->left->type == NODE_IDENT) vname = n->left->name;
        int idx = find_local(E, vname);
        if (idx < 0) error(E, "assign to undefined '%s'", vname ? vname : "(null)");
        else store_local(E, idx);
        break;
    }
    case NODE_MEMBER_ASSIGN: {
        int fb, fo; PurpleType ft; const char *ftn;
        if (!n->left || !n->left->name || !resolve_field(E, n->left->name, &fb, &fo, &ft, &ftn)) {
            error(E, "unknown struct field '%s'", n->left && n->left->name ? n->left->name : "?");
            break;
        }
        if (ft == TY_STRUCT) { error(E, "cannot assign a whole nested struct '%s'; assign its fields", n->left->name); break; }
        gen_expr(E, n->right); push_rax(E);
        load_local(E, fb); B(0x48, 0x89, 0xC1); pop_rax(E);          /* rcx = struct ptr */
        B(0x48, 0x89, 0x81); emit_u32(E, (uint32_t)fo);               /* mov [rcx+off],rax */
        break;
    }
    case NODE_RAW_ASSIGN:
        if (n->left && n->left->type == NODE_HEX_ADDR && n->right) {
            gen_expr(E, n->right);
            B(0x48, 0xB9); emit_u64(E, n->left->addr_val);              /* mov rcx,addr */
            B(0x48, 0x89, 0x01);                                         /* mov [rcx],rax */
        } else error(E, "malformed raw assignment");
        break;
    case NODE_STRUCT_DEF:
        break;
    case NODE_RETURN:
        if (n->left) gen_expr(E, n->left);
        else mov_rax_imm64(E, 0);
        emit_jmp_rel32(E, E->epilogue_label, 0);
        break;
    case NODE_IF: {
        /* if (cond) body [else elsebody] */
        gen_expr(E, n->left); /* cond */
        test_rax_rax(E);
        int lab_else = new_label(E);
        int lab_end = new_label(E);
        emit_jmp_rel32(E, lab_else, 1); /* jz else */
        if (n->body) gen_stmt(E, n->body);
        emit_jmp_rel32(E, lab_end, 0);
        define_label(E, lab_else);
        if (n->right) gen_stmt(E, n->right);
        define_label(E, lab_end);
        break;
    }
    case NODE_WHILE: {
        int lab_top = new_label(E);
        int lab_end = new_label(E);
        define_label(E, lab_top);
        gen_expr(E, n->left);
        test_rax_rax(E);
        emit_jmp_rel32(E, lab_end, 1);
        if (n->body) gen_stmt(E, n->body);
        emit_jmp_rel32(E, lab_top, 0);
        define_label(E, lab_end);
        break;
    }
    case NODE_BLOCK:
        for (ASTNode *s = n->body; s; s = s->next) gen_stmt(E, s);
        break;
    case NODE_HIER_CALL:
        gen_expr(E, n);
        break;
    default:
        /* try as expression statement */
        gen_expr(E, n);
        break;
    }
}

static void gen_function(ElfEmit *E, ASTNode *fn, int label) {
    for (int i = 0; i < E->local_count; i++) { free(E->locals[i].name); free(E->locals[i].sname); E->locals[i].name = NULL; E->locals[i].sname = NULL; }
    E->local_count = 0;
    E->cur_fn = fn;
    int np = 0;
    for (ASTNode *a = fn->args; a; a = a->next) np++;
    for (ASTNode *a = fn->args; a; a = a->next) {
        int li = add_local_idx(E, a->name, a->var_type ? a->var_type : TY_INT);
        if (li >= 0 && a->var_type == TY_STRUCT && a->type_name && find_struct(E, a->type_name)) E->locals[li].sname = strdup(a->type_name);
    }
    collect_locals(E, fn->body);
    int ast_pos = 1;
    for (ASTNode *a = fn->args; a; a = a->next) mark_local_use(E, a->name, 0);
    analyze_intervals(E, fn->body, &ast_pos);
    assign_local_offsets(E);
    if (E->dump_regalloc) elfemit_dump_regalloc(E, stderr);

    define_label(E, label);
    push_rbp(E);
    mov_rbp_rsp(E);
    B(0x53, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57);    /* push rbx,r12,r13,r14,r15 */
    if (E->frame_size) sub_rsp_imm32(E, E->frame_size);
    int pi = 0;
    for (ASTNode *a = fn->args; a; a = a->next, pi++) {
        int idx = find_local(E, a->name);
        B(0x48, 0x8B, 0x85); emit_u32(E, (uint32_t)(16 + 8 * (np - 1 - pi)));   /* mov rax,[rbp+disp] */
        if (idx >= 0) store_local(E, idx);
    }
    E->epilogue_label = new_label(E);
    if (fn->body) gen_stmt(E, fn->body);
    mov_rax_imm64(E, 0);
    define_label(E, E->epilogue_label);
    B(0x48, 0x8D, 0x65, 0xD8);                                   /* lea rsp,[rbp-40] */
    B(0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5B);     /* pop r15,r14,r13,r12,rbx */
    B(0x5D, 0xC3);                                               /* pop rbp; ret */
}

static void gen_runtime(ElfEmit *E) {
    /* rt_alloc: rax=bytes -> rax=zeroed pointer (16-byte hidden header holds the mapping size), 0 on failure */
    define_label(E, E->rt_alloc);
    B(0x48, 0x8D, 0x70, 0x10); B(0x56);
    mov_reg_imm64(E, 7, 0); mov_reg_imm64(E, 2, 3); mov_reg_imm64(E, 10, 0x22);
    mov_reg_imm64(E, 8, (uint64_t)-1); mov_reg_imm64(E, 9, 0);
    mov_rax_imm64(E, 9); syscall_instr(E);
    B(0x5E);
    B(0x48, 0x3D, 0x00, 0xF0, 0xFF, 0xFF);
    int fail = new_label(E);
    emit_jmp_rel32(E, fail, 6);
    B(0x48, 0x89, 0x30); B(0x48, 0x83, 0xC0, 0x10); B(0xC3);
    define_label(E, fail); B(0x31, 0xC0); B(0xC3);
    /* rt_free: rax=pointer from rt_alloc (null is ignored) */
    define_label(E, E->rt_free);
    int fdone = new_label(E);
    B(0x48, 0x85, 0xC0); emit_jmp_rel32(E, fdone, 1);
    B(0x48, 0x8D, 0x78, 0xF0); B(0x48, 0x8B, 0x37);
    mov_rax_imm64(E, 11); syscall_instr(E);
    define_label(E, fdone); B(0xC3);
    /* rt_array_new: rax=length -> [len][cap][elements...], matching the hosted runtime layout */
    define_label(E, E->rt_array_new);
    int neg = new_label(E), have = new_label(E), zc = new_label(E), capok = new_label(E), out = new_label(E);
    B(0x48, 0x89, 0xC1); B(0x48, 0x85, 0xC9);
    emit_jmp_rel32(E, neg, 4); emit_jmp_rel32(E, have, 0);
    define_label(E, neg); B(0x31, 0xC9);
    define_label(E, have); B(0x48, 0x89, 0xCA); B(0x48, 0x85, 0xD2);
    emit_jmp_rel32(E, zc, 1); emit_jmp_rel32(E, capok, 0);
    define_label(E, zc); mov_reg_imm64(E, 2, 8);
    define_label(E, capok);
    B(0x51, 0x52);
    B(0x48, 0x8D, 0x04, 0xD5, 0x10, 0x00, 0x00, 0x00);           /* lea rax,[rdx*8+16] */
    emit_call_label(E, E->rt_alloc);
    B(0x5A, 0x59);
    B(0x48, 0x85, 0xC0); emit_jmp_rel32(E, out, 1);
    B(0x48, 0x89, 0x08); B(0x48, 0x89, 0x50, 0x08);
    define_label(E, out); B(0xC3);
}

int elfemit_program(ElfEmit *E, ASTNode *prog) {
    if (!prog || prog->type != NODE_PROGRAM) {
        error(E, "expected program node");
        return 0;
    }
    build_structs(E, prog);
    int main_idx = -1;
    for (ASTNode *f = prog->body; f; f = f->next) {
        if (f->type != NODE_FN || !f->name) continue;
        if (find_func(E, f->name) >= 0) { error(E, "function '%s' defined twice", f->name); return 0; }
        if (E->func_count >= 256) { error(E, "too many functions"); return 0; }
        E->funcs[E->func_count].name = f->name;
        E->funcs[E->func_count].fn = f;
        E->funcs[E->func_count].label = new_label(E);
        if (!strcmp(f->name, "main")) main_idx = E->func_count;
        E->func_count++;
    }
    if (main_idx < 0) {
        error(E, "no main function");
        return 0;
    }
    E->rt_alloc = new_label(E); E->rt_free = new_label(E); E->rt_array_new = new_label(E);
    /* _start: remember envp (for DISPLAY/HOME lookups), then status = main() */
    B(0x48, 0x8B, 0x04, 0x24);                 /* mov rax,[rsp]        ; argc */
    B(0x48, 0x8D, 0x54, 0xC4, 0x10);           /* lea rdx,[rsp+rax*8+16] ; envp */
    B(0x48, 0xB9); emit_u64(E, BSS_ADDR + 2488); /* mov rcx, &globals.envp */
    B(0x48, 0x89, 0x11);                       /* mov [rcx],rdx */
    emit_call_label(E, E->funcs[main_idx].label);
    mov_rdi_rax(E);
    mov_rax_imm64(E, 60);
    syscall_instr(E);
    gen_runtime(E);
    for (int i = 0; i < E->func_count && !E->had_error; i++)
        gen_function(E, E->funcs[i].fn, E->funcs[i].label);
    if (E->had_error) return 0;
    resolve_fixups(E);
    return !E->had_error;
}

int elfemit_write_elf(ElfEmit *E, const char *path) {
    if (E->had_error) return 0;

    /* Layout:
       offset 0:      Elf64_Ehdr
       offset 64:     Elf64_Phdr (2 entries: text+rodata RX, optional)
       offset 0x1000: code
       after code (page align): rodata
    */
    size_t text_off = 0x1000;
    size_t text_vaddr = LOAD_ADDR + 0x1000;
    size_t ro_off = (text_off + E->code_len + 0xFFF) & ~((size_t)0xFFF);
    size_t ro_vaddr = LOAD_ADDR + ro_off;

    /* Patch string movabs immediates now that we know ro_vaddr */
    for (int i = 0; i < E->fixup_count; i++) {
        if (E->fixups[i].label_id < 0) {
            int off = -E->fixups[i].label_id - 1;
            uint64_t abs = ro_vaddr + (uint64_t)off;
            memcpy(E->code + E->fixups[i].patch_at, &abs, 8);
        }
    }

    size_t file_size = ro_off + E->rodata_len;
    uint8_t *file = calloc(1, file_size);
    if (!file) return 0;

    Elf64_Ehdr *eh = (Elf64_Ehdr *)file;
    eh->e_ident[0] = 0x7F; eh->e_ident[1] = 'E'; eh->e_ident[2] = 'L'; eh->e_ident[3] = 'F';
    eh->e_ident[4] = 2; /* ELFCLASS64 */
    eh->e_ident[5] = 1; /* ELFDATA2LSB */
    eh->e_ident[6] = 1; /* EV_CURRENT */
    eh->e_type = ET_EXEC;
    eh->e_machine = EM_X86_64;
    eh->e_version = EV_CURRENT;
    eh->e_entry = text_vaddr;
    eh->e_phoff = 64;
    eh->e_ehsize = 64;
    eh->e_phentsize = 56;
    eh->e_phnum = 2; /* RX text+rodata, and a zeroed RW data segment */

    Elf64_Phdr *ph = (Elf64_Phdr *)(file + 64);
    ph->p_type = PT_LOAD;
    ph->p_flags = PF_R | PF_X;
    ph->p_offset = 0;
    ph->p_vaddr = LOAD_ADDR;
    ph->p_paddr = LOAD_ADDR;
    ph->p_filesz = file_size;
    ph->p_memsz = file_size;
    ph->p_align = 0x1000;

    Elf64_Phdr *ph2 = (Elf64_Phdr *)(file + 64 + 56);
    ph2->p_type = PT_LOAD;
    ph2->p_flags = PF_R | PF_W;
    ph2->p_offset = 0;
    ph2->p_vaddr = BSS_ADDR;
    ph2->p_paddr = BSS_ADDR;
    ph2->p_filesz = 0;
    ph2->p_memsz = BSS_SIZE;
    ph2->p_align = 0x1000;

    memcpy(file + text_off, E->code, E->code_len);
    if (E->rodata_len)
        memcpy(file + ro_off, E->rodata, E->rodata_len);

    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0755);
    if (fd < 0) {
        perror(path);
        free(file);
        return 0;
    }
    ssize_t w = write(fd, file, file_size);
    close(fd);
    free(file);
    if (w != (ssize_t)file_size) {
        fprintf(stderr, "Purplecode: short write to %s\n", path);
        return 0;
    }
    return 1;
}


/* ---- Purplecode-source runtime prelude (File.*, Net.*, encoders...) ---- */
static void collect_call_names(ASTNode *n, const char ***names, int *count, int *cap) {
    if (!n) return;
    if (n->type == NODE_HIER_CALL && n->name) {
        if (*count >= *cap) { *cap = *cap ? *cap * 2 : 64; *names = realloc(*names, *cap * sizeof **names); }
        (*names)[(*count)++] = n->name;
    }
    if (n->type == NODE_BLOCK) { for (ASTNode *s = n->body; s; s = s->next) collect_call_names(s, names, count, cap); return; }
    collect_call_names(n->body, names, count, cap);
    collect_call_names(n->left, names, count, cap);
    collect_call_names(n->right, names, count, cap);
    collect_call_names(n->args, names, count, cap);
    /* statement/argument chains */
    if (n->next && n->type != NODE_FN) collect_call_names(n->next, names, count, cap);
}

/* Append the prelude functions reachable from the program to prog->body; returns the original tail
 * so the caller can detach them again (tail->next = NULL) before using the AST elsewhere. */
ASTNode *elfemit_attach_prelude(ASTNode *prog, ASTNode *prelude) {
    if (!prog || !prelude || !prog->body) return NULL;
    ASTNode *tail = prog->body; while (tail->next) tail = tail->next;
    int total = 0; for (ASTNode *f = prelude->body; f; f = f->next) total++;
    ASTNode **fns = calloc(total ? total : 1, sizeof *fns); char *sel = calloc(total ? total : 1, 1);
    int k = 0; for (ASTNode *f = prelude->body; f; f = f->next) fns[k++] = f;
    const char **names = NULL; int ncount = 0, ncap = 0;
    for (ASTNode *f = prog->body; f; f = f->next) if (f->type == NODE_FN) collect_call_names(f->body, &names, &ncount, &ncap);
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int i = 0; i < total; i++) {
            if (sel[i] || !fns[i]->name) continue;
            int hit = 0;
            for (int j = 0; j < ncount && !hit; j++) if (!strcmp(names[j], fns[i]->name)) hit = 1;
            if (!hit) continue;
            int user_has = 0;
            for (ASTNode *f = prog->body; f; f = f->next) if (f->type == NODE_FN && f->name && !strcmp(f->name, fns[i]->name)) user_has = 1;
            sel[i] = 1; changed = 1;
            if (!user_has) collect_call_names(fns[i]->body, &names, &ncount, &ncap);
        }
    }
    ASTNode *last = tail;
    for (int i = 0; i < total; i++) {
        if (!sel[i]) continue;
        int user_has = 0;
        for (ASTNode *f = prog->body; f && f != last->next; f = f->next) if (f->type == NODE_FN && f->name && !strcmp(f->name, fns[i]->name)) user_has = 1;
        if (user_has) continue;
        last->next = fns[i]; last = fns[i]; last->next = NULL;
    }
    free(names); free(fns); free(sel);
    return tail;
}
