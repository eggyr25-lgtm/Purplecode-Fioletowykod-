/*
 * purpleimage – C rewrite of the former Python tool.
 * Packages a Purplebootcode Multiboot ELF into either:
 *   --img  a PBFIMG1 raw container, or
 *   --iso  a GRUB-bootable ISO (requires grub-mkrescue + xorriso).
 *
 * No Python remains in the critical boot-image path.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <libgen.h>
#include <limits.h>

#define die(...) do { fprintf(stderr, "purpleimage: " __VA_ARGS__); fputc('\n', stderr); exit(1); } while (0)
#define die2(...) do { fprintf(stderr, "purpleimage: " __VA_ARGS__); fputc('\n', stderr); return 2; } while (0)

typedef struct {
    char *src;
    char *dest; /* relative, no leading slash */
} Inject;

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) die("out of memory");
    return p;
}
static char *xstrdup(const char *s) {
    char *d = strdup(s);
    if (!d) die("out of memory");
    return d;
}

static int file_size(const char *path, size_t *out) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    if (!S_ISREG(st.st_mode)) return -1;
    *out = (size_t)st.st_size;
    return 0;
}

static int read_file(const char *path, uint8_t **data, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    uint8_t *buf = xmalloc((size_t)sz);
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return -1; }
    fclose(f);
    *data = buf;
    *len = (size_t)sz;
    return 0;
}

static int write_file(const char *path, const uint8_t *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (fwrite(data, 1, len, f) != len) { fclose(f); return -1; }
    fclose(f);
    return 0;
}

static int which(const char *name, char *out, size_t outsz) {
    const char *path = getenv("PATH");
    if (!path) return -1;
    char *p = xstrdup(path);
    char *save = NULL;
    for (char *tok = strtok_r(p, ":", &save); tok; tok = strtok_r(NULL, ":", &save)) {
        char candidate[PATH_MAX];
        snprintf(candidate, sizeof candidate, "%s/%s", tok, name);
        if (access(candidate, X_OK) == 0) {
            strncpy(out, candidate, outsz - 1);
            out[outsz - 1] = 0;
            free(p);
            return 0;
        }
    }
    free(p);
    return -1;
}

