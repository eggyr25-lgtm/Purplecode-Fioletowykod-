# Purplecode Milestone 1 1.4 — backend notes

This archive remains **Milestone 1 1.4**. It is not renamed to 1.5.

## Native Multiboot i386 (complete for supported language subset)

- Full parser, stack-frame codegen, control flow, expressions.
- Freestanding builtins: Screen, CPU, Memory (Alloc/Free/Read/Write/Set/Copy/Heap), Port, Disk.ReadSector/WriteSector (ATA stubs), System.Panic.
- `purplebc --native` → Multiboot ELF32, zero external toolchain calls.
- `examples/boot_hello.purplebc` fully native.

## Native UEFI x86-64 PE32+ (new)

- `compiler/native/pe32.py` — pure-Python PE32+ writer, subsystem EFI_APPLICATION.
- `compiler/native/x86_64.py` — x86-64 instruction encoder (Microsoft x64 ABI).
- `compiler/native/uefi_emit.py` — UEFI codegen using EFI System Table ConOut for Screen.Clear/Write.
- `purplebc --native --format uefi` → PE32+ EFI application, **no Clang, no lld-link**.
- `examples/uefi_hello.purplebc` fully native.

## Tests

- PATH-poisoned regression tests for both native paths (gcc/clang/lld-link/objcopy stubs exit 99).

## Still expanding

- Full GOP / Input / Disk hardware paths on UEFI.
- Richer register allocation (current paths are stack-based).
- Every hierarchical API on every target.

No claim that QEMU/OVMF was run in this environment.
