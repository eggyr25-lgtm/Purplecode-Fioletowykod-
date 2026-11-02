#define _GNU_SOURCE
#include "purple_rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/syscall.h>
#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <poll.h>
#include <dlfcn.h>
#include <limits.h>
#include <stdbool.h>
#include <setjmp.h>
#include <sys/ioctl.h>
#include <linux/cdrom.h>
#include <scsi/sg.h>
#include <linux/fs.h>
#include <linux/usbdevice_fs.h>
#ifdef PURPLE_HAS_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#endif
#if defined(PURPLE_HAS_PNG) || defined(PURPLE_HAS_IMAGE)
#include <png.h>
#endif
#if defined(PURPLE_HAS_JPEG) || defined(PURPLE_HAS_IMAGE)
#include <jpeglib.h>
#endif

/* ========== Milestone 1 1.6 GUI: Software Blitter + X11 + Wayland ==========
 *
 * Core drawing goes into an ARGB8888 framebuffer (the blitter).
 * X11 and Wayland are presenters of that buffer.
 * This is the path that makes simple games (Pong-scale) actually pleasant.
 */

#ifdef PURPLE_HAS_X11
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
static Display *g_dpy = NULL;
static Window g_win = 0;
static GC g_gc = 0;
static XImage *g_ximg = NULL;
#endif

#ifdef PURPLE_HAS_WAYLAND
#include <wayland-client.h>
#include <sys/mman.h>
#include <sys/syscall.h>

/* ---- Minimal embedded xdg-shell client protocol (stable subset) ----
 * Avoids a hard dependency on wayland-protocols / generated headers.
 * Implements only xdg_wm_base + xdg_surface + xdg_toplevel for a basic
 * top-level window. Preferred over legacy wl_shell when the compositor
 * advertises xdg_wm_base. */
struct xdg_wm_base; struct xdg_surface; struct xdg_toplevel;
#define XDG_WM_BASE_DESTROY 0
#define XDG_WM_BASE_CREATE_POSITIONER 1
#define XDG_WM_BASE_GET_XDG_SURFACE 2
#define XDG_WM_BASE_PONG 3
#define XDG_SURFACE_DESTROY 0
#define XDG_SURFACE_GET_TOPLEVEL 1
#define XDG_SURFACE_ACK_CONFIGURE 4
#define XDG_TOPLEVEL_DESTROY 0
#define XDG_TOPLEVEL_SET_TITLE 2
#define XDG_TOPLEVEL_SET_APP_ID 3
struct xdg_wm_base_listener { void (*ping)(void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial); };
struct xdg_surface_listener { void (*configure)(void *data, struct xdg_surface *xdg_surface, uint32_t serial); };
struct xdg_toplevel_listener {
    void (*configure)(void *data, struct xdg_toplevel *xdg_toplevel, int32_t width, int32_t height, struct wl_array *states);
    void (*close)(void *data, struct xdg_toplevel *xdg_toplevel);
};
extern const struct wl_interface xdg_wm_base_interface;
extern const struct wl_interface xdg_surface_interface;
extern const struct wl_interface xdg_toplevel_interface;
static inline int xdg_wm_base_add_listener(struct xdg_wm_base *x, const struct xdg_wm_base_listener *l, void *d) {
    return wl_proxy_add_listener((struct wl_proxy *)x, (void (**)(void))(void *)l, d);
}
static inline void xdg_wm_base_pong(struct xdg_wm_base *x, uint32_t serial) {
    wl_proxy_marshal((struct wl_proxy *)x, XDG_WM_BASE_PONG, serial);
}
static inline struct xdg_surface *xdg_wm_base_get_xdg_surface(struct xdg_wm_base *x, struct wl_surface *surface) {
    struct wl_proxy *id = wl_proxy_marshal_constructor((struct wl_proxy *)x,
        XDG_WM_BASE_GET_XDG_SURFACE, &xdg_surface_interface, NULL, surface);
    return (struct xdg_surface *)id;
}
static inline int xdg_surface_add_listener(struct xdg_surface *x, const struct xdg_surface_listener *l, void *d) {
    return wl_proxy_add_listener((struct wl_proxy *)x, (void (**)(void))(void *)l, d);
}
static inline struct xdg_toplevel *xdg_surface_get_toplevel(struct xdg_surface *x) {
    struct wl_proxy *id = wl_proxy_marshal_constructor((struct wl_proxy *)x,
        XDG_SURFACE_GET_TOPLEVEL, &xdg_toplevel_interface, NULL);
    return (struct xdg_toplevel *)id;
}
static inline void xdg_surface_ack_configure(struct xdg_surface *x, uint32_t serial) {
    wl_proxy_marshal((struct wl_proxy *)x, XDG_SURFACE_ACK_CONFIGURE, serial);
}
static inline int xdg_toplevel_add_listener(struct xdg_toplevel *x, const struct xdg_toplevel_listener *l, void *d) {
    return wl_proxy_add_listener((struct wl_proxy *)x, (void (**)(void))(void *)l, d);
}
static inline void xdg_toplevel_set_title(struct xdg_toplevel *x, const char *title) {
    wl_proxy_marshal((struct wl_proxy *)x, XDG_TOPLEVEL_SET_TITLE, title);
}
static inline void xdg_toplevel_set_app_id(struct xdg_toplevel *x, const char *app_id) {
    wl_proxy_marshal((struct wl_proxy *)x, XDG_TOPLEVEL_SET_APP_ID, app_id);
}
static inline void xdg_toplevel_set_min_size(struct xdg_toplevel *x, int32_t w, int32_t h) {
    wl_proxy_marshal((struct wl_proxy *)x, 8 /* set_min_size */, w, h);
}
static const struct wl_interface *purple_xdg_types[8];
static const struct wl_message purple_xdg_wm_base_requests[] = {
    { "destroy", "", purple_xdg_types + 0 },
    { "create_positioner", "n", purple_xdg_types + 0 },
    { "get_xdg_surface", "no", purple_xdg_types + 1 },
    { "pong", "u", purple_xdg_types + 0 },
};
static const struct wl_message purple_xdg_wm_base_events[] = {
    { "ping", "u", purple_xdg_types + 0 },
};
const struct wl_interface xdg_wm_base_interface = {
    "xdg_wm_base", 1, 4, purple_xdg_wm_base_requests, 1, purple_xdg_wm_base_events
};
static const struct wl_message purple_xdg_surface_requests[] = {
    { "destroy", "", purple_xdg_types + 0 },
    { "get_toplevel", "n", purple_xdg_types + 3 },
    { "get_popup", "n?oo", purple_xdg_types + 0 },
    { "set_window_geometry", "iiii", purple_xdg_types + 0 },
    { "ack_configure", "u", purple_xdg_types + 0 },
};
static const struct wl_message purple_xdg_surface_events[] = {
    { "configure", "u", purple_xdg_types + 0 },
};
const struct wl_interface xdg_surface_interface = {
    "xdg_surface", 1, 5, purple_xdg_surface_requests, 1, purple_xdg_surface_events
};
static const struct wl_message purple_xdg_toplevel_requests[] = {
    { "destroy", "", purple_xdg_types + 0 },
    { "set_parent", "?o", purple_xdg_types + 0 },
    { "set_title", "s", purple_xdg_types + 0 },
    { "set_app_id", "s", purple_xdg_types + 0 },
    { "show_window_menu", "ouii", purple_xdg_types + 0 },
    { "move", "ou", purple_xdg_types + 0 },
    { "resize", "ouu", purple_xdg_types + 0 },
    { "set_max_size", "ii", purple_xdg_types + 0 },
    { "set_min_size", "ii", purple_xdg_types + 0 },
    { "set_maximized", "", purple_xdg_types + 0 },
    { "unset_maximized", "", purple_xdg_types + 0 },
    { "set_fullscreen", "?o", purple_xdg_types + 0 },
    { "unset_fullscreen", "", purple_xdg_types + 0 },
    { "set_minimized", "", purple_xdg_types + 0 },
};
static const struct wl_message purple_xdg_toplevel_events[] = {
    { "configure", "iia", purple_xdg_types + 0 },
    { "close", "", purple_xdg_types + 0 },
};
const struct wl_interface xdg_toplevel_interface = {
    "xdg_toplevel", 1, 14, purple_xdg_toplevel_requests, 2, purple_xdg_toplevel_events
};
/* ---- Minimal embedded xdg-decoration-unstable-v1 ----
 * Lets us ask the compositor (KDE, Sway, Hyprland, ...) for server-side
 * decorations. Without this, native Wayland toplevels get NO titlebar. */
struct zxdg_decoration_manager_v1; struct zxdg_toplevel_decoration_v1;
#define ZXDG_DECO_MGR_GET_TOPLEVEL_DECORATION 1
#define ZXDG_TOPLEVEL_DECO_SET_MODE 1
#define ZXDG_DECO_MODE_CLIENT_SIDE 1
#define ZXDG_DECO_MODE_SERVER_SIDE 2
struct zxdg_toplevel_decoration_v1_listener {
    void (*configure)(void *data, struct zxdg_toplevel_decoration_v1 *d, uint32_t mode);
};
extern const struct wl_interface zxdg_decoration_manager_v1_interface;
extern const struct wl_interface zxdg_toplevel_decoration_v1_interface;
static const struct wl_interface *purple_zdeco_types[2];
static const struct wl_message purple_zdeco_mgr_requests[] = {
    { "destroy", "", purple_zdeco_types + 0 },
    { "get_toplevel_decoration", "no", purple_zdeco_types + 0 },
};
const struct wl_interface zxdg_decoration_manager_v1_interface = {
    "zxdg_decoration_manager_v1", 1, 2, purple_zdeco_mgr_requests, 0, NULL
};
static const struct wl_message purple_zdeco_requests[] = {
    { "destroy", "", purple_zdeco_types + 0 },
    { "set_mode", "u", purple_zdeco_types + 0 },
    { "unset_mode", "", purple_zdeco_types + 0 },
};
static const struct wl_message purple_zdeco_events[] = {
    { "configure", "u", purple_zdeco_types + 0 },
};
const struct wl_interface zxdg_toplevel_decoration_v1_interface = {
    "zxdg_toplevel_decoration_v1", 1, 3, purple_zdeco_requests, 1, purple_zdeco_events
};
static void purple_xdg_init_types(void) {
    purple_xdg_types[0] = NULL;
    purple_xdg_types[1] = &xdg_surface_interface;
    purple_xdg_types[2] = &wl_surface_interface;
    purple_xdg_types[3] = &xdg_toplevel_interface;
    /* get_toplevel_decoration: new_id + xdg_toplevel object */
    purple_zdeco_types[0] = &zxdg_toplevel_decoration_v1_interface;
    purple_zdeco_types[1] = &xdg_toplevel_interface;
}

static struct wl_display *g_wl_dpy = NULL;
static struct wl_registry *g_wl_reg = NULL;
static struct wl_compositor *g_wl_comp = NULL;
static struct wl_shm *g_wl_shm = NULL;
static struct wl_shell *g_wl_shell = NULL;
static struct xdg_wm_base *g_xdg_wm = NULL;
static struct xdg_surface *g_xdg_surf = NULL;
static struct xdg_toplevel *g_xdg_top = NULL;
static struct zxdg_decoration_manager_v1 *g_deco_mgr = NULL;
static struct zxdg_toplevel_decoration_v1 *g_deco = NULL;
static uint32_t g_deco_mode = 0;
static int g_xdg_configured = 0;
static struct wl_seat *g_wl_seat = NULL;
static struct wl_keyboard *g_wl_kbd = NULL;
static struct wl_pointer *g_wl_ptr = NULL;
static int g_wl_mouse_x = 0, g_wl_mouse_y = 0, g_wl_press_x = 0, g_wl_press_y = 0, g_wl_mouse_down = 0, g_wl_mouse_released = 0;
static struct wl_surface *g_wl_surf = NULL;
static struct wl_shell_surface *g_wl_ssurf = NULL;
static struct wl_buffer *g_wl_buf = NULL;
static int g_wl_fd = -1;
static void *g_wl_map = NULL;
static int g_wl_ready = 0;
/* HiDPI / buffer scale. Logical size is what Blow apps see; physical = logical * scale. */
static int g_wl_scale = 1;
static int g_logical_w = 0, g_logical_h = 0;
static int g_wl_buf_w = 0, g_wl_buf_h = 0; /* physical buffer dimensions */
/* Simple key queue (same codes as X11 path for compatibility) */
#define WL_KEYQ 32
static int g_wl_keyq[WL_KEYQ];
static int g_wl_keyq_r = 0, g_wl_keyq_w = 0;
static void wl_keyq_push(int code) {
    int n = (g_wl_keyq_w + 1) % WL_KEYQ;
    if (n == g_wl_keyq_r) return; /* full */
    g_wl_keyq[g_wl_keyq_w] = code;
    g_wl_keyq_w = n;
}
static int wl_keyq_pop(void) {
    if (g_wl_keyq_r == g_wl_keyq_w) return 0;
    int c = g_wl_keyq[g_wl_keyq_r];
    g_wl_keyq_r = (g_wl_keyq_r + 1) % WL_KEYQ;
    return c;
}
static void wl_dispatch_nonblock(void) {
    if(!g_wl_dpy)return;
    while(wl_display_prepare_read(g_wl_dpy)!=0) wl_display_dispatch_pending(g_wl_dpy);
    wl_display_flush(g_wl_dpy);
    struct pollfd pfd={wl_display_get_fd(g_wl_dpy),POLLIN,0};
    int r=poll(&pfd,1,0);
    if(r>0 && (pfd.revents&POLLIN)) { if(wl_display_read_events(g_wl_dpy)==0) wl_display_dispatch_pending(g_wl_dpy); }
    else wl_display_cancel_read(g_wl_dpy);
}
/* Linux evdev-style keycodes → same numbers X11 path returns */
static int wl_map_keycode(uint32_t key) {
    /* key is evdev code (not X keysym). Common set: */
    switch (key) {
        case 1:  return 27;    /* ESC */
        case 57: return 32;    /* SPACE */
        case 105: return 1000; /* LEFT */
        case 106: return 1001; /* RIGHT */
        case 103: return 1002; /* UP */
        case 108: return 1003; /* DOWN */
        case 17: return 'w';
        case 31: return 's';
        case 30: return 'a';
        case 32: return 'd';
        case 16: return 'q';
        case 18: return 'e';
        case 19: return 'r';
        case 33: return 'f';
        case 25: return 'p';
        case 28: return 10;    /* ENTER as newline-ish */
        default:
            if (key >= 2 && key <= 11) { /* 1-0 keys */
                static const char dig[] = "1234567890";
                return dig[key - 2];
            }
            return 0;
    }
}
#endif

/* Shared software framebuffer (ARGB8888, host endian) */
static uint32_t *g_fb = NULL;
static int g_fb_w = 0, g_fb_h = 0, g_fb_stride = 0; /* stride in pixels */
static int g_gui_backend = 0; /* 0=none 1=X11 2=Wayland */
static int g_win_open = 0;
static int g_decorationless = 0; /* 1 = no titlebar/borders (client request) */
/* Pending resize from WM/compositor (0 = none). */
static int g_resize_w = 0, g_resize_h = 0;
static size_t g_wl_map_size = 0;
static int purple_gui_resize(int w, int h); /* defined after Wayland SHM helpers */

/* ---- Blitter primitives (backend-agnostic) ---- */
static void blit_ensure(int w, int h) {
    if (w < 64) w = 64;
    if (h < 64) h = 64;
    if (w > 8192) w = 8192;
    if (h > 8192) h = 8192;
    if (g_fb && g_fb_w == w && g_fb_h == h) return;
    free(g_fb);
    g_fb_w = w; g_fb_h = h; g_fb_stride = w;
    g_fb = (uint32_t *)calloc((size_t)w * (size_t)h, sizeof(uint32_t));
}

static inline void blit_set_pixel(int x, int y, uint32_t color) {
    if (!g_fb || x < 0 || y < 0 || x >= g_fb_w || y >= g_fb_h) return;
    /* Accept 0xRRGGBB or 0xAARRGGBB; force opaque if no alpha */
    if ((color & 0xFF000000u) == 0) color |= 0xFF000000u;
    g_fb[y * g_fb_stride + x] = color;
}

static void blit_fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (!g_fb || w <= 0 || h <= 0) return;
    if ((color & 0xFF000000u) == 0) color |= 0xFF000000u;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_fb_w) w = g_fb_w - x;
    if (y + h > g_fb_h) h = g_fb_h - y;
    if (w <= 0 || h <= 0) return;
    for (int row = y; row < y + h; row++) {
        uint32_t *p = g_fb + row * g_fb_stride + x;
        for (int col = 0; col < w; col++) p[col] = color;
    }
}

static void blit_clear(uint32_t color) {
    if (!g_fb) return;
    if ((color & 0xFF000000u) == 0) color |= 0xFF000000u;
    size_t n = (size_t)g_fb_w * (size_t)g_fb_h;
    for (size_t i = 0; i < n; i++) g_fb[i] = color;
}

