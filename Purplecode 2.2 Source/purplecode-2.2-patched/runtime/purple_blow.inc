/* Purplecode 2.1 – BlowUI: immediate-mode, anti-aliased, rounded UI on the software blitter.
 * Pure C, no GTK. Loop shape:  Blow.Poll() -> Blow.Begin(0) -> widgets -> Blow.End().
 * Mouse + typing need X11 (works on XWayland). Text buffers (TextBox) are 256 bytes. */
#include <math.h>

typedef struct { uint32_t bg, surface, surface2, border, text, muted, accent, accent_hi, accent_lo, good, bad; } BlowTheme;
static const BlowTheme g_blow_themes[3] = {
    { 0x12101C, 0x1B1830, 0x26224A, 0x34305C, 0xECE9FF, 0x9A95C4, 0x8B5CF6, 0xA78BFA, 0x6D43D6, 0x34D399, 0xF87171 }, /* 0 Violet Night */
    { 0xF4F3FA, 0xFFFFFF, 0xEDEBF7, 0xD9D6EC, 0x1F1B3A, 0x6B6890, 0x7C4DFF, 0x9575FF, 0x5E35D8, 0x10B981, 0xEF4444 }, /* 1 Soft Light */
    { 0x0B1220, 0x111A2E, 0x1A2647, 0x26365F, 0xE6EEFF, 0x8FA3CC, 0x3B82F6, 0x60A5FA, 0x2563EB, 0x22C55E, 0xF43F5E }, /* 2 Midnight Blue */
};
static int g_blow_theme_id = 0;
#define BT (g_blow_themes[g_blow_theme_id])
static int g_mx, g_my, g_mdown, g_released, g_pressx, g_pressy, g_active, g_focus, g_blow_closed;
static int g_keyq[32], g_keyq_n;
/* Built-in font ids (permanent):
 * 0 = Builtin Emergency Font (always available, soft 16×16 over 8×8 bitmap)
 * 1 = FantasqueSansM Nerd Font Propo (preferred scalable; falls back to 0)
 * 2 = Arial (system / Liberation / DejaVu; falls back to 0)
 * 3+ = user-registered via Blow.IFont
 */
static int g_blow_font_id = 1;

