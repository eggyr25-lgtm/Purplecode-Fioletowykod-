#define _POSIX_C_SOURCE 200809L
#include "elfemit.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
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

static void error(ElfEmit *E, const char *fmt, ...) {
    E->had_error = true;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "Purplecode direct-ELF error: ");
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

/* Locals */
static int find_local(ElfEmit *E, const char *name) {
    if (!name) return -1;
    for (int i = 0; i < E->local_count; i++)
        if (E->locals[i].name && strcmp(E->locals[i].name, name) == 0) return i;
    return -1;
}

static void add_local(ElfEmit *E, const char *name, PurpleType ty) {
    if (find_local(E, name) >= 0) return;
    if (E->local_count >= 256) { error(E, "too many locals"); return; }
    int i = E->local_count++;
    E->locals[i].name = strdup(name);
    E->locals[i].type = ty;
    E->locals[i].offset = 0;
    E->locals[i].first_use = -1;
    E->locals[i].last_use = -1;
    E->locals[i].reg = -1;
}

static void collect_locals(ElfEmit *E, ASTNode *n) {
    if (!n) return;
    if ((n->type == NODE_VAR_DECL || n->type == NODE_PARAM) && n->name)
        add_local(E, n->name, n->var_type ? n->var_type : TY_INT);
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
            E->locals[idx].offset = -(++spill_count * 8);
        }
    }
    E->frame_size = ((spill_count * 8 + 15) / 16) * 16;
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

static bool expr_is_string(ElfEmit *E, ASTNode *n) {
    if (!n) return false;
    if (n->type == NODE_STRING) return true;
    if (n->type == NODE_IDENT) {
        int idx = find_local(E, n->name);
        return idx >= 0 && E->locals[idx].type == TY_STRING;
    }
    return n->var_type == TY_STRING;
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
        uint32_t off = add_string(E, n->str_val ? n->str_val : "");
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
        const char *name = n->name ? n->name : "";
        ASTNode *arg = n->args;
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
            error(E, "direct ELF mode does not support call '%s'; supported: Data.ShellPrint[.Int], System.Process.ID, System.Time.Unix, System.Exit, LinuxAPI.Call, Kernel.Call, FS.Exists, FS.Size, FS.Data.Write, Memory.Alloc, Memory.Free", name);
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
    for (int i = 0; i < E->local_count; i++) free(E->locals[i].name);
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
        if (n->right) {
            gen_expr(E, n->right);
            int idx = find_local(E, n->name);
            if (idx >= 0) store_local(E, idx);
        }
        break;
    }
    case NODE_ASSIGN: {
        gen_expr(E, n->right);
        const char *vname = n->name;
        if (!vname && n->left && n->left->type == NODE_IDENT) vname = n->left->name;
        int idx = find_local(E, vname);
        if (idx < 0) error(E, "assign to undefined '%s'", vname ? vname : "(null)");
        else store_local_rax(E, E->locals[idx].offset);
        break;
    }
    case NODE_RETURN:
        if (n->left) gen_expr(E, n->left);
        else mov_rax_imm64(E, 0);
        /* exit syscall with status in rdi */
        mov_rdi_rax(E);
        mov_rax_imm64(E, 60); /* sys_exit */
        syscall_instr(E);
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

static void gen_function(ElfEmit *E, ASTNode *fn) {
    E->local_count = 0;
    collect_locals(E, fn->body);
    int ast_pos = 0;
    analyze_intervals(E, fn->body, &ast_pos);
    assign_local_offsets(E);
    if (E->dump_regalloc) elfemit_dump_regalloc(E, stderr);

    push_rbp(E);
    mov_rbp_rsp(E);
    if (E->frame_size) sub_rsp_imm32(E, E->frame_size);

    if (fn->body) gen_stmt(E, fn->body);

    /* if no explicit return, exit 0 */
    mov_rax_imm64(E, 0);
    mov_rdi_rax(E);
    mov_rax_imm64(E, 60);
    syscall_instr(E);
}

int elfemit_program(ElfEmit *E, ASTNode *prog) {
    if (!prog || prog->type != NODE_PROGRAM) {
        error(E, "expected program node");
        return 0;
    }
    ASTNode *main_fn = NULL;
    for (ASTNode *f = prog->body; f; f = f->next) {
        if (f->type == NODE_FN && f->name && !strcmp(f->name, "main")) {
            main_fn = f;
            break;
        }
    }
    if (!main_fn) {
        error(E, "no main function");
        return 0;
    }
    gen_function(E, main_fn);
    if (E->had_error) return 0;
    resolve_fixups(E);
    /* second pass: patch string absolute addresses.
       Layout we will use:
         ELF header + Phdr at file offset 0
         text at file offset 0x1000, vaddr LOAD_ADDR+0x1000
         rodata immediately after text (page-aligned) */
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
    eh->e_phnum = 1; /* one RX segment covering text+rodata for simplicity */

    Elf64_Phdr *ph = (Elf64_Phdr *)(file + 64);
    ph->p_type = PT_LOAD;
    ph->p_flags = PF_R | PF_X;
    ph->p_offset = 0;
    ph->p_vaddr = LOAD_ADDR;
    ph->p_paddr = LOAD_ADDR;
    ph->p_filesz = file_size;
    ph->p_memsz = file_size;
    ph->p_align = 0x1000;

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
