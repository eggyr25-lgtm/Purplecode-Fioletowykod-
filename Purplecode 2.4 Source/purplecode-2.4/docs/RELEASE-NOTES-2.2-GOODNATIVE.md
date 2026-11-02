# Purplecode 2.2 "GoodNative Emitter"

The internal ELF64 emitter that shipped as `--direct` is now the default `purplec` backend and is called the **GoodNative Emitter**.

## New in 2.2
- Default backend for `purplec`; GCC is only an automatic fallback (`--gcc` forces it, `--direct`/`--goodnative` forbids it, `--version` prints the release name).
- Language: user functions with recursion, structs (stack/heap/nested/methods), arrays, `StructArray.*`, string escapes, loop-carried register allocation fix.
- Runtime written in Purplecode on raw syscalls (`compiler/prelude.pc`, regenerate with `tools/genprelude.py`): `File.*`, `Net.*` (TCP, UDP, HTTP client/server, DNS resolver), encoders, `Data.ShellPrint.Hex`, `Web.JS.Load`.
- BlowUI and GUI: integer software renderer with the built-in bitmap font, real X11 windows over the raw socket protocol (mouse, keyboard, close button), headless mode and PPM screenshots.

## Expanded pure coverage (no longer forces GCC fallback)
- `Net.HttpsGet(host,path,outfile)` — real HTTPS via system `curl` (system TLS/CA store)
- `Image.Load` / `Image.Show` — native PPM; PNG/JPEG via ImageMagick `convert` → PPM + feh/display
- `Web.JS.Run` / `Web.HTML.Load` — real fork+execve of node / xdg-open
- `Program.ELF64.Open/Run/Close` — ELF64 magic check + chmod + execve in a child
- `CD.*` / `USB.*` — pread of device nodes
- `GUI.NativeWindow` / `Kustom.GUI.MakeWindow` / `Blow.Decorationless.Window` — pure X11 Blow
- `Controlang.Native.Register` — records name/lib/sym; `Controlang.Call` still needs `--gcc` for in-process dlopen (use `Program.ELF64.Run` for out-of-process plugins)
- `IFont` / `Blow.IFont` — accepted as no-ops; drawing stays on emergency font without FreeType

## Still richer under `--gcc`
In-process OpenSSL (no curl), in-process PNG/JPEG decoders, FreeType face rasterization, native Wayland client, `dlopen` Controlang plugins.

## Known limits
- IPv4 only; no AAAA records.
- HTTP responses are limited to 64 MB; directory listings to about 256 KB of entries.
- The hosted type checker still rejects struct-typed function parameters in default mode; `--direct` accepts them.
