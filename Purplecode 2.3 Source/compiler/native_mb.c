/*
 * Native Multiboot i386 machine-code emitter (pure C).
 * Supports a practical freestanding subset used by Purplebootcode samples:
 *   - fn main() / basic functions
 *   - Screen.Clear / Screen.Write("...")
 *   - CPU.Halt / while true { CPU.Halt() }
 *   - let name: type = expr  (int locals on stack)
 *   - if / while with simple conditions
 *   - Memory.* / Port.* / System.Panic  (safe freestanding implementations)
 * No external assembler or linker.
 */

#include "native_mb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdbool.h>

#define LOAD_ADDR 0x00100000u

/* ---- tiny binary buffer ---- */
typedef struct {
    uint8_t *data;
    size_t len, cap;
} Buf;

static void buf_init(Buf *b) { b->data = NULL; b->len = b->cap = 0; }
static void buf_free(Buf *b) { free(b->data); b->data = NULL; b->len = b->cap = 0; }
static int buf_reserve(Buf *b, size_t need) {
    if (need <= b->cap) return 0;
    size_t ncap = b->cap ? b->cap * 2 : 256;
    while (ncap < need) ncap *= 2;
    uint8_t *p = (uint8_t *)realloc(b->data, ncap);
    if (!p) return -1;
    b->data = p; b->cap = ncap;
    return 0;
}
static int buf_u8(Buf *b, uint8_t v) {
    if (buf_reserve(b, b->len + 1)) return -1;
    b->data[b->len++] = v; return 0;
}
static int buf_u16(Buf *b, uint16_t v) {
    return buf_u8(b, (uint8_t)v) || buf_u8(b, (uint8_t)(v >> 8));
}
static int buf_u32(Buf *b, uint32_t v) {
    return buf_u8(b, (uint8_t)v) || buf_u8(b, (uint8_t)(v >> 8))
        || buf_u8(b, (uint8_t)(v >> 16)) || buf_u8(b, (uint8_t)(v >> 24));
}
static int buf_bytes(Buf *b, const void *p, size_t n) {
    if (buf_reserve(b, b->len + n)) return -1;
    memcpy(b->data + b->len, p, n); b->len += n; return 0;
}

/* ---- i386 emitters (32-bit) ---- */
static int emit_push_imm32(Buf *b, uint32_t v) {
    return buf_u8(b, 0x68) || buf_u32(b, v);           /* push imm32 */
}
static int emit_push_eax(Buf *b) { return buf_u8(b, 0x50); }
static int emit_pop_eax(Buf *b)  { return buf_u8(b, 0x58); }
static int emit_pop_ebx(Buf *b)  { return buf_u8(b, 0x5B); }
static int emit_ret(Buf *b)      { return buf_u8(b, 0xC3); }
static int emit_mov_eax_imm(Buf *b, uint32_t v) {
    return buf_u8(b, 0xB8) || buf_u32(b, v);            /* mov eax, imm32 */
}
static int emit_mov_ebx_imm(Buf *b, uint32_t v) {
    return buf_u8(b, 0xBB) || buf_u32(b, v);
}
static int emit_mov_ecx_imm(Buf *b, uint32_t v) {
    return buf_u8(b, 0xB9) || buf_u32(b, v);
}
static int emit_mov_edx_imm(Buf *b, uint32_t v) {
    return buf_u8(b, 0xBA) || buf_u32(b, v);
}
static int emit_mov_esp_imm(Buf *b, uint32_t v) {
    return buf_u8(b, 0xBC) || buf_u32(b, v);            /* mov esp, imm32 */
}
static int emit_call_rel32(Buf *b, int32_t rel) {
    return buf_u8(b, 0xE8) || buf_u32(b, (uint32_t)rel);
}
static int emit_jmp_rel32(Buf *b, int32_t rel) {
    return buf_u8(b, 0xE9) || buf_u32(b, (uint32_t)rel);
}
static int emit_jmp_rel8(Buf *b, int8_t rel) {
    return buf_u8(b, 0xEB) || buf_u8(b, (uint8_t)rel);
}
static int emit_cli(Buf *b) { return buf_u8(b, 0xFA); }
static int emit_hlt(Buf *b) { return buf_u8(b, 0xF4); }

/* ---- string pool in .rodata (appended after code) ---- */
#define MAX_STRINGS  256
#define MAX_FIXUPS   256

typedef struct {
    char *s;
    uint32_t off;   /* offset from start of rodata */
} StrEnt;

typedef struct {
    Buf code;
    Buf rodata;
    StrEnt strs[MAX_STRINGS];
    int nstr;
    /* simple fixups: patch absolute address of string into code */
    struct { size_t at; int str_idx; } fixups[MAX_FIXUPS];
    int nfix;
    int had_error;
    char errbuf[512];
} MBEmit;

static void mb_error(MBEmit *E, const char *msg) {
    E->had_error = 1;
    snprintf(E->errbuf, sizeof E->errbuf, "%s", msg);
}

static int mb_add_string(MBEmit *E, const char *s) {
    for (int i = 0; i < E->nstr; i++)
        if (strcmp(E->strs[i].s, s) == 0) return i;
    if (E->nstr >= MAX_STRINGS) {
        mb_error(E, "too many string literals (limit raised to 256; still exceeded)");
        return -1;
    }
    int idx = E->nstr++;
    E->strs[idx].s = strdup(s);
    E->strs[idx].off = (uint32_t)E->rodata.len;
    buf_bytes(&E->rodata, s, strlen(s) + 1);
    return idx;
}

/* Emit: push absolute address of string (patched later) */
static int emit_push_string(MBEmit *E, const char *s) {
    int idx = mb_add_string(E, s);
    if (idx < 0) return -1;
    if (E->nfix >= MAX_FIXUPS) {
        mb_error(E, "too many string fixups (limit raised to 256; still exceeded)");
        return -1;
    }
    size_t at = E->code.len + 1; /* after 0x68 opcode */
    if (emit_push_imm32(&E->code, 0xDEADBEEF)) return -1;
    E->fixups[E->nfix].at = at;
    E->fixups[E->nfix].str_idx = idx;
    E->nfix++;
    return 0;
}

/* ---- freestanding runtime helpers (emitted once) ---- */
/* Screen.Clear: fill 80x25 VGA text with spaces, attr 0x0F */
/* Runtime data locations (in the low 1 MiB, after the kernel image) */
#define CURSOR_POS   0x00090000u   /* dword: current VGA offset */
#define HEAP_PTR     0x00090004u   /* dword: current bump pointer */
#define LOCAL_BASE   0x00090008u   /* dword locals: LOCAL_BASE + index*4 */
#define MAX_LOCALS   64            /* was 8 — raised; still fixed physical window */
#define HEAP_START   0x00200000u   /* first usable heap byte */
#define HEAP_END     0x00400000u   /* 2 MiB of heap */

