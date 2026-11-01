# Purplecode 2.1 (hosted `purplec`)

- **BlowUI** – new built-in UI library: anti-aliased rounded widgets, 3 themes, soft shadows, mouse + typing. Pure C on the software blitter, no GTK.
- **Net.\*** – TCP client/server, UDP (incl. broadcast), DNS, HTTP client (GET/POST) and a static HTTP file server.
- **File.\*** – read, write, append, copy, rename, delete, mkdir, directory listing.
- Compiler: any `Blow.*`, `Net.*`, `File.*` call maps to `purple_<name>()` in the runtime (up to 6 args, stack kept 16-byte aligned).
- New runtime modules: `runtime/purple_blow.inc`, `purple_net.inc`, `purple_files.inc` (included by `purple_rt.c`; `make install` copies them).
- Boot path (`purplebc`) is unchanged: these APIs are hosted-only.

Limits: plain HTTP only (no TLS/HTTPS, no redirects); BlowUI mouse/keyboard need X11 (XWayland works), font is the 8x8 bitmap font (scale 1-6); buffers passed to `Net.Recv*`, `File.Read`, `Net.Resolve` must be `max+1` bytes; TextBox buffers are 256 bytes. Buffers are `int` variables: `let buf: int = System.Memory.Alloc(256)` (zeroed).
