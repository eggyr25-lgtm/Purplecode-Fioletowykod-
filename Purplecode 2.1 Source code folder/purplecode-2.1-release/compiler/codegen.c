#include <ctype.h>
#define _POSIX_C_SOURCE 200809L
#include "codegen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

void codegen_init(CodeGen *CG, FILE *out) {
    memset(CG, 0, sizeof *CG);
    CG->out = out;
    CG->label_counter = 0;
    CG->str_counter = 0;
    CG->current_function = NULL;
    CG->local_count = 0;
    CG->frame_size = 48;
    CG->had_error = false;
    CG->struct_count = 0;
    CG->program_root = NULL;
}

static void type_error(CodeGen *CG, ASTNode *n, const char *fmt, ...) {
    (void)n;
    va_list ap;
    fprintf(stderr, "Purplecode type error: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    CG->had_error = true;
}

static const char *type_name(PurpleType t) {
    switch (t) {
        case TY_INT: return "int";
        case TY_BOOL: return "bool";
        case TY_STRING: return "string";
        case TY_STRUCT: return "struct";
        default: return "unknown";
    }
}

/* ---------- Milestone 1.7 struct support ---------- */

static LocalVar *find_local(CodeGen *CG, const char *name); /* forward */

static StructDef *find_struct(CodeGen *CG, const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < CG->struct_count; ++i)
        if (CG->structs[i].name && strcmp(CG->structs[i].name, name) == 0)
            return &CG->structs[i];
    return NULL;
}

static int type_size(CodeGen *CG, PurpleType t, const char *type_name) {
    switch (t) {
        case TY_INT: case TY_BOOL: case TY_STRING: return 8;
        case TY_STRUCT: {
            StructDef *sd = find_struct(CG, type_name);
            return sd ? sd->size : 8;
        }
        default: return 8;
    }
}

static void layout_struct(CodeGen *CG, StructDef *sd) {
    int off = 0;
    for (int i = 0; i < sd->field_count; ++i) {
        StructField *f = &sd->fields[i];
        f->size = type_size(CG, f->type, f->type_name);
        if (off % 8) off += 8 - (off % 8);
        f->offset = off;
        off += f->size;
    }
    if (off % 8) off += 8 - (off % 8);
    sd->size = off > 0 ? off : 8;
}

static void register_structs(CodeGen *CG, ASTNode *prog) {
    CG->struct_count = 0;
    for (ASTNode *n = prog ? prog->body : NULL; n; n = n->next) {
        if (n->type != NODE_STRUCT_DEF || !n->name) continue;
        if (CG->struct_count >= PURPLE_MAX_STRUCTS) {
            fprintf(stderr, "Purplecode: too many structs (max %d)\n", PURPLE_MAX_STRUCTS);
            CG->had_error = true;
            return;
        }
        StructDef *sd = &CG->structs[CG->struct_count++];
        memset(sd, 0, sizeof *sd);
        sd->name = strdup(n->name);
        for (ASTNode *f = n->body; f; f = f->next) {
            if (f->type != NODE_FIELD || !f->name) continue;
            if (sd->field_count >= PURPLE_MAX_FIELDS) break;
            StructField *sf = &sd->fields[sd->field_count++];
            sf->name = strdup(f->name);
            sf->type = f->var_type;
            sf->type_name = f->type_name ? strdup(f->type_name) : NULL;
        }
    }
    for (int pass = 0; pass < 4; ++pass)
        for (int i = 0; i < CG->struct_count; ++i)
            layout_struct(CG, &CG->structs[i]);
}

/* Resolve "base.field.sub" → base local name + total byte offset + final type */
static PurpleType resolve_field_path(CodeGen *CG, const char *dotted,
                                     char *base_name, size_t base_len,
                                     int *out_offset) {
    *out_offset = 0;
    if (!dotted) return TY_NONE;

    const char *dot = strchr(dotted, '.');
    size_t blen = dot ? (size_t)(dot - dotted) : strlen(dotted);
    if (blen >= base_len) blen = base_len - 1;
    memcpy(base_name, dotted, blen);
    base_name[blen] = 0;

    LocalVar *lv = find_local(CG, base_name);
    if (!lv) return TY_NONE;
    if (!dot) return lv->type;

    const char *p = dot + 1;
    PurpleType cur_type = lv->type;
    const char *cur_type_name = lv->type_name;
    int offset = 0;

    while (p && *p) {
        if (cur_type != TY_STRUCT || !cur_type_name) {
            type_error(CG, NULL, "field access on non-struct in '%s'", dotted);
            return TY_NONE;
        }
        StructDef *sd = find_struct(CG, cur_type_name);
        if (!sd) {
            type_error(CG, NULL, "unknown struct '%s'", cur_type_name);
            return TY_NONE;
        }
        const char *next_dot = strchr(p, '.');
        size_t flen = next_dot ? (size_t)(next_dot - p) : strlen(p);
        char fname[128];
        if (flen >= sizeof fname) flen = sizeof fname - 1;
        memcpy(fname, p, flen);
        fname[flen] = 0;

        StructField *found = NULL;
        for (int i = 0; i < sd->field_count; ++i)
            if (sd->fields[i].name && strcmp(sd->fields[i].name, fname) == 0) {
                found = &sd->fields[i];
                break;
            }
        if (!found) {
            type_error(CG, NULL, "struct '%s' has no field '%s'", cur_type_name, fname);
            return TY_NONE;
        }
        offset += found->offset;
        cur_type = found->type;
        cur_type_name = found->type_name;
        p = next_dot ? next_dot + 1 : NULL;
    }
    *out_offset = offset;
    return cur_type;
}

/* ---------- end struct helpers ---------- */

static LocalVar *find_local(CodeGen *CG, const char *name) {
    for (int i = 0; i < CG->local_count; ++i)
        if (strcmp(CG->locals[i].name, name) == 0) return &CG->locals[i];
    return NULL;
}

static int param_index(CodeGen *CG, const char *name) {
    int i = 0;
    for (ASTNode *a = CG->current_function ? CG->current_function->args : NULL; a; a = a->next, ++i)
        if (a->name && strcmp(a->name, name) == 0) return i;
    return -1;
}

