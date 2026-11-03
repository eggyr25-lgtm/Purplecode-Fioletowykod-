#ifndef PURPLE_RT_H
#define PURPLE_RT_H

#include <stdint.h>
#include <stddef.h>

/* Result of the most recent Binarify/Hexify/Base64ify call. */
extern char *purple_LastHexResult;
int64_t purple_net_httpsget(const char *host, const char *path, const char *outfile);

#ifdef __cplusplus
extern "C" {
#endif

/* File system */
int purple_fs_write(const char *data, const char *path);

/* Printing */
void purple_shell_print(const char *s);
void purple_shell_print_addr(uint64_t addr);
void purple_shell_print_int(int64_t value);

/* Linux syscall interface – extremely thin */
int64_t purple_linux_api_call(uint64_t number, ...);
/* Explicit low-level Linux syscall bridge: syscall number + three arguments. */
int64_t purple_kernel_call(uint64_t number, uint64_t a1, uint64_t a2, uint64_t a3);
int64_t purple_web_js_load(const char *path);
int64_t purple_web_js_run(const char *path);
int64_t purple_web_html_load(const char *path);
int64_t purple_web_css_load(const char *path);
int64_t purple_image_load(const char *path);
int64_t purple_media_read(const char *path, const char *kind);
int64_t purple_media_read_sector(const char *device, uint64_t lba, const char *kind);
int64_t purple_media_read_sector_to(const char *device, uint64_t lba, const char *output, const char *kind);
int64_t purple_image_show(const char *path);
int64_t purple_controlang_register(const char *name, const char *library, const char *symbol);
int64_t purple_controlang_call(const char *name, const int64_t args[6]);
int64_t purple_kustom_gui_make_window(const char *title);
int64_t purple_web_js_load_memaddr(uint64_t address, uint64_t length);
int64_t purple_fs_exists(const char *path);
int64_t purple_fs_size(const char *path);
int64_t purple_process_id(void);
int64_t purple_time_unix(void);
uint64_t purple_memory_alloc(uint64_t size);
void purple_memory_free(uint64_t ptr);

/* PurpleAPI convenience helpers – Milestone 1 1.2 */
int64_t purple_api_call(uint64_t number, ...);
/* Predefined numbers:
 * 1   : drawRect into software blitter (x,y,w,h,color) – call 4 to present
 * 2   : clear blitter (black)
 * 3   : setPixel into blitter
 * 4   : present blitter to display (X11 or Wayland)
 * 5   : pollKey (arrows 1000-1003, Esc 27, -1 = closed)
 * 6   : pump(ms)
 * 10  : createWindow 800x500
 * 11  : createWindow(w, h, title_ptr)
 * 20  : Blit.FillRect (explicit)
 * 21  : Blit.Clear(color)
 * 22  : Blit.Present
 *
 * Software blitter = ARGB8888 framebuffer. X11 and Wayland are presenters.
 * Build: -DPURPLE_HAS_X11 -lX11   and/or   -DPURPLE_HAS_WAYLAND -lwayland-client
 */

/* GUI dispatcher for hierarchical GUI.* calls */
int64_t purple_gui_dispatch(const char *name, int64_t arg0, ...);

/* Data.Write.Memaddr – write string or file content into a memory address */
int purple_data_write_memaddr(uint64_t addr, const char *data_or_path);

/* Program.ELF64.Open – open / load an ELF64 binary (returns fd or handle) */
int64_t purple_program_elf64_open(const char *path);
int64_t purple_program_elf64_run(const char *path);

/* Extra helpers */
void purple_mem_poke(uint64_t addr, uint64_t value);
uint64_t purple_mem_peek(uint64_t addr);

/* ========== Milestone 1 1.6 Arrays ==========
 * Dynamic arrays of int64_t elements. Header layout:
 *   [0] length (int64_t)
 *   [1] capacity (int64_t)
 *   [2..] elements
 * Returned pointer is to the whole block; user code usually works with
 * the data pointer (base+2) via Array.Data / indexing helpers.
 */
int64_t *purple_array_new(int64_t size);
int64_t  purple_array_len(int64_t *arr);
int64_t  purple_array_get(int64_t *arr, int64_t index);
int64_t  purple_array_set(int64_t *arr, int64_t index, int64_t value);
void     purple_array_free(int64_t *arr);
int64_t *purple_array_data(int64_t *arr); /* pointer to first element */


#ifdef __cplusplus
}
#endif

#endif
