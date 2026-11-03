# Freestanding and Boot Backend Status

**Version:** 2.4 NextGen (NG)  
**Scope:** What the pure native Multiboot path (`purplebc` without `--gcchost`) and the UEFI path actually implement.

This file is the capability contract for boot code. It overrides aspirational sections of the language reference when they disagree.

---

## Multiboot2 native (`purplebc` default)

### Output

- ELF32, Multiboot2 header (`0xE85250D6`), end tag, 8-byte alignment
- Entry point set to compiler-generated stub (stack setup, runtime init, call `main`)
- Optional flat binary (`--format bin`) with Multiboot2 address tags
- Section headers include `.text`, `.rodata`, `.shstrtab` for tooling compatibility

### Runtime model

- 32-bit protected mode as provided by the Multiboot2 loader
- Stack placed by entry stub
- VGA text at `0xB8000` with software cursor
- Bump heap over a fixed physical range (2 MiB from `0x200000`)
- No paging setup, no IDT/GDT rebuild, no device drivers beyond VGA text and the heap

### Language subset accepted by the native emitter

| Construct | Status |
|-----------|--------|
| `fn main() { ... }` | Supported |
| `Screen.Clear()` | Supported |
| `Screen.Write("...")` | Supported (string literals; `\n` handled) |
| `let x: ptr = Memory.Alloc(n)` | Supported |
| `Memory.Free(p)` | Supported (no-op for bump allocator) |
| `Memory.HeapFree()` | Supported |
| `Memory.Write32(p, imm)` / `Memory.Write8` | Supported |
| `let v = Memory.Read32(p)` / `Read8` | Supported |
| Named locals (`let`, up to 64) | Supported |
| `let x: int = <integer>` | Supported |
| `if x == imm` / `if x != imm` | Supported |
| `else` branches | Supported |
| `System.Panic("...")` | Supported |
| `while true { CPU.Halt() }` | Supported |
| `return` | Accepted |
| Full expression grammar, structs, arrays | Not lowered — use hosted path |

### Verification

```sh
./purplebc examples/boot_hello.purplebc -o kernel.elf
./mbcheck --elf kernel.elf
./purpleimage --kernel kernel.elf --iso boot.iso
qemu-system-i386 -cdrom boot.iso -serial stdio
```

Expected VGA output for `examples/boot_hello.purplebc`:

```
Hello from Purplebootcode!
This code runs in kernel context.
Memory read/write passed.
```

### Loader note

QEMU `-kernel` implements Multiboot1 only. Use GRUB (`purpleimage --iso`) or another Multiboot2 loader for these kernels.

---

## UEFI (`purplebc --format uefi`)

- Produces PE32+ EFI applications via the host toolchain (`--gcchost` / Clang + lld-link workflow)
- Text console, subset of GOP graphics, limited Block I/O inspection
- Not a pure Purplecode-owned x86-64 emitter; code generation relies on the host compiler for this target
- See UEFI examples under `examples/uefi_*.purplebc`

Firmware behaviour varies. A successful build does not guarantee every UEFI implementation will run the image.

---

## Hosted (`purplec`)

- Fuller language surface (arrays, structs, GUI libraries, etc.)
- Depends on GCC and the hosted runtime
- Not freestanding

---

## Integrity rules for this document

1. If a feature is not listed as supported for Multiboot native above, boot code must not rely on it.
2. Changes to the native emitter must update this file in the same change.
3. Release notes record history; this file records present capability.