static void add_local(CodeGen *CG, const char *name, PurpleType type, const char *type_name) {
    if (find_local(CG, name)) {
        type_error(CG, NULL, "duplicate local variable '%s'", name);
        return;
    }
    if (CG->local_count >= PURPLE_MAX_LOCALS) {
        fprintf(stderr, "Purplecode: too many local variables (max %d)\n", PURPLE_MAX_LOCALS);
        CG->had_error = true;
        return;
    }
    int sz = type_size(CG, type, type_name);
    /* grow frame downward */
    CG->frame_size += sz;
    if (CG->frame_size % 16) CG->frame_size += 16 - (CG->frame_size % 16);

    LocalVar *v = &CG->locals[CG->local_count++];
    v->name = strdup(name);
    v->type = type;
    v->type_name = type_name ? strdup(type_name) : NULL;
    v->size = sz;
    v->offset = -CG->frame_size;
}

static void collect_declared_locals(CodeGen *CG, ASTNode *stmt) {
    for (ASTNode *s = stmt; s; s = s->next) {
        if (s->type == NODE_VAR_DECL && s->name)
            add_local(CG, s->name, s->var_type, s->type_name);
        if (s->type == NODE_BLOCK || s->type == NODE_IF || s->type == NODE_WHILE)
            collect_declared_locals(CG, s->body);
        if (s->type == NODE_IF && s->right)
            collect_declared_locals(CG, s->right);
    }
}

static PurpleType infer_expr(CodeGen *CG, ASTNode *n);

void codegen_free(CodeGen *CG) {
    (void)CG;
}

static int next_str(CodeGen *CG) {
    return CG->str_counter++;
}

// Emit a string constant and return its label id
static int emit_string(CodeGen *CG, const char *s) {
    int id = next_str(CG);
    fprintf(CG->out, "    .section .rodata\n");
    fprintf(CG->out, ".LC%d:\n", id);
    fprintf(CG->out, "    .string \"");
    // basic escape
    for (const char *p = s; *p; p++) {
        if (*p == '"' || *p == '\\') fprintf(CG->out, "\\%c", *p);
        else if (*p == '\n') fprintf(CG->out, "\\n");
        else if (*p == '\t') fprintf(CG->out, "\\t");
        else fputc(*p, CG->out);
    }
    fprintf(CG->out, "\"\n");
    fprintf(CG->out, "    .text\n");
    return id;
}

static void gen_expr(CodeGen *CG, ASTNode *n);

/* Evaluate up to six integer/pointer arguments without losing earlier values. */
static int gen_call_args(CodeGen *CG, ASTNode *args, int limit) {
    ASTNode *items[6];
    int argc = 0;
    for (ASTNode *a = args; a; a = a->next) {
        if (argc == limit) {
            fprintf(stderr, "Purplecode: builtin accepts at most %d arguments\n", limit);
            break;
        }
        items[argc++] = a;
    }
    for (int i = argc - 1; i >= 0; --i) {
        gen_expr(CG, items[i]);
        fprintf(CG->out, "    pushq %%rax\n");
    }
    static const char *regs[] = {"%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"};
    for (int i = 0; i < argc; ++i) fprintf(CG->out, "    popq %s\n", regs[i]);
    return argc;
}

static PurpleType infer_expr(CodeGen *CG, ASTNode *n) {
    if (!n) return TY_INT;
    switch (n->type) {
        case NODE_INT: case NODE_HEX_ADDR: case NODE_HIER_CALL: return TY_INT;
        case NODE_BOOL: return TY_BOOL;
        case NODE_STRING: return TY_STRING;
        case NODE_UNARY: return TY_INT;
        case NODE_IDENT: {
            if (n->name && !strcmp(n->name, "$LastHexResult")) return TY_STRING;
            if (n->name && strchr(n->name, '.')) {
                char base[128];
                int foff = 0;
                return resolve_field_path(CG, n->name, base, sizeof base, &foff);
            }
            LocalVar *v = n->name ? find_local(CG, n->name) : NULL;
            if (v) return v->type;
            if (param_index(CG, n->name) >= 0) return TY_INT;
            type_error(CG, n, "undefined variable '%s'", n->name);
            return TY_NONE;
        }
        case NODE_BINARY: {
            const char *op = n->name ? n->name : "";
            PurpleType lt = infer_expr(CG, n->left);
            PurpleType rt = infer_expr(CG, n->right);
            bool comparison = !strcmp(op,"==") || !strcmp(op,"!=") || !strcmp(op,"<") ||
                              !strcmp(op,"<=") || !strcmp(op,">") || !strcmp(op,">=");
            if (comparison) {
                if (lt != TY_INT || rt != TY_INT)
                    type_error(CG, n, "comparison '%s' requires two int operands", op);
                return TY_BOOL;
            }
            if (strcmp(op, ">") == 0 && n->flags == 1) return TY_INT;
            if (lt != TY_INT || rt != TY_INT)
                type_error(CG, n, "arithmetic '%s' requires two int operands", op);
            return TY_INT;
        }
        default: return TY_INT;
    }
}

static void store_local(CodeGen *CG, const char *name, PurpleType value_type) {
    LocalVar *v = name ? find_local(CG, name) : NULL;
    if (!v) {
        type_error(CG, NULL, "assignment to undefined variable '%s'", name);
        return;
    }
    if (value_type != TY_NONE && value_type != v->type && v->type != TY_STRUCT)
        type_error(CG, NULL, "cannot assign %s to %s variable '%s'",
                   type_name(value_type), type_name(v->type), name);
    fprintf(CG->out, "    movq %%rax, %d(%%rbp)\n", v->offset);
}

/* Store %rax into base_local + field_offset */
static void store_field(CodeGen *CG, const char *base, int field_offset) {
    LocalVar *v = base ? find_local(CG, base) : NULL;
    if (!v) {
        type_error(CG, NULL, "assignment to undefined variable '%s'", base);
        return;
    }
    if (v->size == 8 && v->type == TY_STRUCT) {
        /* pointer-to-struct: load the base pointer, then store through it */
        fprintf(CG->out, "    movq %d(%%rbp), %%rcx\n", v->offset); /* base ptr */
        fprintf(CG->out, "    movq %%rax, %d(%%rcx)\n", field_offset);
    } else {
        /* stack-resident struct */
        fprintf(CG->out, "    movq %%rax, %d(%%rbp)\n", v->offset + field_offset);
    }
}

/* Load base_local + field_offset into %rax */
static void load_field(CodeGen *CG, const char *base, int field_offset) {
    LocalVar *v = base ? find_local(CG, base) : NULL;
    if (!v) {
        type_error(CG, NULL, "use of undefined variable '%s'", base);
        fprintf(CG->out, "    movq $0, %%rax\n");
        return;
    }
    if (v->size == 8 && v->type == TY_STRUCT) {
        /* pointer-to-struct */
        fprintf(CG->out, "    movq %d(%%rbp), %%rcx\n", v->offset);
        fprintf(CG->out, "    movq %d(%%rcx), %%rax\n", field_offset);
    } else {
        fprintf(CG->out, "    movq %d(%%rbp), %%rax\n", v->offset + field_offset);
    }
}

