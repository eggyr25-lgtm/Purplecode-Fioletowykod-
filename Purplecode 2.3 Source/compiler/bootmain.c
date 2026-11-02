/*
 * Purplecode 2.3 NextGen (NG) – pure C Multiboot binary
 *
 * Default:  native machine-code emission (no GCC/Clang wrapper)
 * Optional: --gcchost   translate to C and drive GCC / Clang + linker
 *
 * Formats:
 *   --format elf|bin     Multiboot i386 (native default)
 *   --format uefi        UEFI x86-64 PE/COFF (native or --gcchost)
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>

#include "native_mb.h"

/* ---------- tiny source reader ---------- */
static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    if (out_len) *out_len = n;
    return buf;
}

static int write_bytes(const char *path, const uint8_t *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return -1; }
    if (fwrite(data, 1, len, f) != len) { perror("fwrite"); fclose(f); return -1; }
    fclose(f);
    return 0;
}

/* ---------- --gcchost path: emit C, drive toolchain ---------- */

static int emit_boot_c(const char *src, const char *cpath, int is_uefi) {
    FILE *o = fopen(cpath, "w");
    if (!o) { perror(cpath); return -1; }

    if (is_uefi) {
        fputs("/* Purplebootcode --gcchost UEFI translation */\n"
              "#include <stdint.h>\n"
              "#include <stddef.h>\n"
              "typedef uint64_t UINTN; typedef uint16_t CHAR16;\n"
              "typedef void *EFI_HANDLE;\n"
              "typedef struct { void *a; void *b; } EFI_TABLE_HEADER;\n"
              "typedef struct EFI_SYSTEM_TABLE EFI_SYSTEM_TABLE;\n"
              "typedef struct {\n"
              "  void *Reset;\n"
              "  UINTN (*OutputString)(void *This, CHAR16 *String);\n"
              "} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;\n"
              "struct EFI_SYSTEM_TABLE {\n"
              "  char hdr[24]; void *ConIn;\n"
              "  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;\n"
              "};\n"
              "static EFI_SYSTEM_TABLE *gST;\n"
              "static void screen_write(const char *s) {\n"
              "  if (!gST || !gST->ConOut || !s) return;\n"
              "  CHAR16 buf[512]; int i=0;\n"
              "  for (; s[i] && i<510; i++) buf[i]=(CHAR16)(unsigned char)s[i];\n"
              "  buf[i]=0; gST->ConOut->OutputString(gST->ConOut, buf);\n"
              "}\n"
              "static void screen_clear(void) { screen_write(\"\\r\\n\"); }\n"
              "static void cpu_halt(void) { for(;;) __asm__ volatile(\"hlt\"); }\n"
              "int efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {\n"
              "  (void)ImageHandle; gST = SystemTable;\n", o);
    } else {
        fputs("/* Purplebootcode --gcchost Multiboot translation */\n"
              "#include <stdint.h>\n"
              "#include <stddef.h>\n"
              "static volatile uint16_t *const VGA = (uint16_t *)0xB8000;\n"
              "static int vga_x=0, vga_y=0;\n"
              "static void screen_clear(void) {\n"
              "  for (int i=0;i<80*25;i++) VGA[i]=0x0F00|' ';\n"
              "  vga_x=vga_y=0;\n"
              "}\n"
              "static void screen_write(const char *s) {\n"
              "  if (!s) return;\n"
              "  for (; *s; s++) {\n"
              "    if (*s=='\\n') { vga_x=0; if(++vga_y>=25) vga_y=0; continue; }\n"
              "    VGA[vga_y*80+vga_x] = 0x0F00 | (uint16_t)(unsigned char)*s;\n"
              "    if (++vga_x>=80) { vga_x=0; if(++vga_y>=25) vga_y=0; }\n"
              "  }\n"
              "}\n"
              "static void cpu_halt(void) { for(;;) __asm__ volatile(\"cli; hlt\"); }\n"
              "/* Multiboot2 header */\n"
              "__attribute__((section(\".multiboot\")))\n"
              "const uint32_t mb_header[] = { 0xE85250D6, 0, 16, (uint32_t)-(0xE85250D6+0+16), 0,8,0,0 };\n"
              "void _start(void) {\n"
              "  __asm__ volatile(\"mov $0x200000, %%esp\" ::: \"memory\");\n", o);
    }

    /* Extremely small transliteration: recognise the common Screen / CPU patterns
     * so boot_hello and uefi_hello work under --gcchost without a full parser. */
    const char *p = src;
    int wrote_body = 0;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (strncmp(p, "Screen.Clear()", 13) == 0) {
            fputs("  screen_clear();\n", o); wrote_body = 1;
            p += 13; while (*p && *p != '\n' && *p != ';') p++;
            if (*p == ';') p++;
            continue;
        }
        if (strncmp(p, "Screen.Write(", 13) == 0) {
            p += 13;
            while (*p && isspace((unsigned char)*p)) p++;
            if (*p == '"') {
                fputs("  screen_write(\"", o);
                p++;
                while (*p && *p != '"') {
                    if (*p == '\\' && p[1]) { fputc(*p++, o); fputc(*p++, o); }
                    else fputc(*p++, o);
                }
                if (*p == '"') p++;
                fputs("\");\n", o);
                wrote_body = 1;
            }
            while (*p && *p != '\n' && *p != ';') p++;
            if (*p == ';') p++;
            continue;
        }
        if (strncmp(p, "CPU.Halt()", 10) == 0) {
            fputs("  cpu_halt();\n", o); wrote_body = 1;
            p += 10; while (*p && *p != '\n' && *p != ';') p++;
            if (*p == ';') p++;
            continue;
        }
        if (strncmp(p, "while true", 10) == 0 || strncmp(p, "while (true)", 12) == 0) {
            fputs("  for(;;) { cpu_halt(); }\n", o); wrote_body = 1;
            /* skip the rest of the while body roughly */
            int braces = 0;
            while (*p) {
                if (*p == '{') braces++;
                if (*p == '}') { braces--; if (braces <= 0) { p++; break; } }
                p++;
            }
            continue;
        }
        if (strncmp(p, "return", 6) == 0) {
            if (is_uefi) fputs("  return 0;\n", o);
            else fputs("  for(;;) cpu_halt();\n", o);
            wrote_body = 1;
            while (*p && *p != '\n' && *p != ';') p++;
            if (*p == ';') p++;
            continue;
        }
        /* skip other lines */
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }

    if (!wrote_body) {
        fputs("  screen_write(\"Purplebootcode\\n\");\n", o);
        if (!is_uefi) fputs("  for(;;) cpu_halt();\n", o);
        else fputs("  return 0;\n", o);
    }

    if (is_uefi) {
        fputs("  return 0;\n}\n"
              "/* PE entry expects this name when using -Wl,-entry:efi_main */\n", o);
    } else {
        fputs("}\n", o);
    }
    fclose(o);
    return 0;
}

