# Purplecode 2.1 — BlowUI, fonts, encoding and HTTPS

This is the BlowUI feature supplement to [`RELEASE-NOTES-2.1.md`](RELEASE-NOTES-2.1.md). The project release version is **2.1**.

## BlowUI

- Hosted immediate-mode UI widgets on the software framebuffer.
- Native Wayland pointer motion, left-button events and keyboard event forwarding when built with Wayland client development files.
- X11 presenter remains available as an alternative/fallback when built.
- Built-in softened 16×16 `Builtin Emergency Font` fallback.
- Default font selection attempts `FantasqueSansM Nerd Font Propo` when FreeType and an installed font file are available.
- Optional `Arial` named selection; Linux may substitute Liberation Sans. Arial itself is not bundled.
- `IFont` registration accepts Base64, Hex or Raw font data; FreeType is still required for scalable rasterization.
- `Blow.StyledLabel` supports combined Bold, Italic, Underline and Strikethru effects. Current implementation applies these effects to the emergency bitmap glyphs; normal `Blow.Label` can use the selected FreeType face.

## Encoding and networking

- Added `Binarify`, `Hexify` and `Base64ify`; each updates the hosted `$LastHexResult` global.
- Added `Encoding.LastResult()` as a read accessor for the most recent encoded result.
- Added `Net.HttpsGet` using OpenSSL TLS 1.2+, system CA-chain verification and hostname verification. No curl dependency is used.

## Build and platform notes

- Hosted `purplec` programs require Linux and the normal C toolchain.
- HTTPS linking requires OpenSSL development libraries (`libssl` and `libcrypto`).
- Scalable font rendering requires FreeType development files and an installed/embedded supported font.
- Wayland and X11 are optional presenters, selected according to the build environment.
- **Wayland:** native surface role prefers modern xdg-shell (`xdg_wm_base` + `xdg_toplevel`); classic `wl_shell` is retained as fallback. Protocol tables are embedded so no wayland-protocols package is required at build time.
- These APIs are hosted-only and are not available in freestanding Multiboot or UEFI programs.

## Documentation

- [`BLOWUI-GUIDE.md`](BLOWUI-GUIDE.md) — complete BlowUI usage and font guide.
- [`BLOWUI-NET-FILES.md`](BLOWUI-NET-FILES.md) — compact API reference.
- [`LANGUAGE.md`](LANGUAGE.md) — Purplecode language manual, including the 2.1 hosted additions.