static ASTNode *g_program_root = NULL;
static int has_user_function(const char *name) {
    if (!g_program_root || !name) return 0;
    for (ASTNode *f = g_program_root->body; f; f = f->next)
        if (f->type == NODE_FN && f->name && !strcmp(f->name, name)) return 1;
    return 0;
}
static void gen_user_call(CodeGen *CG, ASTNode *n) {
    ASTNode *args[6]; int argc=0;
    for (ASTNode *a=n->args; a; a=a->next) {
        if (argc == 6) { fprintf(stderr, "Purplecode: function calls support at most six arguments\n"); CG->had_error=true; return; }
        args[argc++]=a;
    }
    for (int i=argc-1;i>=0;i--) { gen_expr(CG,args[i]); fprintf(CG->out,"    pushq %%rax\n"); }
    static const char *regs[]={"%rdi","%rsi","%rdx","%rcx","%r8","%r9"};
    for (int i=0;i<argc;i++) fprintf(CG->out,"    popq %s\n",regs[i]);
    fprintf(CG->out,"    call %s\n",n->name);
}
static void gen_hier_call(CodeGen *CG, ASTNode *n) {
    const char *name = n->name ? n->name : "";
    /* Imported Purple Lib functions take precedence over built-in names. */
    if (has_user_function(name)) { gen_user_call(CG,n); return; }

    /* Method call desugaring: p.move(args) where p is a struct (or
       pointer-to-struct) local → TypeName.move(p, args...) */
    {
        const char *dot = name ? strrchr(name, '.') : NULL;
        if (dot && dot != name) {
            char base[128];
            size_t blen = (size_t)(dot - name);
            if (blen >= sizeof base) blen = sizeof base - 1;
            memcpy(base, name, blen);
            base[blen] = 0;
            const char *method = dot + 1;
            LocalVar *lv = find_local(CG, base);
            if (lv && lv->type == TY_STRUCT && lv->type_name && method && *method) {
                char mangled[256];
                snprintf(mangled, sizeof mangled, "%s.%s", lv->type_name, method);
                if (has_user_function(mangled)) {
                    /* Receiver + original args. Push in reverse so first
                       arg (receiver) ends up in %rdi (System V ABI). */
                    ASTNode *args[5];
                    int argc = 0;
                    for (ASTNode *a = n->args; a && argc < 5; a = a->next)
                        args[argc++] = a;
                    /* push original args from last to first */
                    for (int i = argc - 1; i >= 0; i--) {
                        gen_expr(CG, args[i]);
                        fprintf(CG->out, "    pushq %%rax\n");
                    }
                    /* push receiver last so it is popped first into %rdi */
                    if (lv->size == 8) {
                        fprintf(CG->out, "    movq %d(%%rbp), %%rax\n", lv->offset);
                    } else {
                        fprintf(CG->out, "    leaq %d(%%rbp), %%rax\n", lv->offset);
                    }
                    fprintf(CG->out, "    pushq %%rax\n");
                    static const char *regs[] = {"%rdi","%rsi","%rdx","%rcx","%r8","%r9"};
                    int total = argc + 1;
                    for (int i = 0; i < total; i++)
                        fprintf(CG->out, "    popq %s\n", regs[i]);
                    fprintf(CG->out, "    call %s\n", mangled);
                    return;
                }
            }
        }
    }

    // Purplecode 2.1: Blow.*, Net.*, File.* map 1:1 to purple_<name>() runtime symbols (up to 6 args)
    if (!strncmp(name, "Blow.", 5) || !strncmp(name, "Net.", 4) || !strncmp(name, "File.", 5)) {
        char sym[160]; size_t k = (size_t)snprintf(sym, sizeof sym, "purple_");
        for (const char *c = name; *c && k < sizeof sym - 1; ++c) sym[k++] = (*c == '.') ? '_' : (char)tolower((unsigned char)*c);
        sym[k] = 0;
        gen_call_args(CG, n->args, 6);
        fprintf(CG->out, "    pushq %%rbx\n    movq %%rsp, %%rbx\n    andq $-16, %%rsp\n    call %s\n    movq %%rbx, %%rsp\n    popq %%rbx\n", sym);
        return;
    }
    // Map hierarchical names to runtime functions
    if (strcmp(name, "Data.ShellPrint") == 0 || strstr(name, "Data.ShellPrint") == name) {
        // One argument
        if (n->args) {
            gen_expr(CG, n->args);
            // If it was an address, call print_addr, else print string
            if (n->args->type == NODE_HEX_ADDR) {
                fprintf(CG->out, "    movq %%rax, %%rdi\n");
                fprintf(CG->out, "    call purple_shell_print_addr\n");
            } else if (n->args->type == NODE_STRING || (n->args->type == NODE_IDENT && n->args->name && !strcmp(n->args->name, "$LastHexResult"))) {
                fprintf(CG->out, "    movq %%rax, %%rdi\n");
                fprintf(CG->out, "    call purple_shell_print\n");
            } else {
                fprintf(CG->out, "    movq %%rax, %%rdi\n");
                fprintf(CG->out, "    call purple_shell_print_int\n");
            }
        } else {
            fprintf(CG->out, "    movq $0, %%rdi\n");
            fprintf(CG->out, "    call purple_shell_print\n");
        }
        return;
    }

    if (strcmp(name, "FS.Data.Write") == 0) {
        // Special: the expression parser may have made a BINARY > node
        // Or args + later handling. For the common form we expect the
        // statement to be EXPR_STMT whose left is BINARY with left= value, right=path
        // Here if called directly with args we treat first as data, second as path.
        ASTNode *data = n->args;
        ASTNode *path = data ? data->next : NULL;
        if (data) gen_expr(CG, data);
        else fprintf(CG->out, "    movq $0, %%rax\n");
        fprintf(CG->out, "    pushq %%rax\n");
        if (path) gen_expr(CG, path);
        else fprintf(CG->out, "    movq $0, %%rax\n");
        fprintf(CG->out, "    movq %%rax, %%rsi\n"); // path
        fprintf(CG->out, "    popq %%rdi\n");        // data
        fprintf(CG->out, "    call purple_fs_write\n");
        return;
    }

    if (strcmp(name, "LinuxAPI.Call") == 0) {
        /* LinuxAPI.Call(number, a1, a2, a3, ...)
         * First argument is the syscall number. Remaining arguments are
         * passed according to the System V AMD64 ABI (rdi, rsi, rdx, rcx...).
         * The runtime accepts a variable number and falls back to raw syscall
         * for numbers it does not special-case. */
        ASTNode *arg = n->args;
        int argi = 0;
        static const char *argregs[] = {"%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"};
        /* Evaluate arguments right-to-left so we can push them, then pop into regs. */
        ASTNode *args[6] = {0};
        while (arg && argi < 6) {
            args[argi++] = arg;
            arg = arg->next;
        }
        for (int i = argi - 1; i >= 0; i--) {
            gen_expr(CG, args[i]);
            fprintf(CG->out, "    pushq %%rax\n");
        }
        for (int i = 0; i < argi; i++) {
            fprintf(CG->out, "    popq %s\n", argregs[i]);
        }
        /* Zero remaining registers for cleanliness when fewer args are supplied. */
        for (int i = argi; i < 4; i++) {
            fprintf(CG->out, "    xorq %s, %s\n", argregs[i], argregs[i]);
        }
        fprintf(CG->out, "    call purple_linux_api_call\n");
        return;
    }

    if (strcmp(name, "PurpleAPI.Call") == 0) {
        if (n->args) {
            gen_expr(CG, n->args);
            fprintf(CG->out, "    movq %%rax, %%rdi\n");
            // pass remaining as needed
            ASTNode *a2 = n->args->next;
            if (a2) {
                gen_expr(CG, a2);
                fprintf(CG->out, "    movq %%rax, %%rsi\n");
            }
            ASTNode *a3 = a2 ? a2->next : NULL;
            if (a3) {
                gen_expr(CG, a3);
                fprintf(CG->out, "    movq %%rax, %%rdx\n");
            }
            // more can be added
        } else {
            fprintf(CG->out, "    movq $0, %%rdi\n");
        }
        fprintf(CG->out, "    call purple_api_call\n");
        return;
    }

    if (strcmp(name, "Data.Write.Memaddr") == 0 || strstr(name, "Data.Write.Memaddr") != NULL) {
        // Data.Write.Memaddr($addrH, "data or /path")
        ASTNode *addr = n->args;
        ASTNode *data = addr ? addr->next : NULL;
        if (addr) gen_expr(CG, addr);
        else fprintf(CG->out, "    movq $0, %%rax\n");
        fprintf(CG->out, "    pushq %%rax\n");
        if (data) gen_expr(CG, data);
        else fprintf(CG->out, "    movq $0, %%rax\n");
        fprintf(CG->out, "    movq %%rax, %%rsi\n");   // data/path
        fprintf(CG->out, "    popq %%rdi\n");          // addr
        fprintf(CG->out, "    call purple_data_write_memaddr\n");
        return;
    }

    if (strcmp(name, "Program.ELF64.Open") == 0 || strstr(name, "Program.ELF64.Open") != NULL) {
        if (n->args) {
            gen_expr(CG, n->args);
            fprintf(CG->out, "    movq %%rax, %%rdi\n");
        } else {
            fprintf(CG->out, "    movq $0, %%rdi\n");
        }
        fprintf(CG->out, "    call purple_program_elf64_open\n");
        return;
    }
    if (strcmp(name, "Program.ELF64.Run") == 0) {
        if (n->args) { gen_expr(CG, n->args); fprintf(CG->out, "    movq %%rax, %%rdi\n"); }
        else fprintf(CG->out, "    movq $0, %%rdi\n");
        fprintf(CG->out, "    call purple_program_elf64_run\n"); return;
    }

    if (!strcmp(name, "Data.ShellPrint.Int")) {
        gen_call_args(CG, n->args, 1);
        fprintf(CG->out, "    call purple_shell_print_int\n");
        return;
    }
    if (!strcmp(name, "Data.ShellPrint.Hex")) {
        gen_call_args(CG, n->args, 1);
        fprintf(CG->out, "    call purple_shell_print_addr\n");
        return;
    }

    /* General native Controlang plugin ABI: int64_t fn(int64_t x6). */
    if (!strcmp(name, "Controlang.Native.Register")) {
        ASTNode *a[3]; int i=0; for(ASTNode *p=n->args;p && i<3;p=p->next)a[i++]=p;
        if(i!=3) { fprintf(stderr,"Controlang.Native.Register expects name, library path, symbol\n"); CG->had_error=true; return; }
        for(int j=2;j>=0;j--){gen_expr(CG,a[j]);fprintf(CG->out,"    pushq %%rax\n");}
        fprintf(CG->out,"    popq %%rdi\n    popq %%rsi\n    popq %%rdx\n    call purple_controlang_register\n"); return;
    }
    /* Web, media and Controlang runtime APIs. */
    if (!strcmp(name, "Web.JS.Run")) { gen_call_args(CG, n->args, 1); fprintf(CG->out, "    call purple_web_js_run\n"); return; }
    if (!strcmp(name, "Web.HTML.Load")) { gen_call_args(CG, n->args, 1); fprintf(CG->out, "    call purple_web_html_load\n"); return; }
    if (!strcmp(name, "Web.CSS.Load")) { gen_call_args(CG, n->args, 1); fprintf(CG->out, "    call purple_web_css_load\n"); return; }
    if (!strcmp(name, "Image.Load")) { gen_call_args(CG, n->args, 1); fprintf(CG->out, "    call purple_image_load\n"); return; }
    if (!strcmp(name, "Image.Show")) { gen_call_args(CG, n->args, 1); fprintf(CG->out, "    call purple_image_show\n"); return; }
    if (!strcmp(name, "CD.ReadSectorTo") || !strcmp(name, "USB.ReadSectorTo")) {
        int sid=emit_string(CG,!strcmp(name,"CD.ReadSectorTo")?"CD":"USB"); ASTNode *a=n->args; ASTNode *lba=a?a->next:NULL; ASTNode *out=lba?lba->next:NULL;
        if(a)gen_expr(CG,a);else fprintf(CG->out,"    movq $0, %%rax\n");fprintf(CG->out,"    pushq %%rax\n");
        if(lba)gen_expr(CG,lba);else fprintf(CG->out,"    movq $0, %%rax\n");fprintf(CG->out,"    pushq %%rax\n");
        if(out)gen_expr(CG,out);else fprintf(CG->out,"    movq $0, %%rax\n");fprintf(CG->out,"    movq %%rax, %%rdx\n    popq %%rsi\n    popq %%rdi\n    leaq .LC%d(%%rip), %%rcx\n    call purple_media_read_sector_to\n",sid);return;
    }
    if (!strcmp(name, "CD.ReadSector") || !strcmp(name, "USB.ReadSector")) {
        int sid=emit_string(CG,!strcmp(name,"CD.ReadSector")?"CD":"USB"); ASTNode *a=n->args; ASTNode *lba=a?a->next:NULL;
        if(a)gen_expr(CG,a);else fprintf(CG->out,"    movq $0, %%rax\n");fprintf(CG->out,"    pushq %%rax\n");
        if(lba)gen_expr(CG,lba);else fprintf(CG->out,"    movq $0, %%rax\n");fprintf(CG->out,"    movq %%rax, %%rsi\n    popq %%rdi\n    leaq .LC%d(%%rip), %%rdx\n    call purple_media_read_sector\n",sid);return;
    }
    if (!strcmp(name, "CD.Read") || !strcmp(name, "USB.Read")) {
        int sid = emit_string(CG, !strcmp(name, "CD.Read") ? "CD" : "USB");
        ASTNode *a = n->args;
        if (a) gen_expr(CG, a); else fprintf(CG->out, "    movq $0, %%rax\n");
        fprintf(CG->out, "    movq %%rax, %%rdi\n    leaq .LC%d(%%rip), %%rsi\n    call purple_media_read\n", sid);
        return;
    }
    if (!strcmp(name, "Kustom.GUI.MakeWindow")) { gen_call_args(CG, n->args, 1); fprintf(CG->out, "    call purple_kustom_gui_make_window\n"); return; }
    if (!strcmp(name, "GUI.NativeWindow")) { gen_call_args(CG, n->args, 1); fprintf(CG->out, "    call purple_kustom_gui_make_window\n"); return; }

    /* NYMNOG namespace: dotted names are stable API paths, not special syntax. */
    if (!strcmp(name, "Web.JS.Load")) {
        gen_call_args(CG, n->args, 1);
        fprintf(CG->out, "    call purple_web_js_load\n");
        return;
    }
    if (!strcmp(name, "Web.JS.Load.Memaddr")) {
        gen_call_args(CG, n->args, 2);
        fprintf(CG->out, "    call purple_web_js_load_memaddr\n");
        return;
    }
    if (!strcmp(name, "FS.Exists")) {
        gen_call_args(CG, n->args, 1);
        fprintf(CG->out, "    call purple_fs_exists\n");
        return;
    }
    if (!strcmp(name, "FS.Size")) {
        gen_call_args(CG, n->args, 1);
        fprintf(CG->out, "    call purple_fs_size\n");
        return;
    }
    if (!strcmp(name, "System.Process.ID")) {
        fprintf(CG->out, "    call purple_process_id\n");
        return;
    }
    if (!strcmp(name, "System.Time.Unix")) {
        fprintf(CG->out, "    call purple_time_unix\n");
        return;
    }
    if (!strcmp(name, "System.Memory.Alloc")) {
        gen_call_args(CG, n->args, 1);
        fprintf(CG->out, "    call purple_memory_alloc\n");
        return;
    }
    if (!strcmp(name, "System.Memory.Free")) {
        gen_call_args(CG, n->args, 1);
        fprintf(CG->out, "    call purple_memory_free\n");
        return;
    }
    /* Milestone 1.7 – Heap structs */
    if (!strcmp(name, "Struct.New") || !strcmp(name, "Struct.Alloc")) {
        /* Struct.New(TypeName) or Struct.New(size_expr).
           When the argument is an identifier naming a registered struct,
           use that struct's layout size; otherwise evaluate the argument
           as a byte count. Result is a zeroed pointer (int64). */
        int size = 8;
        if (n->args && n->args->type == NODE_IDENT && n->args->name) {
            StructDef *sd = find_struct(CG, n->args->name);
            if (sd) size = sd->size > 0 ? sd->size : 8;
            else {
                /* fall back to evaluating the identifier as an expression */
                gen_expr(CG, n->args);
                fprintf(CG->out, "    movq %%rax, %%rdi\n");
                fprintf(CG->out, "    call purple_memory_alloc\n");
                return;
            }
        } else if (n->args) {
            gen_expr(CG, n->args);
            fprintf(CG->out, "    movq %%rax, %%rdi\n");
            fprintf(CG->out, "    call purple_memory_alloc\n");
            return;
        }
        fprintf(CG->out, "    movq $%d, %%rdi\n", size);
        fprintf(CG->out, "    call purple_memory_alloc\n");
        return;
    }
    if (!strcmp(name, "Struct.Free") || !strcmp(name, "Struct.Destroy")) {
        gen_call_args(CG, n->args, 1);
        fprintf(CG->out, "    call purple_memory_free\n");
        return;
    }
    /* Milestone 1.7 – Arrays of structs */
    if (!strcmp(name, "StructArray.New") || !strcmp(name, "StructArray.Alloc")) {
        /* StructArray.New(TypeName, count) → zeroed array of count elements.
           Returns pointer to the first element. */
        int elem_size = 8;
        ASTNode *type_arg = n->args;
        ASTNode *count_arg = type_arg ? type_arg->next : NULL;
        if (type_arg && type_arg->type == NODE_IDENT && type_arg->name) {
            StructDef *sd = find_struct(CG, type_arg->name);
            if (sd) elem_size = sd->size > 0 ? sd->size : 8;
        }
        if (count_arg) {
            gen_expr(CG, count_arg);
            fprintf(CG->out, "    imulq $%d, %%rax\n", elem_size); /* total bytes */
            fprintf(CG->out, "    movq %%rax, %%rdi\n");
        } else {
            fprintf(CG->out, "    movq $%d, %%rdi\n", elem_size);
        }
        fprintf(CG->out, "    call purple_memory_alloc\n");
        return;
    }
    if (!strcmp(name, "StructArray.At")) {
        /* StructArray.At(base, index, TypeName) → pointer to element
           (base + index * sizeof(Type)). Result can be assigned to a
           pointer-to-struct local for field access. */
        ASTNode *base = n->args;
        ASTNode *idx  = base ? base->next : NULL;
        ASTNode *typ  = idx  ? idx->next  : NULL;
        int elem_size = 8;
        if (typ && typ->type == NODE_IDENT && typ->name) {
            StructDef *sd = find_struct(CG, typ->name);
            if (sd) elem_size = sd->size > 0 ? sd->size : 8;
        }
        if (base) gen_expr(CG, base);
        else fprintf(CG->out, "    movq $0, %%rax\n");
        fprintf(CG->out, "    pushq %%rax\n"); /* base */
        if (idx) gen_expr(CG, idx);
        else fprintf(CG->out, "    movq $0, %%rax\n");
        fprintf(CG->out, "    imulq $%d, %%rax\n", elem_size);
        fprintf(CG->out, "    popq %%rcx\n");
        fprintf(CG->out, "    addq %%rcx, %%rax\n"); /* base + index*size */
        return;
    }
    if (!strcmp(name, "StructArray.Free") || !strcmp(name, "StructArray.Destroy")) {
        gen_call_args(CG, n->args, 1);
        fprintf(CG->out, "    call purple_memory_free\n");
        return;
    }
    /* Milestone 1 1.6 Arrays */
    if (!strcmp(name, "Array.New") || !strcmp(name, "Array.Alloc")) {
        gen_call_args(CG, n->args, 1);
        fprintf(CG->out, "    call purple_array_new\n");
        return;
    }
    if (!strcmp(name, "Array.Len") || !strcmp(name, "Array.Length")) {
        gen_call_args(CG, n->args, 1);
        fprintf(CG->out, "    call purple_array_len\n");
        return;
    }
    if (!strcmp(name, "Array.Get")) {
        gen_call_args(CG, n->args, 2);
        fprintf(CG->out, "    call purple_array_get\n");
        return;
    }
    if (!strcmp(name, "Array.Set")) {
        gen_call_args(CG, n->args, 3);
        fprintf(CG->out, "    call purple_array_set\n");
        return;
    }
    if (!strcmp(name, "Array.Free") || !strcmp(name, "Array.Destroy")) {
        gen_call_args(CG, n->args, 1);
        fprintf(CG->out, "    call purple_array_free\n");
        return;
    }
    if (!strcmp(name, "Array.Data")) {
        gen_call_args(CG, n->args, 1);
        fprintf(CG->out, "    call purple_array_data\n");
        return;
    }
    if (!strcmp(name, "Kernel.Call") || !strcmp(name, "System.Kernel.Call")) {
        /* Thin 4-argument syscall bridge: number, a1, a2, a3 */
        gen_call_args(CG, n->args, 4);
        fprintf(CG->out, "    call purple_kernel_call\n");
        return;
    }

    if (strstr(name, "GUI.") == name || strstr(name, "Blit.") == name) {
        /* Hierarchical GUI / Blitter calls. Pass up to 5 user arguments so
         * DrawLine(x1,y1,x2,y2,color), DrawText, Rect, etc. all work cleanly. */
        int sid = emit_string(CG, name);
        fprintf(CG->out, "    leaq .LC%d(%%rip), %%rdi\n", sid);
        ASTNode *a = n->args;
        if (a) { gen_expr(CG, a); fprintf(CG->out, "    movq %%rax, %%rsi\n"); a = a->next; }
        else fprintf(CG->out, "    movq $0, %%rsi\n");
        if (a) { gen_expr(CG, a); fprintf(CG->out, "    movq %%rax, %%rdx\n"); a = a->next; }
        else fprintf(CG->out, "    movq $0, %%rdx\n");
        if (a) { gen_expr(CG, a); fprintf(CG->out, "    movq %%rax, %%rcx\n"); a = a->next; }
        else fprintf(CG->out, "    movq $0, %%rcx\n");
        if (a) { gen_expr(CG, a); fprintf(CG->out, "    movq %%rax, %%r8\n"); a = a->next; }
        else fprintf(CG->out, "    movq $0, %%r8\n");
        if (a) { gen_expr(CG, a); fprintf(CG->out, "    movq %%rax, %%r9\n"); }
        else fprintf(CG->out, "    movq $0, %%r9\n");
        fprintf(CG->out, "    call purple_gui_dispatch\n");
        return;
    }

    /* Native runtime extension points (including the encoding built-ins). */
    if (strchr(name,'.') || !strcmp(name,"Binarify") || !strcmp(name,"Hexify") || !strcmp(name,"Base64ify") || !strcmp(name,"IFont")) {
        int sid=emit_string(CG,name); ASTNode *args[6]; int argc=0;
        for(ASTNode *a=n->args;a;a=a->next){if(argc==6){fprintf(stderr,"Purplecode: Controlang calls support at most six arguments\n");CG->had_error=true;return;}args[argc++]=a;}
        for(int i=argc-1;i>=0;i--){gen_expr(CG,args[i]);fprintf(CG->out,"    pushq %%rax\n");}
        for(int i=0;i<argc;i++) fprintf(CG->out,"    popq %s\n",(const char*[]) {"%rsi","%rdx","%rcx","%r8","%r9","%r10"}[i]);
        fprintf(CG->out,"    leaq .LC%d(%%rip), %%rdi\n",sid);
        /* ABI arguments are passed as an array to avoid register-shuffle hazards. */
        fprintf(CG->out,"    subq $48, %%rsp\n    movq $0, 0(%%rsp)\n    movq $0, 8(%%rsp)\n    movq $0, 16(%%rsp)\n    movq $0, 24(%%rsp)\n    movq $0, 32(%%rsp)\n    movq $0, 40(%%rsp)\n");
        /* The values above are copied from registers in reverse-independent order below. */
        static const char *rgs[]={"%rsi","%rdx","%rcx","%r8","%r9","%r10"};
        for(int i=0;i<argc;i++) fprintf(CG->out,"    movq %s, %d(%%rsp)\n",rgs[i],i*8);
        fprintf(CG->out,"    movq %%rsp, %%rsi\n    call purple_controlang_call\n    addq $48, %%rsp\n");
        return;
    }
    /* Ordinary user-defined function call. */
    gen_user_call(CG,n);
}