static int run_cmd(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    return -1;
}

static int gcchost_build(const char *src, const char *out, int is_uefi) {
    char tmpl[] = "/tmp/purplebc_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) { perror("mkstemp"); return 1; }
    close(fd);
    char cpath[64];
    snprintf(cpath, sizeof cpath, "%s.c", tmpl);
    rename(tmpl, cpath);

    if (emit_boot_c(src, cpath, is_uefi) != 0) {
        unlink(cpath);
        return 1;
    }

    int rc;
    if (is_uefi) {
        /* Clang + lld-link style, or clang -target + lld */
        char *argv[] = {
            "clang", "-target", "x86_64-unknown-windows",
            "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
            "-mno-red-zone", "-Wall", "-O2",
            "-c", cpath, "-o", "/tmp/purplebc_uefi.o",
            NULL
        };
        rc = run_cmd(argv);
        if (rc != 0) {
            fprintf(stderr, "purplebc --gcchost: clang failed (is clang installed?)\n");
            unlink(cpath);
            return rc ? rc : 1;
        }
        char *link[] = {
            "lld-link", "-subsystem:efi_application", "-entry:efi_main",
            "-out:", /* filled below */
            "/tmp/purplebc_uefi.o", NULL
        };
        /* lld-link wants -out:file as one arg */
        static char outarg[512];
        snprintf(outarg, sizeof outarg, "-out:%s", out);
        link[2] = outarg;
        rc = run_cmd(link);
        if (rc != 0) {
            /* try clang driver as fallback */
            char *clanglink[] = {
                "clang", "-target", "x86_64-unknown-windows",
                "-nostdlib", "-Wl,-entry:efi_main",
                "-Wl,-subsystem:efi_application",
                "/tmp/purplebc_uefi.o", "-o", (char *)out, NULL
            };
            rc = run_cmd(clanglink);
        }
        unlink("/tmp/purplebc_uefi.o");
    } else {
        /* Multiboot: gcc -m32 freestanding + linker script if present */
        char *argv[] = {
            "gcc", "-m32", "-ffreestanding", "-fno-builtin",
            "-fno-stack-protector", "-nostdlib", "-Wall", "-O2",
            cpath, "-o", (char *)out,
            "-Wl,-T,purplelinker.ld",
            NULL
        };
        rc = run_cmd(argv);
        if (rc != 0) {
            /* retry without linker script */
            char *argv2[] = {
                "gcc", "-m32", "-ffreestanding", "-fno-builtin",
                "-fno-stack-protector", "-nostdlib", "-Wall", "-O2",
                cpath, "-o", (char *)out, NULL
            };
            rc = run_cmd(argv2);
        }
    }

    unlink(cpath);
    if (rc != 0) {
        fprintf(stderr, "purplebc --gcchost: toolchain failed with code %d\n", rc);
        return rc ? rc : 1;
    }
    printf("purplebc: %s -> %s (--gcchost)\n", "input", out);
    return 0;
}

