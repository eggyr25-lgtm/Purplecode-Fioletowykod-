# BlowUI, Net and File reference (Purplecode 2.3, hosted)

Colors are `0xRRGGBB`. Widgets are immediate-mode: call them every frame; they draw and return state.

## BlowUI
Frame loop: `Blow.Window(w,h,"title")` once, then `Blow.Poll()` (returns -1 when closed) -> `Blow.Begin(0)` (0 = theme background) -> widgets -> `Blow.End()`.

| Call | Does |
|---|---|
| `Blow.Window(w,h,title)` / `Blow.Headless(w,h)` | open a window / render with no display (CI, screenshots) |
| `Blow.Theme(n)` | 0 Violet Night, 1 Soft Light, 2 Midnight Blue |
| `Blow.Poll()` `Blow.Begin(bg)` `Blow.End()` | frame control |
| `Blow.Panel(x,y,w,h,radius)` | card with shadow + border |
| `Blow.Label(x,y,text,scale)` `Blow.Muted(...)` `Blow.LabelInt(x,y,value,scale)` | text (scale 1-6) |
| `Blow.Button(x,y,w,h,text)` | returns 1 on click |
| `Blow.Toggle(x,y,value)` / `Blow.Checkbox(x,y,value,text)` | return the new 0/1 value |
| `Blow.Slider(id,x,y,w,value,max)` | returns value (id must be unique, non-zero) |
| `Blow.Progress(x,y,w,h,value,max)` | progress bar |
| `Blow.TextBox(id,x,y,w,buf)` | 256-byte text field, returns 1 on Enter |
| `Blow.Badge(x,y,text,color)` `Blow.Divider(x,y,w)` | pill tag / line |
| `Blow.RRect(x,y,w,h,r,color)` `Blow.Circle(x,y,r,color)` | anti-aliased shapes |
| `Blow.MouseX/MouseY/MouseDown()` `Blow.Key()` `Blow.Millis()` `Blow.TextWidth(text,scale)` | input / time / metrics |
| `Blow.Screenshot(path)` `Blow.SimClick(x,y)` | save a PPM; fake a click (tests) |

## Net
| Call | Does |
|---|---|
| `Net.Connect(host,port)` -> fd, `Net.Listen(port)` -> fd | TCP |
| `Net.Accept(fd)` `Net.AcceptTimeout(fd,ms)` | client fd (timeout: 0) |
| `Net.Send(fd,text)` `Net.SendBuf(fd,buf,len)` | bytes sent |
| `Net.Recv(fd,buf,max)` `Net.RecvTimeout(fd,buf,max,ms)` `Net.RecvLine(fd,buf,max)` | bytes read (timeout: -2) |
| `Net.Close(fd)` | close |
| `Net.UdpOpen(port)` `Net.UdpSend(fd,host,port,text)` `Net.UdpRecv(fd,buf,max)` `Net.Peer(buf,max)` | UDP, broadcast enabled, sender as `ip:port` |
| `Net.HttpGet(host,port,path,outfile)` `Net.HttpPost(host,port,path,body,outfile)` | body bytes written to file (`"-"` = stdout); `Net.Status()` = HTTP code |
| `Net.HttpServe(port,dir,max_requests)` | static file server (0 = forever) |
| `Net.Resolve(host,buf,max)` `Net.HostName(buf,max)` `Net.Print(buf)` | DNS / helpers |

## File
`File.Read(path,buf,max)` `File.Write(path,text)` `File.Append(path,text)` `File.Copy(a,b)` `File.Rename(a,b)` `File.Delete(path)` `File.Mkdir(path)` `File.Exists(path)` `File.IsDir(path)` `File.Size(path)` `File.Count(dir)` `File.Name(dir,index,buf,max)` `File.Print(path)`.

See `examples/blow_demo.purple`, `blow_snapshot.purple`, `net_*.purple`, `files_demo.purple`.

## BlowUI fonts and text styles (2.1)

- Default attempt on `Blow.Window`: `FantasqueSansM Nerd Font Propo` (requires FreeType and an installed font file).
- Always-available fallback: `Builtin Emergency Font`, a softened 16×16 bitmap face.
- Optional named choice: `Arial`; Linux may use Liberation Sans as a compatible substitute. Arial is not bundled.
- `Blow.Font(name)` selects a face and returns a non-negative ID on success or `-1` on failure.
- `IFont(name,data,encoding)` registers embedded font data. Encodings: `Base64`/`B64`, `Hex`, `Raw`. The four-argument form `IFont(name,data,length,encoding)` is also accepted. Base64 or Hex is recommended for source strings. FreeType is required to rasterize scalable faces.
- `Blow.StyledLabel(x,y,text,style,color,scale)` accepts combined `!Bold`, `!Italic`, `!Underline`, and `!Strikethru` tokens. The `!` prefix is optional after the first comma-separated token. **StyledLabel currently uses the emergency bitmap glyphs even when a scalable face is selected.** Normal `Blow.Label` rendering uses the selected scalable face when FreeType is available.

## Encoding and HTTPS (2.1)

- `Binarify(text)`, `Hexify(text)`, and `Base64ify(text)` return an encoded string and update `$LastHexResult`.
- `Encoding.LastResult()` reads the same current result.
- `Net.HttpsGet(host,path,outfile)` performs a TLS 1.2+ HTTPS GET on port 443 using OpenSSL, the system CA trust store, and hostname verification. `outfile = "-"` writes the response body to stdout. It returns the body byte count or a negative value on failure. It is a simple GET helper, not a general TLS socket interface.

See [`BLOWUI-GUIDE.md`](BLOWUI-GUIDE.md) for examples, full usage notes and backend limitations. Wayland prefers modern xdg-shell (`xdg_wm_base` + `xdg_toplevel`) and falls back to classic `wl_shell` when needed.