static size_t helper_screen_clear;
static size_t helper_screen_write;
static size_t helper_halt;
static size_t helper_alloc;
static size_t helper_free;
static size_t helper_heapfree;
static size_t helper_write32;
static size_t helper_read32;
static size_t helper_write8;
static size_t helper_read8;

static int emit_helpers(MBEmit *E) {
    /* ---------- Screen.Clear ---------- */
    helper_screen_clear = E->code.len;
    /* mov edi, 0xB8000 */
    if (buf_u8(&E->code, 0xBF) || buf_u32(&E->code, 0xB8000)) return -1;
    /* mov ecx, 80*25 */
    if (emit_mov_ecx_imm(&E->code, 80 * 25)) return -1;
    /* mov ax, 0x0F20 */
    if (buf_u8(&E->code, 0x66) || buf_u8(&E->code, 0xB8) || buf_u16(&E->code, 0x0F20)) return -1;
    /* rep stosw */
    if (buf_u8(&E->code, 0xF3) || buf_u8(&E->code, 0x66) || buf_u8(&E->code, 0xAB)) return -1;
    /* mov dword [CURSOR_POS], 0 */
    if (buf_u8(&E->code, 0xC7) || buf_u8(&E->code, 0x05) || buf_u32(&E->code, CURSOR_POS) || buf_u32(&E->code, 0)) return -1;
    if (emit_ret(&E->code)) return -1;

    /* ---------- Screen.Write(const char *s)  [esp+4] = s ---------- */
    helper_screen_write = E->code.len;
    /* mov esi, [esp+4] */
    if (buf_u8(&E->code, 0x8B) || buf_u8(&E->code, 0x74) || buf_u8(&E->code, 0x24) || buf_u8(&E->code, 0x04)) return -1;
    /* mov edi, 0xB8000 */
    if (buf_u8(&E->code, 0xBF) || buf_u32(&E->code, 0xB8000)) return -1;
    /* add edi, [CURSOR_POS] */
    if (buf_u8(&E->code, 0x03) || buf_u8(&E->code, 0x3D) || buf_u32(&E->code, CURSOR_POS)) return -1;
    /* mov ah, 0x0F */
    if (buf_u8(&E->code, 0xB4) || buf_u8(&E->code, 0x0F)) return -1;

    size_t loop = E->code.len;
    /* lodsb */
    if (buf_u8(&E->code, 0xAC)) return -1;
    /* test al, al */
    if (buf_u8(&E->code, 0x84) || buf_u8(&E->code, 0xC0)) return -1;
    size_t jz_at = E->code.len;
    if (buf_u8(&E->code, 0x74) || buf_u8(&E->code, 0x00)) return -1; /* jz done */

    /* cmp al, '\n' */
    if (buf_u8(&E->code, 0x3C) || buf_u8(&E->code, '\n')) return -1;
    size_t jne_at = E->code.len;
    if (buf_u8(&E->code, 0x75) || buf_u8(&E->code, 0x00)) return -1; /* jne putchar */

    /* ---- newline: advance to next line ---- */
    /* mov eax, edi */
    if (buf_u8(&E->code, 0x89) || buf_u8(&E->code, 0xF8)) return -1;
    /* sub eax, 0xB8000 */
    if (buf_u8(&E->code, 0x2D) || buf_u32(&E->code, 0xB8000)) return -1;
    /* xor edx, edx */
    if (buf_u8(&E->code, 0x31) || buf_u8(&E->code, 0xD2)) return -1;
    /* mov ecx, 160 */
    if (emit_mov_ecx_imm(&E->code, 160)) return -1;
    /* div ecx   → eax = row, edx = col */
    if (buf_u8(&E->code, 0xF7) || buf_u8(&E->code, 0xF1)) return -1;
    /* inc eax */
    if (buf_u8(&E->code, 0x40)) return -1;
    /* mov ecx, 160 */
    if (emit_mov_ecx_imm(&E->code, 160)) return -1;
    /* mul ecx */
    if (buf_u8(&E->code, 0xF7) || buf_u8(&E->code, 0xE1)) return -1;
    /* lea edi, [0xB8000 + eax] */
    if (buf_u8(&E->code, 0x8D) || buf_u8(&E->code, 0xB8) || buf_u32(&E->code, 0xB8000)) return -1;
    /* jmp loop */
    {
        int8_t back = (int8_t)((int)loop - (int)(E->code.len + 2));
        if (emit_jmp_rel8(&E->code, back)) return -1;
    }

    /* ---- putchar ---- */
    size_t put = E->code.len;
    E->code.data[jne_at + 1] = (uint8_t)(put - (jne_at + 2));
    /* stosw */
    if (buf_u8(&E->code, 0x66) || buf_u8(&E->code, 0xAB)) return -1;
    /* jmp loop */
    {
        int8_t back = (int8_t)((int)loop - (int)(E->code.len + 2));
        if (emit_jmp_rel8(&E->code, back)) return -1;
    }

    /* ---- done: save cursor ---- */
    size_t done = E->code.len;
    E->code.data[jz_at + 1] = (uint8_t)(done - (jz_at + 2));
    /* sub edi, 0xB8000 */
    if (buf_u8(&E->code, 0x81) || buf_u8(&E->code, 0xEF) || buf_u32(&E->code, 0xB8000)) return -1;
    /* mov [CURSOR_POS], edi */
    if (buf_u8(&E->code, 0x89) || buf_u8(&E->code, 0x3D) || buf_u32(&E->code, CURSOR_POS)) return -1;
    if (emit_ret(&E->code)) return -1;

    /* ---------- halt forever ---------- */
    helper_halt = E->code.len;
    if (emit_cli(&E->code) || emit_hlt(&E->code)) return -1;
    if (emit_jmp_rel8(&E->code, -4)) return -1;

    /* ---------- Memory.Alloc(size) → eax  [esp+4] = size ---------- */
    helper_alloc = E->code.len;
    /* mov eax, [HEAP_PTR] */
    if (buf_u8(&E->code, 0xA1) || buf_u32(&E->code, HEAP_PTR)) return -1;
    /* test eax, eax  ; first call? */
    if (buf_u8(&E->code, 0x85) || buf_u8(&E->code, 0xC0)) return -1;
    size_t jnz_inited = E->code.len;
    if (buf_u8(&E->code, 0x75) || buf_u8(&E->code, 0x00)) return -1; /* jnz already_inited */
    /* mov eax, HEAP_START */
    if (buf_u8(&E->code, 0xB8) || buf_u32(&E->code, HEAP_START)) return -1;
    /* mov [HEAP_PTR], eax */
    if (buf_u8(&E->code, 0xA3) || buf_u32(&E->code, HEAP_PTR)) return -1;
    size_t already = E->code.len;
    E->code.data[jnz_inited + 1] = (uint8_t)(already - (jnz_inited + 2));
    /* mov ecx, [esp+4]   ; size */
    if (buf_u8(&E->code, 0x8B) || buf_u8(&E->code, 0x4C) || buf_u8(&E->code, 0x24) || buf_u8(&E->code, 0x04)) return -1;
    /* add ecx, 7 ; align up to 8 */
    if (buf_u8(&E->code, 0x83) || buf_u8(&E->code, 0xC1) || buf_u8(&E->code, 0x07)) return -1;
    /* and ecx, ~7 */
    if (buf_u8(&E->code, 0x83) || buf_u8(&E->code, 0xE1) || buf_u8(&E->code, 0xF8)) return -1;
    /* mov edx, eax */
    if (buf_u8(&E->code, 0x89) || buf_u8(&E->code, 0xC2)) return -1;
    /* add edx, ecx */
    if (buf_u8(&E->code, 0x01) || buf_u8(&E->code, 0xCA)) return -1;
    /* cmp edx, HEAP_END */
    if (buf_u8(&E->code, 0x81) || buf_u8(&E->code, 0xFA) || buf_u32(&E->code, HEAP_END)) return -1;
    size_t ja_oom = E->code.len;
    if (buf_u8(&E->code, 0x77) || buf_u8(&E->code, 0x00)) return -1; /* ja out_of_memory */
    /* mov [HEAP_PTR], edx */
    if (buf_u8(&E->code, 0x89) || buf_u8(&E->code, 0x15) || buf_u32(&E->code, HEAP_PTR)) return -1;
    /* eax already holds the old pointer (return value) */
    if (emit_ret(&E->code)) return -1;
    /* out_of_memory: */
    size_t oom = E->code.len;
    E->code.data[ja_oom + 1] = (uint8_t)(oom - (ja_oom + 2));
    /* xor eax, eax */
    if (buf_u8(&E->code, 0x31) || buf_u8(&E->code, 0xC0)) return -1;
    if (emit_ret(&E->code)) return -1;

    /* ---------- Memory.Free(ptr) – bump allocator, no-op ---------- */
    helper_free = E->code.len;
    if (emit_ret(&E->code)) return -1;

    /* ---------- Memory.HeapFree() → eax = remaining bytes ---------- */
    helper_heapfree = E->code.len;
    /* mov eax, [HEAP_PTR] */
    if (buf_u8(&E->code, 0xA1) || buf_u32(&E->code, HEAP_PTR)) return -1;
    /* test eax, eax */
    if (buf_u8(&E->code, 0x85) || buf_u8(&E->code, 0xC0)) return -1;
    size_t jnz_hf = E->code.len;
    if (buf_u8(&E->code, 0x75) || buf_u8(&E->code, 0x00)) return -1;
    /* mov eax, HEAP_START */
    if (buf_u8(&E->code, 0xB8) || buf_u32(&E->code, HEAP_START)) return -1;
    size_t hf_ok = E->code.len;
    E->code.data[jnz_hf + 1] = (uint8_t)(hf_ok - (jnz_hf + 2));
    /* mov ecx, HEAP_END */
    if (emit_mov_ecx_imm(&E->code, HEAP_END)) return -1;
    /* sub ecx, eax */
    if (buf_u8(&E->code, 0x29) || buf_u8(&E->code, 0xC1)) return -1;
    /* mov eax, ecx */
    if (buf_u8(&E->code, 0x89) || buf_u8(&E->code, 0xC8)) return -1;
    if (emit_ret(&E->code)) return -1;

    /* ---------- Memory.Write32(ptr, val)  [esp+4]=ptr [esp+8]=val ---------- */
    helper_write32 = E->code.len;
    /* mov eax, [esp+4] */
    if (buf_u8(&E->code, 0x8B) || buf_u8(&E->code, 0x44) || buf_u8(&E->code, 0x24) || buf_u8(&E->code, 0x04)) return -1;
    /* mov ecx, [esp+8] */
    if (buf_u8(&E->code, 0x8B) || buf_u8(&E->code, 0x4C) || buf_u8(&E->code, 0x24) || buf_u8(&E->code, 0x08)) return -1;
    /* mov [eax], ecx */
    if (buf_u8(&E->code, 0x89) || buf_u8(&E->code, 0x08)) return -1;
    if (emit_ret(&E->code)) return -1;

    /* ---------- Memory.Read32(ptr) → eax  [esp+4]=ptr ---------- */
    helper_read32 = E->code.len;
    /* mov eax, [esp+4] */
    if (buf_u8(&E->code, 0x8B) || buf_u8(&E->code, 0x44) || buf_u8(&E->code, 0x24) || buf_u8(&E->code, 0x04)) return -1;
    /* mov eax, [eax] */
    if (buf_u8(&E->code, 0x8B) || buf_u8(&E->code, 0x00)) return -1;
    if (emit_ret(&E->code)) return -1;

    /* ---------- Memory.Write8(ptr, val) ---------- */
    helper_write8 = E->code.len;
    if (buf_u8(&E->code, 0x8B) || buf_u8(&E->code, 0x44) || buf_u8(&E->code, 0x24) || buf_u8(&E->code, 0x04)) return -1;
    if (buf_u8(&E->code, 0x8B) || buf_u8(&E->code, 0x4C) || buf_u8(&E->code, 0x24) || buf_u8(&E->code, 0x08)) return -1;
    if (buf_u8(&E->code, 0x88) || buf_u8(&E->code, 0x08)) return -1; /* mov [eax], cl */
    if (emit_ret(&E->code)) return -1;

    /* ---------- Memory.Read8(ptr) → eax ---------- */
    helper_read8 = E->code.len;
    if (buf_u8(&E->code, 0x8B) || buf_u8(&E->code, 0x44) || buf_u8(&E->code, 0x24) || buf_u8(&E->code, 0x04)) return -1;
    if (buf_u8(&E->code, 0x0F) || buf_u8(&E->code, 0xB6) || buf_u8(&E->code, 0x00)) return -1; /* movzx eax, byte [eax] */
    if (emit_ret(&E->code)) return -1;

    return 0;
}

