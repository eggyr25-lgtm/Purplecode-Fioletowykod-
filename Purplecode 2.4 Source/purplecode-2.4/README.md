# Purplecode 2.4 – NextGen (NG)

Purplecode is a systems language with separate hosted and freestanding toolchains.

| Target | Driver | Role |
|--------|--------|------|
| Hosted Linux | `purplec` | Ordinary programs and the fuller language surface |
| Freestanding Multiboot | `purplebc` | Kernels and boot code (native machine-code emitter) |
| UEFI | `purplebc --format uefi` | PE32+ EFI applications |

Hosted APIs are not automatically available in boot code. The freestanding Multiboot surface is documented in `docs/BOOT-CODE-STATUS.md`.

## Documentation

| Document | Contents |
|----------|----------|
| [`docs/LANGUAGE.md`](docs/LANGUAGE.md) | Language reference |
| [`docs/BOOT-CODE-STATUS.md`](docs/BOOT-CODE-STATUS.md) | Exact Multiboot / UEFI capability contract |
| [`docs/NATIVE-BACKENDS.md`](docs/NATIVE-BACKENDS.md) | Backend overview |
| [`docs/BLOWUI-GUIDE.md`](docs/BLOWUI-GUIDE.md) | BlowUI tutorial, widgets, fonts and examples |
| [`docs/BLOWUI-NET-FILES.md`](docs/BLOWUI-NET-FILES.md) | Compact BlowUI, Net and File reference (2.1) |
| [`docs/RELEASE-NOTES-2.1-BLOWUI.md`](docs/RELEASE-NOTES-2.1-BLOWUI.md) | BlowUI fonts, Wayland, HTTPS and encoding updates |
| [`docs/RELEASE-NOTES-2.4.md`](docs/RELEASE-NOTES-2.4.md) | 2.4 security hardening (image builder + Net runtime) |

## Highlights carried forward from 2.0 NextGen (NG)

- `purplebc` is a C binary; Multiboot code generation does not require GCC for the target
- Multiboot2 ELF with correct section headers and entry point
- `purpleimage` and `mbcheck` are pure C
- Working freestanding example: VGA text, bump heap, memory read/write
- Multiboot native path: up to **64** named locals (was 8) and **256** string literals
- Native emitter diagnostics now report `file:line:column` with clearer messages


## Purplecode 2.4 hosted additions

- Native Wayland pointer and keyboard event integration for BlowUI, with X11 fallback
- Default font attempt: FantasqueSansM Nerd Font Propo; softened 16×16 Builtin Emergency Font fallback; optional Arial-compatible system face and embedded `IFont` faces (FreeType required for scalable rendering)
- Combinable `Blow.StyledLabel` modifiers: `!Bold`, `!Italic`, `!Underline`, `!Strikethru`
- `Binarify`, `Hexify`, `Base64ify`, and `$LastHexResult`
- `Net.HttpsGet` with OpenSSL TLS 1.2+, CA-chain and hostname verification

See [`docs/BLOWUI-GUIDE.md`](docs/BLOWUI-GUIDE.md) for complete usage instructions and [`docs/RELEASE-NOTES-2.1-BLOWUI.md`](docs/RELEASE-NOTES-2.1-BLOWUI.md) for backend and dependency details.

## Quick start

```sh
make all

# Hosted
./purplec examples/hello.purple -o hello && ./hello

# Boot / Multiboot – native by default, pure C emitter, no GCC
./purplebc examples/boot_hello.purplebc -o kernel.elf
./purplebc examples/boot_hello.purplebc --format bin -o kernel.bin
# Flat binary includes Multiboot2 address and entry-address tags.
# Boot it with a Multiboot2-aware loader (e.g. GRUB's multiboot2 command).

# UEFI via host toolchain when you want it
./purplebc examples/uefi_hello.purplebc --format uefi --gcchost -o BOOTX64.EFI
```

## 1. Requirements

- Hosted (`purplec`): GCC, make, Linux, OpenSSL development libraries (`libssl`/`libcrypto`). Optional: X11 or Wayland development files for GUI, and FreeType development files for scalable fonts.
- Multiboot native (`purplebc` default): **nothing beyond a C compiler to build purplebc itself**. No GCC at compile-of-kernel time.
- Multiboot / UEFI `--gcchost`: GCC (32-bit) or Clang + lld-link as appropriate.
- QEMU + OVMF for UEFI testing.

On Arch/CachyOS, typical packages are:

```sh
sudo pacman -S --needed base-devel clang lld qemu-system-x86 edk2-ovmf
```

Package names may differ on other distributions.

## 2. Build and verify

Run from the extracted project directory:

```sh
make clean all
make test
```

`make test` runs hosted/compiler regression tests and boot-target tests. UEFI tests are skipped if `clang` or `lld-link` is missing. A passing build does **not** prove the application works on every firmware implementation.

Check the UEFI toolchain before building:

```sh
command -v clang
command -v lld-link
```

