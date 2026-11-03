# Purplecode Language Reference

**Version:** 2.4  
**Status:** Working language and toolchains; freestanding Multiboot backend is intentionally smaller than the hosted surface.

This document describes the language as implemented. Where a feature exists only on a specific backend, that is stated explicitly. For the exact freestanding Multiboot surface, see `docs/BOOT-CODE-STATUS.md`.

---

## 1. Design

Purplecode is a practical systems language. It is intended for:

- Freestanding environments (Multiboot kernels, UEFI applications)
- Hosted Linux tools and programs
- Code that needs a direct mapping to machine concepts (pointers, explicit memory, hierarchical APIs)

It is not a memory-safe research language and does not hide the machine. Unsafe operations are explicit. Hierarchical names (`Screen.Write`, `Memory.Alloc`, `Array.New`) are preferred over cryptic abbreviations.

There are two primary compilation paths:

| Path | Driver | Output | Runtime |
|------|--------|--------|---------|
| Hosted | `purplec` | Linux executable (via GCC) | Host libc + Purple runtime |
| Freestanding Multiboot | `purplebc` | ELF32 Multiboot2 or flat binary | None (helpers emitted by the compiler) |
| UEFI | `purplebc --format uefi` | PE32+ EFI application | UEFI services (via host toolchain today) |

The freestanding Multiboot path emits machine code directly in C (`compiler/native_mb.c`). It does not invoke GCC for the target program.

---

## 2. Lexical structure

- Source is UTF-8 text.
- Comments: `//` line comments and `/* ... */` block comments.
- Identifiers: `[A-Za-z_][A-Za-z0-9_]*`. Hierarchical names use `.` (e.g. `Memory.Alloc`).
- Keywords include: `fn`, `let`, `if`, `else`, `while`, `for`, `return`, `true`, `false`.
- Integer literals: decimal or hexadecimal (`0x...`).
- String literals: double-quoted, with escapes `\n`, `\t`, `\\`, `\"`.
- Semicolons are optional at end of line; braces delimit blocks.

---

## 3. Types

| Type | Meaning |
|------|---------|
| `int` | Signed integer. Hosted path: 64-bit. Multiboot i386 path: 32-bit register width. |
| `bool` | Boolean; conditions use this (or integer comparisons). |
| `ptr` | Untyped pointer (freestanding path). |
| `string` | Pointer to null-terminated bytes (pointer assignment, not deep copy). |
| arrays | See §8 (hosted / fuller backends). |
| structs | See §9 (hosted path; milestone feature). |

The freestanding Multiboot backend currently treats values as machine words and does not run a full static type checker. The hosted path performs more checking during code generation.

---

## 4. Variables

```purple
let name: type = expression
let name = expression          // type inferred where the backend supports it
name = expression              // assignment
```

- `let` introduces a local.
- Uninitialised storage holds whatever was present (no automatic zeroing of arbitrary locals on the freestanding path).
- On the Multiboot native backend, up to 64 named locals are supported. Prefer straightforward boot-style programs.

---

## 5. Expressions and operators

Evaluation is left-to-right for equal precedence. Precedence follows common C-family order:

1. Primary (literals, names, calls, `(...)`)
2. Unary `+ - ! ~`
3. `* / %`
4. `+ -`
5. `<< >>`
6. `< <= > >=`
7. `== !=`
8. `&`  then  `^`  then  `|`
9. `&&`  then  `||`

Calls:

```purple
name(arg1, arg2)
Screen.Write("text")
Memory.Alloc(128)
```

---

## 6. Statements and control flow

```purple
// Declaration / assignment
let x: int = 0
x = x + 1

// If
if condition {
    // then
} else {
    // optional else
}

// While
while condition {
    // body
}

// Return
return expression
return
```

On the freestanding Multiboot backend, `if` supports comparisons against integer literals (including `0` and hex constants). `while true { ... }` is supported for halt loops.

---

## 7. Functions

```purple
fn name(param1, param2) {
    // body
    return value
}
```

- A program entry function named `main` is required.
- Nested functions are not supported.
- On Multiboot, `main` is invoked from a small compiler-generated entry stub that sets the stack and initialises runtime globals.

---

## 8. Arrays (hosted and fuller backends)

Dynamic arrays of integers are available on the hosted path:

```purple
let a = Array.New(10)     // length 10, zeroed
let n = Array.Len(a)
let v = Array.Get(a, i)
Array.Set(a, i, value)
let p = Array.Data(a)     // pointer to element 0
Array.Free(a)
```

Layout (hosted):

```
offset 0:  length
offset 8:  capacity
offset 16: elements...
```

Out-of-bounds `Get`/`Set` behaviour is defined by the runtime (returns 0 / -1 rather than trapping). Add explicit checks when bounds matter.

**Multiboot native backend:** `Array.*` is not implemented. Use `Memory.Alloc` and explicit loads/stores.

---

## 9. Structs (hosted path)

User-defined aggregate types exist on the hosted compilation path (milestone 1.7+). Field access and layout follow the hosted ABI. Freestanding Multiboot does not yet lower structs in the pure native emitter.

---

## 10. Hierarchical APIs

APIs are grouped by prefix. Availability depends on backend.

### 10.1 Freestanding Multiboot (`purplebc` default)

Implemented and tested:

| API | Role |
|-----|------|
| `Screen.Clear()` | Clear VGA text buffer, reset cursor |
| `Screen.Write(string)` | Write null-terminated string to VGA text (handles `\n`) |
| `Memory.Alloc(size)` | Bump allocator; returns pointer or 0 |
| `Memory.Free(ptr)` | No-op in current bump allocator |
| `Memory.HeapFree()` | Remaining bytes in heap region |
| `Memory.Write32(ptr, value)` | Store 32-bit word |
| `Memory.Read32(ptr)` | Load 32-bit word |
| `Memory.Write8(ptr, value)` | Store byte |
| `Memory.Read8(ptr)` | Load byte (zero-extended) |
| `System.Panic(string)` | Write message and halt |
| `CPU.Halt()` | CLI + HLT loop |

Entry stub initialises stack, VGA cursor state, and heap pointer before `main`.

### 10.2 UEFI

Text output, a subset of graphics (GOP), and limited block I/O are available when building with `--format uefi`. See `docs/BOOT-CODE-STATUS.md` and the UEFI examples.

### 10.3 Hosted

Additional libraries (GUI, arrays, shell printing, controlangs, etc.) are provided by the hosted runtime. They are not available in pure Multiboot output.

---

## 11. Memory model (freestanding)

On Multiboot i386:

- Kernel is loaded at `0x100000` (typical Multiboot contract).
- Compiler entry sets `esp` to a fixed stack region above the load address.
- Heap is a bump allocator over a fixed physical range (currently 2 MiB starting at `0x200000`).
- VGA text is at `0xB8000`.
- Small runtime globals (cursor, heap pointer, one local slot) live in low memory reserved by the entry code.

There is no paging setup, no GDT rebuild, and no interrupt handling in the minimal runtime. The program runs in the environment the Multiboot loader provides.

---

## 12. Compilation and tooling

```sh
# Build the tools
make clean all

# Hosted program
./purplec examples/hello.purple -o hello

# Freestanding Multiboot ELF (native emitter, no GCC for the target)
./purplebc examples/boot_hello.purplebc -o kernel.elf
./mbcheck --elf kernel.elf

# Bootable GRUB ISO
./purpleimage --kernel kernel.elf --iso boot.iso
qemu-system-i386 -cdrom boot.iso -serial stdio
```

- `purplebc` — freestanding / boot compiler
- `mbcheck` — Multiboot2 + ELF sanity check
- `purpleimage` — PBF image and GRUB ISO helper

QEMU’s `-kernel` option understands Multiboot1 only. Multiboot2 kernels should be booted via GRUB (ISO) or another Multiboot2-capable loader.

---

## 13. Error handling

- Hosted path: compile-time diagnostics for many type and syntax errors.
- Freestanding path: the native parser is intentionally small; unsupported constructs are skipped or rejected. Prefer the subset in `docs/BOOT-CODE-STATUS.md` for boot code.
- Runtime: `System.Panic` for unrecoverable freestanding failures. There are no exceptions.

---

## 14. Example (freestanding)

```purplebc
fn main() {
    Screen.Clear()
    Screen.Write("Hello from Purplebootcode!\n")
    Screen.Write("This code runs in kernel context.\n")
    let heap: ptr = Memory.Alloc(128)
    if heap == 0 {
        System.Panic("out of kernel heap")
    }
    Memory.Write32(heap, 0x50555250)
    let magic: int = Memory.Read32(heap)
    if magic == 0x50555250 {
        Screen.Write("Memory read/write passed.\n")
    }
    Memory.Free(heap)
    while true {
        CPU.Halt()
    }
    return 0
}
```