static void gen_expr(CodeGen *CG, ASTNode *n) {
    if (!n) {
        fprintf(CG->out, "    movq $0, %%rax\n");
        return;
    }

    switch (n->type) {
        case NODE_INT:
            fprintf(CG->out, "    movq $%lld, %%rax\n", (long long)n->int_val);
            break;
        case NODE_BOOL:
            fprintf(CG->out, "    movq $%lld, %%rax\n", (long long)n->int_val);
            break;
        case NODE_HEX_ADDR:
            fprintf(CG->out, "    movq $0x%lx, %%rax\n", (unsigned long)n->addr_val);
            break;
        case NODE_STRING: {
            int id = emit_string(CG, n->str_val ? n->str_val : "");
            fprintf(CG->out, "    leaq .LC%d(%%rip), %%rax\n", id);
            break;
        }
        case NODE_IDENT: {
            if (n->name && !strcmp(n->name, "$LastHexResult")) {
                fprintf(CG->out, "    movq purple_LastHexResult(%%rip), %%rax\n");
                break;
            }
            /* Field access: name contains '.' */
            if (n->name && strchr(n->name, '.')) {
                char base[128];
                int foff = 0;
                resolve_field_path(CG, n->name, base, sizeof base, &foff);
                load_field(CG, base, foff);
                break;
            }
            LocalVar *v = n->name ? find_local(CG, n->name) : NULL;
            if (v) {
                fprintf(CG->out, "    movq %d(%%rbp), %%rax\n", v->offset);
                break;
            }
            int idx = param_index(CG, n->name);
            if (idx >= 0 && idx < 6)
                fprintf(CG->out, "    movq -%d(%%rbp), %%rax\n", 8 * (idx + 1));
            else {
                type_error(CG, n, "undefined variable '%s'", n->name);
                fprintf(CG->out, "    movq $0, %%rax\n");
            }
            break;
        }
        case NODE_UNARY:
            gen_expr(CG,n->left);
            if(n->name && strcmp(n->name,"-")==0) fprintf(CG->out,"    negq %%rax\n");
            break;
        case NODE_HIER_CALL:
            gen_hier_call(CG, n);
            break;
        case NODE_BINARY:
            if (n->flags == 1 && n->name && strcmp(n->name, ">") == 0) {
                // This is the Write form: left = data, right = path
                // We generate the call here
                gen_expr(CG, n->left);
                fprintf(CG->out, "    pushq %%rax\n");
                gen_expr(CG, n->right);
                fprintf(CG->out, "    movq %%rax, %%rsi\n");
                fprintf(CG->out, "    popq %%rdi\n");
                fprintf(CG->out, "    call purple_fs_write\n");
            } else {
                gen_expr(CG,n->left); fprintf(CG->out,"    pushq %%rax\n");
                gen_expr(CG,n->right); fprintf(CG->out,"    movq %%rax, %%rcx\n    popq %%rax\n");
                const char *op=n->name?n->name:"+";
                if(!strcmp(op,"+"))fprintf(CG->out,"    addq %%rcx, %%rax\n");
                else if(!strcmp(op,"-"))fprintf(CG->out,"    subq %%rcx, %%rax\n");
                else if(!strcmp(op,"*"))fprintf(CG->out,"    imulq %%rcx, %%rax\n");
                else if(!strcmp(op,"/"))fprintf(CG->out,"    cqto\n    idivq %%rcx\n");
                else if(!strcmp(op,"%"))fprintf(CG->out,"    cqto\n    idivq %%rcx\n    movq %%rdx, %%rax\n");
                else { int id=CG->label_counter++; const char *j=!strcmp(op,"==")?"je":!strcmp(op,"!=")?"jne":!strcmp(op,"<")?"jl":!strcmp(op,"<=")?"jle":!strcmp(op,">")?"jg":"jge"; fprintf(CG->out,"    cmpq %%rcx, %%rax\n    %s .Lcmp_true_%d\n    xorq %%rax, %%rax\n    jmp .Lcmp_end_%d\n.Lcmp_true_%d:\n    movq $1, %%rax\n.Lcmp_end_%d:\n",j,id,id,id,id); }
            }
            break;
        default:
            fprintf(CG->out, "    movq $0, %%rax\n");
            break;
    }
}

