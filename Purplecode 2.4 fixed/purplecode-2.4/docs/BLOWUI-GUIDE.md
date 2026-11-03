# BlowUI Guide — Purplecode 2.4

BlowUI is the hosted, immediate-mode UI toolkit included with Purplecode 2.4. It draws into Purplecode's software framebuffer and presents that framebuffer through an available Linux display backend. It is not part of the freestanding Multiboot or UEFI API.

## 1. Build requirements

- Linux, GCC and `make` for hosted programs.
- OpenSSL development libraries for `Net.HttpsGet`.
- Wayland client development files for the native Wayland presenter, or X11 development files for the X11 presenter.
- FreeType development files for scalable fonts. Without FreeType, BlowUI uses its built-in emergency bitmap font.

Optional libraries are detected by the hosted compiler's link recipes. Install the relevant development packages before building programs that need those features.

## 2. Minimal window and frame loop

```purple
fn main() {
    Blow.Window(640, 400, "My BlowUI app")
    let running: int = 1
    while running == 1 {
        if Blow.Poll() == -1 { running = 0 }
        Blow.Begin(0)
        Blow.Label(24, 24, "Hello from BlowUI", 2)
        if Blow.Button(24, 80, 160, 40, "Quit") == 1 {
            running = 0
        }
        Blow.End()
    }
    return 0
}
```

Call `Blow.Window` once for a normal desktop window (titlebar + borders + resize handles on KDE and other compositors).  
Use `Blow.Decorationless.Window` when you want a borderless / frameless window.  
Each frame, call `Blow.Poll`, `Blow.Begin`, draw widgets, and call `Blow.End`.  
`Blow.Poll()` returns `-1` after the window closes.  
`Blow.Headless(w,h)` creates a framebuffer without a display window, useful for tests and screenshots.

Windows created with `Blow.Window` are resizable; the runtime automatically recreates the software framebuffer and the X11/Wayland buffers when the compositor or window manager changes the size.

## 3. Core API

Colors use `0xRRGGBB`.

| API | Description |
|---|---|
| `Blow.Window(w,h,title)` | Create a **normal** desktop window (decorations + resizable). |
| `Blow.Decorationless.Window(w,h,title)` | Create a **borderless / frameless** window. |
| `Blow.Headless(w,h)` | Create a display-free framebuffer. |
| `Blow.Poll()` | Process input and return `-1` when closed. |
| `Blow.Begin(background)` / `Blow.End()` | Begin and present a frame. Background `0` selects the active theme background. |
| `Blow.Theme(n)` | Select theme: `0` Violet Night, `1` Soft Light, `2` Midnight Blue. |
| `Blow.Panel(x,y,w,h,radius)` | Rounded panel with shadow and border. |
| `Blow.Label(x,y,text,scale)` | Draw text using the selected face when scalable rendering is available. |
| `Blow.Muted(x,y,text,scale)` | Draw theme-muted text. |
| `Blow.LabelInt(x,y,value,scale)` | Draw an integer. |
| `Blow.Button(x,y,w,h,text)` | Draw a button; returns `1` when clicked. |
| `Blow.Toggle(x,y,value)` | Draw a toggle; returns its current 0/1 value. |
| `Blow.Checkbox(x,y,value,text)` | Draw a checkbox; returns its current 0/1 value. |
| `Blow.Slider(id,x,y,w,value,max)` | Draw a slider; `id` must be unique and non-zero. Returns the value. |
| `Blow.Progress(x,y,w,h,value,max)` | Draw a progress bar. |
| `Blow.TextBox(id,x,y,w,buffer)` | 256-byte text input buffer; returns `1` on Enter. |
| `Blow.Badge(x,y,text,color)` / `Blow.Divider(x,y,w)` | Draw a pill badge or horizontal divider. |
| `Blow.RRect(x,y,w,h,r,color)` / `Blow.Circle(x,y,r,color)` | Draw rounded rectangle or circle. |
| `Blow.MouseX()` / `Blow.MouseY()` / `Blow.MouseDown()` | Read pointer state. |
| `Blow.Key()` | Pop one queued key code; returns `0` if the queue is empty. |
| `Blow.Millis()` | Monotonic milliseconds. |
| `Blow.TextWidth(text,scale)` | Measure text width. |
| `Blow.Screenshot(path)` | Save the framebuffer as PPM. |
| `Blow.SimClick(x,y)` | Inject a synthetic click for tests. |

## 4. Fonts

### Default and fallback behavior

When a BlowUI window is opened, the runtime attempts to select **FantasqueSansM Nerd Font Propo** as its default face. If the face cannot be loaded or scalable font support is unavailable, normal text rendering falls back to the built-in **Builtin Emergency Font**. The emergency face is a built-in 16×16 bitmap rendering derived from the compact built-in glyph table, with softened edge coverage; it does not require an external font file.

**Arial** is an optional named choice, not the default. The runtime checks common Microsoft Arial locations and then a Liberation Sans-compatible substitute on Linux. Arial itself is proprietary; the project does not redistribute it. Install a properly licensed copy if you specifically need Arial.

Scalable font loading requires FreeType at build time and the requested font file at runtime. The FantasqueSansM Nerd Font Propo file is not bundled in this source package. Install it in a standard system font directory or `~/.local/share/fonts`.