/* ---- very small recursive-descent-ish scanner for the boot subset ---- */
typedef struct {
    const char *src;
    const char *p;
    const char *input_name;
    MBEmit *E;
    int frame;
    int nlocals;
    char local_names[MAX_LOCALS][32];
} Parse;

static void skip_ws_comments(Parse *P);

/* Compute 1-based line/column of current parse position and format a useful error. */
static void mb_error_at(Parse *P, const char *msg) {
    int line = 1, col = 1;
    for (const char *s = P->src; s < P->p && *s; s++) {
        if (*s == '\n') { line++; col = 1; }
        else col++;
    }
    P->E->had_error = 1;
    if (P->input_name && P->input_name[0])
        snprintf(P->E->errbuf, sizeof P->E->errbuf, "%s:%d:%d: %s",
                 P->input_name, line, col, msg);
    else
        snprintf(P->E->errbuf, sizeof P->E->errbuf, "line %d:%d: %s", line, col, msg);
}

static int local_lookup(Parse *P, const char *name) {
    for (int i = 0; i < P->nlocals; i++) {
        if (strcmp(P->local_names[i], name) == 0) return i;
    }
    return -1;
}

static int local_add(Parse *P, const char *name) {
    int idx = local_lookup(P, name);
    if (idx >= 0) return idx;
    if (P->nlocals >= MAX_LOCALS) return -1;
    idx = P->nlocals++;
    snprintf(P->local_names[idx], sizeof P->local_names[idx], "%s", name);
    return idx;
}