/* Bresenham line into the software blitter. Clipped per-pixel. */
static void blit_draw_line(int x0, int y0, int x1, int y1, uint32_t color) {
    if (!g_fb) return;
    if ((color & 0xFF000000u) == 0) color |= 0xFF000000u;
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int sx = (x0 < x1) ? 1 : -1;
    int dy = (y1 > y0) ? (y0 - y1) : (y1 - y0);
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        blit_set_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err << 1;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}


/* ========== Milestone 1 1.6: Built-in 8x8 bitmap font + text drawing ==========
 * Covers printable ASCII 32-126. Glyphs are 8 bytes (one row each).
 * Drawing is pure software into the ARGB8888 blitter; works on X11 and Wayland.
 */
static const unsigned char purple_font8x8[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* space */
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, /* ! */
    {0x6C,0x6C,0x24,0x00,0x00,0x00,0x00,0x00}, /* " */
    {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00}, /* # */
    {0x18,0x7E,0xC0,0x7C,0x06,0xFC,0x18,0x00}, /* $ */
    {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00}, /* % */
    {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00}, /* & */
    {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00}, /* ' */
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, /* ( */
    {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00}, /* ) */
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, /* * */
    {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00}, /* + */
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, /* , */
    {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}, /* - */
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, /* . */
    {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00}, /* / */
    {0x7C,0xC6,0xCE,0xD6,0xE6,0xC6,0x7C,0x00}, /* 0 */
    {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00}, /* 1 */
    {0x7C,0xC6,0x06,0x1C,0x30,0x66,0xFE,0x00}, /* 2 */
    {0x7C,0xC6,0x06,0x3C,0x06,0xC6,0x7C,0x00}, /* 3 */
    {0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x1E,0x00}, /* 4 */
    {0xFE,0xC0,0xFC,0x06,0x06,0xC6,0x7C,0x00}, /* 5 */
    {0x38,0x60,0xC0,0xFC,0xC6,0xC6,0x7C,0x00}, /* 6 */
    {0xFE,0xC6,0x0C,0x18,0x30,0x30,0x30,0x00}, /* 7 */
    {0x7C,0xC6,0xC6,0x7C,0xC6,0xC6,0x7C,0x00}, /* 8 */
    {0x7C,0xC6,0xC6,0x7E,0x06,0x0C,0x78,0x00}, /* 9 */
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00}, /* : */
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30}, /* ; */
    {0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x00}, /* < */
    {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00}, /* = */
    {0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x00}, /* > */
    {0x7C,0xC6,0x0C,0x18,0x18,0x00,0x18,0x00}, /* ? */
    {0x7C,0xC6,0xDE,0xDE,0xDE,0xC0,0x7C,0x00}, /* @ */
    {0x30,0x78,0xCC,0xCC,0xFC,0xCC,0xCC,0x00}, /* A */
    {0xFC,0x66,0x66,0x7C,0x66,0x66,0xFC,0x00}, /* B */
    {0x3C,0x66,0xC0,0xC0,0xC0,0x66,0x3C,0x00}, /* C */
    {0xF8,0x6C,0x66,0x66,0x66,0x6C,0xF8,0x00}, /* D */
    {0xFE,0x62,0x68,0x78,0x68,0x62,0xFE,0x00}, /* E */
    {0xFE,0x62,0x68,0x78,0x68,0x60,0xF0,0x00}, /* F */
    {0x3C,0x66,0xC0,0xC0,0xCE,0x66,0x3A,0x00}, /* G */
    {0xCC,0xCC,0xCC,0xFC,0xCC,0xCC,0xCC,0x00}, /* H */
    {0x78,0x30,0x30,0x30,0x30,0x30,0x78,0x00}, /* I */
    {0x1E,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00}, /* J */
    {0xE6,0x66,0x6C,0x78,0x6C,0x66,0xE6,0x00}, /* K */
    {0xF0,0x60,0x60,0x60,0x62,0x66,0xFE,0x00}, /* L */
    {0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0x00}, /* M */
    {0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00}, /* N */
    {0x38,0x6C,0xC6,0xC6,0xC6,0x6C,0x38,0x00}, /* O */
    {0xFC,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00}, /* P */
    {0x7C,0xC6,0xC6,0xC6,0xD6,0x7C,0x0E,0x00}, /* Q */
    {0xFC,0x66,0x66,0x7C,0x6C,0x66,0xE6,0x00}, /* R */
    {0x7C,0xC6,0x60,0x38,0x0C,0xC6,0x7C,0x00}, /* S */
    {0xFC,0xB4,0x30,0x30,0x30,0x30,0x78,0x00}, /* T */
    {0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xFC,0x00}, /* U */
    {0xCC,0xCC,0xCC,0xCC,0xCC,0x78,0x30,0x00}, /* V */
    {0xC6,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0x00}, /* W */
    {0xC6,0xC6,0x6C,0x38,0x6C,0xC6,0xC6,0x00}, /* X */
    {0xCC,0xCC,0xCC,0x78,0x30,0x30,0x78,0x00}, /* Y */
    {0xFE,0xC6,0x8C,0x18,0x32,0x66,0xFE,0x00}, /* Z */
    {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00}, /* [ */
    {0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00}, /* \ */
    {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00}, /* ] */
    {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00}, /* ^ */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, /* _ */
    {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00}, /* ` */
    {0x00,0x00,0x78,0x0C,0x7C,0xCC,0x76,0x00}, /* a */
    {0xE0,0x60,0x7C,0x66,0x66,0x66,0xDC,0x00}, /* b */
    {0x00,0x00,0x7C,0xC6,0xC0,0xC6,0x7C,0x00}, /* c */
    {0x1C,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00}, /* d */
    {0x00,0x00,0x7C,0xC6,0xFE,0xC0,0x7C,0x00}, /* e */
    {0x3C,0x66,0x60,0xF8,0x60,0x60,0xF0,0x00}, /* f */
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0xF8}, /* g */
    {0xE0,0x60,0x6C,0x76,0x66,0x66,0xE6,0x00}, /* h */
    {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00}, /* i */
    {0x06,0x00,0x06,0x06,0x06,0x66,0x66,0x3C}, /* j */
    {0xE0,0x60,0x66,0x6C,0x78,0x6C,0xE6,0x00}, /* k */
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, /* l */
    {0x00,0x00,0xEC,0xFE,0xD6,0xD6,0xC6,0x00}, /* m */
    {0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x00}, /* n */
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0x00}, /* o */
    {0x00,0x00,0xDC,0x66,0x66,0x7C,0x60,0xF0}, /* p */
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0x1E}, /* q */
    {0x00,0x00,0xDC,0x76,0x60,0x60,0xF0,0x00}, /* r */
    {0x00,0x00,0x7E,0xC0,0x7C,0x06,0xFC,0x00}, /* s */
    {0x30,0x30,0xFC,0x30,0x30,0x36,0x1C,0x00}, /* t */
    {0x00,0x00,0xCC,0xCC,0xCC,0xCC,0x76,0x00}, /* u */
    {0x00,0x00,0xCC,0xCC,0xCC,0x78,0x30,0x00}, /* v */
    {0x00,0x00,0xC6,0xD6,0xD6,0xFE,0x6C,0x00}, /* w */
    {0x00,0x00,0xC6,0x6C,0x38,0x6C,0xC6,0x00}, /* x */
    {0x00,0x00,0xC6,0xC6,0xC6,0x7E,0x06,0xFC}, /* y */
    {0x00,0x00,0xFE,0x8C,0x18,0x32,0xFE,0x00}, /* z */
    {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00}, /* { */
    {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, /* | */
    {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00}, /* } */
    {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00}  /* ~ */
};

#define PURPLE_FONT_W 16
#define PURPLE_FONT_H 16

static void blit_blend_pixel(int x,int y,uint32_t color,unsigned char alpha) {
    if(!g_fb||x<0||y<0||x>=g_fb_w||y>=g_fb_h||!alpha)return;
    uint32_t *p=&g_fb[y*g_fb_stride+x];unsigned a=alpha,ia=255-a;
    unsigned sr=(color>>16)&255,sg=(color>>8)&255,sb=color&255,dr=(*p>>16)&255,dg=(*p>>8)&255,db=*p&255;
    *p=0xFF000000u|(((sr*a+dr*ia)/255)<<16)|(((sg*a+dg*ia)/255)<<8)|((sb*a+db*ia)/255);
}
static unsigned int purple_utf8_next(const unsigned char **pp){
    const unsigned char *p=*pp;unsigned int c=*p++;if(c<0x80){*pp=p;return c;}
    if((c&0xE0)==0xC0 && p[0] && (p[0]&0xC0)==0x80){unsigned int v=((c&0x1F)<<6)|(p[0]&0x3F);*pp=p+1;return v;}
    if((c&0xF0)==0xE0 && p[0] && p[1] && (p[0]&0xC0)==0x80 && (p[1]&0xC0)==0x80){unsigned int v=((c&15)<<12)|((p[0]&63)<<6)|(p[1]&63);*pp=p+2;return v;}
    if((c&0xF8)==0xF0 && p[0] && p[1] && p[2] && (p[0]&0xC0)==0x80 && (p[1]&0xC0)==0x80 && (p[2]&0xC0)==0x80){unsigned int v=((c&7)<<18)|((p[0]&63)<<12)|((p[1]&63)<<6)|(p[2]&63);*pp=p+3;return v;}
    *pp=p;return '?';
}
/* Permanent built-in faces (always recognised by Blow.Font / purple_font_select):
 *   0  "Builtin Emergency Font"          – soft 16×16 coverage over the 8×8 bitmap (no FreeType needed)
 *   1  "FantasqueSansM Nerd Font Propo"  – preferred scalable face
 *   2  "Arial"                           – Arial / Liberation / DejaVu fallback
 *
 * Deep FreeType embedding of font files as binary data (primary path):
 *   1. Blow.IFont(name, data, len, encoding)  – encoding "Raw" | "Base64" | "Hex"
 *      Bytes stay in process memory; FT_New_Memory_Face is used. Preferred over
 *      any system path search.
 *   2. Compile-time embedding: define PURPLE_EMBEDDED_FONT_DATA / SIZE / NAME
 *      (e.g. via a generated .c array) before including the runtime; the blob is
 *      registered automatically at first font select.
 *   3. System path search is only a last-resort fallback when no registered or
 *      embedded face matches the requested name.
 */
#ifdef PURPLE_HAS_FREETYPE
static FT_Library g_ft_library; static FT_Face g_ft_face; static int g_ft_ready;
static int purple_ft_init(void){if(g_ft_ready)return 1;if(FT_Init_FreeType(&g_ft_library))return 0;g_ft_ready=1;return 1;}
static int purple_ft_load_path(const char *path){if(!path||!*path)return 0;if(!purple_ft_init())return 0;FT_Face f=NULL;if(FT_New_Face(g_ft_library,path,0,&f))return 0;if(g_ft_face)FT_Done_Face(g_ft_face);g_ft_face=f;return 1;}
static int purple_ft_load_memory(const unsigned char *data, size_t size){
    if(!data||size==0)return 0;
    if(!purple_ft_init())return 0;
    FT_Face f=NULL;
    if(FT_New_Memory_Face(g_ft_library, data, (FT_Long)size, 0, &f))return 0;
    if(g_ft_face)FT_Done_Face(g_ft_face);
    g_ft_face=f;
    return 1;
}
/* Optional compile-time embedded face (application supplies the array).
 * Define before compiling the runtime:
 *   -DPURPLE_EMBEDDED_FONT_DATA=my_font_bytes
 *   -DPURPLE_EMBEDDED_FONT_SIZE=sizeof(my_font_bytes)
 *   -DPURPLE_EMBEDDED_FONT_NAME=\"MyFace\"   (optional; default Fantasque…)
 * The blob is loaded via FT_New_Memory_Face – no system path search required. */
#if defined(PURPLE_EMBEDDED_FONT_DATA) && defined(PURPLE_EMBEDDED_FONT_SIZE)
#ifndef PURPLE_EMBEDDED_FONT_NAME
#define PURPLE_EMBEDDED_FONT_NAME "FantasqueSansM Nerd Font Propo"
#endif
static int purple_embedded_font_loaded = 0;
static int purple_try_embedded_font(const char *name) {
    if (!name || strcmp(name, PURPLE_EMBEDDED_FONT_NAME) != 0) return 0;
    if (purple_embedded_font_loaded && g_ft_face) return 1;
    if (purple_ft_load_memory((const unsigned char *)PURPLE_EMBEDDED_FONT_DATA,
                              (size_t)PURPLE_EMBEDDED_FONT_SIZE)) {
        purple_embedded_font_loaded = 1;
        return 1;
    }
    return 0;
}
#else
static int purple_try_embedded_font(const char *name) { (void)name; return 0; }
#endif
static int purple_ft_select_system(const char *name){
    const char *paths[24]={0}; char homepath[1024]; size_t n=0;
    const char *home = getenv("HOME");
    if(!strcmp(name,"FantasqueSansM Nerd Font Propo")){
        paths[n++]="/usr/share/fonts/truetype/fantasque-sans-mono/FantasqueSansMNerdFontPropo-Regular.ttf";
        paths[n++]="/usr/share/fonts/TTF/FantasqueSansMNerdFontPropo-Regular.ttf";
        paths[n++]="/usr/share/fonts/fantasque-sans-mono/FantasqueSansMNerdFontPropo-Regular.ttf";
        paths[n++]="/usr/local/share/fonts/FantasqueSansMNerdFontPropo-Regular.ttf";
        paths[n++]="/usr/share/fonts/truetype/FantasqueSansMNerdFontPropo-Regular.ttf";
        paths[n++]="/usr/share/fonts/opentype/fantasque-sans-mono/FantasqueSansMNerdFontPropo-Regular.otf";
        /* common Nerd Font packaging variants */
        paths[n++]="/usr/share/fonts/TTF/FantasqueSansM Nerd Font Propo Regular.ttf";
        paths[n++]="/usr/share/fonts/truetype/nerd-fonts/FantasqueSansMNerdFontPropo-Regular.ttf";
        if(home){
            snprintf(homepath,sizeof homepath,"%s/.local/share/fonts/FantasqueSansMNerdFontPropo-Regular.ttf",home); paths[n++]=homepath;
            snprintf(homepath,sizeof homepath,"%s/.fonts/FantasqueSansMNerdFontPropo-Regular.ttf",home); paths[n++]=homepath;
        }
    } else if(!strcmp(name,"Arial")){
        paths[n++]="/usr/share/fonts/truetype/msttcorefonts/Arial.ttf";
        paths[n++]="/usr/share/fonts/truetype/msttcorefonts/arial.ttf";
        paths[n++]="/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf";
        paths[n++]="/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf";
        paths[n++]="/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
        paths[n++]="/usr/share/fonts/TTF/DejaVuSans.ttf";
        paths[n++]="/usr/share/fonts/truetype/freefont/FreeSans.ttf";
        paths[n++]="/usr/share/fonts/gnu-free/FreeSans.otf";
        if(home){
            snprintf(homepath,sizeof homepath,"%s/.local/share/fonts/Arial.ttf",home); paths[n++]=homepath;
            snprintf(homepath,sizeof homepath,"%s/.fonts/Arial.ttf",home); paths[n++]=homepath;
        }
    }
    for(size_t i=0;i<n;i++) if(paths[i] && purple_ft_load_path(paths[i])) return 1;
    return 0;
}
static int purple_ft_draw_glyph(int x,int y,unsigned int cp,uint32_t color,int px){
    if(!g_ft_face||FT_Set_Pixel_Sizes(g_ft_face,0,(FT_UInt)px)||FT_Load_Char(g_ft_face,cp,FT_LOAD_RENDER))return 0;
    FT_GlyphSlot g=g_ft_face->glyph;FT_Bitmap *b=&g->bitmap;int pitch=b->pitch;
    for(unsigned row=0;row<b->rows;row++){const unsigned char *line=pitch>=0?b->buffer+row*pitch:b->buffer+(b->rows-1-row)*(-pitch);
        for(unsigned col=0;col<b->width;col++){unsigned char a=b->pixel_mode==FT_PIXEL_MODE_MONO?((line[col>>3]&(0x80>>(col&7)))?255:0):line[col];if(a)blit_blend_pixel(x+g->bitmap_left+(int)col,y+(int)px-(int)g->bitmap_top+(int)row,color,a);}}
    return g->advance.x>>6;
}
static int purple_ft_measure(const char *text,int px){if(!g_ft_face||!text||FT_Set_Pixel_Sizes(g_ft_face,0,(FT_UInt)px))return -1;int w=0;const unsigned char*p=(const unsigned char*)text;while(*p){unsigned int cp=purple_utf8_next(&p);if(FT_Load_Char(g_ft_face,cp,FT_LOAD_DEFAULT))continue;w+=(int)(g_ft_face->glyph->advance.x>>6);}return w;}
#else
static int purple_ft_select_system(const char *name){(void)name;return 0;}
static int purple_ft_draw_glyph(int x,int y,unsigned int cp,uint32_t color,int px){(void)x;(void)y;(void)cp;(void)color;(void)px;return 0;}
static int purple_ft_measure(const char *text,int px){(void)text;(void)px;return -1;}
#endif