static void gen_stmt(CodeGen *CG, ASTNode *n) {
    if (!n) return;

    switch (n->type) {
        case NODE_EXPR_STMT:
            gen_expr(CG, n->left);
            break;
        case NODE_RETURN:
            if (n->left) gen_expr(CG, n->left);
            else fprintf(CG->out, "    movq $0, %%rax\n");
            /* Return immediately. The function-end epilogue remains as the
               implicit return path for control flow that reaches the end. */
            fprintf(CG->out, "    leave\n    ret\n");
            break;
        case NODE_VAR_DECL: {
            PurpleType vt = TY_NONE;
            const char *struct_type_name = NULL;
            int is_struct_ptr = 0;
            if (n->right) {
                /* Detect Struct.New(TypeName) / StructArray.New(TypeName, ...) /
                   StructArray.At(..., TypeName) so the local becomes a
                   pointer-to-struct (size 8, type TY_STRUCT). */
                if (n->right->type == NODE_HIER_CALL && n->right->name) {
                    const char *hn = n->right->name;
                    if ((!strcmp(hn, "Struct.New") || !strcmp(hn, "Struct.Alloc")) &&
                        n->right->args && n->right->args->type == NODE_IDENT && n->right->args->name) {
                        StructDef *sd = find_struct(CG, n->right->args->name);
                        if (sd) {
                            vt = TY_STRUCT;
                            struct_type_name = n->right->args->name;
                            is_struct_ptr = 1;
                        }
                    } else if ((!strcmp(hn, "StructArray.New") || !strcmp(hn, "StructArray.Alloc")) &&
                               n->right->args && n->right->args->type == NODE_IDENT && n->right->args->name) {
                        StructDef *sd = find_struct(CG, n->right->args->name);
                        if (sd) {
                            vt = TY_STRUCT;
                            struct_type_name = n->right->args->name;
                            is_struct_ptr = 1;
                        }
                    } else if (!strcmp(hn, "StructArray.At")) {
                        /* third arg is the element type name */
                        ASTNode *a = n->right->args;
                        ASTNode *t = a && a->next ? a->next->next : NULL;
                        if (t && t->type == NODE_IDENT && t->name) {
                            StructDef *sd = find_struct(CG, t->name);
                            if (sd) {
                                vt = TY_STRUCT;
                                struct_type_name = t->name;
                                is_struct_ptr = 1;
                            }
                        }
                    }
                }
                if (vt == TY_NONE)
                    vt = infer_expr(CG, n->right);
                if (n->var_type != TY_NONE && vt != TY_NONE && vt != n->var_type && !is_struct_ptr)
                    type_error(CG, n, "cannot initialize %s variable '%s' with %s",
                               type_name(n->var_type), n->name ? n->name : "", type_name(vt));
            } else {
                /* no initializer – treat as zeroed; respect declared type */
                vt = n->var_type != TY_NONE ? n->var_type : TY_INT;
            }
            LocalVar *local = n->name ? find_local(CG, n->name) : NULL;
            if (local) {
                if (local->type == TY_NONE && vt != TY_NONE)
                    local->type = vt;
                if (is_struct_ptr) {
                    local->type = TY_STRUCT;
                    if (local->type_name) free(local->type_name);
                    local->type_name = strdup(struct_type_name);
                    local->size = 8; /* pointer */
                }
            }
            if (n->right) {
                gen_expr(CG, n->right);
                store_local(CG, n->name, vt);
            } else {
                /* zero the entire local (important for structs) */
                LocalVar *lv = n->name ? find_local(CG, n->name) : NULL;
                if (lv) {
                    int bytes = lv->size > 0 ? lv->size : 8;
                    for (int off = 0; off < bytes; off += 8) {
                        fprintf(CG->out, "    movq $0, %d(%%rbp)\n", lv->offset + off);
                    }
                } else {
                    fprintf(CG->out, "    movq $0, %%rax\n");
                    store_local(CG, n->name, vt);
                }
            }
            break;
        }
        case NODE_MEMBER_ASSIGN: {
            /* Full field-offset lowering */
            if (n->right && n->left && n->left->name) {
                char base[128];
                int foff = 0;
                resolve_field_path(CG, n->left->name, base, sizeof base, &foff);
                gen_expr(CG, n->right);
                store_field(CG, base, foff);
            }
            break;
        }
        case NODE_STRUCT_DEF:
            /* no code emitted for the definition itself */
            break;
        case NODE_ASSIGN: {
            PurpleType vt = infer_expr(CG, n->right);
            gen_expr(CG, n->right);
            store_local(CG, n->left ? n->left->name : NULL, vt);
            break;
        }
        case NODE_IF: {
            int id = CG->label_counter++;
            PurpleType ct = infer_expr(CG, n->left);
            if (ct != TY_BOOL)
                type_error(CG, n->left, "if condition must be bool, got %s", type_name(ct));
            gen_expr(CG, n->left);
            fprintf(CG->out, "    testq %%rax, %%rax\n    je .Lelse_%d\n", id);
            if (n->body) gen_stmt(CG, n->body);
            fprintf(CG->out, "    jmp .Lend_%d\n.Lelse_%d:\n", id, id);
            if (n->right) gen_stmt(CG, n->right);
            fprintf(CG->out, ".Lend_%d:\n", id);
            break;
        }
        case NODE_WHILE: {
            int id = CG->label_counter++;
            fprintf(CG->out, ".Lwhile_%d:\n", id);
            PurpleType ct = infer_expr(CG, n->left);
            if (ct != TY_BOOL)
                type_error(CG, n->left, "while condition must be bool, got %s", type_name(ct));
            gen_expr(CG, n->left);
            fprintf(CG->out, "    testq %%rax, %%rax\n    je .Lwend_%d\n", id);
            if (n->body) gen_stmt(CG, n->body);
            fprintf(CG->out, "    jmp .Lwhile_%d\n.Lwend_%d:\n", id, id);
            break;
        }
        case NODE_RAW_ASSIGN: {
            // raw $addr = value  →  mov value, (addr)
            gen_expr(CG, n->right);
            fprintf(CG->out, "    movq %%rax, %%rcx\n"); // value
            gen_expr(CG, n->left); // address into rax
            fprintf(CG->out, "    movq %%rcx, (%%rax)\n");
            break;
        }
        case NODE_BLOCK:
            for (ASTNode *s = n->body; s; s = s->next)
                gen_stmt(CG, s);
            break;
        case NODE_FN:
            fprintf(CG->out, "\n    .globl %s\n", n->name ? n->name : "fn");
            fprintf(CG->out, "%s:\n", n->name ? n->name : "fn");
            CG->current_function=n;
            CG->local_count=0;
            CG->frame_size = 48; /* reset before collecting; add_local grows it */
            collect_declared_locals(CG, n->body);
            /* Keep the size computed by add_local (respects struct sizes).
               Only ensure 16-byte alignment for the ABI. */
            if (CG->frame_size % 16) CG->frame_size += 16 - (CG->frame_size % 16);
            if (CG->frame_size < 48) CG->frame_size = 48;
            fprintf(CG->out, "    pushq %%rbp\n");
            fprintf(CG->out, "    movq %%rsp, %%rbp\n    subq $%d, %%rsp\n", CG->frame_size);
            { static const char *regs[]={"%rdi","%rsi","%rdx","%rcx","%r8","%r9"}; int i=0; for(ASTNode *a=n->args;a;a=a->next,i++) if(i<6) fprintf(CG->out,"    movq %s, -%d(%%rbp)\n",regs[i],8*(i+1)); }
            if (n->body) gen_stmt(CG, n->body);
            CG->current_function=NULL;
            // implicit return 0 if none
            fprintf(CG->out, "    movq $0, %%rax\n");
            fprintf(CG->out, "    popq %%rbp\n");
            fprintf(CG->out, "    ret\n");
            break;
        default:
            break;
    }
}