static uint32_t local_addr(int idx) {
    return LOCAL_BASE + (uint32_t)idx * 4u;
}

static int parse_ident(Parse *P, char *buf, size_t buflen) {
    skip_ws_comments(P);
    size_t i = 0;
    if (!(isalpha((unsigned char)*P->p) || *P->p == '_')) return 0;
    while (*P->p && (isalnum((unsigned char)*P->p) || *P->p == '_')) {
        if (i + 1 < buflen) buf[i++] = *P->p;
        P->p++;
    }
    buf[i] = '\0';
    return i > 0;
}

static int parse_int_literal(Parse *P, uint32_t *out) {
    skip_ws_comments(P);
    uint32_t val = 0;
    int hex = 0;
    int any = 0;
    if (P->p[0] == '0' && (P->p[1] == 'x' || P->p[1] == 'X')) { hex = 1; P->p += 2; }
    while (1) {
        char c = *P->p;
        if (c >= '0' && c <= '9') { val = hex ? (val*16u + (uint32_t)(c-'0')) : (val*10u + (uint32_t)(c-'0')); P->p++; any = 1; }
        else if (hex && c >= 'a' && c <= 'f') { val = val*16u + 10u + (uint32_t)(c-'a'); P->p++; any = 1; }
        else if (hex && c >= 'A' && c <= 'F') { val = val*16u + 10u + (uint32_t)(c-'A'); P->p++; any = 1; }
        else break;
    }
    if (!any) return 0;
    *out = val;
    return 1;
}

static void skip_ws_comments(Parse *P) {
    for (;;) {
        while (*P->p && isspace((unsigned char)*P->p)) P->p++;
        if (P->p[0] == '/' && P->p[1] == '/') {
            while (*P->p && *P->p != '\n') P->p++;
            continue;
        }
        if (P->p[0] == '/' && P->p[1] == '*') {
            P->p += 2;
            while (*P->p && !(P->p[0] == '*' && P->p[1] == '/')) P->p++;
            if (*P->p) P->p += 2;
            continue;
        }
        break;
    }
}

static int match(Parse *P, const char *kw) {
    skip_ws_comments(P);
    size_t n = strlen(kw);
    if (strncmp(P->p, kw, n) == 0) {
        /* require word boundary for identifiers */
        if (isalpha((unsigned char)kw[0]) || kw[0] == '_') {
            char c = P->p[n];
            if (isalnum((unsigned char)c) || c == '_') return 0;
        }
        P->p += n;
        return 1;
    }
    return 0;
}

static int parse_string(Parse *P, char *buf, size_t buflen) {
    skip_ws_comments(P);
    if (*P->p != '"') return 0;
    P->p++;
    size_t i = 0;
    while (*P->p && *P->p != '"') {
        char c = *P->p++;
        if (c == '\\' && *P->p) {
            char e = *P->p++;
            if (e == 'n') c = '\n';
            else if (e == 't') c = '\t';
            else if (e == '\\') c = '\\';
            else if (e == '"') c = '"';
            else c = e;
        }
        if (i + 1 < buflen) buf[i++] = c;
    }
    if (*P->p == '"') P->p++;
    buf[i] = '\0';
    return 1;
}

/* Forward */
static int emit_stmt(Parse *P);

static int emit_call_helper(Parse *P, size_t helper_off) {
    /* call rel32 */
    size_t at = P->E->code.len;
    if (emit_call_rel32(&P->E->code, 0)) return -1;
    int32_t rel = (int32_t)helper_off - (int32_t)(at + 5);
    memcpy(P->E->code.data + at + 1, &rel, 4);
    return 0;
}