/* Safe argument-vector execution – never goes through a shell */
static int run(const char *const argv[]) {
    if (!argv || !argv[0]) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    int status;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

/* ---------- PBFIMG1 container ---------- */
static int build_img(const char *kernel_path, const Inject *inj, int ninj,
                     int size_mib, const char *out_path)
{
    if (size_mib < 2) die("--size-mib must be at least 2");

    uint8_t *kdata = NULL;
    size_t klen = 0;
    if (read_file(kernel_path, &kdata, &klen) != 0)
        die("cannot read kernel: %s", kernel_path);

    /* First pass: collect file data and compute layout. */
    typedef struct { uint8_t *data; size_t len; char *dest; uint32_t offset; } Ent;
    Ent *ents = ninj ? xmalloc(ninj * sizeof(Ent)) : NULL;
    for (int i = 0; i < ninj; i++) {
        if (read_file(inj[i].src, &ents[i].data, &ents[i].len) != 0)
            die("cannot read inject source: %s", inj[i].src);
        ents[i].dest = inj[i].dest;
        ents[i].offset = 0;
    }

    const uint32_t header_size = 4096;
    uint32_t meta_offset = 64;
    uint32_t kernel_offset = header_size;
    uint32_t cursor = 0;

    /* Growable manifest builder – safely handles long paths / many files */
    char *manifest = NULL;
    size_t mcap = 0, mlen = 0;

#define MANIFEST_APPEND(...) do {                                         \
        for (;;) {                                                        \
            int need = snprintf(manifest ? manifest + mlen : NULL,        \
                                manifest ? (mcap - mlen) : 0,             \
                                __VA_ARGS__);                             \
            if (need < 0) die("snprintf failed while building manifest"); \
            if ((size_t)need + 1 <= (mcap - mlen)) {                      \
                mlen += (size_t)need;                                     \
                break;                                                    \
            }                                                             \
            size_t ncap = mcap ? mcap * 2 : 4096;                         \
            while (ncap < mlen + (size_t)need + 1) ncap *= 2;             \
            char *n = realloc(manifest, ncap);                            \
            if (!n) die("out of memory growing manifest");                \
            manifest = n;                                                 \
            mcap = ncap;                                                  \
        }                                                                 \
    } while (0)

    /* Iterate a few times until offsets stabilise (JSON size depends on numbers). */
    for (int pass = 0; pass < 8; pass++) {
        mlen = 0;
        MANIFEST_APPEND(
            "{\"format\":\"PBFIMG1\",\"kernel\":{\"path\":\"/boot/kernel.elf\",\"offset\":%u,\"size\":%zu},\"files\":[",
            kernel_offset, klen);
        for (int i = 0; i < ninj; i++) {
            if (i) MANIFEST_APPEND(",");
            MANIFEST_APPEND("{\"path\":\"%s\",\"size\":%zu,\"offset\":%u}",
                            ents[i].dest, ents[i].len, ents[i].offset);
        }
        MANIFEST_APPEND("]}");

        uint32_t new_kernel_offset = (header_size + (uint32_t)mlen + 4095u) & ~4095u;
        cursor = (new_kernel_offset + (uint32_t)klen + 4095u) & ~4095u;
        for (int i = 0; i < ninj; i++) {
            ents[i].offset = cursor;
            cursor = (cursor + (uint32_t)ents[i].len + 4095u) & ~4095u;
        }
        if (new_kernel_offset == kernel_offset) break;
        kernel_offset = new_kernel_offset;
    }

    /* Final manifest with correct offsets */
    mlen = 0;
    MANIFEST_APPEND(
        "{\"format\":\"PBFIMG1\",\"kernel\":{\"path\":\"/boot/kernel.elf\",\"offset\":%u,\"size\":%zu},\"files\":[",
        kernel_offset, klen);
    for (int i = 0; i < ninj; i++) {
        if (i) MANIFEST_APPEND(",");
        MANIFEST_APPEND("{\"path\":\"%s\",\"size\":%zu,\"offset\":%u}",
                        ents[i].dest, ents[i].len, ents[i].offset);
    }
    MANIFEST_APPEND("]}");

#undef MANIFEST_APPEND

    uint64_t total = (uint64_t)size_mib * 1024ull * 1024ull;
    if ((uint64_t)cursor > total)
        die("contents need %u bytes, exceeding image size %llu", cursor, (unsigned long long)total);

    FILE *f = fopen(out_path, "wb");
    if (!f) die("cannot create %s: %s", out_path, strerror(errno));
    if (ftruncate(fileno(f), (off_t)total) != 0) {
        fclose(f);
        die("ftruncate failed");
    }

    /* Header: "PBFIMG1\0" + 4×uint64 little-endian */
    uint8_t hdr[64] = {0};
    memcpy(hdr, "PBFIMG1", 7);
    uint64_t meta_off64 = meta_offset;
    uint64_t meta_len64 = mlen;
    uint64_t koff64 = kernel_offset;
    uint64_t klen64 = klen;
    memcpy(hdr + 8,  &meta_off64, 8);
    memcpy(hdr + 16, &meta_len64, 8);
    memcpy(hdr + 24, &koff64, 8);
    memcpy(hdr + 32, &klen64, 8);
    if (fwrite(hdr, 1, 64, f) != 64) { fclose(f); die("write header"); }

    fseek(f, meta_offset, SEEK_SET);
    if (fwrite(manifest, 1, mlen, f) != mlen) { fclose(f); die("write meta"); }

    fseek(f, kernel_offset, SEEK_SET);
    if (fwrite(kdata, 1, klen, f) != klen) { fclose(f); die("write kernel"); }

    for (int i = 0; i < ninj; i++) {
        fseek(f, ents[i].offset, SEEK_SET);
        if (fwrite(ents[i].data, 1, ents[i].len, f) != ents[i].len) {
            fclose(f); die("write inject");
        }
    }
    fclose(f);

    printf("purpleimage: wrote %s (%d MiB PBFIMG1 container, %d injected files)\n",
           out_path, size_mib, ninj);
    printf("Note: PBFIMG1 is a Purpleboot payload container, not a partitioned, directly BIOS-bootable disk image.\n");

    free(kdata);
    free(manifest);
    for (int i = 0; i < ninj; i++) free(ents[i].data);
    free(ents);
    return 0;
}

