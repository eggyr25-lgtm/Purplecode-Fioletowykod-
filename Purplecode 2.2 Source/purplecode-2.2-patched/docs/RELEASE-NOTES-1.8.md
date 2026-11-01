# Purplecode 1.8 — Systems Focus + Solid GUI

This release removes incomplete high-level stubs and strengthens both the systems identity and the GUI.

## Removed

- **GPU.VRAM.* stubs** — Completely gone. Fake messages are no longer part of the language.
- Staged / “coming soon” GUI control messages (e.g. Button.Create). Unknown hierarchical GUI names now simply return -1.

## GUI improvements (same look & feel, more solid)

- Hierarchical API is the preferred path (`GUI.Window.CreateSized`, `GUI.Draw.Rect`, `GUI.DrawText`, `GUI.Present`, `GUI.PollKey`, `GUI.Pump`).
- New primitives that fit the existing style:
  - `GUI.DrawLine(x0, y0, x1, y1, color)` — Bresenham, clipped
  - `GUI.GetWidth()` / `GUI.GetHeight()`
  - `GUI.Clear(color)` — optional colour
- Codegen now passes up to five arguments for GUI/Blit hierarchical calls, so DrawLine and similar work cleanly.
- Examples rewritten to use the hierarchical API and look more polished (`hello_gui.purple`, `pong_sketch.purple`).
- No more staged/stub prints from the GUI dispatcher.

## Low-level / systems

- `LinuxAPI.Call` and `Kernel.Call` / `System.Kernel.Call` lower multiple arguments correctly (System V AMD64).
- Focus remains on honest facilities: memory, syscalls, ELF, Multiboot, real UEFI PE/COFF, structs + methods + arrays of structs.

## Compatibility

- Programs that never used the removed GPU APIs need no changes.
- Hierarchical GUI programs continue to work; new drawing helpers are additive.
- The software-blitter model is unchanged — same visual style, fewer sharp edges.

## Direction

Keep shipping things that actually work. Prefer solid systems primitives and a reliable software GUI over incomplete high-level features.