void codegen_program(CodeGen *CG, ASTNode *prog) {
    g_program_root = prog;
    CG->program_root = prog;
    register_structs(CG, prog);
    fprintf(CG->out, "    .text\n");
    fprintf(CG->out, "    .globl main\n");

    // If there is an explicit main, use it; otherwise wrap top-level statements
    bool has_main = false;
    for (ASTNode *n = prog->body; n; n = n->next) {
        if (n->type == NODE_FN && n->name && strcmp(n->name, "main") == 0) {
            has_main = true;
            break;
        }
    }

    if (!has_main) {
        fprintf(CG->out, "main:\n");
        fprintf(CG->out, "    pushq %%rbp\n");
        fprintf(CG->out, "    movq %%rsp, %%rbp\n");
    }

    for (ASTNode *n = prog->body; n; n = n->next) {
        if (n->type == NODE_FN) {
            gen_stmt(CG, n);
        } else if (n->type == NODE_STRUCT_DEF) {
            /* Struct definitions are registered for type/layout purposes.
               No code is emitted for the definition itself. */
            (void)n;
        } else {
            if (!has_main) gen_stmt(CG, n);
        }
    }

    if (!has_main) {
        fprintf(CG->out, "    movq $0, %%rax\n");
        fprintf(CG->out, "    popq %%rbp\n");
        fprintf(CG->out, "    ret\n");
    }

    // Some runtime helpers that may be needed
    fprintf(CG->out, "\n# End of Purplecode generated assembly\n");
}