/* ---------- GRUB ISO ---------- */
static int build_iso(const char *kernel_path, const Inject *inj, int ninj,
                     bool uefi, const char *out_path)
{
    char grub[PATH_MAX], xorriso[PATH_MAX];
    if (which("grub-mkrescue", grub, sizeof grub) != 0 ||
        which("xorriso", xorriso, sizeof xorriso) != 0) {
        fprintf(stderr, "purpleimage: ISO creation requires grub-mkrescue and xorriso. "
                "Install GRUB tools; no fake ISO was created.\n");
        return 1;
    }

    if (uefi) {
        const char *efi_dirs[] = {
            "/usr/lib/grub/x86_64-efi",
            "/usr/lib/grub/x86_64-efi-signed",
            "/usr/share/grub/x86_64-efi",
            NULL
        };
        bool found = false;
        for (int i = 0; efi_dirs[i]; i++) {
            struct stat st;
            if (stat(efi_dirs[i], &st) == 0 && S_ISDIR(st.st_mode)) {
                found = true; break;
            }
        }
        if (!found) {
            fprintf(stderr, "purpleimage: --uefi requires installed x86_64-efi GRUB modules "
                    "(for example grub-efi-x86_64-bin); no ISO was created.\n");
            return 1;
        }
    }

    char tmpdir[] = "/tmp/purpleimage-XXXXXX";
    if (!mkdtemp(tmpdir)) die("mkdtemp failed");

    char root[PATH_MAX];
    snprintf(root, sizeof root, "%s/iso", tmpdir);
    char bootgrub[PATH_MAX];
    snprintf(bootgrub, sizeof bootgrub, "%s/boot/grub", root);

    /* All external commands now use execvp via run() – no shell, no injection */
    {
        const char *argv[] = { "mkdir", "-p", bootgrub, NULL };
        if (run(argv) != 0) { rmdir(tmpdir); die("mkdir failed"); }
    }

    char kdst[PATH_MAX];
    snprintf(kdst, sizeof kdst, "%s/boot/kernel.elf", root);
    {
        const char *argv[] = { "cp", kernel_path, kdst, NULL };
        if (run(argv) != 0) die("copy kernel failed");
    }

    const char *cfg =
        "set timeout=0\n"
        "set default=0\n"
        "insmod part_gpt\n"
        "insmod iso9660\n"
        "insmod multiboot2\n"
        "menuentry \"Purplebootcode\" {\n"
        "  multiboot2 /boot/kernel.elf\n"
        "  boot\n"
        "}\n";
    char cfgpath[PATH_MAX];
    snprintf(cfgpath, sizeof cfgpath, "%s/boot/grub/grub.cfg", root);
    if (write_file(cfgpath, (const uint8_t *)cfg, strlen(cfg)) != 0)
        die("write grub.cfg failed");

    for (int i = 0; i < ninj; i++) {
        char tdir[PATH_MAX];
        snprintf(tdir, sizeof tdir, "%s/%s", root, inj[i].dest);
        /* dirname of tdir */
        char *slash = strrchr(tdir, '/');
        if (slash) {
            *slash = 0;
            const char *argv[] = { "mkdir", "-p", tdir, NULL };
            run(argv);
            *slash = '/';
        }
        {
            const char *argv[] = { "cp", inj[i].src, tdir, NULL };
            if (run(argv) != 0) die("copy inject failed");
        }
    }

    int rc;
    {
        const char *argv[] = { grub, "-o", out_path, root, NULL };
        rc = run(argv);
    }

    /* cleanup – still safe because tmpdir is under our control */
    {
        const char *argv[] = { "rm", "-rf", tmpdir, NULL };
        run(argv);
    }

    if (rc != 0) {
        fprintf(stderr, "purpleimage: grub-mkrescue failed\n");
        return 1;
    }

    if (uefi) {
        printf("purpleimage: wrote hybrid BIOS/UEFI GRUB ISO %s with %d injected files\n",
               out_path, ninj);
        printf("UEFI path uses x86_64 GRUB EFI; validate the i386 Multiboot2 kernel on target firmware/emulator before deployment.\n");
    } else {
        printf("purpleimage: wrote bootable GRUB ISO %s with %d injected files\n",
               out_path, ninj);
    }
    return 0;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s --kernel KERNEL.elf (--img OUT.img | --iso OUT.iso) [options]\n"
        "\n"
        "  --kernel FILE     compiled Purplebc Multiboot ELF (required)\n"
        "  --img OUT.img     write a PBFIMG1 raw container\n"
        "  --iso OUT.iso     write a GRUB Multiboot2 ISO (needs grub-mkrescue + xorriso)\n"
        "  --inject SRC:DST  add a host file at path DST inside the image (repeatable)\n"
        "  --size-mib N      PBFIMG container size in MiB (default 64)\n"
        "  --uefi            for --iso, require x86_64-efi GRUB modules\n"
        "  -h, --help        this help\n",
        prog);
}