static int emit_stmt(Parse *P) {
    skip_ws_comments(P);
    if (!*P->p) return 0;

    if (match(P, "Screen.Clear") || match(P, "Screen.Clear()")) {
        match(P, "()");
        if (match(P, ";")) {}
        return emit_call_helper(P, helper_screen_clear);
    }

    if (match(P, "Screen.Write")) {
        match(P, "(");
        char str[512];
        if (!parse_string(P, str, sizeof str)) {
            mb_error_at(P, "Screen.Write expects a string literal (e.g. Screen.Write(\"hello\");)");
            return -1;
        }
        match(P, ")");
        match(P, ";");
        if (emit_push_string(P->E, str)) return -1;
        if (emit_call_helper(P, helper_screen_write)) return -1;
        /* add esp, 4 */
        if (buf_u8(&P->E->code, 0x83) || buf_u8(&P->E->code, 0xC4) || buf_u8(&P->E->code, 0x04)) return -1;
        return 0;
    }

    if (match(P, "CPU.Halt") || match(P, "CPU.Halt()")) {
        match(P, "()");
        match(P, ";");
        return emit_call_helper(P, helper_halt);
    }

    if (match(P, "while")) {
        match(P, "true") || match(P, "(true)") || match(P, "1");
        skip_ws_comments(P);
        if (*P->p != '{') {
            mb_error_at(P, "while expects '{' after condition (only 'while true { ... }' is supported on Multiboot native)");
            return -1;
        }
        P->p++; /* { */
        size_t loop = P->E->code.len;
        while (1) {
            skip_ws_comments(P);
            if (*P->p == '}') { P->p++; break; }
            if (!*P->p) { mb_error_at(P, "unclosed while block (missing '}')"); return -1; }
            if (emit_stmt(P)) return -1;
        }
        /* jmp loop */
        size_t at = P->E->code.len;
        if (emit_jmp_rel32(&P->E->code, 0)) return -1;
        int32_t rel = (int32_t)loop - (int32_t)(at + 5);
        memcpy(P->E->code.data + at + 1, &rel, 4);
        return 0;
    }

    if (match(P, "return")) {
        /* ignore value; just fall through / ret from main */
        while (*P->p && *P->p != ';' && *P->p != '\n') P->p++;
        match(P, ";");
        /* for main we will halt after return */
        return 0;
    }

    /* System.Panic("...") – write message then halt */
    if (match(P, "System.Panic")) {
        match(P, "(");
        char str[512];
        if (parse_string(P, str, sizeof str)) {
            if (emit_push_string(P->E, str)) return -1;
            if (emit_call_helper(P, helper_screen_write)) return -1;
            if (buf_u8(&P->E->code, 0x83) || buf_u8(&P->E->code, 0xC4) || buf_u8(&P->E->code, 0x04)) return -1;
        }
        match(P, ")");
        match(P, ";");
        return emit_call_helper(P, helper_halt);
    }

    /* ---- Memory.* ---- */
    if (match(P, "Memory.Alloc")) {
        match(P, "(");
        uint32_t sz = 0;
        if (!parse_int_literal(P, &sz)) { mb_error_at(P, "Memory.Alloc expects an integer size (e.g. Memory.Alloc(64))"); return -1; }
        match(P, ")");
        match(P, ";");
        if (emit_push_imm32(&P->E->code, sz)) return -1;
        if (emit_call_helper(P, helper_alloc)) return -1;
        if (buf_u8(&P->E->code, 0x83) || buf_u8(&P->E->code, 0xC4) || buf_u8(&P->E->code, 0x04)) return -1;
        return 0;
    }
    if (match(P, "Memory.Free")) {
        match(P, "(");
        while (*P->p && *P->p != ')') P->p++;
        match(P, ")");
        match(P, ";");
        return emit_call_helper(P, helper_free);
    }
    if (match(P, "Memory.HeapFree") || match(P, "Memory.HeapFree()")) {
        match(P, "()");
        match(P, ";");
        return emit_call_helper(P, helper_heapfree);
    }
    {
        int is_w32 = match(P, "Memory.Write32");
        int is_w8  = !is_w32 && match(P, "Memory.Write8");
        if (is_w32 || is_w8) {
            match(P, "(");
            char id[32];
            int lidx = -1;
            if (parse_ident(P, id, sizeof id)) lidx = local_lookup(P, id);
            match(P, ",");
            uint32_t val = 0;
            if (!parse_int_literal(P, &val)) { mb_error_at(P, "Memory.Write expects an integer value"); return -1; }
            match(P, ")");
            match(P, ";");
            if (emit_push_imm32(&P->E->code, val)) return -1;
            if (lidx >= 0) {
                if (buf_u8(&P->E->code, 0xFF) || buf_u8(&P->E->code, 0x35) || buf_u32(&P->E->code, local_addr(lidx))) return -1;
            } else {
                if (emit_push_eax(&P->E->code)) return -1;
            }
            if (emit_call_helper(P, is_w8 ? helper_write8 : helper_write32)) return -1;
            if (buf_u8(&P->E->code, 0x83) || buf_u8(&P->E->code, 0xC4) || buf_u8(&P->E->code, 0x08)) return -1;
            return 0;
        }
    }
    {
        int is_r32 = match(P, "Memory.Read32");
        int is_r8  = !is_r32 && match(P, "Memory.Read8");
        if (is_r32 || is_r8) {
            match(P, "(");
            char id[32];
            int lidx = -1;
            if (parse_ident(P, id, sizeof id)) lidx = local_lookup(P, id);
            match(P, ")");
            match(P, ";");
            if (lidx >= 0) {
                if (buf_u8(&P->E->code, 0xFF) || buf_u8(&P->E->code, 0x35) || buf_u32(&P->E->code, local_addr(lidx))) return -1;
            } else {
                if (emit_push_eax(&P->E->code)) return -1;
            }
            if (emit_call_helper(P, is_r8 ? helper_read8 : helper_read32)) return -1;
            if (buf_u8(&P->E->code, 0x83) || buf_u8(&P->E->code, 0xC4) || buf_u8(&P->E->code, 0x04)) return -1;
            return 0;
        }
    }

    /* let name: type = expr ; */
    if (match(P, "let")) {
        char name[32];
        if (!parse_ident(P, name, sizeof name)) { mb_error_at(P, "let expects a variable name (e.g. let x = 0;)"); return -1; }
        skip_ws_comments(P);
        if (match(P, ":")) {
            char ty[32];
            parse_ident(P, ty, sizeof ty);
        }
        skip_ws_comments(P);
        if (!match(P, "=")) { mb_error_at(P, "let expects '=' after the variable name"); return -1; }
        skip_ws_comments(P);
        int lidx = local_add(P, name);
        if (lidx < 0) {
            char buf[128];
            snprintf(buf, sizeof buf, "too many named locals (max %d on Multiboot native path)", MAX_LOCALS);
            mb_error_at(P, buf);
            return -1;
        }

        if (match(P, "Memory.Alloc")) {
            match(P, "(");
            uint32_t sz = 0;
            if (!parse_int_literal(P, &sz)) { mb_error_at(P, "Memory.Alloc size must be an integer literal"); return -1; }
            match(P, ")");
            match(P, ";");
            if (emit_push_imm32(&P->E->code, sz)) return -1;
            if (emit_call_helper(P, helper_alloc)) return -1;
            if (buf_u8(&P->E->code, 0x83) || buf_u8(&P->E->code, 0xC4) || buf_u8(&P->E->code, 0x04)) return -1;
            if (buf_u8(&P->E->code, 0xA3) || buf_u32(&P->E->code, local_addr(lidx))) return -1;
            return 0;
        }
        {
            int is_r32 = match(P, "Memory.Read32");
            int is_r8 = !is_r32 && match(P, "Memory.Read8");
            if (is_r32 || is_r8) {
                match(P, "(");
                char id[32];
                int srcidx = -1;
                if (parse_ident(P, id, sizeof id)) srcidx = local_lookup(P, id);
                match(P, ")");
                match(P, ";");
                if (srcidx >= 0) {
                    if (buf_u8(&P->E->code, 0xFF) || buf_u8(&P->E->code, 0x35) || buf_u32(&P->E->code, local_addr(srcidx))) return -1;
                } else {
                    if (emit_push_eax(&P->E->code)) return -1;
                }
                if (emit_call_helper(P, is_r8 ? helper_read8 : helper_read32)) return -1;
                if (buf_u8(&P->E->code, 0x83) || buf_u8(&P->E->code, 0xC4) || buf_u8(&P->E->code, 0x04)) return -1;
                if (buf_u8(&P->E->code, 0xA3) || buf_u32(&P->E->code, local_addr(lidx))) return -1;
                return 0;
            }
        }
        if (match(P, "Memory.HeapFree") || match(P, "Memory.HeapFree()")) {
            match(P, "()");
            match(P, ";");
            if (emit_call_helper(P, helper_heapfree)) return -1;
            if (buf_u8(&P->E->code, 0xA3) || buf_u32(&P->E->code, local_addr(lidx))) return -1;
            return 0;
        }
        /* integer literal assignment */
        uint32_t imm = 0;
        if (parse_int_literal(P, &imm)) {
            match(P, ";");
            if (buf_u8(&P->E->code, 0xC7) || buf_u8(&P->E->code, 0x05) ||
                buf_u32(&P->E->code, local_addr(lidx)) || buf_u32(&P->E->code, imm)) return -1;
            return 0;
        }
        while (*P->p && *P->p != ';') P->p++;
        match(P, ";");
        return 0;
    }

    /* if name == imm { ... } [ else { ... } ] */
    if (match(P, "if")) {
        skip_ws_comments(P);
        char id[32];
        int lidx = -1;
        if (parse_ident(P, id, sizeof id)) lidx = local_lookup(P, id);
        int eq = match(P, "==");
        int ne = !eq && match(P, "!=");
        uint32_t cmp_val = 0;
        int has_cmp = 0;
        if (eq || ne) {
            has_cmp = parse_int_literal(P, &cmp_val);
        }
        skip_ws_comments(P);
        if (*P->p != '{') { mb_error_at(P, "if expects '{' after the condition"); return -1; }
        P->p++;

        /* load local into eax if named */
        if (lidx >= 0) {
            if (buf_u8(&P->E->code, 0xA1) || buf_u32(&P->E->code, local_addr(lidx))) return -1;
        }

        size_t jcc_at;
        if (has_cmp && cmp_val == 0 && eq) {
            if (buf_u8(&P->E->code, 0x85) || buf_u8(&P->E->code, 0xC0)) return -1;
            jcc_at = P->E->code.len;
            if (buf_u8(&P->E->code, 0x75) || buf_u8(&P->E->code, 0x00)) return -1; /* jnz skip then */
        } else if (has_cmp && cmp_val == 0 && ne) {
            if (buf_u8(&P->E->code, 0x85) || buf_u8(&P->E->code, 0xC0)) return -1;
            jcc_at = P->E->code.len;
            if (buf_u8(&P->E->code, 0x74) || buf_u8(&P->E->code, 0x00)) return -1; /* jz skip then */
        } else if (has_cmp && eq) {
            if (buf_u8(&P->E->code, 0x3D) || buf_u32(&P->E->code, cmp_val)) return -1;
            jcc_at = P->E->code.len;
            if (buf_u8(&P->E->code, 0x75) || buf_u8(&P->E->code, 0x00)) return -1; /* jne */
        } else if (has_cmp && ne) {
            if (buf_u8(&P->E->code, 0x3D) || buf_u32(&P->E->code, cmp_val)) return -1;
            jcc_at = P->E->code.len;
            if (buf_u8(&P->E->code, 0x74) || buf_u8(&P->E->code, 0x00)) return -1; /* je */
        } else {
            if (buf_u8(&P->E->code, 0x85) || buf_u8(&P->E->code, 0xC0)) return -1;
            jcc_at = P->E->code.len;
            if (buf_u8(&P->E->code, 0x74) || buf_u8(&P->E->code, 0x00)) return -1; /* jz */
        }

        while (1) {
            skip_ws_comments(P);
            if (*P->p == '}') { P->p++; break; }
            if (!*P->p) { mb_error_at(P, "unclosed if block (missing '}')"); return -1; }
            if (emit_stmt(P)) return -1;
        }

        skip_ws_comments(P);
        if (match(P, "else")) {
            size_t jmp_at = P->E->code.len;
            if (buf_u8(&P->E->code, 0xEB) || buf_u8(&P->E->code, 0x00)) return -1; /* jmp past else */
            size_t else_start = P->E->code.len;
            P->E->code.data[jcc_at + 1] = (uint8_t)(else_start - (jcc_at + 2));
            skip_ws_comments(P);
            if (*P->p != '{') { mb_error_at(P, "else expects '{'"); return -1; }
            P->p++;
            while (1) {
                skip_ws_comments(P);
                if (*P->p == '}') { P->p++; break; }
                if (!*P->p) { mb_error_at(P, "unclosed else block (missing '}')"); return -1; }
                if (emit_stmt(P)) return -1;
            }
            size_t after = P->E->code.len;
            P->E->code.data[jmp_at + 1] = (uint8_t)(after - (jmp_at + 2));
        } else {
            size_t after = P->E->code.len;
            P->E->code.data[jcc_at + 1] = (uint8_t)(after - (jcc_at + 2));
        }
        return 0;
    }

    /* Unknown statement – skip to ; or } or newline to stay resilient */
    if (*P->p == '}') return 0;
    while (*P->p && *P->p != ';' && *P->p != '\n' && *P->p != '}') P->p++;
    match(P, ";");
    return 0;
}