Build and run:

```sh
./purplebc examples/boot_hello.purplebc -o kernel.elf
./purpleimage --kernel kernel.elf --iso boot.iso
qemu-system-i386 -cdrom boot.iso -serial stdio
```

---

## 15. Relationship to C

Purplecode is not a C dialect. Similarities (operators, braces, explicit memory) are intentional for systems programmers. Differences include hierarchical APIs, a smaller freestanding surface, and a compiler that can emit Multiboot kernels without a separate assembler/linker step for that path.

A C programmer should expect to read `compiler/native_mb.c` when extending the freestanding backend. That file is the authoritative description of what the Multiboot path currently lowers.

---

## 16. Documentation map

| Document | Role |
|----------|------|
| `docs/LANGUAGE.md` | This reference |
| `docs/BLOWUI-GUIDE.md` | Hosted BlowUI usage, fonts and examples |
| `docs/BLOWUI-NET-FILES.md` | Compact hosted BlowUI, Net and File API reference |
| `docs/BOOT-CODE-STATUS.md` | Exact freestanding / UEFI capability matrix |
| `docs/NATIVE-BACKENDS.md` | Backend overview |
| `README.md` | Build, quick start, requirements |
| `docs/RELEASE-NOTES-*.md` | Historical milestone notes |

When the manual and the freestanding emitter disagree, the emitter and `BOOT-CODE-STATUS.md` take precedence for boot code.


## 17. Hosted BlowUI, fonts, encoding and HTTPS (2.1)

These APIs are available to hosted Linux programs compiled with `purplec`. They are not available to the freestanding Multiboot or UEFI targets.

### BlowUI

BlowUI is an immediate-mode UI API. Open a window with `Blow.Window(w,h,title)` or create a display-free framebuffer with `Blow.Headless(w,h)`. Each frame follows this order:

```purple
Blow.Poll()
Blow.Begin(0)
// draw widgets here
Blow.End()
```

`Blow.Poll()` returns `-1` when the window closes. Colors are `0xRRGGBB`. The available widgets, input calls, and exact signatures are documented in [`BLOWUI-NET-FILES.md`](BLOWUI-NET-FILES.md) and the step-by-step [`BLOWUI-GUIDE.md`](BLOWUI-GUIDE.md).

### Fonts and text styles

- Default BlowUI face: `FantasqueSansM Nerd Font Propo`, attempted when `Blow.Window` opens.
- Always-available fallback: `Builtin Emergency Font`, a softened 16×16 bitmap face.
- Optional named face: `Arial` (the runtime may use Liberation Sans as a compatible Linux substitute; Arial is not redistributed).
- `Blow.Font(name)` selects a named or registered face and returns a non-negative ID on success or `-1` on failure.
- `IFont(name,data,encoding)` registers embedded font data. `encoding` may be `Base64`, `B64`, `Hex`, or `Raw`. A four-argument form `IFont(name,data,length,encoding)` is also supported. Scalable rendering requires FreeType.
- `Blow.StyledLabel(x,y,text,style,color,scale)` accepts comma-separated `!Bold`, `!Italic`, `!Underline`, and `!Strikethru` modifiers. These modifiers currently draw with the emergency bitmap glyph renderer; normal `Blow.Label` text uses the selected scalable face when available.

### Encoding result global

`Binarify(text)`, `Hexify(text)`, and `Base64ify(text)` return their encoded string and update the hosted global `$LastHexResult`. `Encoding.LastResult()` reads the same value. The global is overwritten by the next encoding helper call.

### HTTPS

`Net.HttpsGet(host,path,outfile)` performs an HTTPS GET on port 443 using OpenSSL TLS 1.2 or newer, system CA-chain verification, and hostname verification. It returns the response body byte count or a negative value on failure. Use `"-"` as `outfile` to write the body to standard output. This is a simple GET helper, not a general TLS socket API.

For backend build requirements and current Wayland protocol limitations, see [`NATIVE-BACKENDS.md`](NATIVE-BACKENDS.md) and [`BLOWUI-GUIDE.md`](BLOWUI-GUIDE.md).
