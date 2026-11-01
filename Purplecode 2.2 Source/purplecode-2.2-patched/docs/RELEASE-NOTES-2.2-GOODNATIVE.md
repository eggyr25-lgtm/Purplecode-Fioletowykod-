# Purplecode 2.2 "GoodNative Emitter"

The internal ELF64 emitter that shipped as `--direct` is now the default `purplec` backend and is called the **GoodNative Emitter**.

## New in 2.2
- Default backend for `purplec`; GCC is only an automatic fallback (`--gcc` forces it, `--direct`/`--goodnative` forbids it, `--version` prints the release name).
- Language: user functions with recursion, structs (stack/heap/nested/methods), arrays, `StructArray.*`, string escapes, loop-carried register allocation fix.
- Runtime written in Purplecode on raw syscalls (`compiler/prelude.pc`, regenerate with `tools/genprelude.py`): `File.*`, `Net.*` (TCP, UDP, HTTP client/server, DNS resolver), encoders, `Data.ShellPrint.Hex`, `Web.JS.Load`.
- BlowUI and GUI: integer software renderer with the built-in bitmap font, real X11 windows over the raw socket protocol (mouse, keyboard, close button), headless mode and PPM screenshots.

## Not in the GoodNative Emitter (automatic GCC fallback)
HTTPS/TLS, PNG/JPEG `Image.*`, FreeType fonts and `IFont`, native Wayland windows, `Web.JS.Run`/`Web.HTML.Load`, Controlang, `PurpleAPI.Call`, `Program.ELF64.*`, `CD.*`/`USB.*`.

## Known limits
- IPv4 only; no AAAA records.
- HTTP responses are limited to 64 MB; directory listings to about 256 KB of entries.
- The hosted type checker still rejects struct-typed function parameters in default mode; `--direct` accepts them.
