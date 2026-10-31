# Native Backends Overview

| Backend | Driver | Code generation | Typical use |
|---------|--------|-----------------|-------------|
| Multiboot2 i386 | `purplebc` | Pure C emitter (`compiler/native_mb.c`) | Kernels, freestanding tests |
| Multiboot flat binary | `purplebc --format bin` | Same emitter | Loaders that want a raw image |
| UEFI x86-64 | `purplebc --format uefi` | Host Clang/LLD today | EFI applications |
| Hosted Linux | `purplec` | Assembly + GCC | Tools, GUI demos, full language |

The Multiboot path is intentionally self-contained: building `purplebc` needs a host C compiler; compiling a kernel with `purplebc` does not require GCC on the target path.

Capability detail: `docs/BOOT-CODE-STATUS.md`  
Language surface: `docs/LANGUAGE.md`

**Wayland compatibility note:** the current surface role uses the legacy `wl_shell` protocol. The backend only selects this protocol when the compositor advertises `wl_shell`; otherwise it falls back to X11 if available. Modern compositors that expose only `xdg-shell` are not yet supported by the native surface role.
