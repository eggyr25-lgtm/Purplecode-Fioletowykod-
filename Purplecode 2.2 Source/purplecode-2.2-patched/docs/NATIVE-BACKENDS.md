# Native Backends Overview

| Backend | Driver | Code generation | Typical use |
|---------|--------|-----------------|-------------|
| Multiboot2 i386 | `purplebc` | Pure C emitter (`compiler/native_mb.c`) | Kernels, freestanding tests |
| Multiboot flat binary | `purplebc --format bin` | Same emitter | Loaders that want a raw image |
| UEFI x86-64 | `purplebc --format uefi` | Host Clang/LLD today | EFI applications |
| Linux ELF64 (default) | `purplec` | Internal emitter (`compiler/elfemit.c`), no GCC | Command-line tools, structs, arrays, recursion |
| Hosted Linux (fallback) | `purplec` | Assembly + GCC runtime | GUI/BlowUI, HTTPS, Web/JS, images, fonts, networking, `File.*` |

## `purplec` GoodNative Emitter (2.2)

`purplec file.purple` tries the **GoodNative Emitter** first (`compiler/elfemit.c` + the Purplecode-source runtime in `compiler/prelude.pc`) and uses it whenever the whole program fits. Otherwise it says why and falls back to assembly + the GCC runtime. `--direct` (alias `--goodnative`) never falls back; `--gcc` never tries the GoodNative Emitter; `purplec --version` prints the version. The hosted type checker runs first in both cases, so type errors are reported the same way.

Supported by the GoodNative Emitter (no GCC, no libc):

- **Language:** functions with parameters/returns and recursion; int/bool/string; `if`/`while`; all integer operators; structs (stack, `Struct.New` heap, nested fields, `Type.method` calls); string escapes (`\n \r \t \\ \" \xHH`, octal).
- **Memory:** `Memory.Alloc/Free` and `System.Memory.*` (mmap-backed, zeroed), `Memory.Read8/16/32/64`, `Memory.Write8/16/32/64`, `raw $addr = v`, `Struct.*`, `StructArray.New/At/Free`, `Array.New/Len/Get/Set/Data/Free` (same `[len][cap][data]` layout as the hosted runtime; out-of-bounds reads give 0).
- **Files:** `File.Read/Write/Append/Copy/Rename/Delete/Mkdir/Exists/IsDir/Size/Count/Name/Print` (directory listings sorted like the hosted runtime), plus `FS.Exists/Size/Data.Write`.
- **Network (IPv4):** `Net.Connect/Listen/Accept/AcceptTimeout/Send/SendBuf/Recv/RecvTimeout/RecvLine/Close`, `Net.UdpOpen/UdpSend/UdpRecv/Peer`, `Net.HttpGet/HttpPost/Status`, `Net.HttpServe`, `Net.Resolve/HostName/Print`. Host names are resolved by the compiler's own resolver: dotted IPv4, `localhost`, `/etc/hosts`, then a UDP DNS query (A records) to the first `nameserver` in `/etc/resolv.conf`. No IPv6/AAAA, no `/etc/nsswitch.conf` or mDNS lookups.
- **Encoding / output:** `Binarify`, `Hexify`, `Base64ify`, `$LastHexResult`, `Encoding.LastResult()`, `Data.ShellPrint[.Int/.Hex]`.
- **BlowUI and GUI (new in 2.2):** every `Blow.*` call except scalable fonts (`Blow.Window/Headless/Theme/Poll/Begin/End`, `Panel/Label/Muted/LabelInt/StyledLabel/Badge/Button/Toggle/Checkbox/Slider/Progress/TextBox/RRect/Circle/Divider`, mouse/key/time helpers, `Screenshot`, `SimClick`) and `GUI.Window.CreateSized`, `GUI.Clear/Draw.Rect/DrawLine/DrawText/DrawChar/Present/PollKey/Pump/GetWidth/GetHeight`, `Blit.*`. Rendering is an integer/fixed-point software renderer written in Purplecode with the built-in bitmap font (the "Builtin Emergency Font"); windows are real X11 windows (or XWayland) driven over the raw X11 socket protocol, including mouse, keyboard and the window-close button. Headless mode and screenshots (PPM) need no display at all. `Blow.Font` accepts only `"Builtin Emergency Font"`; FreeType faces, `IFont` and native Wayland (`wl_shell`) are not ported.
- **System:** `System.Process.ID/Time.Unix/Exit`, `LinuxAPI.Call`/`Kernel.Call`, `Web.JS.Load`.

The network and file functions are written in Purplecode itself (`compiler/prelude.pc`, regenerate `compiler/prelude.c` with `python3 tools/genprelude.py`) on top of raw syscalls, and only the parts a program reaches are linked in.

Still on the GCC runtime fallback (the compiler prints which call caused it): `Net.HttpsGet` (TLS is deliberately not hand-rolled), `Image.*` (PNG/JPEG decoders), `Blow.Font` with FreeType faces and `IFont`, native Wayland windows, `Web.JS.Run` / `Web.HTML.Load` (spawn `node` / `xdg-open`), `Controlang.*` (dynamic loading), `PurpleAPI.Call`, `Program.ELF64.*` (SafeCodeOnly/Virtual Mode policy), `CD.*`/`USB.*`, `GUI.NativeWindow` and `Kustom.GUI.MakeWindow`.

The Multiboot path is intentionally self-contained: building `purplebc` needs a host C compiler; compiling a kernel with `purplebc` does not require GCC on the target path.

Capability detail: `docs/BOOT-CODE-STATUS.md`  
Language surface: `docs/LANGUAGE.md`

**Wayland compatibility note:** the surface role prefers modern **xdg-shell** (`xdg_wm_base` + `xdg_surface` + `xdg_toplevel`, embedded protocol tables – no wayland-protocols dependency). Classic `wl_shell` is used only when `xdg_wm_base` is absent. If neither is available the backend falls back to X11 when present.