static unsigned char purple_emergency_coverage(const unsigned char *glyph,int row,int col){
    int sr=row/2,sc=col/2;unsigned char bit=(unsigned char)(0x80>>sc);if(!(glyph[sr]&bit))return 0;
    int edge=0;if(row%2==0&&sr>0&&!(glyph[sr-1]&bit))edge=1;
    if(row%2==1&&sr<7&&!(glyph[sr+1]&bit))edge=1;
    if(col%2==0&&sc>0&&!(glyph[sr]&(0x80>>(sc-1))))edge=1;
    if(col%2==1&&sc<7&&!(glyph[sr]&(0x80>>(sc+1))))edge=1;
    return edge?208:255;
}

static void blit_draw_char(int x, int y, int ch, uint32_t color) {
    if (!g_fb) return;
    if (purple_ft_draw_glyph(x,y,(unsigned int)(ch<0?'?':ch),color,16)) return;
    if (ch < 32 || ch > 126) ch = '?';
    const unsigned char *glyph = purple_font8x8[ch - 32];
    if ((color & 0xFF000000u) == 0) color |= 0xFF000000u;
    for (int row = 0; row < 16; row++) for (int col = 0; col < 16; col++) {
        unsigned char coverage=purple_emergency_coverage(glyph,row,col);
        if(coverage)blit_blend_pixel(x+col,y+row,color,coverage);
    }
}

static void blit_draw_text(int x, int y, const char *text, uint32_t color) {
    if (!g_fb || !text) return;
    int cx = x;
    for (const char *p = text; *p; p++) {
        if (*p == '\n') {
            cx = x;
            y += PURPLE_FONT_H + 1;
            continue;
        }
        blit_draw_char(cx, y, (unsigned char)*p, color);
        cx += PURPLE_FONT_W;
    }
}

static int64_t purple_gui_draw_text(int64_t x, int64_t y, const char *text, int64_t color) {
    blit_draw_text((int)x, (int)y, text ? text : "", (uint32_t)color);
    return 0;
}

static int64_t purple_gui_draw_char(int64_t x, int64_t y, int64_t ch, int64_t color) {
    blit_draw_char((int)x, (int)y, (int)ch, (uint32_t)color);
    return 0;
}


/* Copy a rectangular region from src (RGB24 or ARGB32) into the framebuffer */
static void blit_image_rgb(int dst_x, int dst_y, int src_w, int src_h,
                           const unsigned char *rgb, int src_stride_bytes, int has_alpha) {
    if (!g_fb || !rgb || src_w <= 0 || src_h <= 0) return;
    for (int y = 0; y < src_h; y++) {
        int dy = dst_y + y;
        if (dy < 0 || dy >= g_fb_h) continue;
        for (int x = 0; x < src_w; x++) {
            int dx = dst_x + x;
            if (dx < 0 || dx >= g_fb_w) continue;
            const unsigned char *px = rgb + y * src_stride_bytes + x * (has_alpha ? 4 : 3);
            uint32_t c;
            if (has_alpha) {
                c = ((uint32_t)px[3] << 24) | ((uint32_t)px[0] << 16) | ((uint32_t)px[1] << 8) | px[2];
            } else {
                c = 0xFF000000u | ((uint32_t)px[0] << 16) | ((uint32_t)px[1] << 8) | px[2];
            }
            g_fb[dy * g_fb_stride + dx] = c;
        }
    }
}

/* ---- Wayland registry + shell surface helpers ---- */
#ifdef PURPLE_HAS_WAYLAND
static int g_wl_has_xdg = 0;
static void xdg_wm_base_ping(void *data, struct xdg_wm_base *wm, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(wm, serial);
}
static const struct xdg_wm_base_listener g_xdg_wm_listener = { xdg_wm_base_ping };
static void xdg_surface_configure(void *data, struct xdg_surface *surf, uint32_t serial) {
    (void)data;
    xdg_surface_ack_configure(surf, serial);
    g_xdg_configured = 1;
}
static const struct xdg_surface_listener g_xdg_surf_listener = { xdg_surface_configure };
static void xdg_toplevel_configure(void *data, struct xdg_toplevel *top,
                                   int32_t width, int32_t height, struct wl_array *states) {
    (void)data; (void)top; (void)states;
    /* Compositor-suggested size (0,0 means client may pick). Queue auto-resize. */
    if (width > 0 && height > 0) {
        g_resize_w = (int)width;
        g_resize_h = (int)height;
    }
}
static void xdg_toplevel_close(void *data, struct xdg_toplevel *top) {
    (void)data; (void)top;
    g_win_open = 0;
}
static void zdeco_configure(void *data, struct zxdg_toplevel_decoration_v1 *d, uint32_t mode) {
    (void)data; (void)d; g_deco_mode = mode;
}
static const struct zxdg_toplevel_decoration_v1_listener g_zdeco_listener = { zdeco_configure };
static const struct xdg_toplevel_listener g_xdg_top_listener = {
    xdg_toplevel_configure, xdg_toplevel_close
};
/* ---- wl_output scale tracking (integer HiDPI) ---- */
static void wl_output_geometry(void *data, struct wl_output *output,
                               int32_t x, int32_t y, int32_t phys_w, int32_t phys_h,
                               int32_t subpixel, const char *make, const char *model, int32_t transform) {
    (void)data; (void)output; (void)x; (void)y; (void)phys_w; (void)phys_h;
    (void)subpixel; (void)make; (void)model; (void)transform;
}
static void wl_output_mode(void *data, struct wl_output *output,
                           uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
    (void)data; (void)output; (void)flags; (void)width; (void)height; (void)refresh;
}
static void wl_output_done(void *data, struct wl_output *output) {
    (void)data; (void)output;
}
static void wl_output_scale(void *data, struct wl_output *output, int32_t factor) {
    (void)data; (void)output;
    if (factor < 1) factor = 1;
    if (factor > 8) factor = 8;
    if (factor > g_wl_scale) {
        g_wl_scale = factor;
        /* Queue a resize so the next Poll/End recreates the buffer at the new scale */
        if (g_wl_ready && g_logical_w > 0 && g_logical_h > 0) {
            g_resize_w = g_logical_w;
            g_resize_h = g_logical_h;
        }
    }
}
static const struct wl_output_listener g_output_listener = {
    wl_output_geometry, wl_output_mode, wl_output_done, wl_output_scale
};

static void wl_registry_global(void *data, struct wl_registry *reg,
                               uint32_t id, const char *iface, uint32_t ver) {
    (void)data;
    if (strcmp(iface, "wl_compositor") == 0) {
        g_wl_comp = wl_registry_bind(reg, id, &wl_compositor_interface, 1);
    } else if (strcmp(iface, "wl_shm") == 0) {
        g_wl_shm = wl_registry_bind(reg, id, &wl_shm_interface, 1);
    } else if (strcmp(iface, "wl_shell") == 0) {
        g_wl_shell = wl_registry_bind(reg, id, &wl_shell_interface, 1);
    } else if (strcmp(iface, "wl_seat") == 0) {
        g_wl_seat = wl_registry_bind(reg, id, &wl_seat_interface, 1);
    } else if (strcmp(iface, "wl_output") == 0) {
        /* version 2+ advertises scale; bind at least 2 when available */
        uint32_t bind_ver = ver >= 2 ? 2 : 1;
        struct wl_output *out = wl_registry_bind(reg, id, &wl_output_interface, bind_ver);
        if (out && bind_ver >= 2)
            wl_output_add_listener(out, &g_output_listener, NULL);
    } else if (strcmp(iface, "zxdg_decoration_manager_v1") == 0) {
        g_deco_mgr = wl_registry_bind(reg, id, &zxdg_decoration_manager_v1_interface, 1);
    } else if (strcmp(iface, "xdg_wm_base") == 0) {
        /* Modern xdg-shell – bind and prefer over legacy wl_shell. */
        g_xdg_wm = wl_registry_bind(reg, id, &xdg_wm_base_interface, 1);
        if (g_xdg_wm) {
            g_wl_has_xdg = 1;
            xdg_wm_base_add_listener(g_xdg_wm, &g_xdg_wm_listener, NULL);
        }
    }
}
static void wl_registry_global_remove(void *data, struct wl_registry *reg, uint32_t id) {
    (void)data; (void)reg; (void)id;
}
static const struct wl_registry_listener g_reg_listener = {
    wl_registry_global, wl_registry_global_remove
};

#ifdef PURPLE_HAS_WAYLAND
static void wl_kbd_keymap(void *data, struct wl_keyboard *kbd, uint32_t fmt, int32_t fd, uint32_t size) {
    (void)data; (void)kbd; (void)fmt; (void)size;
    if (fd >= 0) close(fd);
}
static void wl_kbd_enter(void *data, struct wl_keyboard *kbd, uint32_t serial,
                         struct wl_surface *surf, struct wl_array *keys) {
    (void)data; (void)kbd; (void)serial; (void)surf; (void)keys;
}
static void wl_kbd_leave(void *data, struct wl_keyboard *kbd, uint32_t serial, struct wl_surface *surf) {
    (void)data; (void)kbd; (void)serial; (void)surf;
}
static void wl_kbd_key(void *data, struct wl_keyboard *kbd, uint32_t serial,
                       uint32_t time, uint32_t key, uint32_t state) {
    (void)data; (void)kbd; (void)serial; (void)time;
    /* WL_KEYBOARD_KEY_STATE_PRESSED == 1 */
    if (state == 1) {
        int code = wl_map_keycode(key);
        if (code) wl_keyq_push(code);
    }
}
static void wl_kbd_modifiers(void *data, struct wl_keyboard *kbd, uint32_t serial,
                             uint32_t deps, uint32_t lats, uint32_t locked, uint32_t group) {
    (void)data; (void)kbd; (void)serial; (void)deps; (void)lats; (void)locked; (void)group;
}
static const struct wl_keyboard_listener g_kbd_listener = {
    wl_kbd_keymap, wl_kbd_enter, wl_kbd_leave, wl_kbd_key, wl_kbd_modifiers
};

static void wl_ptr_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t x, wl_fixed_t y) { (void)d;(void)p;(void)serial;(void)s;g_wl_mouse_x=wl_fixed_to_int(x);g_wl_mouse_y=wl_fixed_to_int(y); }
static void wl_ptr_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) { (void)d;(void)p;(void)serial;(void)s; }
static void wl_ptr_motion(void *d, struct wl_pointer *p, uint32_t time, wl_fixed_t x, wl_fixed_t y) { (void)d;(void)p;(void)time;g_wl_mouse_x=wl_fixed_to_int(x);g_wl_mouse_y=wl_fixed_to_int(y); }
static void wl_ptr_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
    (void)d;(void)p;(void)serial;(void)time;
    if(button==0x110u){ if(state==WL_POINTER_BUTTON_STATE_PRESSED){g_wl_mouse_down=1;g_wl_mouse_released=0;g_wl_press_x=g_wl_mouse_x;g_wl_press_y=g_wl_mouse_y;}else{g_wl_mouse_down=0;g_wl_mouse_released=1;} }
}
static void wl_ptr_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v){(void)d;(void)p;(void)t;(void)a;(void)v;}
static void wl_ptr_frame(void *d, struct wl_pointer *p){(void)d;(void)p;}
static void wl_ptr_axis_source(void *d, struct wl_pointer *p, uint32_t s){(void)d;(void)p;(void)s;}
static void wl_ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a){(void)d;(void)p;(void)t;(void)a;}
static void wl_ptr_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v){(void)d;(void)p;(void)a;(void)v;}
static const struct wl_pointer_listener g_ptr_listener={wl_ptr_enter,wl_ptr_leave,wl_ptr_motion,wl_ptr_button,wl_ptr_axis,wl_ptr_frame,wl_ptr_axis_source,wl_ptr_axis_stop,wl_ptr_axis_discrete};

static void wl_seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
    (void)data;
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !g_wl_kbd) {
        g_wl_kbd = wl_seat_get_keyboard(seat);
        if (g_wl_kbd) wl_keyboard_add_listener(g_wl_kbd, &g_kbd_listener, NULL);
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !g_wl_ptr) { g_wl_ptr=wl_seat_get_pointer(seat); if(g_wl_ptr) wl_pointer_add_listener(g_wl_ptr,&g_ptr_listener,NULL); }
    if (!(caps & WL_SEAT_CAPABILITY_POINTER) && g_wl_ptr) { wl_pointer_destroy(g_wl_ptr); g_wl_ptr=NULL; g_wl_mouse_down=0; }
}
static void wl_seat_name(void *data, struct wl_seat *seat, const char *name) {
    (void)data; (void)seat; (void)name;
}
static const struct wl_seat_listener g_seat_listener = {
    wl_seat_capabilities, wl_seat_name
};
#endif



static int wl_create_shm_buffer(int logical_w, int logical_h) {
    int scale = g_wl_scale < 1 ? 1 : g_wl_scale;
    int w = logical_w * scale;
    int h = logical_h * scale;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    int stride = w * 4;
    int size = stride * h;
    /* memfd if available, else temporary file */
    int fd = -1;
#ifdef __NR_memfd_create
    fd = (int)syscall(__NR_memfd_create, "purple-wl", 0);
#endif
    if (fd < 0) {
        char tmpl[] = "/tmp/purple-wl-XXXXXX";
        fd = mkstemp(tmpl);
        if (fd >= 0) unlink(tmpl);
    }
    if (fd < 0) return -1;
    if (ftruncate(fd, size) < 0) { close(fd); return -1; }
    void *map = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { close(fd); return -1; }
    g_wl_fd = fd;
    g_wl_map = map;
    g_wl_map_size = (size_t)size;
    g_wl_buf_w = w;
    g_wl_buf_h = h;

    struct wl_shm_pool *pool = wl_shm_create_pool(g_wl_shm, fd, size);
    if (!pool) return -1;
    g_wl_buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    return g_wl_buf ? 0 : -1;
}

/* Nearest-neighbour upscale from logical g_fb into the physical SHM mapping. */
static void wl_present_scaled(void) {
    if (!g_fb || !g_wl_map || g_logical_w <= 0 || g_logical_h <= 0) return;
    int scale = g_wl_scale < 1 ? 1 : g_wl_scale;
    uint32_t *dst = (uint32_t *)g_wl_map;
    int pw = g_wl_buf_w, ph = g_wl_buf_h;
    if (scale == 1 && pw == g_fb_w && ph == g_fb_h) {
        memcpy(dst, g_fb, (size_t)g_fb_w * (size_t)g_fb_h * 4);
        return;
    }
    for (int y = 0; y < ph; y++) {
        int sy = y / scale;
        if (sy >= g_fb_h) sy = g_fb_h - 1;
        const uint32_t *src_row = g_fb + sy * g_fb_stride;
        uint32_t *dst_row = dst + y * pw;
        for (int x = 0; x < pw; x++) {
            int sx = x / scale;
            if (sx >= g_fb_w) sx = g_fb_w - 1;
            dst_row[x] = src_row[sx];
        }
    }
}
#endif /* PURPLE_HAS_WAYLAND – wl helpers */

/* Resize software FB + active presenter (X11 XImage or Wayland SHM).
 * w/h are always logical (app / compositor) sizes. */