int main(int argc, char **argv) {
    const char *kernel = NULL;
    const char *out_img = NULL;
    const char *out_iso = NULL;
    int size_mib = 64;
    bool uefi = false;
    Inject *inj = NULL;
    int ninj = 0, cap = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]); return 0;
        } else if (!strcmp(argv[i], "--kernel") && i + 1 < argc) {
            kernel = argv[++i];
        } else if (!strcmp(argv[i], "--img") && i + 1 < argc) {
            out_img = argv[++i];
        } else if (!strcmp(argv[i], "--iso") && i + 1 < argc) {
            out_iso = argv[++i];
        } else if (!strcmp(argv[i], "--size-mib") && i + 1 < argc) {
            size_mib = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--uefi")) {
            uefi = true;
        } else if (!strcmp(argv[i], "--inject") && i + 1 < argc) {
            char *arg = argv[++i];
            char *colon = strchr(arg, ':');
            if (!colon) die("injection must be SOURCE:DEST");
            *colon = 0;
            if (ninj >= cap) {
                cap = cap ? cap * 2 : 4;
                inj = realloc(inj, cap * sizeof(Inject));
                if (!inj) die("oom");
            }
            inj[ninj].src = xstrdup(arg);
            char *dest = colon + 1;
            while (*dest == '/') dest++;
            if (strstr(dest, "..")) die("injection destination cannot contain ..");
            inj[ninj].dest = xstrdup(dest);
            ninj++;
        } else {
            fprintf(stderr, "purpleimage: unknown argument: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    if (!kernel) { fprintf(stderr, "purpleimage: --kernel is required\n"); return 2; }
    if (!out_img && !out_iso) { fprintf(stderr, "purpleimage: one of --img or --iso is required\n"); return 2; }
    if (out_img && out_iso) { fprintf(stderr, "purpleimage: --img and --iso are mutually exclusive\n"); return 2; }
    if (uefi && !out_iso) { fprintf(stderr, "purpleimage: --uefi is only valid with --iso\n"); return 2; }

    struct stat st;
    if (stat(kernel, &st) != 0 || !S_ISREG(st.st_mode))
        die("kernel not found: %s", kernel);

    int rc;
    if (out_img)
        rc = build_img(kernel, inj, ninj, size_mib, out_img);
    else
        rc = build_iso(kernel, inj, ninj, uefi, out_iso);

    for (int i = 0; i < ninj; i++) {
        free(inj[i].src);
        free(inj[i].dest);
    }
    free(inj);
    return rc;
}