```purple
fn main() {
    Blow.Window(640, 360, "Font selection")
    Blow.Font("FantasqueSansM Nerd Font Propo")
    // Blow.Font returns a non-negative face ID on success, -1 on failure.
    Blow.Font("Builtin Emergency Font")
    // Arial is optional and only works if an appropriate face can be found.
    Blow.Font("Arial")
    return 0
}
```

`Blow.Font(name)` selects one of the built-in named choices or a face previously registered with `IFont`. Use the return value to detect whether selection succeeded. The emergency face is always available.

### Embedded fonts with IFont

`IFont` registers font bytes under a name. Use Base64 or Hex text so binary font data can safely be carried in a Purplecode string. The accepted font signatures include TrueType, OpenType, WOFF and WOFF2 containers.

```purple
// Three-argument form: IFont(name, encodedData, encoding)
let face: int = IFont("MyEmbeddedFace", FONT_BASE64_DATA, "Base64")
if face >= 0 {
    Blow.Font("MyEmbeddedFace")
}
```

A four-argument form is also accepted when the caller needs to specify the encoded input length:

```purple
let face: int = IFont("MyEmbeddedFace", FONT_HEX_DATA, encoded_length, "Hex")
```

Supported encoding names are `Base64`/`B64`, `Hex`, and `Raw`. For ordinary source-code strings, Base64 or Hex is recommended. Registration stores the decoded font data in memory. Actual scalable rendering still requires FreeType to be available and able to load that font format. A successful registration without FreeType does not mean the face can be rasterized; the emergency font remains the fallback.

### Styled text

`Blow.StyledLabel(x,y,text,style,color,scale)` accepts comma-separated style names:

- `!Bold`
- `!Italic`
- `!Underline`
- `!Strikethru`

The exclamation mark is optional after the first token. Styles combine:

```purple
Blow.StyledLabel(24, 120, "Bold and underlined", "!Bold,!Underline", 0xFFFFFF, 1)
Blow.StyledLabel(24, 150, "All four", "!Bold,!Italic,!Underline,!Strikethru", 0xFFFFFF, 1)
```

**Current implementation detail:** `StyledLabel` applies these effects to the built-in 16×16 emergency bitmap glyphs. It does not currently use the selected FreeType face. `Blow.Label` and related normal text paths use the selected scalable face when FreeType and that face are available.

## 5. Encoding helpers

These hosted helpers return an encoded string and update the global `$LastHexResult`:

```purple
fn main() {
    Hexify("kebab")
    Data.ShellPrint($LastHexResult) // 6b65626162

    Base64ify("YES")
    Data.ShellPrint($LastHexResult) // WUVT

    Binarify("A")
    Data.ShellPrint($LastHexResult) // 01000001
    return 0
}
```

- `Binarify(text)` converts each input byte to eight binary digits.
- `Hexify(text)` converts each input byte to two hexadecimal digits.
- `Base64ify(text)` converts the input bytes to Base64.
- `$LastHexResult` contains the most recent result, regardless of which of the three helpers produced it.
- `Encoding.LastResult()` returns the same current result.

The global is updated by each encoding call. Copy the result into your own variable if you need to preserve it across another conversion.

## 6. HTTPS

`Net.HttpsGet(host,path,outfile)` performs an HTTPS GET against port 443. It uses OpenSSL with TLS 1.2 or newer, verifies the certificate chain against the system trust store, and checks the certificate hostname. It returns the response body byte count on success or a negative value on failure. `outfile = "-"` writes the response body to standard output; another path writes it to a file.

```purple
fn main() {
    let bytes: int = Net.HttpsGet("example.com", "/", "-")
    Data.ShellPrint(bytes)
    return 0
}
```

The hosted executable must link against OpenSSL (`libssl` and `libcrypto`). This API is a simple GET client: it does not currently expose custom headers, arbitrary ports, redirect handling, or a general TLS socket interface.

## 7. Linux display backend notes

The hosted runtime can present its software framebuffer through X11 or Wayland when the corresponding development libraries were available at build time. The Wayland path handles pointer motion, left-button events, and keyboard events. The native Wayland surface role prefers modern **xdg-shell** (`xdg_wm_base` + `xdg_toplevel`) when the compositor advertises it; classic `wl_shell` remains as a fallback. X11 is the available fallback when neither is usable. Wayland is not part of the freestanding boot targets.

**Deep FreeType embedding:** preferred scalable faces are loaded first from a compile-time embedded blob (`PURPLE_EMBEDDED_FONT_DATA` / `SIZE` / `NAME`) or from runtime `Blow.IFont` / `IFont` memory data (`FT_New_Memory_Face`). System path search is only a last-resort fallback.

## 8. Examples

- `examples/blow_demo.purple` — interactive widget showcase.
- `examples/blow_snapshot.purple` — headless framebuffer and screenshot example.
- `examples/blow_fonts_encoding.purple` — font selection, style modifiers and encodings.
- `examples/net_https_get.purple` — HTTPS GET.

See also `docs/BLOWUI-NET-FILES.md` for the compact BlowUI/Net/File API tables.

## Window decorations (fix)
`Blow.Window` now always asks for a real titlebar/borders:
- **Native Wayland (KDE, Sway, Hyprland…)**: negotiates `zxdg_decoration_manager_v1` and requests *server-side* decorations.
- **Compositors without that protocol (e.g. GNOME)**: automatically uses XWayland so the window manager decorates it. Set `PURPLE_GUI=wayland` to force native anyway.
- **X11**: the window manager decorates as usual.
`Blow.Decorationless.Window` requests *client-side* (no frame).