static int purple_gui_resize(int w, int h) {
    if (w < 64) w = 64;
    if (h < 64) h = 64;
    if (w > 8192) w = 8192;
    if (h > 8192) h = 8192;
    int scale = 1;
#ifdef PURPLE_HAS_WAYLAND
    if (g_gui_backend == 2) scale = g_wl_scale < 1 ? 1 : g_wl_scale;
#endif
    /* Skip only when both logical size and current scale match the existing buffer */
    if (g_fb && g_fb_w == w && g_fb_h == h &&
        (g_gui_backend != 2 || (g_wl_buf_w == w * scale && g_wl_buf_h == h * scale))) {
        g_resize_w = g_resize_h = 0;
        return 0;
    }
    /* Recreate software framebuffer at the logical size (content is cleared;
     * the next Blow.Begin / draw cycle will repaint). */
    blit_ensure(w, h);
    g_logical_w = w;
    g_logical_h = h;
#ifdef PURPLE_HAS_X11
    if (g_gui_backend == 1 && g_dpy && g_win) {
        if (g_ximg) {
            g_ximg->data = NULL; /* own g_fb; don't let X free it */
            XDestroyImage(g_ximg);
            g_ximg = NULL;
        }
        int screen = DefaultScreen(g_dpy);
        g_ximg = XCreateImage(g_dpy, DefaultVisual(g_dpy, screen),
                              (unsigned)DefaultDepth(g_dpy, screen),
                              ZPixmap, 0, (char *)g_fb, (unsigned)w, (unsigned)h, 32,
                              g_fb_stride * 4);
        if (g_ximg) {
            g_ximg->byte_order = LSBFirst;
            g_ximg->bitmap_bit_order = LSBFirst;
        }
        /* Keep the X window geometry in sync so the next present matches */
        XResizeWindow(g_dpy, g_win, (unsigned)w, (unsigned)h);
    }
#endif
#ifdef PURPLE_HAS_WAYLAND
    if (g_gui_backend == 2 && g_wl_dpy && g_wl_shm) {
        if (g_wl_buf) { wl_buffer_destroy(g_wl_buf); g_wl_buf = NULL; }
        if (g_wl_map && g_wl_map_size) {
            munmap(g_wl_map, g_wl_map_size);
            g_wl_map = NULL; g_wl_map_size = 0;
        }
        if (g_wl_fd >= 0) { close(g_wl_fd); g_wl_fd = -1; }
        if (wl_create_shm_buffer(w, h) != 0) {
            fprintf(stderr, "[GUI] Wayland resize: SHM buffer recreate failed\n");
            return -1;
        }
        /* Re-attach the new buffer so the compositor sees a consistent size */
        if (g_wl_surf && g_wl_buf && g_wl_map) {
            memset(g_wl_map, 0, g_wl_map_size);
            wl_surface_set_buffer_scale(g_wl_surf, scale);
            wl_surface_attach(g_wl_surf, g_wl_buf, 0, 0);
            wl_surface_damage(g_wl_surf, 0, 0, w, h);
            wl_surface_commit(g_wl_surf);
            wl_display_flush(g_wl_dpy);
        }
    }
#endif
    g_resize_w = g_resize_h = 0;
    return 0;
}

static int purple_gui_probe(void) {
#ifdef PURPLE_HAS_WAYLAND
    if (!g_wl_dpy) {
        purple_xdg_init_types();
        g_wl_dpy = wl_display_connect(NULL);
        if (g_wl_dpy) {
            g_wl_reg = wl_display_get_registry(g_wl_dpy);
            wl_registry_add_listener(g_wl_reg, &g_reg_listener, NULL);
            wl_display_roundtrip(g_wl_dpy);
            if (g_wl_seat) {
                wl_seat_add_listener(g_wl_seat, &g_seat_listener, NULL);
                wl_display_roundtrip(g_wl_dpy); /* pick up keyboard + pointer */
            }
            /* Prefer modern xdg-shell; fall back to classic wl_shell. */
            if (g_wl_comp && g_wl_shm && (g_xdg_wm || g_wl_shell)) {
                g_gui_backend = 2;
                g_wl_ready = 1;
                if (g_xdg_wm)
                    fprintf(stderr, "[GUI] Native Wayland backend ready (xdg-shell%s)\n",
                            g_wl_shell ? "; wl_shell also present" : "");
                else
                    fprintf(stderr, "[GUI] Native Wayland backend ready (wl_shell)\n");
                return 2;
            }
            /* Incomplete compositor – fall through to X11/XWayland */
            fprintf(stderr, "[GUI] Wayland present but incomplete (need compositor+shm+xdg_wm_base|wl_shell); falling back\n");
            wl_display_disconnect(g_wl_dpy);
            g_wl_dpy = NULL; g_wl_reg = NULL; g_wl_comp = NULL; g_wl_shm = NULL;
            g_wl_shell = NULL; g_xdg_wm = NULL; g_wl_seat = NULL;
        }
    } else if (g_wl_ready) {
        return 2;
    }
#endif
#ifdef PURPLE_HAS_X11
    if (!g_dpy) {
        g_dpy = XOpenDisplay(NULL);
        if (g_dpy) {
            g_gui_backend = 1;
            fprintf(stderr, "[GUI] X11 backend ready%s\n", getenv("WAYLAND_DISPLAY") ? " (likely XWayland)" : "");
            return 1;
        }
    } else {
        return 1;
    }
#endif
    return 0;
}

static int64_t purple_gui_create_window(int w, int h, const char *title, int decorationless) {
    if (w < 64) w = 800;
    if (h < 64) h = 500;
    if (!title) title = "Purplecode Milestone 1 1.6";
    g_decorationless = decorationless ? 1 : 0;

    int backend = purple_gui_probe();
    if (backend == 0) {
        fprintf(stderr, "[GUI] No display backend (build with -DPURPLE_HAS_X11 -lX11 and/or -DPURPLE_HAS_WAYLAND -lwayland-client)\n");
        return -1;
    }

#if defined(PURPLE_HAS_WAYLAND) && defined(PURPLE_HAS_X11)
    /* Normal window requested but the compositor can't draw decorations for us
     * (no xdg-decoration, e.g. GNOME/Mutter): use XWayland, where the WM always
     * decorates. Set PURPLE_GUI=wayland to force native anyway. */
    if (backend == 2 && !decorationless && !g_deco_mgr && !(getenv("PURPLE_GUI") && !strcmp(getenv("PURPLE_GUI"), "wayland"))) {
        if (!g_dpy) g_dpy = XOpenDisplay(NULL);
        if (g_dpy) {
            fprintf(stderr, "[GUI] Wayland compositor has no server-side decorations; using XWayland for a decorated window\n");
            g_gui_backend = 1; g_wl_ready = 0; backend = 1;
        }
    }
#endif
    blit_ensure(w, h);
    blit_clear(0xFF111111);

#ifdef PURPLE_HAS_X11
    if (backend == 1) {
        int screen = DefaultScreen(g_dpy);
        /* border width 1 for normal windows; 0 for decorationless */
        unsigned border = decorationless ? 0u : 1u;
        g_win = XCreateSimpleWindow(g_dpy, RootWindow(g_dpy, screen),
                                    80, 60, (unsigned)w, (unsigned)h, border,
                                    BlackPixel(g_dpy, screen), BlackPixel(g_dpy, screen));
        XStoreName(g_dpy, g_win, title);
        XSelectInput(g_dpy, g_win,
                     ExposureMask | KeyPressMask | KeyReleaseMask |
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                     StructureNotifyMask);
        Atom wm_delete = XInternAtom(g_dpy, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(g_dpy, g_win, &wm_delete, 1);

        if (decorationless) {
            /* Request no decorations via Motif WM hints.
             * This keeps the window managed (so it stays resizable and
             * participates in focus/stacking) while removing titlebar and borders.
             * Works on KDE Plasma, GNOME, XFCE, etc. under X11 / XWayland. */
            typedef struct {
                unsigned long flags;
                unsigned long functions;
                unsigned long decorations;
                long input_mode;
                unsigned long status;
            } MotifWmHints;
            #define MWM_HINTS_DECORATIONS (1L << 1)
            MotifWmHints hints;
            memset(&hints, 0, sizeof hints);
            hints.flags = MWM_HINTS_DECORATIONS;
            hints.decorations = 0; /* none */
            Atom motif = XInternAtom(g_dpy, "_MOTIF_WM_HINTS", False);
            XChangeProperty(g_dpy, g_win, motif, motif, 32, PropModeReplace,
                            (unsigned char *)&hints, 5);
        }

        XMapWindow(g_dpy, g_win);
        g_gc = XCreateGC(g_dpy, g_win, 0, NULL);

        /* Backing XImage that points at our framebuffer */
        g_ximg = XCreateImage(g_dpy, DefaultVisual(g_dpy, screen),
                              (unsigned)DefaultDepth(g_dpy, screen),
                              ZPixmap, 0, (char *)g_fb, (unsigned)w, (unsigned)h, 32,
                              g_fb_stride * 4);
        if (g_ximg) {
            g_ximg->byte_order = LSBFirst;
            g_ximg->bitmap_bit_order = LSBFirst;
        }
        g_fb_w = w; g_fb_h = h;
        g_win_open = 1;
        /* Initial present */
        if (g_ximg) {
            XPutImage(g_dpy, g_win, g_gc, g_ximg, 0, 0, 0, 0, (unsigned)w, (unsigned)h);
            XFlush(g_dpy);
        }
        return 1;
    }
#endif

#ifdef PURPLE_HAS_WAYLAND
    if (backend == 2) {
        g_wl_surf = wl_compositor_create_surface(g_wl_comp);
        if (!g_wl_surf) return -1;
        /* Prefer modern xdg-shell when bound; otherwise classic wl_shell.
         * On KDE Plasma, a normal xdg_toplevel receives server-side decorations
         * (titlebar + resize borders). Decorations are negotiated via
         * zxdg_decoration_manager_v1 (see below). */
        if (g_xdg_wm) {
            g_xdg_surf = xdg_wm_base_get_xdg_surface(g_xdg_wm, g_wl_surf);
            if (!g_xdg_surf) {
                fprintf(stderr, "[GUI] Wayland: xdg_wm_base_get_xdg_surface failed\n");
                return -1;
            }
            xdg_surface_add_listener(g_xdg_surf, &g_xdg_surf_listener, NULL);
            g_xdg_top = xdg_surface_get_toplevel(g_xdg_surf);
            if (!g_xdg_top) {
                fprintf(stderr, "[GUI] Wayland: xdg_surface_get_toplevel failed\n");
                return -1;
            }
            xdg_toplevel_add_listener(g_xdg_top, &g_xdg_top_listener, NULL);
            xdg_toplevel_set_title(g_xdg_top, title ? title : "Purplecode");
            xdg_toplevel_set_app_id(g_xdg_top, "purplecode");
            xdg_toplevel_set_min_size(g_xdg_top, 64, 64); /* prevent zero-size explosions */
            /* Ask for server-side decorations (titlebar/borders) on normal windows,
             * client-side (= none drawn by us) on decorationless ones. */
            if (g_deco_mgr) {
                g_deco = (struct zxdg_toplevel_decoration_v1 *)wl_proxy_marshal_constructor(
                    (struct wl_proxy *)g_deco_mgr, ZXDG_DECO_MGR_GET_TOPLEVEL_DECORATION,
                    &zxdg_toplevel_decoration_v1_interface, NULL, g_xdg_top);
                if (g_deco) {
                    wl_proxy_add_listener((struct wl_proxy *)g_deco, (void (**)(void))(void *)&g_zdeco_listener, NULL);
                    wl_proxy_marshal((struct wl_proxy *)g_deco, ZXDG_TOPLEVEL_DECO_SET_MODE,
                                     decorationless ? ZXDG_DECO_MODE_CLIENT_SIDE : ZXDG_DECO_MODE_SERVER_SIDE);
                }
            }
            /* Initial commit required before the first configure */
            wl_surface_commit(g_wl_surf);
            /* Wait for the first configure so the compositor accepts our buffer */
            g_xdg_configured = 0;
            while (!g_xdg_configured && g_wl_dpy)
                wl_display_dispatch(g_wl_dpy);
        } else if (g_wl_shell) {
            g_wl_ssurf = wl_shell_get_shell_surface(g_wl_shell, g_wl_surf);
            if (g_wl_ssurf) {
                wl_shell_surface_set_toplevel(g_wl_ssurf);
                wl_shell_surface_set_title(g_wl_ssurf, title);
            }
        }
        g_logical_w = w;
        g_logical_h = h;
        if (wl_create_shm_buffer(w, h) != 0) {
            fprintf(stderr, "[GUI] Wayland: failed to create shm buffer\n");
            return -1;
        }
        /* Upscale logical FB into physical SHM buffer and present with correct scale */
        wl_present_scaled();
        int scale = g_wl_scale < 1 ? 1 : g_wl_scale;
        wl_surface_set_buffer_scale(g_wl_surf, scale);
        wl_surface_attach(g_wl_surf, g_wl_buf, 0, 0);
        wl_surface_damage(g_wl_surf, 0, 0, w, h);
        wl_surface_commit(g_wl_surf);
        wl_display_flush(g_wl_dpy);
        g_win_open = 1;
        g_wl_ready = 1;
        return 1;
    }
#endif
    return -1;
}

static void purple_gui_draw_rect(int x, int y, int w, int h, uint32_t color) {
    blit_fill_rect(x, y, w, h, color);
}

static void purple_gui_clear(uint32_t color) {
    blit_clear(color);
}

static void purple_gui_set_pixel(int x, int y, uint32_t color) {
    blit_set_pixel(x, y, color);
}

static void purple_gui_present(void) {
    if (!g_fb || !g_win_open) return;
#ifdef PURPLE_HAS_X11
    if (g_gui_backend == 1 && g_dpy && g_win && g_ximg) {
        /* XImage data pointer already points at g_fb */
        XPutImage(g_dpy, g_win, g_gc, g_ximg, 0, 0, 0, 0,
                  (unsigned)g_fb_w, (unsigned)g_fb_h);
        XFlush(g_dpy);
    }
#endif
#ifdef PURPLE_HAS_WAYLAND
    if (g_gui_backend == 2 && g_wl_dpy && g_wl_surf && g_wl_map && g_wl_buf) {
        wl_present_scaled();
        int scale = g_wl_scale < 1 ? 1 : g_wl_scale;
        wl_surface_set_buffer_scale(g_wl_surf, scale);
        wl_surface_attach(g_wl_surf, g_wl_buf, 0, 0);
        wl_surface_damage(g_wl_surf, 0, 0, g_logical_w > 0 ? g_logical_w : g_fb_w,
                          g_logical_h > 0 ? g_logical_h : g_fb_h);
        wl_surface_commit(g_wl_surf);
        wl_display_flush(g_wl_dpy);
    }
#endif
}

static int64_t purple_gui_poll_key(void) {
#ifdef PURPLE_HAS_X11
    if (g_gui_backend == 1 && g_dpy) {
        while (XPending(g_dpy) > 0) {
            XEvent ev;
            XNextEvent(g_dpy, &ev);
            if (ev.type == KeyPress) {
                KeySym ks = XLookupKeysym(&ev.xkey, 0);
                if (ks == XK_Left)  return 1000;
                if (ks == XK_Right) return 1001;
                if (ks == XK_Up)    return 1002;
                if (ks == XK_Down)  return 1003;
                if (ks == XK_Escape) return 27;
                if (ks == XK_space) return 32;
                if (ks >= 32 && ks < 127) return (int64_t)ks;
            } else if (ev.type == ConfigureNotify) {
                if (ev.xconfigure.width > 0 && ev.xconfigure.height > 0)
                    purple_gui_resize(ev.xconfigure.width, ev.xconfigure.height);
            } else if (ev.type == ClientMessage) {
                g_win_open = 0;
                return -1;
            } else if (ev.type == DestroyNotify) {
                g_win_open = 0;
                return -1;
            } else if (ev.type == Expose) {
                purple_gui_present();
            }
        }
    }
#endif
#ifdef PURPLE_HAS_WAYLAND
    if (g_gui_backend == 2 && g_wl_dpy) {
        wl_dispatch_nonblock();
        if (g_resize_w > 0 && g_resize_h > 0)
            purple_gui_resize(g_resize_w, g_resize_h);
        int k = wl_keyq_pop();
        if (k) return k;
    }
#endif
    return 0;
}

static int64_t purple_gui_pump(int ms) {
    if (ms < 0) ms = 16;
#ifdef PURPLE_HAS_X11
    if (g_gui_backend == 1 && g_dpy) {
        struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
        nanosleep(&ts, NULL);
        while (XPending(g_dpy) > 0) {
            XEvent ev;
            XNextEvent(g_dpy, &ev);
            if (ev.type == DestroyNotify || ev.type == ClientMessage) {
                g_win_open = 0;
                return -1;
            }
            if (ev.type == Expose) purple_gui_present();
        }
        return g_win_open ? 1 : -1;
    }
#endif
#ifdef PURPLE_HAS_WAYLAND
    if (g_gui_backend == 2 && g_wl_dpy) {
        struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
        nanosleep(&ts, NULL);
        wl_dispatch_nonblock();
        return 1;
    }
#endif
    usleep((useconds_t)ms * 1000);
    return 1;
}

/* Public blitter entry used by PurpleAPI numbers 20+ */
static int64_t purple_blit_fill(int x, int y, int w, int h, uint32_t color) {
    blit_fill_rect(x, y, w, h, color);
    return 1;
}
static int64_t purple_blit_clear(uint32_t color) {
    blit_clear(color);
    return 1;
}

/* ========== FS ========== */
int purple_fs_write(const char *data, const char *path) {
    if (!path) return -1;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("purple_fs_write open");
        return -1;
    }
    size_t len = data ? strlen(data) : 0;
    ssize_t n = write(fd, data ? data : "", len);
    close(fd);
    return (n < 0) ? -1 : (int)n;
}