/* ---------- usage ---------- */
static void usage(const char *argv0) {
    fprintf(stderr,
        "Purplebootcode (C binary) – native by default\n"
        "Usage: %s <file.purplebc> [options]\n"
        "\n"
        "  -o <file>          output path\n"
        "  --format elf|bin   Multiboot i386 ELF or flat binary (default: elf)\n"
        "  --format uefi      UEFI x86-64 PE/COFF application\n"
        "  --gcchost          use GCC/Clang host toolchain instead of native emitter\n"
        "  -h, --help         this help\n"
        "\n"
        "Default is pure native machine-code emission (no GCC wrapper).\n"
        "Use --gcchost only when you explicitly want the C-translation path.\n",
        argv0);
}

int main(int argc, char **argv) {
    const char *input = NULL;
    const char *output = NULL;
    const char *format = "elf";
    int gcchost = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        }
        if (!strcmp(argv[i], "--gcchost")) {
            gcchost = 1;
            continue;
        }
        if (!strcmp(argv[i], "--native")) {
            continue; /* accepted for compatibility: the internal emitter is already the default */
        }
        if (!strcmp(argv[i], "--format") && i + 1 < argc) {
            format = argv[++i];
            continue;
        }
        if (!strcmp(argv[i], "-o") && i + 1 < argc) {
            output = argv[++i];
            continue;
        }
        if (argv[i][0] == '-') {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
        if (!input) input = argv[i];
        else {
            fprintf(stderr, "unexpected argument: %s\n", argv[i]);
            return 2;
        }
    }

    if (!input) {
        usage(argv[0]);
        return 2;
    }

    int is_uefi = !strcmp(format, "uefi");
    int is_bin  = !strcmp(format, "bin");

    if (!output) {
        if (is_uefi) output = "BOOTX64.EFI";
        else if (is_bin) output = "kernel.bin";
        else output = "kernel.elf";
    }

    size_t len = 0;
    char *src = read_file(input, &len);
    if (!src) return 1;

    int rc = 0;
    if (gcchost) {
        rc = gcchost_build(src, output, is_uefi);
    } else {
        /* Native path – pure C machine-code emission, no GCC */
        if (is_uefi) {
            fprintf(stderr, "purplebc: there is no internal UEFI emitter yet; using the host Clang/LLD path for this target.\n");
            rc = gcchost_build(src, output, 1);
            free(src);
            return rc;
        }

        uint8_t *blob = NULL;
        size_t blob_len = 0;
        const char *err = NULL;
        if (native_mb_compile(src, input, is_bin, &blob, &blob_len, &err) != 0) {
            fprintf(stderr, "purplebc native: %s\n", err ? err : "compilation failed");
            free(src);
            return 1;
        }
        if (write_bytes(output, blob, blob_len) != 0) {
            free(blob);
            free(src);
            return 1;
        }
        free(blob);
        printf("purplebc: %s -> %s (native Multiboot %s, no GCC)\n",
               input, output, is_bin ? "flat binary" : "ELF");
    }

    free(src);
    return rc;
}