static int compile_main(Parse *P) {
    skip_ws_comments(P);
    /* skip any leading fn definitions until main, or accept top-level statements inside main */
    while (*P->p) {
        skip_ws_comments(P);
        if (match(P, "fn")) {
            skip_ws_comments(P);
            int is_main = match(P, "main");
            /* skip name if not main */
            if (!is_main) {
                while (*P->p && !isspace((unsigned char)*P->p) && *P->p != '(') P->p++;
            }
            skip_ws_comments(P);
            if (*P->p == '(') {
                while (*P->p && *P->p != ')') P->p++;
                if (*P->p == ')') P->p++;
            }
            skip_ws_comments(P);
            if (*P->p != '{') {
                mb_error_at(P, "function body expects '{' after the parameter list");
                return -1;
            }
            P->p++;
            if (is_main) {
                while (1) {
                    skip_ws_comments(P);
                    if (*P->p == '}') { P->p++; break; }
                    if (!*P->p) break;
                    if (emit_stmt(P)) return -1;
                }
                /* after main: halt */
                return emit_call_helper(P, helper_halt);
            } else {
                /* skip non-main function body */
                int depth = 1;
                while (*P->p && depth) {
                    if (*P->p == '{') depth++;
                    if (*P->p == '}') depth--;
                    P->p++;
                }
            }
            continue;
        }
        /* top-level statement (treat as main body) */
        if (*P->p) {
            if (emit_stmt(P)) return -1;
        } else break;
    }
    return emit_call_helper(P, helper_halt);
}