/* ========== Print ========== */
void purple_shell_print(const char *s) {
    if (!s) s = "(null)";
    fwrite(s, 1, strlen(s), stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

void purple_shell_print_addr(uint64_t addr) {
    printf("0x%lx\n", (unsigned long)addr);
    fflush(stdout);
}

void purple_shell_print_int(int64_t value) {
    printf("%lld\n", (long long)value);
    fflush(stdout);
}

/* Filesystem/system helpers exposed through NYMNOG dotted APIs. */
int64_t purple_fs_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0;
}

int64_t purple_fs_size(const char *path) {
    struct stat st;
    if (!path || stat(path, &st) != 0) return -1;
    return (int64_t)st.st_size;
}

int64_t purple_process_id(void) { return (int64_t)getpid(); }
int64_t purple_time_unix(void) { return (int64_t)time(NULL); }
uint64_t purple_memory_alloc(uint64_t size) {
    if (size == 0 || size > (uint64_t)SIZE_MAX) return 0;
    return (uint64_t)(uintptr_t)calloc(1, (size_t)size);
}
void purple_memory_free(uint64_t ptr) { free((void *)(uintptr_t)ptr); }

/* ========== Milestone 1 1.6 – Dynamic arrays ========== */
int64_t *purple_array_new(int64_t size) {
    if (size < 0) size = 0;
    int64_t cap = size > 0 ? size : 8;
    /* header: len, cap, then elements */
    size_t bytes = (size_t)(2 + cap) * sizeof(int64_t);
    int64_t *block = (int64_t *)calloc(1, bytes);
    if (!block) return NULL;
    block[0] = size;   /* length */
    block[1] = cap;    /* capacity */
    return block;
}

int64_t purple_array_len(int64_t *arr) {
    if (!arr) return 0;
    return arr[0];
}

int64_t *purple_array_data(int64_t *arr) {
    if (!arr) return NULL;
    return arr + 2;
}

int64_t purple_array_get(int64_t *arr, int64_t index) {
    if (!arr) return 0;
    int64_t len = arr[0];
    if (index < 0 || index >= len) {
        /* systems language: undefined / zero on OOB in release */
        return 0;
    }
    return arr[2 + index];
}

int64_t purple_array_set(int64_t *arr, int64_t index, int64_t value) {
    if (!arr) return -1;
    int64_t len = arr[0];
    if (index < 0 || index >= len) return -1;
    arr[2 + index] = value;
    return 0;
}

void purple_array_free(int64_t *arr) {
    free(arr);
}


/* Loads JavaScript bytes into a bounded temporary buffer and reports the byte
 * count. This deliberately does NOT execute JavaScript: no JS engine is bundled. */
int64_t purple_web_js_load(const char *path) {
    if (!path) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) { perror("Web.JS.Load"); return -1; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long size = ftell(f);
    if (size < 0 || size > 16 * 1024 * 1024 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f); errno = EFBIG; perror("Web.JS.Load size"); return -1;
    }
    unsigned char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); return -1; }
    size_t n = fread(buf, 1, (size_t)size, f);
    int failed = ferror(f);
    fclose(f);
    if (failed) { free(buf); return -1; }
    buf[n] = 0;
    printf("[Web.JS] Loaded %zu bytes from %s (not executed; no JS engine configured)\n", n, path);
    free(buf);
    return (int64_t)n;
}

/* Execute a JavaScript file using Node.js without a shell. The child inherits
 * stdio; its actual exit code is returned. Missing Node and script errors fail. */
int64_t purple_web_js_run(const char *path) {
    if (!path || !*path) { errno = EINVAL; perror("Web.JS.Run"); return -1; }
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) { perror("Web.JS.Run script"); return -1; }
    pid_t pid = fork();
    if (pid < 0) { perror("Web.JS.Run fork"); return -1; }
    if (pid == 0) { execlp("node", "node", path, (char *)NULL); perror("Web.JS.Run node"); _exit(127); }
    int status;
    while (waitpid(pid, &status, 0) < 0) { if (errno != EINTR) { perror("Web.JS.Run waitpid"); return -1; } }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

static int64_t purple_readable_regular_file(const char *path, const char *what) {
    if (!path || !*path) { errno = EINVAL; perror(what); return -1; }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { perror(what); return -1; }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        if (errno == 0) errno = EINVAL;
        perror(what); close(fd); return -1;
    }
    if (st.st_size < 0) { close(fd); return -1; }
    close(fd);
    return (int64_t)st.st_size;
}

/* Open an HTML document in the user's configured browser. */
int64_t purple_web_html_load(const char *path) {
    if (purple_readable_regular_file(path, "Web.HTML.Load") < 0) return -1;
    char *resolved = realpath(path, NULL);
    if (!resolved) { perror("Web.HTML.Load realpath"); return -1; }
    pid_t pid = fork();
    if (pid < 0) { perror("Web.HTML.Load fork"); free(resolved); return -1; }
    if (pid == 0) { execlp("xdg-open", "xdg-open", resolved, (char *)NULL); perror("Web.HTML.Load xdg-open"); _exit(127); }
    free(resolved);
    /* Do not block on a browser process; reap immediately if it already exited. */
    int status;
    if (waitpid(pid, &status, WNOHANG) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 127) return -1;
    return 0;
}

/* CSS.Load validates and reads the stylesheet, returning its byte count.
 * Browsers apply CSS by linking it from an HTML document; CSS alone is not a page. */
int64_t purple_web_css_load(const char *path) {
    int64_t size = purple_readable_regular_file(path, "Web.CSS.Load");
    if (size < 0) return -1;
    FILE *f=fopen(path,"rb");if(!f){perror("Web.CSS.Load");return -1;}
    int c, quote=0, escape=0, comment=0; int depth=0; int bad=0; int prev=0;
    while((c=fgetc(f))!=EOF){
        if(comment){if(prev=='*'&&c=='/')comment=0;prev=c;continue;}
        if(quote){if(escape)escape=0;else if(c=='\\')escape=1;else if(c==quote)quote=0;continue;}
        if(prev=='/'&&c=='*'){comment=1;prev=0;continue;}
        if(c=='"'||c=='\''){quote=c;prev=c;continue;}
        if(c=='{')depth++;else if(c=='}'&&--depth<0){bad=1;break;}
        prev=c;
    }
    fclose(f);
    if(quote||comment||depth!=0)bad=1;
    if(bad){fprintf(stderr,"Web.CSS.Load: malformed CSS (unbalanced braces, string, or comment): %s\n",path);return -1;}
    printf("[Web.CSS] Parsed stylesheet asset: %s (%lld bytes); link it from HTML to apply it\n",path,(long long)size);
    return size;
}

/* Read a file from a mounted optical/USB filesystem. The caller supplies a path
 * under the mount point; this is file-level access, not raw device-sector I/O. */
int64_t purple_media_read(const char *path, const char *kind) {
    if (!path || !*path) { errno = EINVAL; perror(kind ? kind : "Media.Read"); return -1; }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { perror(kind ? kind : "Media.Read"); return -1; }
    struct stat st;
    if (fstat(fd, &st) != 0 || !(S_ISREG(st.st_mode) || S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode))) {
        fprintf(stderr, "%s: expected a file or readable device path\n", kind ? kind : "Media.Read"); close(fd); return -1;
    }
    /* Bounded device reads: never slurp an entire multi-terabyte block device. */
    const int64_t limit = S_ISREG(st.st_mode) ? (st.st_size < 0 ? 0 : st.st_size) : 16 * 1024 * 1024;
    unsigned char buf[16384]; int64_t total = 0;
    while (total < limit) {
        size_t want = (limit - total) < (int64_t)sizeof buf ? (size_t)(limit-total) : sizeof buf;
        ssize_t n = read(fd, buf, want);
        if (n == 0) break;
        if (n < 0) { if (errno == EINTR) continue; perror(kind ? kind : "Media.Read"); close(fd); return -1; }
        total += n;
    }
    close(fd); printf("[%s] Read %lld bytes from %s%s\n", kind ? kind : "Media", (long long)total, path, S_ISREG(st.st_mode) ? "" : " (device read capped at 16 MiB)"); return total;
}

int64_t purple_media_read_sector(const char *device, uint64_t lba, const char *kind) {
    if (!device || !*device || lba > (uint64_t)(INT64_MAX / 2048)) { errno=EINVAL; perror("Media.ReadSector"); return -1; }
    int fd=open(device,O_RDONLY|O_CLOEXEC|O_NONBLOCK);
    if(fd<0){perror(kind ? kind : "Media.ReadSector");return -1;}
    struct stat st; if(fstat(fd,&st)!=0 || !(S_ISBLK(st.st_mode)||S_ISREG(st.st_mode))) { fprintf(stderr,"%s: device must be a block device or test image file\n",kind?kind:"Media.ReadSector");close(fd);return -1; }
    size_t sector=(kind && !strcmp(kind,"CD"))?2048:512;
    if(lba>(uint64_t)(INT64_MAX/(int64_t)sector)){close(fd);errno=EOVERFLOW;return -1;}
    unsigned char data[2048]; ssize_t n=pread(fd,data,sector,(off_t)(lba*sector));
    if(n<0){perror(kind?kind:"Media.ReadSector");close(fd);return -1;} close(fd);
    printf("[%s] LBA %llu: read %lld/%zu bytes from %s\n",kind?kind:"Media",(unsigned long long)lba,(long long)n,sector,device);
    return n;
}

int64_t purple_media_read_sector_to(const char *device,uint64_t lba,const char *output,const char *kind) {
    if(!output||!*output){errno=EINVAL;perror("Media.ReadSectorTo output");return -1;}
    if(!device||!*device){errno=EINVAL;perror("Media.ReadSectorTo device");return -1;}
    size_t sector=(kind&&!strcmp(kind,"CD"))?2048:512;
    if(lba>(uint64_t)(INT64_MAX/(int64_t)sector)){errno=EOVERFLOW;return -1;}
    int fd=open(device,O_RDONLY|O_CLOEXEC|O_NONBLOCK);if(fd<0){perror(kind?kind:"Media.ReadSectorTo");return -1;}
    struct stat st;if(fstat(fd,&st)!=0||!(S_ISBLK(st.st_mode)||S_ISREG(st.st_mode))){fprintf(stderr,"%s: expected block device or disk-image file\n",kind?kind:"Media.ReadSectorTo");close(fd);return -1;}
    unsigned char data[2048];ssize_t n=pread(fd,data,sector,(off_t)(lba*sector));int saved=errno;close(fd);errno=saved;if(n<0){perror(kind?kind:"Media.ReadSectorTo");return -1;}
    int out=open(output,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0644);if(out<0){perror("Media.ReadSectorTo output");return -1;}size_t done=0;while(done<(size_t)n){ssize_t w=write(out,data+done,(size_t)n-done);if(w<0){if(errno==EINTR)continue;perror("Media.ReadSectorTo write");close(out);return -1;}done+=(size_t)w;}if(close(out)!=0){perror("Media.ReadSectorTo close");return -1;}
    printf("[%s] Saved LBA %llu (%lld bytes) from %s to %s\n",kind?kind:"Media",(unsigned long long)lba,(long long)n,device,output);return n;
}

/* Image decoders normalize supported formats to packed RGB24. PNG uses
 * libpng's simplified API; JPEG uses libjpeg's memory source. BMP and PPM
 * remain self-contained decoders. Decoded dimensions are capped to avoid
 * unreasonable allocations from malformed/untrusted image files. */
static int purple_decode_bmp(const unsigned char *b,size_t n,unsigned *w,unsigned *h,unsigned char **rgb) {
    if(n<54 || b[0]!='B'||b[1]!='M') return 0;
    uint32_t off=(uint32_t)b[10]|((uint32_t)b[11]<<8)|((uint32_t)b[12]<<16)|((uint32_t)b[13]<<24);
    uint32_t dib=(uint32_t)b[14]|((uint32_t)b[15]<<8)|((uint32_t)b[16]<<16)|((uint32_t)b[17]<<24);
    uint32_t wraw=(uint32_t)b[18]|((uint32_t)b[19]<<8)|((uint32_t)b[20]<<16)|((uint32_t)b[21]<<24);
    uint32_t hraw=(uint32_t)b[22]|((uint32_t)b[23]<<8)|((uint32_t)b[24]<<16)|((uint32_t)b[25]<<24);
    int32_t wi=(int32_t)wraw, hi=(int32_t)hraw;
    uint16_t planes=(uint16_t)(b[26]|((uint16_t)b[27]<<8)), bits=(uint16_t)(b[28]|((uint16_t)b[29]<<8));
    uint32_t comp=(uint32_t)b[30]|((uint32_t)b[31]<<8)|((uint32_t)b[32]<<16)|((uint32_t)b[33]<<24);
    if(dib<40||wi<=0||hi==0||hi==INT32_MIN||planes!=1||(bits!=24&&bits!=32)||comp!=0) return 0;
    unsigned H=(unsigned)(hi<0?-hi:hi), W=(unsigned)wi; if((uint64_t)W*H>64000000u) return 0;
    size_t stride=((size_t)W*(bits/8)+3)&~(size_t)3; if(off>n||stride>n||H>(n-off)/stride) return 0;
    unsigned char *out=malloc((size_t)W*H*3); if(!out)return 0;
    for(unsigned y=0;y<H;y++){unsigned sy=hi>0?H-1-y:y;const unsigned char *row=b+off+(size_t)sy*stride;for(unsigned x=0;x<W;x++){size_t d=((size_t)y*W+x)*3;out[d]=row[x*(bits/8)+2];out[d+1]=row[x*(bits/8)+1];out[d+2]=row[x*(bits/8)];}}
    *w=W;*h=H;*rgb=out;return 1;
}
static int purple_decode_ppm(const unsigned char *b, size_t n, unsigned *w, unsigned *h, unsigned char **rgb) {
    if (n < 3 || b[0] != 'P' || b[1] != '6') return 0;
    size_t i = 2;
    while (i < n) { if (isspace(b[i])) { i++; continue; } if (b[i] == '#') { while (i < n && b[i] != '\n') i++; continue; } break; }
    unsigned W=0,H=0,max=0; int fields=0;
    while (i<n && fields<3) { while (i<n && isspace(b[i])) i++; if (i<n && b[i]=='#') { while(i<n && b[i]!='\n') i++; continue; }
        unsigned v=0; int got=0; while(i<n && isdigit(b[i])) { got=1; v=v*10+(b[i++]-'0'); if(v>1000000)return 0; }
        if(!got)return 0;
        if(fields==0)W=v; else if(fields==1)H=v; else max=v;
        fields++;
    }
    if(fields!=3||!W||!H||max!=255||(uint64_t)W*H>64000000u)return 0;
    if(i>=n||!isspace(b[i]))return 0;
    i++; size_t bytes=(size_t)W*H*3; if(bytes>n-i)return 0;
    unsigned char*out=malloc(bytes);if(!out)return 0; memcpy(out,b+i,bytes);*w=W;*h=H;*rgb=out;return 1;
}
#if defined(PURPLE_HAS_PNG) || defined(PURPLE_HAS_IMAGE)
static int purple_decode_png(const unsigned char *b,size_t n,unsigned *w,unsigned *h,unsigned char **rgb) {
    if(n<8 || png_sig_cmp((png_const_bytep)b,0,8)) return 0;
    png_image image; memset(&image,0,sizeof(image)); image.version=PNG_IMAGE_VERSION;
    if(!png_image_begin_read_from_memory(&image,b,n)) return 0;
    if(!image.width||!image.height||(uint64_t)image.width*image.height>64000000u) { png_image_free(&image); return 0; }
    image.format=PNG_FORMAT_RGB; png_alloc_size_t bytes=PNG_IMAGE_SIZE(image);
    unsigned char *out=malloc((size_t)bytes); if(!out){png_image_free(&image);return 0;}
    if(!png_image_finish_read(&image,NULL,out,0,NULL)){free(out);png_image_free(&image);return 0;}
    *w=image.width;*h=image.height;*rgb=out;png_image_free(&image);return 1;
}
#else
static int purple_decode_png(const unsigned char *b,size_t n,unsigned *w,unsigned *h,unsigned char **rgb) {
    (void)b;(void)n;(void)w;(void)h;(void)rgb; return 0;
}
#endif

