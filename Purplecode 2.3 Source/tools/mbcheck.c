/*
 * mbcheck – pure C Multiboot2 header validator for Purplebootcode.
 * No Python.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

static int check_elf(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 64) { fclose(f); fprintf(stderr, "file too small\n"); return 1; }
    uint8_t *data = malloc((size_t)sz);
    if (!data || fread(data, 1, (size_t)sz, f) != (size_t)sz) {
        free(data); fclose(f); return 1;
    }
    fclose(f);

    if (memcmp(data,  "\x7f" "ELF", 4) != 0) {
        fprintf(stderr, "not ELF\n"); free(data); return 1;
    }
    if (data[4] != 1) { fprintf(stderr, "not ELF32\n"); free(data); return 1; }

    /* Find Multiboot2 magic in first 32 KiB, 8-byte aligned */
    const uint32_t magic = 0xE85250D6u;
    int found = -1;
    size_t limit = (size_t)sz < 32768 ? (size_t)sz : 32768;
    for (size_t off = 0; off + 24 <= limit; off += 8) {
        uint32_t m;
        memcpy(&m, data + off, 4);
        if (m == magic) { found = (int)off; break; }
    }
    if (found < 0) {
        fprintf(stderr, "Multiboot2 header not found in first 32 KiB\n");
        free(data); return 1;
    }

    uint32_t arch, length, checksum;
    memcpy(&arch,     data + found + 4, 4);
    memcpy(&length,   data + found + 8, 4);
    memcpy(&checksum, data + found + 12, 4);

    if (arch != 0) {
        fprintf(stderr, "unexpected architecture %u (want 0 = i386)\n", arch);
        free(data); return 1;
    }
    if (length != 24) {
        fprintf(stderr, "header length %u (want 24 for minimal ELF form)\n", length);
        free(data); return 1;
    }
    if (((magic + arch + length + checksum) & 0xffffffffu) != 0) {
        fprintf(stderr, "bad Multiboot2 checksum\n");
        free(data); return 1;
    }

    /* End tag must follow immediately */
    uint16_t typ, flags;
    uint32_t size;
    memcpy(&typ,   data + found + 16, 2);
    memcpy(&flags, data + found + 18, 2);
    memcpy(&size,  data + found + 20, 4);
    if (typ != 0 || flags != 0 || size != 8) {
        fprintf(stderr, "missing or invalid Multiboot2 end tag\n");
        free(data); return 1;
    }

    /* Basic ELF entry sanity */
    uint32_t e_entry;
    memcpy(&e_entry, data + 24, 4);
    if (e_entry < 0x100000 || e_entry > 0x110000) {
        fprintf(stderr, "suspicious e_entry 0x%x\n", e_entry);
        free(data); return 1;
    }

    printf("ELF Multiboot2 header OK (offset %d, entry 0x%x)\n", found, e_entry);
    free(data);
    return 0;
}

static int check_flat(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    uint8_t hdr[128];
    size_t n = fread(hdr, 1, sizeof hdr, f);
    fclose(f);
    if (n < 64) { fprintf(stderr, "flat binary too small\n"); return 1; }

    uint32_t magic, arch, length, checksum;
    memcpy(&magic,    hdr + 0, 4);
    memcpy(&arch,     hdr + 4, 4);
    memcpy(&length,   hdr + 8, 4);
    memcpy(&checksum, hdr + 12, 4);

    if (magic != 0xE85250D6u || arch != 0) {
        fprintf(stderr, "bad Multiboot2 magic/arch\n"); return 1;
    }
    if (length != 64 || ((magic + arch + length + checksum) & 0xffffffffu) != 0) {
        fprintf(stderr, "bad Multiboot2 length/checksum\n"); return 1;
    }

    /* Expect tags: address (2), entry-address (3), end (0) */
    size_t off = 16;
    int saw_addr = 0, saw_entry = 0, saw_end = 0;
    while (off + 8 <= length) {
        uint16_t typ, flags;
        uint32_t size;
        memcpy(&typ,   hdr + off, 2);
        memcpy(&flags, hdr + off + 2, 2);
        memcpy(&size,  hdr + off + 4, 4);
        if (size < 8 || off + size > length) {
            fprintf(stderr, "invalid tag size at %zu\n", off); return 1;
        }
        if (typ == 2 && size == 24) saw_addr = 1;
        else if (typ == 3 && size == 12) saw_entry = 1;
        else if (typ == 0 && flags == 0 && size == 8) { saw_end = 1; break; }
        off += (size + 7) & ~7u;
    }
    if (!saw_addr || !saw_entry || !saw_end) {
        fprintf(stderr, "flat binary missing required Multiboot2 tags\n"); return 1;
    }
    printf("Flat Multiboot2 header OK\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s --elf|--bin FILE\n", argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "--elf")) return check_elf(argv[2]);
    if (!strcmp(argv[1], "--bin")) return check_flat(argv[2]);
    fprintf(stderr, "unknown mode %s\n", argv[1]);
    return 2;
}