## 3. Your first UEFI application

Source: `examples/uefi_hello.purplebc`

```purplebc
fn main() {
    Screen.Clear()
    Screen.Write("Hello from Purplebootcode UEFI!\n")
    Screen.Write("This is a PE/COFF EFI application.\n")
    return 0
}
```

Build the EFI application:

```sh
./purplebc examples/uefi_hello.purplebc --format uefi -o BOOTX64.EFI
file BOOTX64.EFI
```

The output should identify a **PE32+ executable for EFI (application), x86-64**. The output is not an ELF file with its extension changed.

For removable-media testing, the standard x86-64 UEFI fallback path is `EFI/BOOT/BOOTX64.EFI`. Put the file at that path on a FAT-formatted EFI-readable partition, or use the QEMU/OVMF workflow below. Do not format a drive or write to a physical disk unless you have checked the target device carefully.

## 4. Test with QEMU + OVMF

First locate the firmware files installed by your distribution:

```sh
find /usr/share/edk2 /usr/share/OVMF -type f \( -iname '*OVMF*CODE*.fd' -o -iname '*OVMF*VARS*.fd' \) 2>/dev/null
```

Create a disposable FAT directory tree and image (requires `mtools` and `dosfstools`):

```sh
mkdir -p build/esp/EFI/BOOT
cp BOOTX64.EFI build/esp/EFI/BOOT/BOOTX64.EFI
truncate -s 64M build/esp.img
mkfs.vfat build/esp.img
mcopy -i build/esp.img -s build/esp/EFI ::
```

Then launch QEMU using your installed OVMF CODE and a **copy** of its VARS file. Exact paths vary by distro:

```sh
cp /path/to/OVMF_VARS.fd build/OVMF_VARS.fd
qemu-system-x86_64 \
  -machine q35 -m 256M \
  -drive if=pflash,format=raw,readonly=on,file=/path/to/OVMF_CODE.fd \
  -drive if=pflash,format=raw,file=build/OVMF_VARS.fd \
  -drive format=raw,file=fat:rw:build/esp
```

This directory-backed FAT drive is convenient for development. If your QEMU build does not support it, use `build/esp.img` as a raw drive instead. Firmware menus differ; choose the UEFI QEMU hard drive entry. Close QEMU to stop the test. Never use a live disk image as your writable VARS file.

## 5. Purplebootcode API

Only the API names implemented by the selected target are legal. Unknown and hosted-only APIs fail validation rather than generating a knowingly broken kernel.

### UEFI x86-64 target

| API | Meaning |
|---|---|
| `Screen.Write("text")` | Write ASCII-compatible text using UEFI console output; `\n` is translated to CR/LF. |
| `Screen.WriteBuffer(ptr)` | Treat `ptr` as a NUL-terminated string pointer. |
| `Screen.Clear()` | Clear the firmware text console. |
| `GUI.Text("text")`, `GUI.WriteText("text")` | Text-output aliases. |
| `GUI.Clear()` | Clear-screen alias. |
| `Kernel.Call(1, ptr, 0)` | Text-output compatibility service. |
| `Kernel.Call(2, 0, 0)` | Clear-screen compatibility service. |
| `System.Panic("reason")` | Print a panic message and stop. |
| `CPU.Halt()` | Current implementation pauses in a loop; it is not a power-management operation. |

**Important limitations:** This target has basic GOP pixel/fill operations, non-blocking console key reads, and read-only firmware Block I/O helpers, but it does not provide a full graphics/input event layer, general-purpose memory allocator, raw port I/O, writable filesystem, or VFS. The boot-code validator rejects unsupported services. Do not assume `FS.*`, `Memory.*`, `CD.*`, or `USB.*` calls work in UEFI merely because similarly named APIs exist for another target.

### Multiboot / flat binary target

The `elf` target creates a freestanding i386 Multiboot2 ELF kernel and the `bin` target creates a flat binary. The default path uses the internal Multiboot emitter for its documented subset. These are not UEFI applications. Current documented boot APIs include text output, basic memory operations, port I/O, ATA sector access, and ISO-9660 helpers where implemented by the selected boot path. Hardware access depends on the boot environment and must be tested.

## 6. Make a UEFI app that checks a condition

```purplebc
fn main() {
    Screen.Clear()
    Screen.Write("Purplebootcode diagnostics\n")
    let status: int = 0
    if status == 0 {
        Screen.Write("[ OK ] startup reached\n")
    } else {
        Screen.Write("[FAIL] startup status nonzero\n")
        System.Panic("startup failed")
    }
    Screen.Write("End of diagnostics.\n")
    while true {
        CPU.Halt()
    }
    return 0
}
```

Build it with:

```sh
./purplebc examples/uefi_diagnostics.purplebc --format uefi -o build/BOOTX64.EFI
```

The example intentionally uses only APIs available on the UEFI target.

## 7. Purplebootcode validation and common errors