#if defined(PURPLE_HAS_JPEG) || defined(PURPLE_HAS_IMAGE)
typedef struct { struct jpeg_error_mgr pub; jmp_buf jump; } PurpleJpegError;
static void purple_jpeg_error_exit(j_common_ptr cinfo) { PurpleJpegError *err=(PurpleJpegError*)cinfo->err; longjmp(err->jump,1); }
static int purple_decode_jpeg(const unsigned char *b,size_t n,unsigned *w,unsigned *h,unsigned char **rgb) {
    if(n<3||b[0]!=0xff||b[1]!=0xd8) return 0;
    struct jpeg_decompress_struct cinfo; PurpleJpegError jerr; cinfo.err=jpeg_std_error(&jerr.pub); jerr.pub.error_exit=purple_jpeg_error_exit;
    if(setjmp(jerr.jump)){jpeg_destroy_decompress(&cinfo);return 0;}
    jpeg_create_decompress(&cinfo); jpeg_mem_src(&cinfo,(unsigned char*)b,(unsigned long)n);
    if(jpeg_read_header(&cinfo,TRUE)!=JPEG_HEADER_OK){jpeg_destroy_decompress(&cinfo);return 0;}
    cinfo.out_color_space=JCS_RGB; jpeg_start_decompress(&cinfo);
    if(!cinfo.output_width||!cinfo.output_height||(uint64_t)cinfo.output_width*cinfo.output_height>64000000u||cinfo.output_components!=3){jpeg_destroy_decompress(&cinfo);return 0;}
    unsigned W=cinfo.output_width,H=cinfo.output_height; size_t stride=(size_t)W*3; unsigned char*out=malloc(stride*H);
    if(!out){jpeg_destroy_decompress(&cinfo);return 0;}
    while(cinfo.output_scanline<cinfo.output_height){JSAMPROW row=out+(size_t)cinfo.output_scanline*stride;if(jpeg_read_scanlines(&cinfo,&row,1)!=1){free(out);jpeg_destroy_decompress(&cinfo);return 0;}}
    jpeg_finish_decompress(&cinfo);jpeg_destroy_decompress(&cinfo);*w=W;*h=H;*rgb=out;return 1;
}
#else
static int purple_decode_jpeg(const unsigned char *b,size_t n,unsigned *w,unsigned *h,unsigned char **rgb) {
    (void)b;(void)n;(void)w;(void)h;(void)rgb; return 0;
}
#endif

static int purple_decode_image(const char *path,unsigned *w,unsigned *h,unsigned char **rgb) {
    FILE*f=fopen(path,"rb");if(!f){perror("Image.Load");return -1;}
    if(fseek(f,0,SEEK_END)){fclose(f);return -1;}long z=ftell(f);
    if(z<0||z>256*1024*1024L||fseek(f,0,SEEK_SET)){fclose(f);errno=EFBIG;return -1;}
    unsigned char*b=malloc((size_t)z);if(!b){fclose(f);return -1;}size_t n=fread(b,1,(size_t)z,f);fclose(f);
    int ok=purple_decode_png(b,n,w,h,rgb)||purple_decode_jpeg(b,n,w,h,rgb)||purple_decode_bmp(b,n,w,h,rgb)||purple_decode_ppm(b,n,w,h,rgb);free(b);
    if(!ok){fprintf(stderr,"Image.Load: unsupported or invalid image; supported formats: PNG, JPEG/JPG, uncompressed 24/32-bit BMP, and PPM P6: %s\n",path);return -1;}return 0;
}
int64_t purple_image_load(const char *path) {
    unsigned w=0,h=0; unsigned char *rgb=NULL;
    if(!path || purple_decode_image(path,&w,&h,&rgb)<0) return -1;
    free(rgb);
    printf("[Image] Decoded %s: %ux%u RGB24\n", path, w, h);
    return (int64_t)w * h;
}

/* Draw decoded image into the current software framebuffer (BlowUI / GUI blitter).
 * Returns pixel count on success, -1 on failure. Works with X11 or Wayland presenter. */
int64_t purple_image_draw(const char *path, int64_t x, int64_t y) {
    unsigned w=0, h=0; unsigned char *rgb=NULL;
    if(!path || purple_decode_image(path, &w, &h, &rgb)<0) return -1;
    if(!g_fb){ free(rgb); fprintf(stderr,"Image.Draw: no framebuffer (create a window first)\n"); return -1; }
    blit_image_rgb((int)x, (int)y, (int)w, (int)h, rgb, (int)w*3, 0);
    free(rgb);
    return (int64_t)w * h;
}

int64_t purple_image_show(const char *path) {
    unsigned w=0,h=0;unsigned char*rgb=NULL;if(!path||purple_decode_image(path,&w,&h,&rgb)<0)return -1;
#ifdef PURPLE_HAS_X11
    Display*d=XOpenDisplay(NULL);if(!d){free(rgb);fprintf(stderr,"Image.Show: cannot open X display\n");return -1;}int scr=DefaultScreen(d);Window win=XCreateSimpleWindow(d,RootWindow(d,scr),40,40,w,h,0,BlackPixel(d,scr),WhitePixel(d,scr));XStoreName(d,win,path);XSelectInput(d,win,ExposureMask|KeyPressMask|StructureNotifyMask);XMapWindow(d,win);GC gc=XCreateGC(d,win,0,NULL);XEvent ev;int done=0;while(!done){XNextEvent(d,&ev);if(ev.type==Expose){XImage*im=XCreateImage(d,DefaultVisual(d,scr),(unsigned)DefaultDepth(d,scr),ZPixmap,0,NULL,w,h,32,0);if(im){im->data=calloc((size_t)im->bytes_per_line,h);if(im->data){for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++){size_t k=((size_t)y*w+x)*3;unsigned long px=((unsigned long)rgb[k]<<16)|((unsigned long)rgb[k+1]<<8)|rgb[k+2];XPutPixel(im,(int)x,(int)y,px);}XPutImage(d,win,gc,im,0,0,0,0,w,h);}XDestroyImage(im);}}else if(ev.type==KeyPress||ev.type==DestroyNotify)done=1;}XFreeGC(d,gc);XDestroyWindow(d,win);XCloseDisplay(d);free(rgb);return 0;
#else
    free(rgb);fprintf(stderr,"Image.Show: rebuild with -DPURPLE_HAS_X11 -lX11 to display decoded pixels\n");return -1;
#endif
}

/* PURPLECODE CONFIG: ~/.config/purplecode/config.ini (or PURPLECODE_CONFIG).
 * Missing settings default to compatibility mode: native libraries allowed,
 * ELF execution allowed, and Virtual Mode disabled. */
static int purple_config_value(const char *key, int fallback) {
    const char *override = getenv("PURPLECODE_CONFIG");
    char path[PATH_MAX];
    if (override && *override) snprintf(path, sizeof path, "%s", override);
    else {
        const char *home = getenv("HOME");
        if (!home || !*home) return fallback;
        snprintf(path, sizeof path, "%s/.config/purplecode/config.ini", home);
    }
    FILE *f = fopen(path, "r"); if (!f) return fallback;
    char line[512]; int result = fallback;
    while (fgets(line, sizeof line, f)) {
        char *p = line; while (isspace((unsigned char)*p)) p++;
        if (*p == '#' || *p == ';' || !*p) continue;
        char *eq = strchr(p, '='); if (!eq) continue; *eq++ = 0;
        char *end = p + strlen(p); while (end > p && isspace((unsigned char)end[-1])) *--end = 0;
        while (isspace((unsigned char)*eq)) eq++;
        end = eq + strlen(eq); while (end > eq && isspace((unsigned char)end[-1])) *--end = 0;
        if (!strcmp(p, key)) {
            if (!strcasecmp(eq, "true") || !strcasecmp(eq, "yes") || !strcmp(eq, "1") || !strcasecmp(eq,"on")) result = 1;
            else if (!strcasecmp(eq, "false") || !strcasecmp(eq, "no") || !strcmp(eq, "0") || !strcasecmp(eq,"off")) result = 0;
            break;
        }
    }
    fclose(f); return result;
}
static int purple_safe_code_only(void) { return purple_config_value("SafeCodeOnly", 0); }
static int purple_virtual_mode(void) { return purple_config_value("VirtualMode", 0); }

typedef int64_t (*PurpleNativeControlang)(int64_t,int64_t,int64_t,int64_t,int64_t,int64_t);
typedef struct { char *name; void *handle; PurpleNativeControlang fn; } PurpleControlangEntry;
static PurpleControlangEntry *purple_controlangs = NULL;
static size_t purple_controlang_count = 0, purple_controlang_capacity = 0;
int64_t purple_controlang_register(const char *name,const char *library,const char *symbol) {
    if(!name||!*name||!library||!*library||!symbol||!*symbol){fprintf(stderr,"Controlang.Native.Register: name, library and symbol required\n");return -1;}
    for(const unsigned char*p=(const unsigned char*)name;*p;p++)if(!(isalnum(*p)||*p=='_'||*p=='.')){fprintf(stderr,"Controlang.Native.Register: invalid name %s\n",name);return -1;}
    if (purple_safe_code_only()) { fprintf(stderr, "PURPLECODE CONFIG SafeCodeOnly blocks native ELF plugins\n"); return -1; }
    if (purple_virtual_mode()) { fprintf(stderr, "PURPLECODE CONFIG VirtualMode: in-process .so plugins cannot be isolated; launch a separate ELF with Program.ELF64.Run instead\n"); return -1; }
    void*h=dlopen(library,RTLD_NOW|RTLD_LOCAL);if(!h){fprintf(stderr,"Controlang.Native.Register: %s\n",dlerror());return -1;}
    dlerror();PurpleNativeControlang fn=(PurpleNativeControlang)dlsym(h,symbol);const char*e=dlerror();if(e||!fn){fprintf(stderr,"Controlang.Native.Register: symbol '%s': %s\n",symbol,e?e:"not found");dlclose(h);return -1;}
    for(size_t i=0;i<purple_controlang_count;i++)if(!strcmp(purple_controlangs[i].name,name)){dlclose(purple_controlangs[i].handle);free(purple_controlangs[i].name);purple_controlangs[i].name=strdup(name);purple_controlangs[i].handle=h;purple_controlangs[i].fn=fn;return 0;}
    if (purple_controlang_count == purple_controlang_capacity) {
        size_t cap = purple_controlang_capacity ? purple_controlang_capacity * 2 : 16;
        PurpleControlangEntry *next = realloc(purple_controlangs, cap * sizeof *next);
        if (!next) { dlclose(h); return -1; }
        purple_controlangs = next; purple_controlang_capacity = cap;
    }
    PurpleControlangEntry*e2=&purple_controlangs[purple_controlang_count++];e2->name=strdup(name);e2->handle=h;e2->fn=fn;if(!e2->name){dlclose(h);purple_controlang_count--;return -1;}printf("[Controlang] Registered native feature %s from %s:%s\n",name,library,symbol);return 0;
}
int64_t purple_net_httpsget(const char *host, const char *path, const char *outfile);
int64_t purple_blow_styledlabel(int64_t x,int64_t y,const char *t,const char *style,int64_t color,int64_t sc);
int64_t purple_blow_font(const char *name);
int purple_font_is_registered(const char *name);
int64_t purple_program_elf64_open(const char *path);
int64_t purple_program_elf64_run(const char *path);
int64_t purple_cd_open(const char *path);
int64_t purple_usb_open(const char *path);
int64_t purple_device_read(int64_t fd, char *buf, int64_t max);
int64_t purple_cd_read(int64_t fd, char *buf, int64_t max);
int64_t purple_usb_read(int64_t fd, char *buf, int64_t max);
int64_t purple_cd_toc(int64_t fd);
int64_t purple_cd_status(int64_t fd);
int64_t purple_cd_read_sector(int64_t fd, int64_t lba, char *buf, int64_t nsectors);
int64_t purple_usb_control(int64_t fd, int64_t bmRequestType, int64_t bRequest,
                           int64_t wValue, int64_t wIndex, char *data, int64_t len);
int64_t purple_usb_bulk(int64_t fd, int64_t ep, char *data, int64_t len);
int64_t purple_scsi_command(int64_t fd, const unsigned char *cdb, int64_t cdb_len,
                            char *data, int64_t data_len, int64_t timeout_ms);