static inline void blow_px(int x, int y, uint32_t rgb, float a) {
    if (!g_fb || x < 0 || y < 0 || x >= g_fb_w || y >= g_fb_h || a <= 0.f) return;
    if (a > 1.f) a = 1.f;
    uint32_t *p = &g_fb[y * g_fb_stride + x], d = *p;
    int sr = (rgb >> 16) & 255, sg = (rgb >> 8) & 255, sb = rgb & 255;
    int dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
    *p = 0xFF000000u | ((uint32_t)(int)(dr + (sr - dr) * a + .5f) << 16) | ((uint32_t)(int)(dg + (sg - dg) * a + .5f) << 8) | (uint32_t)(int)(db + (sb - db) * a + .5f);
}
/* Signed-distance rounded box; ring>0 draws only an inner outline of that thickness. */
static void blow_rr(float x, float y, float w, float h, float r, uint32_t col, float alpha, float ring) {
    if (!g_fb || w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2; if (r > h / 2) r = h / 2; if (r < 0) r = 0;
    int x0 = (int)floorf(x) - 1, y0 = (int)floorf(y) - 1, x1 = (int)ceilf(x + w) + 1, y1 = (int)ceilf(y + h) + 1;
    float cx = x + w / 2, cy = y + h / 2, hw = w / 2 - r, hh = h / 2 - r;
    for (int py = y0; py < y1; py++) {
        if (py < 0 || py >= g_fb_h) continue;
        for (int px = x0; px < x1; px++) {
            if (px < 0 || px >= g_fb_w) continue;
            float qx = fabsf(px + .5f - cx) - hw, qy = fabsf(py + .5f - cy) - hh, d;
            if (qx <= 0 && qy <= 0) d = (qx > qy ? qx : qy) - r;
            else { float ax = qx > 0 ? qx : 0, ay = qy > 0 ? qy : 0; d = sqrtf(ax * ax + ay * ay) - r; }
            float a = ring > 0 ? 0.5f - (fabsf(d + ring * .5f) - ring * .5f) : 0.5f - d;
            if (a <= 0) continue; if (a > 1) a = 1;
            blow_px(px, py, col, a * alpha);
        }
    }
}
static void blow_shadow(float x, float y, float w, float h, float r, uint32_t col, float strength) {
    for (int i = 6; i >= 1; i--) blow_rr(x - i, y - i + 4, w + 2 * i, h + 2 * i, r + i, col, strength / 6.f * (7 - i) / 6.f, 0);
}
static int blow_text_w(const char *s, int sc) { if(!s)return 0; int ft=purple_font_measure(s,16*sc); return ft>=0?ft:(int)strlen(s)*PURPLE_FONT_W*sc; }
static void blow_text(int x, int y, const char *s, uint32_t col, int sc) {
    if (!s || !g_fb) return; if (sc < 1) sc = 1; if (sc > 6) sc = 6;
    int cx=x;const unsigned char *p=(const unsigned char*)s;
    while(*p){unsigned int cp=purple_utf8_next(&p);
        if(cp=='\n'){cx=x;y+=(PURPLE_FONT_H+1)*sc;continue;}
        int adv=purple_ft_draw_glyph(cx,y,cp,col,16*sc);if(adv>0){cx+=adv;continue;}
        int ch=(cp>=32&&cp<=126)?(int)cp:'?';const unsigned char *g=purple_font8x8[ch-32];
        for(int r=0;r<16;r++)for(int c=0;c<16;c++){unsigned char a=purple_emergency_coverage(g,r,c);if(a)for(int sy=0;sy<sc;sy++)for(int sx=0;sx<sc;sx++)blit_blend_pixel(cx+c*sc+sx,y+r*sc+sy,col,a);}
        cx+=PURPLE_FONT_W*sc;
    }
}

static int blow_in(int px, int py, int x, int y, int w, int h) { return px >= x && py >= y && px < x + w && py < y + h; }
static int blow_clicked(int x, int y, int w, int h) { return g_released && blow_in(g_pressx, g_pressy, x, y, w, h) && blow_in(g_mx, g_my, x, y, w, h); }
static int blow_hover(int x, int y, int w, int h) { return blow_in(g_mx, g_my, x, y, w, h); }
static void blow_keypush(int k) { if (g_keyq_n < 32) g_keyq[g_keyq_n++] = k; }
static int blow_keypop(void) { if (!g_keyq_n) return 0; int k = g_keyq[0]; memmove(g_keyq, g_keyq + 1, (size_t)(--g_keyq_n) * sizeof(int)); return k; }
static uint32_t blow_mix(uint32_t a, uint32_t b, float t) {
    int r = (int)(((a >> 16) & 255) * (1 - t) + ((b >> 16) & 255) * t), g = (int)(((a >> 8) & 255) * (1 - t) + ((b >> 8) & 255) * t), bl = (int)((a & 255) * (1 - t) + (b & 255) * t);
    return (uint32_t)(r << 16 | g << 8 | bl);
}

/* ---- window / frame ---- */
int64_t purple_blow_window(int64_t w, int64_t h, const char *title) {
    /* Prefer the permanent Fantasque built-in; falls back to emergency automatically */
    if(g_blow_font_id == 1) purple_blow_font("FantasqueSansM Nerd Font Propo");
    g_blow_closed = 0; g_active = g_focus = 0;
    /* Normal KDE / desktop window with decorations + resize handles */
    return purple_gui_create_window((int)w, (int)h, title ? title : "BlowUI", 0);
}
int64_t purple_blow_decorationless_window(int64_t w, int64_t h, const char *title) {
    if(g_blow_font_id == 1) purple_blow_font("FantasqueSansM Nerd Font Propo");
    g_blow_closed = 0; g_active = g_focus = 0;
    /* Borderless / decorationless window (Motif hints + override-redirect on X11) */
    return purple_gui_create_window((int)w, (int)h, title ? title : "BlowUI", 1);
}
int64_t purple_blow_headless(int64_t w, int64_t h) { blit_ensure((int)w, (int)h); g_win_open = 0; return 0; }
int64_t purple_blow_theme(int64_t n) { g_blow_theme_id = (n < 0 || n > 2) ? 0 : (int)n; return g_blow_theme_id; }
int64_t purple_blow_poll(void) {
#ifdef PURPLE_HAS_X11
    if (g_gui_backend == 1 && g_dpy) {
        g_released = 0;
        while (XPending(g_dpy) > 0) {
            XEvent ev; XNextEvent(g_dpy, &ev);
            switch (ev.type) {
            case MotionNotify: g_mx = ev.xmotion.x; g_my = ev.xmotion.y; break;
            case ButtonPress:   if (ev.xbutton.button == 1) { g_mdown = 1; g_mx = g_pressx = ev.xbutton.x; g_my = g_pressy = ev.xbutton.y; } break;
            case ButtonRelease: if (ev.xbutton.button == 1) { g_mdown = 0; g_released = 1; g_mx = ev.xbutton.x; g_my = ev.xbutton.y; } break;
            case KeyPress: {
                char b[8]; KeySym ks; int n = XLookupString(&ev.xkey, b, sizeof b, &ks, NULL), k = 0;
                if (ks == XK_Left) k = 1000; else if (ks == XK_Right) k = 1001; else if (ks == XK_Up) k = 1002; else if (ks == XK_Down) k = 1003;
                else if (ks == XK_BackSpace) k = 8; else if (ks == XK_Return || ks == XK_KP_Enter) k = 13; else if (ks == XK_Tab) k = 9;
                else if (ks == XK_Escape) k = 27; else if (ks == XK_Delete) k = 127;
                else if (n == 1 && (unsigned char)b[0] >= 32 && (unsigned char)b[0] < 127) k = (unsigned char)b[0];
                if (k) blow_keypush(k); break; }
            case ConfigureNotify:
                /* Auto-resize: WM changed our window size. */
                if (ev.xconfigure.width > 0 && ev.xconfigure.height > 0)
                    purple_gui_resize(ev.xconfigure.width, ev.xconfigure.height);
                break;
            case ClientMessage: case DestroyNotify: g_win_open = 0; g_blow_closed = 1; break;
            case Expose: purple_gui_present(); break;
            }
        }
    }
#endif
#ifdef PURPLE_HAS_WAYLAND
    if (g_gui_backend == 2 && g_wl_dpy) {
        wl_dispatch_nonblock();
        g_mx=g_wl_mouse_x; g_my=g_wl_mouse_y; g_mdown=g_wl_mouse_down;
        g_released=g_wl_mouse_released; g_wl_mouse_released=0;
        g_pressx=g_wl_press_x; g_pressy=g_wl_press_y;
        int wk; while ((wk=wl_keyq_pop())!=0) blow_keypush(wk);
        /* Apply compositor-driven resize from xdg_toplevel.configure */
        if (g_resize_w > 0 && g_resize_h > 0)
            purple_gui_resize(g_resize_w, g_resize_h);
    }
#endif
    return g_blow_closed ? -1 : 0;
}
int64_t purple_blow_width(void) { return (int64_t)g_fb_w; }
int64_t purple_blow_height(void) { return (int64_t)g_fb_h; }
int64_t purple_blow_begin(int64_t bg) { blit_clear(bg ? (uint32_t)bg | 0xFF000000u : BT.bg | 0xFF000000u); return 0; }
int64_t purple_blow_end(void) {
    if (g_win_open) { purple_gui_present(); struct timespec ts = { 0, 12 * 1000000L }; nanosleep(&ts, NULL); }
    if (g_released) g_active = 0;
    g_released = 0; return 0;
}
int64_t purple_blow_mousex(void) { return g_mx; }
int64_t purple_blow_mousey(void) { return g_my; }
int64_t purple_blow_mousedown(void) { return g_mdown; }
int64_t purple_blow_key(void) { return blow_keypop(); }
int64_t purple_blow_millis(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
int64_t purple_blow_simclick(int64_t x, int64_t y) { g_mx = g_pressx = (int)x; g_my = g_pressy = (int)y; g_released = 1; return 0; } /* test hook */
int64_t purple_blow_screenshot(const char *path) {
    if (!g_fb || !path) return -1;
    FILE *f = fopen(path, "wb"); if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", g_fb_w, g_fb_h);
    for (int i = 0; i < g_fb_w * g_fb_h; i++) { uint32_t p = g_fb[i]; unsigned char c[3] = { (p >> 16) & 255, (p >> 8) & 255, p & 255 }; fwrite(c, 1, 3, f); }
    fclose(f); return 0;
}

/* ---- shapes + text ---- */
int64_t purple_blow_rrect(int64_t x, int64_t y, int64_t w, int64_t h, int64_t r, int64_t color) { blow_rr((float)x, (float)y, (float)w, (float)h, (float)r, (uint32_t)color, 1.f, 0); return 0; }
int64_t purple_blow_circle(int64_t x, int64_t y, int64_t r, int64_t color) { blow_rr((float)(x - r), (float)(y - r), (float)(2 * r), (float)(2 * r), (float)r, (uint32_t)color, 1.f, 0); return 0; }
int64_t purple_blow_divider(int64_t x, int64_t y, int64_t w) { blit_fill_rect((int)x, (int)y, (int)w, 1, BT.border); return 0; }
int64_t purple_blow_label(int64_t x, int64_t y, const char *t, int64_t sc) { blow_text((int)x, (int)y, t, BT.text, (int)sc); return blow_text_w(t, (int)sc); }
static int blow_has_style(const char *style,const char *wanted){
    if(!style||!wanted)return 0;const char *p=style;
    while(*p){while(*p==' '||*p=='\t'||*p==',' )p++;if(*p=='!')p++;const char *a=p;while(*p&&*p!=',')p++;size_t n=(size_t)(p-a);while(n&&a[n-1]==' ')n--;if(strlen(wanted)==n&&!strncmp(a,wanted,n))return 1;if(*p==',')p++;}
    return 0;
}
static void blow_styled_text(int x,int y,const char *s,uint32_t col,int sc,const char *style) {
    if(!s)return; int bold=blow_has_style(style,"Bold"),italic=blow_has_style(style,"Italic"),under=blow_has_style(style,"Underline"),strike=blow_has_style(style,"Strikethru");
    int ox=x, starty=y;
    for(const unsigned char *p=(const unsigned char*)s;*p;p++){
        if(*p=='\n'){x=ox;y+=17*sc;continue;} int ch=*p;if(ch<32||ch>126)ch='?';
        const unsigned char *g=purple_font8x8[ch-32];
        for(int r=0;r<16;r++)for(int c=0;c<16;c++)if(g[r/2]&(0x80>>(c/2))){int shear=italic?((15-r)/5):0;blit_fill_rect(x+c*sc+shear,y+r*sc,sc,sc,col);if(bold)blit_fill_rect(x+c*sc+shear+sc,y+r*sc,sc,sc,col);}
        x+=(16+(bold?1:0))*sc;
    }
    int endx=x;
    if(under)blit_fill_rect(ox,starty+16*sc,endx-ox,sc,col);
    if(strike)blit_fill_rect(ox,starty+8*sc,endx-ox,sc,col);
}
int64_t purple_blow_styledlabel(int64_t x,int64_t y,const char *t,const char *style,int64_t color,int64_t sc){
    if(sc<1)sc=1;if(sc>6)sc=6;blow_styled_text((int)x,(int)y,t,(uint32_t)color,(int)sc,style);return blow_text_w(t,(int)sc);
}
int64_t purple_blow_font(const char *name){
    if(!name) return -1;
    /* Permanent built-ins – always succeed (scalable faces fall back to emergency if missing) */
    if(!strcmp(name,"Builtin Emergency Font")){
        g_blow_font_id = 0;
        purple_font_select(name);
        return 0;
    }
    if(!strcmp(name,"FantasqueSansM Nerd Font Propo")){
        if(purple_font_select(name)){ g_blow_font_id = 1; return 1; }
        /* missing on this system → stay on emergency */
        g_blow_font_id = 0; purple_font_select("Builtin Emergency Font"); return 0;
    }
    if(!strcmp(name,"Arial")){
        if(purple_font_select(name)){ g_blow_font_id = 2; return 2; }
        g_blow_font_id = 0; purple_font_select("Builtin Emergency Font"); return 0;
    }
    /* User-registered faces */
    if(purple_font_is_registered(name) && purple_font_select(name)){
        g_blow_font_id = 3; return 3;
    }
    return -1;
}
int64_t purple_blow_muted(int64_t x, int64_t y, const char *t, int64_t sc) { blow_text((int)x, (int)y, t, BT.muted, (int)sc); return blow_text_w(t, (int)sc); }
int64_t purple_blow_labelint(int64_t x, int64_t y, int64_t v, int64_t sc) { char b[32]; snprintf(b, sizeof b, "%lld", (long long)v); blow_text((int)x, (int)y, b, BT.text, (int)sc); return 0; }
int64_t purple_blow_textwidth(const char *t, int64_t sc) { return blow_text_w(t, (int)sc); }
int64_t purple_blow_panel(int64_t x, int64_t y, int64_t w, int64_t h, int64_t r) {
    blow_shadow((float)x, (float)y, (float)w, (float)h, (float)r, 0x000000, g_blow_theme_id == 1 ? 0.18f : 0.55f);
    blow_rr((float)x, (float)y, (float)w, (float)h, (float)r, BT.surface, 1.f, 0);
    blow_rr((float)x, (float)y, (float)w, (float)h, (float)r, BT.border, 1.f, 1.f); return 0;
}
int64_t purple_blow_badge(int64_t x, int64_t y, const char *t, int64_t color) {
    int w = blow_text_w(t, 1) + 18; blow_rr((float)x, (float)y, (float)w, 20, 10, (uint32_t)color, 0.22f, 0);
    blow_text((int)x + 9, (int)y + 6, t, (uint32_t)color, 1); return w;
}

/* ---- widgets (return state; immediate mode) ---- */
int64_t purple_blow_button(int64_t x, int64_t y, int64_t w, int64_t h, const char *label) {
    int hov = blow_hover((int)x, (int)y, (int)w, (int)h), prs = g_mdown && blow_in(g_pressx, g_pressy, (int)x, (int)y, (int)w, (int)h);
    uint32_t c = prs ? BT.accent_lo : hov ? BT.accent_hi : BT.accent; float r = (float)(h < 40 ? h / 2 : 14);
    if (!prs) blow_shadow((float)x, (float)y, (float)w, (float)h, r, c, 0.32f);
    blow_rr((float)x, (float)y, (float)w, (float)h, r, c, 1.f, 0);
    blow_rr((float)x, (float)y, (float)w, (float)h, r, 0xFFFFFF, 0.18f, 1.f);
    int sc = h >= 48 ? 2 : 1, tw = blow_text_w(label, sc);
    blow_text((int)x + ((int)w - tw) / 2, (int)y + ((int)h - 8 * sc) / 2 + (prs ? 1 : 0), label, 0xFFFFFF, sc);
    return blow_clicked((int)x, (int)y, (int)w, (int)h);
}
int64_t purple_blow_toggle(int64_t x, int64_t y, int64_t value) {
    int hov = blow_hover((int)x, (int)y, 44, 24);
    blow_rr((float)x, (float)y, 44, 24, 12, value ? (hov ? BT.accent_hi : BT.accent) : BT.surface2, 1.f, 0);
    if (!value) blow_rr((float)x, (float)y, 44, 24, 12, BT.border, 1.f, 1.f);
    blow_rr((float)(x + (value ? 22 : 2)) + 0.f, (float)y + 2, 20, 20, 10, 0x000000, 0.25f, 0);
    blow_rr((float)(x + (value ? 22 : 2)), (float)y + 1.5f, 20, 20, 10, 0xFFFFFF, 1.f, 0);
    return blow_clicked((int)x, (int)y, 44, 24) ? !value : value;
}
int64_t purple_blow_checkbox(int64_t x, int64_t y, int64_t value, const char *label) {
    int w = 30 + blow_text_w(label, 1), hov = blow_hover((int)x, (int)y, w, 22);
    if (value) blow_rr((float)x, (float)y, 22, 22, 7, hov ? BT.accent_hi : BT.accent, 1.f, 0);
    else { blow_rr((float)x, (float)y, 22, 22, 7, BT.surface2, 1.f, 0); blow_rr((float)x, (float)y, 22, 22, 7, hov ? BT.accent : BT.border, 1.f, 1.5f); }
    if (value) { static const float pts[][2] = {{6,11.5f},{9.5f,15},{16,7.5f}};
        for (int s = 0; s < 2; s++) for (int i = 0; i <= 12; i++) { float t = i / 12.f, px = pts[s][0] + (pts[s+1][0] - pts[s][0]) * t, py = pts[s][1] + (pts[s+1][1] - pts[s][1]) * t;
            blow_rr((float)x + px - 1.6f, (float)y + py - 1.6f, 3.2f, 3.2f, 1.6f, 0xFFFFFF, 1.f, 0); } }
    blow_text((int)x + 30, (int)y + 7, label, BT.text, 1);
    return blow_clicked((int)x, (int)y, w, 22) ? !value : value;
}
int64_t purple_blow_slider(int64_t id, int64_t x, int64_t y, int64_t w, int64_t value, int64_t max) {
    if (max < 1) max = 1; if (value < 0) value = 0; if (value > max) value = max;
    int ax = (int)x - 10, ay = (int)y - 12, aw = (int)w + 20, ah = 30;
    if (g_mdown && !g_active && blow_in(g_pressx, g_pressy, ax, ay, aw, ah)) g_active = (int)id;
    if (g_active == (int)id && g_mdown) { int64_t v = (g_mx - x) * max / (w ? w : 1); value = v < 0 ? 0 : v > max ? max : v; }
    float kx = (float)x + (float)w * (float)value / (float)max; int hot = g_active == (int)id || blow_hover(ax, ay, aw, ah);
    blow_rr((float)x, (float)y - 3, (float)w, 6, 3, BT.surface2, 1.f, 0);
    blow_rr((float)x, (float)y - 3, kx - (float)x + 1, 6, 3, BT.accent, 1.f, 0);
    blow_rr(kx - 11, (float)y - 9, 22, 22, 11, 0x000000, 0.28f, 0);
    blow_rr(kx - 10, (float)y - 10, 20, 20, 10, hot ? BT.accent_hi : BT.accent, 1.f, 0);
    blow_rr(kx - 5, (float)y - 5, 10, 10, 5, 0xFFFFFF, 1.f, 0);
    return value;
}
int64_t purple_blow_progress(int64_t x, int64_t y, int64_t w, int64_t h, int64_t value, int64_t max) {
    if (max < 1) max = 1; if (value < 0) value = 0; if (value > max) value = max;
    blow_rr((float)x, (float)y, (float)w, (float)h, (float)h / 2, BT.surface2, 1.f, 0);
    if (value > 0) { float fw = (float)w * (float)value / (float)max; blow_rr((float)x, (float)y, fw, (float)h, (float)h / 2, BT.accent, 1.f, 0); }
    return value;
}
/* buf = 256-byte buffer (System.Memory.Alloc(256) is zeroed). Returns 1 on the frame Enter is pressed. */
int64_t purple_blow_textbox(int64_t id, int64_t x, int64_t y, int64_t w, char *buf) {
    int h = 38, focused;
    if (blow_clicked((int)x, (int)y, (int)w, h)) g_focus = (int)id;
    else if (g_released && g_focus == (int)id) g_focus = 0;
    focused = g_focus == (int)id; int enter = 0;
    if (focused && buf) { int k, n = (int)strlen(buf);
        while ((k = blow_keypop())) { if (k == 8 && n > 0) buf[--n] = 0; else if (k == 13) enter = 1; else if (k >= 32 && k < 127 && n < 255) { buf[n++] = (char)k; buf[n] = 0; } } }
    blow_rr((float)x, (float)y, (float)w, h, 12, BT.surface2, 1.f, 0);
    blow_rr((float)x, (float)y, (float)w, h, 12, focused ? BT.accent : BT.border, 1.f, focused ? 2.f : 1.f);
    int maxc = ((int)w - 24) / 16; const char *s = buf ? buf : ""; int n = (int)strlen(s); if (n > maxc) s += n - maxc;
    blow_text((int)x + 12, (int)y + 11, s, BT.text, 2);
    if (focused && (purple_blow_millis() / 500) % 2 == 0) blit_fill_rect((int)x + 12 + blow_text_w(s, 2) + 1, (int)y + 9, 2, 20, BT.accent_hi);
    return enter;
}