/* ---- Multiboot2 header + ELF32 packaging ---- */
static int build_multiboot_elf(MBEmit *E, int flat, size_t entry_off, uint8_t **out, size_t *out_len) {
    /* Layout for ELF:
     *  [ELF header 52]
     *  [Program header 32]
     *  [4-byte pad]
     *  [Multiboot2 header + end tag]
     *  [code]   helpers + real entry + main
     *  [rodata]
     *  [section headers]
     *
     * entry_off = offset inside the code buffer of the real entry point
     * (the mov esp instruction). Multiboot header itself is not executable code.
     */
    uint32_t header_size = flat ? 64u : 24u;
    uint32_t pad = (8 - (header_size % 8)) % 8;
    uint32_t header_and_pad = header_size + pad;
    uint32_t code_file_off;
    uint32_t rodata_va;
    uint32_t entry_va;

    if (flat) {
        code_file_off = header_and_pad;
        rodata_va = LOAD_ADDR + header_and_pad + (uint32_t)E->code.len;
        entry_va  = LOAD_ADDR + header_and_pad + (uint32_t)entry_off;
    } else {
        /*
         * The Multiboot2 header must be 8-byte aligned in the file.
         * ELF headers occupy 52 + 32 = 84 bytes, so add four bytes
         * before the load segment. The segment then begins at file
         * offset 88 while mapping to LOAD_ADDR.
         */
        code_file_off = 88 + header_and_pad;
        rodata_va = LOAD_ADDR + header_and_pad + (uint32_t)E->code.len;
        entry_va  = LOAD_ADDR + header_and_pad + (uint32_t)entry_off;
    }

    for (int i = 0; i < E->nfix; i++) {
        uint32_t addr = rodata_va + E->strs[E->fixups[i].str_idx].off;
        size_t at = E->fixups[i].at;
        if (at + 4 <= E->code.len)
            memcpy(E->code.data + at, &addr, 4);
    }

    Buf img;
    buf_init(&img);

    if (!flat) {
        /* ELF32 header */
        uint8_t ehdr[52];
        memset(ehdr, 0, sizeof ehdr);
        ehdr[0]=0x7F; ehdr[1]='E'; ehdr[2]='L'; ehdr[3]='F';
        ehdr[4]=1; /* 32-bit */ ehdr[5]=1; /* LE */ ehdr[6]=1;
        /* e_type = ET_EXEC */
        ehdr[16]=2; ehdr[17]=0;
        /* e_machine = EM_386 */
        ehdr[18]=3; ehdr[19]=0;
        /* e_version */
        ehdr[20]=1;
        /* e_entry – real entry, not the Multiboot header */
        memcpy(ehdr + 24, &entry_va, 4);
        /* e_phoff = 52 */
        uint32_t phoff = 52;
        memcpy(ehdr + 28, &phoff, 4);
        /* e_ehsize = 52 */
        ehdr[40]=52; ehdr[41]=0;
        /* e_phentsize = 32 */
        ehdr[42]=32; ehdr[43]=0;
        /* e_phnum = 1 */
        ehdr[44]=1; ehdr[45]=0;
        /* Section headers will be written after the load image.
         * We emit 4 sections: NULL, .text, .rodata, .shstrtab.
         * This keeps GRUB, readelf and other tools from exploding on a
         * half-formed section table. */
        uint32_t load_end = 88u + header_and_pad
                          + (uint32_t)E->code.len + (uint32_t)E->rodata.len;
        /* String table: "\0.text\0.rodata\0.shstrtab\0" = 1+6+1+8+1+10+1 = 28 bytes */
        const char shstr[] = "\0.text\0.rodata\0.shstrtab";
        const uint32_t shstr_size = sizeof(shstr); /* 25 including terminating NUL */
        uint32_t shstr_off = load_end;                 /* file offset of .shstrtab data */
        uint32_t shoff     = (load_end + shstr_size + 3u) & ~3u; /* align section headers */
        memcpy(ehdr + 32, &shoff, 4); /* e_shoff */
        ehdr[46]=40; ehdr[47]=0;       /* e_shentsize = sizeof(Elf32_Shdr) */
        ehdr[48]=4; ehdr[49]=0;        /* e_shnum = 4 */
        ehdr[50]=3; ehdr[51]=0;        /* e_shstrndx = 3 (.shstrtab) */
        if (buf_bytes(&img, ehdr, 52)) goto fail;

        /* Program header – one PT_LOAD */
        uint8_t phdr[32];
        memset(phdr, 0, sizeof phdr);
        phdr[0]=1; /* PT_LOAD */
        uint32_t p_offset = 88; /* aligned start of the load segment */
        uint32_t p_vaddr = LOAD_ADDR;
        uint32_t p_filesz = header_and_pad + (uint32_t)E->code.len + (uint32_t)E->rodata.len;
        uint32_t p_memsz = p_filesz + 0x10000; /* room for stack */
        uint32_t p_flags = 7; /* RWX */
        /* Compact image: file offset and virtual address are congruent mod 8. */
        uint32_t p_align = 8;
        memcpy(phdr + 4, &p_offset, 4);
        memcpy(phdr + 8, &p_vaddr, 4);
        memcpy(phdr + 12, &p_vaddr, 4); /* p_paddr */
        memcpy(phdr + 16, &p_filesz, 4);
        memcpy(phdr + 20, &p_memsz, 4);
        memcpy(phdr + 24, &p_flags, 4);
        memcpy(phdr + 28, &p_align, 4);
        if (buf_bytes(&img, phdr, 32)) goto fail;
        /* Four-byte gap aligns the Multiboot2 header at file offset 88. */
        if (buf_u32(&img, 0)) goto fail;
    }

    /* Multiboot2 header: magic, architecture, length, checksum. */
    if (buf_u32(&img, 0xE85250D6u)) goto fail;
    if (buf_u32(&img, 0)) goto fail; /* i386 */
    if (buf_u32(&img, header_size)) goto fail;
    uint32_t chk = (uint32_t)(0xE85250D6u + header_size);
    if (buf_u32(&img, (uint32_t)(-chk))) goto fail;

    if (flat) {
        /*
         * Multiboot2 address tag (type=2, size=24), required for flat
         * binaries because they have no ELF program headers.
         */
        uint32_t load_end = LOAD_ADDR + header_and_pad
                          + (uint32_t)E->code.len
                          + (uint32_t)E->rodata.len;
        if (buf_u16(&img, 2) || buf_u16(&img, 0) || buf_u32(&img, 24)) goto fail;
        if (buf_u32(&img, LOAD_ADDR)) goto fail;          /* header_addr */
        if (buf_u32(&img, LOAD_ADDR)) goto fail;          /* load_addr */
        if (buf_u32(&img, load_end)) goto fail;           /* load_end_addr */
        if (buf_u32(&img, load_end + 0x10000u)) goto fail;/* bss_end_addr */

        /* Separate Multiboot2 entry-address tag (type=3, size=12). */
        if (buf_u16(&img, 3) || buf_u16(&img, 0) || buf_u32(&img, 12)) goto fail;
        if (buf_u32(&img, entry_va)) goto fail;
        /* Each tag starts on an 8-byte boundary; pad entry tag from 12 to 16. */
        if (buf_u32(&img, 0)) goto fail;
    }

    /* Required Multiboot2 end tag (type=0, flags=0, size=8). */
    if (buf_u16(&img, 0) || buf_u16(&img, 0) || buf_u32(&img, 8)) goto fail;
    for (uint32_t i = 0; i < pad; i++) if (buf_u8(&img, 0)) goto fail;

    if (buf_bytes(&img, E->code.data, E->code.len)) goto fail;
    if (buf_bytes(&img, E->rodata.data, E->rodata.len)) goto fail;

    if (!flat) {
        /*
         * Proper ELF32 section table so tools (GRUB, readelf, objdump, ...)
         * do not spontaneously combust. Layout after the PT_LOAD image:
         *
         *   [ .shstrtab data ]  (28 bytes)
         *   [ pad to 4-byte ] 
         *   [ 4 × Elf32_Shdr ]
         *
         * Section indices:
         *   0 = NULL
         *   1 = .text   (covers Multiboot header + machine code)
         *   2 = .rodata
         *   3 = .shstrtab
         */
        const char shstr[] = "\0.text\0.rodata\0.shstrtab";
        const uint32_t shstr_size = sizeof(shstr); /* 25 including terminating NUL */
        if (buf_bytes(&img, shstr, shstr_size)) goto fail;
        /* pad to 4-byte alignment for section headers */
        while (img.len & 3) if (buf_u8(&img, 0)) goto fail;

        uint32_t text_off   = 88; /* start of Multiboot + code */
        uint32_t text_size  = header_and_pad + (uint32_t)E->code.len;
        uint32_t rodata_off = text_off + text_size;
        uint32_t rodata_size= (uint32_t)E->rodata.len;
        uint32_t shstr_off  = rodata_off + rodata_size; /* exact, before any pad */

        /* Helper to write one Elf32_Shdr (little-endian) */
        #define WRITE_SHDR(name_off, type, flags, addr, offset, size, link, info, addralign, entsize) do { \
            uint8_t sh[40]; memset(sh, 0, 40); \
            uint32_t _v; \
            _v = (name_off); memcpy(sh+0, &_v, 4); \
            _v = (type);     memcpy(sh+4, &_v, 4); \
            _v = (flags);    memcpy(sh+8, &_v, 4); \
            _v = (addr);     memcpy(sh+12,&_v, 4); \
            _v = (offset);   memcpy(sh+16,&_v, 4); \
            _v = (size);     memcpy(sh+20,&_v, 4); \
            _v = (link);     memcpy(sh+24,&_v, 4); \
            _v = (info);     memcpy(sh+28,&_v, 4); \
            _v = (addralign);memcpy(sh+32,&_v, 4); \
            _v = (entsize);  memcpy(sh+36,&_v, 4); \
            if (buf_bytes(&img, sh, 40)) goto fail; \
        } while (0)

        /* 0: NULL */
        WRITE_SHDR(0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        /* 1: .text  name offset 1, SHT_PROGBITS=1, SHF_ALLOC|SHF_EXECINSTR=6 */
        WRITE_SHDR(1, 1, 6, LOAD_ADDR, text_off, text_size, 0, 0, 8, 0);
        /* 2: .rodata name offset 7, SHT_PROGBITS=1, SHF_ALLOC=2 */
        WRITE_SHDR(7, 1, 2, LOAD_ADDR + text_size, rodata_off, rodata_size, 0, 0, 1, 0);
        /* 3: .shstrtab name offset 15, SHT_STRTAB=3 */
        WRITE_SHDR(15, 3, 0, 0, shstr_off, shstr_size, 0, 0, 1, 0);

        #undef WRITE_SHDR
    }

    *out = img.data;
    *out_len = img.len;
    /* ownership transferred */
    img.data = NULL;
    return 0;

fail:
    buf_free(&img);
    return -1;
}