static int64_t purple_usb_xfer_len = 256;
int64_t purple_image_load(const char *path);
int64_t purple_image_draw(const char *path, int64_t x, int64_t y);
int64_t purple_image_show(const char *path);
char *purple_LastHexResult = NULL;
static char *purple_encode_result = NULL;
static char *purple_encode(const char *input, int mode) {
    if (!input) return NULL;
    size_t n=strlen(input), cap=mode==0?n*9+1:mode==1?n*2+1:((n+2)/3)*4+1;
    char *out=malloc(cap); if(!out)return NULL; size_t j=0;
    static const char hx[]="0123456789ABCDEF", b64[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if(mode==0){ for(size_t i=0;i<n;i++) for(int bit=7;bit>=0;bit--) out[j++]=(input[i]&(1u<<bit))?'1':'0'; }
    else if(mode==1){for(size_t i=0;i<n;i++){unsigned c=(unsigned char)input[i];out[j++]=hx[c>>4];out[j++]=hx[c&15];}}
    else {for(size_t i=0;i<n;i+=3){unsigned a=(unsigned char)input[i],b=i+1<n?(unsigned char)input[i+1]:0,c=i+2<n?(unsigned char)input[i+2]:0;out[j++]=b64[a>>2];out[j++]=b64[((a&3)<<4)|(b>>4)];out[j++]=i+1<n?b64[((b&15)<<2)|(c>>6)]:'=';out[j++]=i+2<n?b64[c&63]:'=';}}
    out[j]=0; free(purple_encode_result); purple_encode_result=out; purple_LastHexResult=out; return out;
}
typedef struct { char *name; unsigned char *bytes; size_t size; } PurpleFontEntry;
static PurpleFontEntry *purple_fonts=NULL; static size_t purple_font_count=0;
static int purple_font_register(const char *name,const char *data,int64_t len,const char *encoding) {
    if(!name||!*name||!data)return -1;
    size_t n=len>0?(size_t)len:strlen(data); if(n>32u*1024u*1024u)return -1;
    const char *enc=encoding?encoding:"Base64"; size_t outn=0; unsigned char *out=NULL;
    if(!strcasecmp(enc,"Hex")){
        if(n%2)return -1; out=malloc(n/2+1);if(!out)return -1;
        for(size_t i=0;i<n;i+=2){int a=isdigit((unsigned char)data[i])?data[i]-'0':tolower((unsigned char)data[i])-'a'+10;int b=isdigit((unsigned char)data[i+1])?data[i+1]-'0':tolower((unsigned char)data[i+1])-'a'+10;if(a<0||a>15||b<0||b>15){free(out);return -1;}out[outn++]=(unsigned char)((a<<4)|b);}
    } else if(!strcasecmp(enc,"Base64")||!strcasecmp(enc,"B64")) {
        out=malloc((n/4+1)*3+4);if(!out)return -1; int val=0,bits=-8;
        for(size_t i=0;i<n;i++){unsigned char c=(unsigned char)data[i];if(c=='='||isspace(c))continue;const char *p=strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/",c);if(!p){free(out);return -1;}val=(val<<6)|(int)(p-"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/");bits+=6;if(bits>=0){out[outn++]=(unsigned char)((val>>bits)&255);bits-=8;}}
    } else if(!strcasecmp(enc,"Raw")) { out=malloc(n?n:1);if(!out)return -1;memcpy(out,data,n);outn=n; }
    else return -1;
    int valid=outn>=12 && ((out[0]==0 && out[1]==1 && out[2]==0 && out[3]==0) || !memcmp(out,"OTTO",4) || !memcmp(out,"wOFF",4) || !memcmp(out,"wOF2",4) || !memcmp(out,"true",4));
    if(!valid){free(out);return -1;}

    for(size_t i=0;i<purple_font_count;i++)if(!strcmp(purple_fonts[i].name,name)){free(purple_fonts[i].bytes);purple_fonts[i].bytes=out;purple_fonts[i].size=outn;return (int64_t)(i+3);}
    PurpleFontEntry *next=realloc(purple_fonts,(purple_font_count+1)*sizeof *next);if(!next){free(out);return -1;}purple_fonts=next;
    purple_fonts[purple_font_count].name=strdup(name);purple_fonts[purple_font_count].bytes=out;purple_fonts[purple_font_count].size=outn;
    if(!purple_fonts[purple_font_count].name){free(out);return -1;}purple_font_count++;return (int64_t)(purple_font_count+2);
}
int purple_font_is_registered(const char *name){if(!name)return 0;for(size_t i=0;i<purple_font_count;i++)if(!strcmp(purple_fonts[i].name,name))return 1;return 0;}
int64_t purple_blow_ifont(const char *name,const char *data,int64_t len,const char *encoding){return purple_font_register(name,data,len,encoding);}
int purple_font_measure(const char *text,int px){return purple_ft_measure(text,px);}
/* Returns 1 on success. Priority for scalable faces:
 *   1. Compile-time embedded blob (PURPLE_EMBEDDED_FONT_*)
 *   2. Runtime-registered Blow.IFont / IFont data (FT_New_Memory_Face)
 *   3. System path search (last resort)
 * Emergency always succeeds without FreeType. */
int purple_font_select(const char *name){
    if(!name) return 0;
    if(!strcmp(name,"Builtin Emergency Font")){
#ifdef PURPLE_HAS_FREETYPE
        if(g_ft_face){FT_Done_Face(g_ft_face);g_ft_face=NULL;}
#endif
        return 1;
    }
#ifdef PURPLE_HAS_FREETYPE
    /* 1. Compile-time deep embedding */
    if (purple_try_embedded_font(name)) return 1;
    /* 2. Runtime-registered in-memory faces (Blow.IFont) */
    for(size_t i=0;i<purple_font_count;i++)if(!strcmp(purple_fonts[i].name,name)){
        if(purple_ft_load_memory(purple_fonts[i].bytes, purple_fonts[i].size)) return 1;
        return 0;
    }
    /* 3. System path search only for the two preferred built-in names */
    if(!strcmp(name,"FantasqueSansM Nerd Font Propo") || !strcmp(name,"Arial"))
        return purple_ft_select_system(name);
    return 0;
#else
    return !strcmp(name,"Builtin Emergency Font") || purple_font_is_registered(name);
#endif
}

int64_t purple_controlang_call(const char*name,const int64_t args[6]) {
    if(name && (!strcmp(name,"Binarify")||!strcmp(name,"Hexify")||!strcmp(name,"Base64ify")))
        return (int64_t)(uintptr_t)purple_encode((const char *)(uintptr_t)args[0],!strcmp(name,"Binarify")?0:!strcmp(name,"Hexify")?1:2);
    if(name && !strcmp(name,"Encoding.LastResult")) return (int64_t)(uintptr_t)(purple_LastHexResult?purple_LastHexResult:"");
    if(name && (!strcmp(name,"Blow.IFont")||!strcmp(name,"IFont"))) {
        const char *enc=args[3]?(const char *)(uintptr_t)args[3]:(const char *)(uintptr_t)args[2];
        int64_t len=args[3]?args[2]:0;
        return purple_font_register((const char *)(uintptr_t)args[0],(const char *)(uintptr_t)args[1],len,enc);
    }
    if(name && !strcmp(name,"Blow.Font")) return purple_blow_font((const char *)(uintptr_t)args[0]);
    if(name && !strcmp(name,"Blow.StyledLabel")) return purple_blow_styledlabel(args[0],args[1],(const char *)(uintptr_t)args[2],(const char *)(uintptr_t)args[3],args[4],args[5]);
    if(name && !strcmp(name,"Net.HttpsGet"))
        return purple_net_httpsget((const char *)(uintptr_t)args[0],(const char *)(uintptr_t)args[1],(const char *)(uintptr_t)args[2]);
    if(name && (!strcmp(name,"Image.Load") || !strcmp(name,"Image.Decode")))
        return purple_image_load((const char *)(uintptr_t)args[0]);
    if(name && (!strcmp(name,"Image.Draw") || !strcmp(name,"Blow.Image") || !strcmp(name,"Image.Blit")))
        return purple_image_draw((const char *)(uintptr_t)args[0], args[1], args[2]);
    if(name && !strcmp(name,"Image.Show"))
        return purple_image_show((const char *)(uintptr_t)args[0]);
    if(name && (!strcmp(name,"Program.ELF64.Open") || !strcmp(name,"Program.Open")))
        return purple_program_elf64_open((const char *)(uintptr_t)args[0]);
    if(name && (!strcmp(name,"Program.ELF64.Run") || !strcmp(name,"Program.Run") || !strcmp(name,"Program.ELF64.Exec")))
        return purple_program_elf64_run((const char *)(uintptr_t)args[0]);
    if(name && (!strcmp(name,"Program.ELF64.Close") || !strcmp(name,"Program.Close"))) {
        int fd = (int)args[0];
        if(fd >= 0) { close(fd); return 0; }
        return -1;
    }
    if(name && !strcmp(name,"CD.Open"))
        return purple_cd_open((const char *)(uintptr_t)args[0]);
    if(name && !strcmp(name,"USB.Open"))
        return purple_usb_open((const char *)(uintptr_t)args[0]);
    if(name && (!strcmp(name,"CD.Read") || !strcmp(name,"USB.Read")))
        return purple_device_read(args[0], (char *)(uintptr_t)args[1], args[2]);
    if(name && (!strcmp(name,"CD.Close") || !strcmp(name,"USB.Close"))) {
        int fd = (int)args[0];
        if(fd >= 0) { close(fd); return 0; }
        return -1;
    }
    if(name && !strcmp(name,"CD.TOC"))
        return purple_cd_toc(args[0]);
    if(name && !strcmp(name,"CD.Status"))
        return purple_cd_status(args[0]);
    if(name && !strcmp(name,"CD.ReadSector"))
        return purple_cd_read_sector(args[0], args[1], (char *)(uintptr_t)args[2], args[3]);
    if(name && !strcmp(name,"USB.Control")) {
        /* args: fd, bmRequestType, bRequest, wValue, wIndex, data_ptr
         * Length taken from the most recent USB.TransferLen (default 256).
         * For zero-length controls pass a null/0 data pointer. */
        char *data = (char *)(uintptr_t)args[5];
        int64_t len = data ? purple_usb_xfer_len : 0;
        if (!data && args[5] > 0 && args[5] <= 65535) len = args[5];
        return purple_usb_control(args[0], args[1], args[2], args[3], args[4], data, len);
    }
    if(name && !strcmp(name,"USB.TransferLen")) {
        /* Set the length used by subsequent USB.Control calls. */
        purple_usb_xfer_len = (args[0] > 0 && args[0] <= 65535) ? args[0] : 256;
        return purple_usb_xfer_len;
    }
    if(name && !strcmp(name,"USB.Bulk")) {
        /* args: fd, ep, data_ptr, len */
        return purple_usb_bulk(args[0], args[1], (char *)(uintptr_t)args[2], args[3]);
    }
    if(name && (!strcmp(name,"SCSI.Command") || !strcmp(name,"CD.SCSI") || !strcmp(name,"USB.SCSI"))) {
        /* args: fd, cdb_ptr, cdb_len, data_ptr, data_len, timeout_ms
         * data_len >0 = FROM_DEV, <0 = TO_DEV (absolute value used). */
        return purple_scsi_command(args[0],
            (const unsigned char *)(uintptr_t)args[1], args[2],
            (char *)(uintptr_t)args[3], args[4], args[5]);
    }
    for(size_t i=0;i<purple_controlang_count;i++)if(!strcmp(purple_controlangs[i].name,name))return purple_controlangs[i].fn(args[0],args[1],args[2],args[3],args[4],args[5]);
    fprintf(stderr,"Controlang: feature '%s' is not registered (use Controlang.Native.Register)\n",name?name:"(null)");return -1;
}

int64_t purple_kustom_gui_make_window(const char *title) {
#ifdef PURPLE_HAS_X11
    Display *d=XOpenDisplay(NULL);if(!d){fprintf(stderr,"Kustom.GUI.MakeWindow: cannot open X display\n");return -1;}
    int screen=DefaultScreen(d);unsigned w=800,h=500;Window win=XCreateSimpleWindow(d,RootWindow(d,screen),100,100,w,h,1,BlackPixel(d,screen),WhitePixel(d,screen));
    XStoreName(d,win,(title&&*title)?title:"Purplecode Window");XSelectInput(d,win,ExposureMask|KeyPressMask|StructureNotifyMask);Atom wm_delete=XInternAtom(d,"WM_DELETE_WINDOW",False);XSetWMProtocols(d,win,&wm_delete,1);GC gc=XCreateGC(d,win,0,NULL);XMapWindow(d,win);XFlush(d);
    int done=0;while(!done){XEvent e;XNextEvent(d,&e);if(e.type==Expose){XSetForeground(d,gc,0x6f42c1);XFillRectangle(d,win,gc,0,0,w,72);XSetForeground(d,gc,WhitePixel(d,screen));const char*label=(title&&*title)?title:"Purplecode Window";XDrawString(d,win,gc,24,42,label,(int)strlen(label));XFlush(d);}else if(e.type==KeyPress)done=1;else if(e.type==ClientMessage&&(Atom)e.xclient.data.l[0]==wm_delete)done=1;else if(e.type==DestroyNotify)done=1;}
    XFreeGC(d,gc);XDestroyWindow(d,win);XCloseDisplay(d);return 0;
#else
    (void)title;fprintf(stderr,"Kustom.GUI.MakeWindow: rebuild with -DPURPLE_HAS_X11 -lX11 to create a real window\\n");return -1;
#endif
}

/* Unsafe by design: caller must provide a valid readable address and length.
 * Copy is capped at 16 MiB; bytes are inspected as data, never executed. */
int64_t purple_web_js_load_memaddr(uint64_t address, uint64_t length) {
    if (!address || length > 16u * 1024u * 1024u) return -1;
    const unsigned char *src = (const unsigned char *)(uintptr_t)address;
    unsigned char *copy = malloc((size_t)length + 1);
    if (!copy) return -1;
    memcpy(copy, src, (size_t)length);
    copy[length] = 0;
    printf("[Web.JS] Loaded %llu bytes from memory address 0x%llx (not executed)\n",
           (unsigned long long)length, (unsigned long long)address);
    free(copy);
    return (int64_t)length;
}

/* Raw Linux syscall bridge. This is intentionally low-level and Linux-specific.
 * Only use it with syscall numbers and pointers you understand. */
int64_t purple_kernel_call(uint64_t number, uint64_t a1, uint64_t a2, uint64_t a3) {
    errno = 0;
    long r = syscall((long)number, (long)a1, (long)a2, (long)a3);
    return (int64_t)r;
}

/* ========== LinuxAPI.Call ========== */
int64_t purple_linux_api_call(uint64_t number, ...) {
    va_list ap;
    va_start(ap, number);
    int64_t ret = -1;

    switch (number) {
        case 1: { /* write */
            int fd = va_arg(ap, int);
            const void *buf = va_arg(ap, const void *);
            size_t count = va_arg(ap, size_t);
            ret = (int64_t)write(fd, buf, count);
            break;
        }
        case 60: { /* exit */
            int code = va_arg(ap, int);
            _exit(code);
            break;
        }
        case 39: /* getpid */
            ret = (int64_t)getpid();
            break;
        default: {
            long a1 = va_arg(ap, long);
            long a2 = va_arg(ap, long);
            long a3 = va_arg(ap, long);
            ret = (int64_t)syscall((long)number, a1, a2, a3);
            break;
        }
    }

    va_end(ap);
    return ret;
}

/* ========== PurpleAPI.Call – Milestone 1 1.6 (Blitter + GUI) ========== */
int64_t purple_api_call(uint64_t number, ...) {
    va_list ap;
    va_start(ap, number);
    int64_t ret = 0;

    switch (number) {
        case 1: { /* DRAW_RECT into software blitter (no auto-present) */
            int x = (int)va_arg(ap, long);
            int y = (int)va_arg(ap, long);
            int w = (int)va_arg(ap, long);
            int h = (int)va_arg(ap, long);
            uint32_t color = (uint32_t)va_arg(ap, long);
            purple_gui_draw_rect(x, y, w, h, color ? color : 0x00FF00);
            ret = 1;
            break;
        }
        case 2: { /* clear via blitter */
            purple_gui_clear(0xFF000000);
            ret = 1;
            break;
        }
        case 3: { /* setPixel via blitter */
            int x = (int)va_arg(ap, long);
            int y = (int)va_arg(ap, long);
            uint32_t color = (uint32_t)va_arg(ap, long);
            purple_gui_set_pixel(x, y, color);
            ret = 1;
            break;
        }
        case 4: { /* present blitter -> display */
            purple_gui_present();
            ret = 1;
            break;
        }
        case 5: { /* pollKey */
            ret = purple_gui_poll_key();
            break;
        }
        case 6: { /* pump(ms) */
            int ms = (int)va_arg(ap, long);
            ret = purple_gui_pump(ms);
            break;
        }
        case 10: { /* createWindow defaults */
            ret = purple_gui_create_window(800, 500, "Purplecode Milestone 1 1.6", 0);
            break;
        }
        case 11: { /* createWindow(w,h,title) */
            int w = (int)va_arg(ap, long);
            int h = (int)va_arg(ap, long);
            const char *title = (const char *)(uintptr_t)va_arg(ap, long);
            if (!title) title = "Purplecode Milestone 1 1.6";
            ret = purple_gui_create_window(w, h, title, 0);
            break;
        }
        case 20: { /* Blit.FillRect */
            int x = (int)va_arg(ap, long);
            int y = (int)va_arg(ap, long);
            int w = (int)va_arg(ap, long);
            int h = (int)va_arg(ap, long);
            uint32_t color = (uint32_t)va_arg(ap, long);
            ret = purple_blit_fill(x, y, w, h, color);
            break;
        }
        case 21: { /* Blit.Clear(color) */
            uint32_t color = (uint32_t)va_arg(ap, long);
            ret = purple_blit_clear(color);
            break;
        }
        case 22: { /* Blit.Present */
            purple_gui_present();
            ret = 1;
            break;
        }
        default:
            printf("[PurpleAPI] unknown call number %lu\n", (unsigned long)number);
            ret = -1;
            break;
    }
    va_end(ap);
    return ret;
}

/* ========== GUI hierarchical dispatcher (Milestone 1 1.6) ========== */
int64_t purple_gui_dispatch(const char *name, int64_t arg0, ...) {
    if (!name) return -1;

    if (strstr(name, "GUI.Window.Create") != NULL || strstr(name, "GUI.Create") != NULL ||
        strstr(name, "GUI.NativeWindow") != NULL) {
        return purple_api_call(10);
    }
    if (strstr(name, "GUI.Window.CreateSized") != NULL || strstr(name, "Kustom.GUI.MakeWindow") != NULL) {
        va_list ap; va_start(ap, arg0);
        int w = (int)arg0;
        int h = (int)va_arg(ap, long);
        const char *title = (const char *)(uintptr_t)va_arg(ap, long);
        va_end(ap);
        /* Kustom.GUI.MakeWindow historically took only a title; size defaults apply */
        if (strstr(name, "Kustom.GUI.MakeWindow") != NULL && h <= 0)
            return purple_kustom_gui_make_window(title ? title : "Purplecode Window");
        return purple_gui_create_window(w > 0 ? w : 800, h > 0 ? h : 500, title ? title : "Purplecode", 0);
    }
    if (strstr(name, "GUI.Draw.Rect") != NULL || strstr(name, "GUI.Rect") != NULL) {
        va_list ap; va_start(ap, arg0);
        int x = (int)arg0;
        int y = (int)va_arg(ap, long);
        int w = (int)va_arg(ap, long);
        int h = (int)va_arg(ap, long);
        uint32_t color = (uint32_t)va_arg(ap, long);
        va_end(ap);
        purple_gui_draw_rect(x, y, w, h, color);
        return 1;
    }
    if (strstr(name, "GUI.Clear") != NULL) {
        /* GUI.Clear() or GUI.Clear(color) – default solid black */
        uint32_t color = (arg0 != 0) ? (uint32_t)arg0 : 0xFF000000u;
        purple_gui_clear(color);
        return 1;
    }
    if (strstr(name, "Blit.Fill") != NULL || strstr(name, "Blit.Rect") != NULL ||
        strstr(name, "GUI.Draw.Rect") != NULL || strstr(name, "GUI.Rect") != NULL) {
        va_list ap; va_start(ap, arg0);
        int x = (int)arg0;
        int y = (int)va_arg(ap, long);
        int w = (int)va_arg(ap, long);
        int h = (int)va_arg(ap, long);
        uint32_t color = (uint32_t)va_arg(ap, long);
        va_end(ap);
        return purple_blit_fill(x, y, w, h, color);
    }
    if (strstr(name, "Blit.Clear") != NULL) {
        return purple_blit_clear((uint32_t)arg0);
    }
    if (strstr(name, "Blit.Present") != NULL ||
        strstr(name, "GUI.Present") != NULL || strstr(name, "GUI.Flush") != NULL) {
        purple_gui_present();
        return 1;
    }
    if (strstr(name, "GUI.PollKey") != NULL || strstr(name, "GUI.Key") != NULL) {
        return purple_gui_poll_key();
    }
    if (strstr(name, "GUI.Pump") != NULL) {
        return purple_gui_pump((int)arg0);
    }
    if (strstr(name, "GUI.DrawLine") != NULL || strstr(name, "GUI.Line") != NULL ||
        strstr(name, "Blit.Line") != NULL) {
        va_list ap; va_start(ap, arg0);
        int x0 = (int)arg0;
        int y0 = (int)va_arg(ap, long);
        int x1 = (int)va_arg(ap, long);
        int y1 = (int)va_arg(ap, long);
        uint32_t color = (uint32_t)va_arg(ap, long);
        va_end(ap);
        blit_draw_line(x0, y0, x1, y1, color);
        return 1;
    }
    if (strstr(name, "GUI.GetWidth") != NULL || strstr(name, "GUI.Width") != NULL) {
        return (int64_t)g_fb_w;
    }
    if (strstr(name, "GUI.GetHeight") != NULL || strstr(name, "GUI.Height") != NULL) {
        return (int64_t)g_fb_h;
    }

    /* Font / text drawing into the blitter */
    if (strstr(name, "GUI.DrawText") != NULL || strstr(name, "GUI.Text") != NULL ||
        strstr(name, "GUI.Font.Print") != NULL || strstr(name, "GUI.Print") != NULL) {
        va_list ap; va_start(ap, arg0);
        int x = (int)arg0;
        int y = (int)va_arg(ap, long);
        const char *text = (const char *)(uintptr_t)va_arg(ap, long);
        uint32_t color = (uint32_t)va_arg(ap, long);
        va_end(ap);
        return purple_gui_draw_text(x, y, text, color);
    }
    if (strstr(name, "GUI.DrawChar") != NULL || strstr(name, "GUI.Char") != NULL) {
        va_list ap; va_start(ap, arg0);
        int x = (int)arg0;
        int y = (int)va_arg(ap, long);
        int ch = (int)va_arg(ap, long);
        uint32_t color = (uint32_t)va_arg(ap, long);
        va_end(ap);
        return purple_gui_draw_char(x, y, ch, color);
    }
    if (strstr(name, "GUI.Font.Width") != NULL) {
        return PURPLE_FONT_W;
    }
    if (strstr(name, "GUI.Font.Height") != NULL) {
        return PURPLE_FONT_H;
    }

    /* Unknown hierarchical GUI name – fail cleanly, no staged/stub messages */
    return -1;
}

/* ========== Data.Write.Memaddr ========== */
int purple_data_write_memaddr(uint64_t addr, const char *data_or_path) {
    if (!data_or_path) return -1;
    const char *src = data_or_path;
    size_t len = 0;
    char *filebuf = NULL;

    if (data_or_path[0] == '/' || strstr(data_or_path, "/") != NULL) {
        FILE *f = fopen(data_or_path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz > 0 && sz < 16*1024*1024) {
                filebuf = (char *)malloc((size_t)sz + 1);
                if (filebuf) {
                    size_t n = fread(filebuf, 1, (size_t)sz, f);
                    filebuf[n] = '\0';
                    src = filebuf;
                    len = n;
                }
            }
            fclose(f);
        }
    }

    if (len == 0) len = strlen(src);

    volatile char *dst = (volatile char *)(uintptr_t)addr;
    for (size_t i = 0; i < len; i++) {
        dst[i] = src[i];
    }
    dst[len] = '\0';

    if (filebuf) free(filebuf);
    return (int)len;
}

/* ========== Program.ELF64.Open ========== */
int64_t purple_program_elf64_open(const char *path) {
    if (!path) return -1;
    if (purple_safe_code_only()) {
        fprintf(stderr, "Program.ELF64.Open blocked: PURPLECODE CONFIG SafeCodeOnly=true\n");
        return -1;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("Program.ELF64.Open");
        return -1;
    }

    unsigned char magic[4];
    if (read(fd, magic, 4) == 4) {
        if (magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F') {
            lseek(fd, 0, SEEK_SET);
            printf("[Program.ELF64] Opened valid ELF64: %s (fd=%d)\n", path, fd);
            return (int64_t)fd;
        }
    }
    close(fd);
    printf("[Program.ELF64] Not a valid ELF or cannot read: %s\n", path);
    return -1;
}

/* ========== CD.* / USB.* – full low-level device, USB control/bulk & SCSI ==========
 * SafeCodeOnly blocks all raw device opens. VirtualMode is not applied here
 * because device nodes cannot be safely isolated by bubblewrap in the same way
 * as user-space ELFs; the caller must already have permission on the host.
 * Typical paths: /dev/sr0 (CD/DVD), /dev/cdrom, /dev/sda1, /dev/sdb, /dev/bus/usb/...
 * USB.Control / USB.Bulk require a usbfs node (/dev/bus/usb/BBB/DDD).
 * SCSI.Command / CD.SCSI / USB.SCSI work on any SG_IO-capable fd (sr*, sg*, many USB MSD).
 */
static int purple_device_open(const char *path, const char *api) {
    if (!path || !*path) return -1;
    if (purple_safe_code_only()) {
        fprintf(stderr, "%s blocked: PURPLECODE CONFIG SafeCodeOnly=true (raw devices denied)\n", api);
        return -1;
    }
    /* Only allow paths under /dev/ to reduce accidental open of regular files */
    if (strncmp(path, "/dev/", 5) != 0) {
        fprintf(stderr, "%s: path must be under /dev/ (got %s)\n", api, path);
        return -1;
    }
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        perror(api);
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !(S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode))) {
        fprintf(stderr, "%s: not a block or character device: %s\n", api, path);
        close(fd);
        return -1;
    }
    printf("[%s] Opened device %s (fd=%d)\n", api, path, fd);
    return fd;
}

