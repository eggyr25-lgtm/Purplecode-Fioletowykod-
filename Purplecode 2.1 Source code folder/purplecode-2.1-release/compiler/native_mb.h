#ifndef PURPLE_NATIVE_MB_H
#define PURPLE_NATIVE_MB_H

#include <stddef.h>
#include <stdint.h>

/*
 * Pure C Multiboot i386 native emitter.
 * Produces a Multiboot2-compatible ELF32 or a flat binary.
 * No GCC, no assembler, no external toolchain.
 *
 * Returns 0 on success. On failure returns non-zero and sets *err_out
 * to a static or heap message (caller must not free unless documented).
 */
int native_mb_compile(
    const char *source,
    const char *input_name,
    int flat_binary,          /* 1 = raw binary, 0 = ELF32 Multiboot */
    uint8_t **out_blob,
    size_t *out_len,
    const char **err_out
);

#endif