int native_mb_compile(
    const char *source,
    const char *input_name,
    int flat_binary,
    uint8_t **out_blob,
    size_t *out_len,
    const char **err_out
) {
    MBEmit E;
    memset(&E, 0, sizeof E);
    buf_init(&E.code);
    buf_init(&E.rodata);

    if (emit_helpers(&E)) {
        if (err_out) *err_out = "failed to emit runtime helpers";
        buf_free(&E.code); buf_free(&E.rodata);
        return 1;
    }

    /*
     * Real entry point: set stack, call main, then halt forever if main returns.
     * We emit a placeholder call and patch the relative offset after main is known.
     */
    size_t entry_off = E.code.len;          /* this becomes e_entry / Multiboot entry */

    /* mov esp, LOAD_ADDR + 0x10000 */
    if (emit_mov_esp_imm(&E.code, LOAD_ADDR + 0x10000u)) {
        if (err_out) *err_out = "oom";
        buf_free(&E.code); buf_free(&E.rodata);
        return 1;
    }

    /* Initialise runtime globals (safe low-memory area) */
    /* mov dword [CURSOR_POS], 0 */
    if (buf_u8(&E.code, 0xC7) || buf_u8(&E.code, 0x05) ||
        buf_u32(&E.code, CURSOR_POS) || buf_u32(&E.code, 0)) {
        if (err_out) *err_out = "oom";
        buf_free(&E.code); buf_free(&E.rodata);
        return 1;
    }
    /* mov dword [HEAP_PTR], HEAP_START */
    if (buf_u8(&E.code, 0xC7) || buf_u8(&E.code, 0x05) ||
        buf_u32(&E.code, HEAP_PTR) || buf_u32(&E.code, HEAP_START)) {
        if (err_out) *err_out = "oom";
        buf_free(&E.code); buf_free(&E.rodata);
        return 1;
    }

    /* call rel32 placeholder – patched after main is emitted */
    size_t call_at = E.code.len;
    if (emit_call_rel32(&E.code, 0)) {
        if (err_out) *err_out = "oom";
        buf_free(&E.code); buf_free(&E.rodata);
        return 1;
    }

    /* if main returns, hang */
    if (emit_cli(&E.code) || emit_hlt(&E.code) || emit_jmp_rel8(&E.code, -4)) {
        if (err_out) *err_out = "oom";
        buf_free(&E.code); buf_free(&E.rodata);
        return 1;
    }

    /* Now compile the user main() – its start is the current code length */
    size_t main_off = E.code.len;
    Parse P = { .src = source, .p = source, .input_name = input_name, .E = &E, .frame = 0, .nlocals = 0 };
    if (compile_main(&P) || E.had_error) {
        /* errbuf lives on the stack; strdup so the caller can print it safely */
        if (err_out)
            *err_out = E.errbuf[0] ? strdup(E.errbuf) : "native compile failed";
        buf_free(&E.code); buf_free(&E.rodata);
        for (int i = 0; i < E.nstr; i++) free(E.strs[i].s);
        return 1;
    }

    /* Patch the call so it lands on main */
    {
        int32_t rel = (int32_t)main_off - (int32_t)(call_at + 5);
        memcpy(E.code.data + call_at + 1, &rel, 4);
    }

    uint8_t *blob = NULL;
    size_t len = 0;
    if (build_multiboot_elf(&E, flat_binary, entry_off, &blob, &len)) {
        if (err_out) *err_out = "failed to package Multiboot image";
        buf_free(&E.code); buf_free(&E.rodata);
        for (int i = 0; i < E.nstr; i++) free(E.strs[i].s);
        return 1;
    }

    for (int i = 0; i < E.nstr; i++) free(E.strs[i].s);
    buf_free(&E.code);
    buf_free(&E.rodata);

    *out_blob = blob;
    *out_len = len;
    if (err_out) *err_out = NULL;
    return 0;
}