int64_t purple_cd_open(const char *path) {
    /* Default to the first optical drive if no path given */
    if (!path || !*path) path = "/dev/sr0";
    return (int64_t)purple_device_open(path, "CD.Open");
}

int64_t purple_usb_open(const char *path) {
    if (!path || !*path) {
        fprintf(stderr, "USB.Open: device path required (e.g. /dev/sdb or /dev/bus/usb/001/002)\n");
        return -1;
    }
    return (int64_t)purple_device_open(path, "USB.Open");
}

/* Read up to `max` bytes from an open device fd into the caller buffer.
 * Returns bytes read, or -1 on error. Sector alignment is the caller's responsibility. */
int64_t purple_device_read(int64_t fd, char *buf, int64_t max) {
    if (fd < 0 || !buf || max < 1) return -1;
    if (max > 16 * 1024 * 1024) max = 16 * 1024 * 1024; /* hard safety cap */
    ssize_t n = read((int)fd, buf, (size_t)max);
    if (n < 0) { perror("CD/USB.Read"); return -1; }
    return (int64_t)n;
}

int64_t purple_cd_read(int64_t fd, char *buf, int64_t max) {
    return purple_device_read(fd, buf, max);
}

int64_t purple_usb_read(int64_t fd, char *buf, int64_t max) {
    return purple_device_read(fd, buf, max);
}

/* ---- CD-ROM SCSI / MMC via SG_IO and linux/cdrom.h ----
 * CD.TOC(fd)           – print / return track count (via CDROMREADTOCHDR)
 * CD.ReadSector(fd, LBA, buf, nsectors) – READ(10) via SG_IO, 2048-byte sectors
 * CD.Status(fd)        – CDROM_DRIVE_STATUS
 * USB.Control(fd, bmRequestType, bRequest, wValue, wIndex, data, len)
 *   – usbfs control transfer when fd is a usbfs device node
 */
int64_t purple_cd_toc(int64_t fd) {
    if (fd < 0) return -1;
    struct cdrom_tochdr hdr;
    memset(&hdr, 0, sizeof hdr);
    if (ioctl((int)fd, CDROMREADTOCHDR, &hdr) < 0) {
        perror("CD.TOC (CDROMREADTOCHDR)");
        return -1;
    }
    printf("[CD.TOC] first track %u, last track %u\n", (unsigned)hdr.cdth_trk0, (unsigned)hdr.cdth_trk1);
    return (int64_t)(hdr.cdth_trk1 - hdr.cdth_trk0 + 1);
}

int64_t purple_cd_status(int64_t fd) {
    if (fd < 0) return -1;
    int st = ioctl((int)fd, CDROM_DRIVE_STATUS, CDSL_CURRENT);
    if (st < 0) { perror("CD.Status"); return -1; }
    const char *msg = "unknown";
    switch (st) {
        case CDS_NO_INFO: msg = "no info"; break;
        case CDS_NO_DISC: msg = "no disc"; break;
        case CDS_TRAY_OPEN: msg = "tray open"; break;
        case CDS_DRIVE_NOT_READY: msg = "not ready"; break;
        case CDS_DISC_OK: msg = "disc ok"; break;
    }
    printf("[CD.Status] %s (%d)\n", msg, st);
    return (int64_t)st;
}

/* READ(10) – 2048-byte Mode-1 sectors. buf must hold nsectors*2048 bytes. */
int64_t purple_cd_read_sector(int64_t fd, int64_t lba, char *buf, int64_t nsectors) {
    if (fd < 0 || !buf || nsectors < 1 || nsectors > 64) return -1;
    if (lba < 0) return -1;
    size_t len = (size_t)nsectors * 2048u;
    unsigned char cdb[10];
    memset(cdb, 0, sizeof cdb);
    cdb[0] = 0x28; /* READ(10) */
    cdb[2] = (unsigned char)((lba >> 24) & 0xff);
    cdb[3] = (unsigned char)((lba >> 16) & 0xff);
    cdb[4] = (unsigned char)((lba >> 8) & 0xff);
    cdb[5] = (unsigned char)(lba & 0xff);
    cdb[7] = (unsigned char)((nsectors >> 8) & 0xff);
    cdb[8] = (unsigned char)(nsectors & 0xff);

    unsigned char sense[32];
    memset(sense, 0, sizeof sense);
    sg_io_hdr_t io;
    memset(&io, 0, sizeof io);
    io.interface_id = 'S';
    io.cmd_len = sizeof cdb;
    io.mx_sb_len = sizeof sense;
    io.dxfer_direction = SG_DXFER_FROM_DEV;
    io.dxfer_len = (unsigned int)len;
    io.dxferp = buf;
    io.cmdp = cdb;
    io.sbp = sense;
    io.timeout = 20000; /* ms */

    if (ioctl((int)fd, SG_IO, &io) < 0) {
        perror("CD.ReadSector (SG_IO)");
        return -1;
    }
    if ((io.info & SG_INFO_OK_MASK) != SG_INFO_OK) {
        fprintf(stderr, "CD.ReadSector: SCSI status 0x%02x host 0x%x driver 0x%x sense key 0x%02x\n",
                io.status, io.host_status, io.driver_status, sense[2] & 0x0f);
        return -1;
    }
    return (int64_t)len;
}

/* Full low-level USB control transfer via usbfs (USBDEVFS_CONTROL).
 * fd must be an open usbfs node such as /dev/bus/usb/BBB/DDD.
 * data may be NULL when len==0. Direction is taken from bmRequestType bit 7.
 * Returns transferred length on success, or -1 on error. */
int64_t purple_usb_control(int64_t fd, int64_t bmRequestType, int64_t bRequest,
                           int64_t wValue, int64_t wIndex, char *data, int64_t len) {
    if (fd < 0) return -1;
    if (len < 0 || len > 65535) return -1;
    if (purple_safe_code_only()) {
        fprintf(stderr, "USB.Control blocked: SafeCodeOnly=true\n");
        return -1;
    }
    struct usbdevfs_ctrltransfer ctrl;
    memset(&ctrl, 0, sizeof ctrl);
    ctrl.bRequestType = (uint8_t)bmRequestType;
    ctrl.bRequest     = (uint8_t)bRequest;
    ctrl.wValue       = (uint16_t)wValue;
    ctrl.wIndex       = (uint16_t)wIndex;
    ctrl.wLength      = (uint16_t)len;
    ctrl.timeout      = 5000;
    ctrl.data         = data;
    if (ioctl((int)fd, USBDEVFS_CONTROL, &ctrl) < 0) {
        perror("USB.Control");
        return -1;
    }
    return (int64_t)ctrl.wLength;
}

/* USB bulk transfer via USBDEVFS_BULK.
 * ep is the endpoint address (including direction bit).
 * Returns bytes transferred or -1. */
int64_t purple_usb_bulk(int64_t fd, int64_t ep, char *data, int64_t len) {
    if (fd < 0 || !data || len < 1 || len > 16*1024*1024) return -1;
    if (purple_safe_code_only()) {
        fprintf(stderr, "USB.Bulk blocked: SafeCodeOnly=true\n");
        return -1;
    }
    struct usbdevfs_bulktransfer bulk;
    memset(&bulk, 0, sizeof bulk);
    bulk.ep = (unsigned int)ep;
    bulk.len = (unsigned int)len;
    bulk.timeout = 5000;
    bulk.data = data;
    if (ioctl((int)fd, USBDEVFS_BULK, &bulk) < 0) {
        perror("USB.Bulk");
        return -1;
    }
    return (int64_t)bulk.len;
}

/* General SCSI command via SG_IO (works for CD/DVD and many USB mass-storage
 * devices that expose a SCSI generic interface). cdb must be 6/10/12/16 bytes;
 * data direction is inferred from dxfer_len sign convention: positive = FROM_DEV.
 * Returns residual or bytes transferred; -1 on error. */
int64_t purple_scsi_command(int64_t fd, const unsigned char *cdb, int64_t cdb_len,
                            char *data, int64_t data_len, int64_t timeout_ms) {
    if (fd < 0 || !cdb || cdb_len < 6 || cdb_len > 16) return -1;
    if (data_len < 0 || data_len > 16*1024*1024) return -1;
    if (purple_safe_code_only()) {
        fprintf(stderr, "SCSI.Command blocked: SafeCodeOnly=true\n");
        return -1;
    }
    unsigned char sense[32];
    memset(sense, 0, sizeof sense);
    sg_io_hdr_t io;
    memset(&io, 0, sizeof io);
    io.interface_id = 'S';
    io.cmd_len = (unsigned char)cdb_len;
    io.mx_sb_len = sizeof sense;
    io.dxfer_direction = data_len > 0 ? SG_DXFER_FROM_DEV : (data_len < 0 ? SG_DXFER_TO_DEV : SG_DXFER_NONE);
    io.dxfer_len = (unsigned int)(data_len < 0 ? -data_len : data_len);
    io.dxferp = data;
    io.cmdp = (unsigned char *)cdb;
    io.sbp = sense;
    io.timeout = timeout_ms > 0 ? (unsigned int)timeout_ms : 20000;
    if (ioctl((int)fd, SG_IO, &io) < 0) {
        perror("SCSI.Command (SG_IO)");
        return -1;
    }
    if ((io.info & SG_INFO_OK_MASK) != SG_INFO_OK) {
        fprintf(stderr, "SCSI.Command: status 0x%02x host 0x%x driver 0x%x sense key 0x%02x\n",
                io.status, io.host_status, io.driver_status, sense[2] & 0x0f);
        return -1;
    }
    return (int64_t)(io.dxfer_len - io.resid);
}

/* Execute a trusted ELF. Virtual Mode fails closed unless bubblewrap (bwrap)
 * is installed; it never silently falls back to running on the host. */
int64_t purple_program_elf64_run(const char *path) {
    if (!path || !*path) return -1;
    if (purple_safe_code_only()) {
        fprintf(stderr, "Program.ELF64.Run blocked: PURPLECODE CONFIG SafeCodeOnly=true\n");
        return -1;
    }
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || access(path, X_OK) != 0) {
        fprintf(stderr, "Program.ELF64.Run: file must exist, be regular and executable: %s\n", path);
        return -1;
    }
    int fd = open(path, O_RDONLY); if (fd < 0) return -1;
    unsigned char magic[4]; ssize_t n = read(fd, magic, sizeof magic); close(fd);
    if (n != 4 || magic[0] != 0x7f || magic[1] != 'E' || magic[2] != 'L' || magic[3] != 'F') {
        fprintf(stderr, "Program.ELF64.Run: not an ELF file: %s\n", path); return -1;
    }
    pid_t pid = fork(); if (pid < 0) { perror("fork"); return -1; }
    if (pid == 0) {
        if (!purple_virtual_mode()) { execl(path, path, (char *)NULL); perror("exec ELF"); _exit(127); }
        /* Minimal filesystem + PID/network/IPC isolation using bubblewrap. */
        char *args[64]; size_t ai = 0;
#define BARG(x) (args[ai++] = (char *)(x))
        BARG("bwrap"); BARG("--die-with-parent"); BARG("--new-session"); BARG("--unshare-all");
        BARG("--clearenv"); BARG("--setenv"); BARG("PATH"); BARG("/usr/bin:/bin");
        BARG("--dev"); BARG("/dev"); BARG("--proc"); BARG("/proc");
        BARG("--tmpfs"); BARG("/tmp"); BARG("--tmpfs"); BARG("/home");
        /* Read-only runtime paths only; user data and writable host paths are not mounted. */
        const char *dirs[][2] = {{"/usr","/usr"},{"/bin","/bin"},{"/lib","/lib"},{"/lib64","/lib64"},{"/etc","/etc"}};
        for (size_t di = 0; di < sizeof dirs / sizeof dirs[0]; di++) {
            struct stat ds;
            if (stat(dirs[di][0], &ds) == 0) { BARG("--ro-bind-try"); BARG(dirs[di][0]); BARG(dirs[di][1]); }
        }
        BARG("--dir"); BARG("/app"); BARG("--ro-bind"); BARG((char *)path); BARG("/app/program");
        BARG("--chdir"); BARG("/tmp"); BARG("/app/program"); args[ai] = NULL;
        execvp("bwrap", args); perror("VirtualMode requires bubblewrap (bwrap)"); _exit(126);
#undef BARG
    }
    int status = 0; while (waitpid(pid, &status, 0) < 0) { if (errno == EINTR) continue; perror("waitpid"); return -1; }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

void purple_mem_poke(uint64_t addr, uint64_t value) {
    *(volatile uint64_t *)(uintptr_t)addr = value;
}

uint64_t purple_mem_peek(uint64_t addr) {
    return *(volatile uint64_t *)(uintptr_t)addr;
}

/* Purplecode 2.3 modules */
#include "purple_files.inc"
#include "purple_net.inc"
#include "purple_blow.inc"