```sh
./purplebc --check examples/uefi_hello.purplebc
```

- `ERROR: ATTEMPTED TO USE USERLAND FUNCTION IN BOOT CODE`: a hosted-only or unknown API was used. Replace it with a documented boot API for your target.
- `UEFI target does not yet implement boot service for ...`: syntax may be valid, but that firmware service is not implemented in the UEFI backend.
- `clang` / `lld-link` missing: install the UEFI toolchain and retry.
- Image builds but does not appear in firmware: check the architecture, FAT filesystem, fallback path `EFI/BOOT/BOOTX64.EFI`, firmware boot menu, and QEMU firmware paths.

## 8. Hosted quick start (recommended first try)

Start with hosted Purplecode before setting up firmware tools. On Linux with GCC and Python 3 installed:

```sh
make all
./purplec examples/hello.purple -o hello
./hello
```

`purplec` produces a native executable by default and links the runtime automatically. It tries available link recipes (X11 plus image libraries, reduced optional-library combinations, then a minimal runtime). GUI programs need a working display backend; if the optional X11 development files are absent, GUI calls report that no display backend is available. For assembly output only, use `./purplec examples/hello.purple -S -o hello.s`.

Explore:
- `examples/` — small programs and boot examples.
- `docs/LANGUAGE.md` — hosted language syntax and runtime API.
- `docs/BOOT-CODE-STATUS.md` — boot-target feature status and limitations.
- `docs/NATIVE-BACKENDS.md` — native backend architecture and register-allocation scope.
- `boot-tests.sh` — boot compiler and artifact regression checks.
- `tests.sh` — hosted compiler/runtime tests.

## 9. Release honesty

The UEFI implementation includes GOP framebuffer drawing and firmware console keyboard input. Start with `examples/uefi_gop_demo.purplebc` and `docs/BOOT-CODE-STATUS.md`. These are firmware protocol clients, not native GPU/USB drivers. Native PCI, USB host-controller/HID, NVMe/AHCI, network, audio and GPU drivers, UEFI filesystem/Block I/O access, a scheduler and a VFS remain unimplemented. UEFI currently uses Clang/LLD for machine-code generation; there is no internal UEFI emitter yet. Build and artifact tests do not replace visual testing in QEMU/OVMF.


## UEFI graphics and input

The UEFI target now provides `GUI.Width()`, `GUI.Height()`, `GUI.Pixel(x, y, color)`, and `GUI.FillRect(x, y, width, height, color)`. Colors use `0xRRGGBB`. `Input.ReadKey()` performs a non-blocking read through UEFI Simple Text Input. The read-only `Disk.BlockSize()`, `Disk.BlockCount()`, and `Disk.ReadByte(lba, offset)` APIs access firmware-provided Block I/O devices. See `examples/uefi_gop_demo.purplebc` for a complete example.

```sh
mkdir -p build
./purplebc examples/uefi_gop_demo.purplebc --format uefi -o build/BOOTX64.EFI
file build/BOOTX64.EFI
```

Copy the artifact to `EFI/BOOT/BOOTX64.EFI` on a FAT EFI System Partition or test with QEMU + OVMF. The demo paints the active framebuffer and does not restore the previous screen. Details and explicit hardware-driver boundaries are in `docs/BOOT-CODE-STATUS.md`.


**Verification note:** Run `make clean all` and `make test` on your own machine to verify this checkout and its available toolchains. Artifact tests check file formats and compiler behavior; they do not prove execution on real firmware. QEMU/OVMF visual and storage testing is still required.

## Native backend status

`purplebc` uses Purplecode's own machine-code emitter by default for Multiboot (no GCC involved); `--native` is accepted as a no-op. `--gcchost` opts into the C-translation path (GCC/binutils for Multiboot, Clang/`lld-link` for UEFI). There is no internal UEFI emitter yet, so `--format uefi` currently uses the Clang/LLD path automatically. `purplec` now compiles with the **2.2 GoodNative Emitter** by default and needs no GCC or libc for programs it supports: user functions and recursion, structs, arrays, raw memory, `File.*`, `Net.*` (TCP, UDP, HTTP client and server, DNS), the encoders, the syscall APIs, and BlowUI/GUI windows (software renderer plus X11 over a raw socket). When a program uses something the internal emitter cannot lower yet (HTTPS, images, FreeType fonts, `Web.JS.Run`/`Web.HTML.Load`, Controlang, `PurpleAPI.Call`, `Program.ELF64.*`, `CD.*`/`USB.*`), `purplec` prints the reason and falls back to assembly plus the GCC runtime. Use `--direct` (or `--goodnative`) to require the GoodNative Emitter (errors instead of falling back) or `--gcc` to force the GCC runtime path. See [`docs/NATIVE-BACKENDS.md`](docs/NATIVE-BACKENDS.md) and [`docs/BOOT-CODE-STATUS.md`](docs/BOOT-CODE-STATUS.md) for the boundaries.

