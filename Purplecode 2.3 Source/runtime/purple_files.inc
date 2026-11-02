/* Purplecode 2.3 – File.* (hosted). Buffers passed to File.Read must hold max+1 bytes. */
#include <dirent.h>

int64_t purple_file_read(const char *path, char *buf, int64_t max) {
    if (!path || !buf || max < 0) return -1;
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    size_t n = fread(buf, 1, (size_t)max, f); fclose(f);
    buf[n] = 0; return (int64_t)n;
}
static int64_t file_put(const char *path, const char *s, const char *mode) {
    if (!path) return -1;
    FILE *f = fopen(path, mode); if (!f) return -1;
    size_t n = s ? strlen(s) : 0; if (n) fwrite(s, 1, n, f);
    fclose(f); return (int64_t)n;
}
int64_t purple_file_write(const char *path, const char *s)  { return file_put(path, s, "wb"); }
int64_t purple_file_append(const char *path, const char *s) { return file_put(path, s, "ab"); }
int64_t purple_file_delete(const char *path) { return path && remove(path) == 0 ? 0 : -1; }
int64_t purple_file_mkdir(const char *path)  { return path && (mkdir(path, 0755) == 0 || errno == EEXIST) ? 0 : -1; }
int64_t purple_file_rename(const char *a, const char *b) { return a && b && rename(a, b) == 0 ? 0 : -1; }
int64_t purple_file_exists(const char *path) { struct stat st; return path && stat(path, &st) == 0; }
int64_t purple_file_isdir(const char *path)  { struct stat st; return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode); }
int64_t purple_file_size(const char *path)   { struct stat st; return path && stat(path, &st) == 0 ? (int64_t)st.st_size : -1; }
int64_t purple_file_copy(const char *a, const char *b) {
    FILE *i = a ? fopen(a, "rb") : NULL; if (!i) return -1;
    FILE *o = b ? fopen(b, "wb") : NULL; if (!o) { fclose(i); return -1; }
    char tmp[8192]; size_t n; int64_t tot = 0;
    while ((n = fread(tmp, 1, sizeof tmp, i)) > 0) { fwrite(tmp, 1, n, o); tot += (int64_t)n; }
    fclose(i); fclose(o); return tot;
}
int64_t purple_file_print(const char *path) {
    FILE *f = path ? fopen(path, "rb") : NULL; if (!f) return -1;
    char tmp[4096]; size_t n; int64_t tot = 0;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) { fwrite(tmp, 1, n, stdout); tot += (int64_t)n; }
    fclose(f); fflush(stdout); return tot;
}
static int file_not_dots(const struct dirent *e) { return strcmp(e->d_name, ".") && strcmp(e->d_name, ".."); }
int64_t purple_file_count(const char *dir) {
    struct dirent **l = NULL; int n = dir ? scandir(dir, &l, file_not_dots, alphasort) : -1;
    if (n < 0) return -1;
    for (int i = 0; i < n; i++) free(l[i]);
    free(l); return n;
}
int64_t purple_file_name(const char *dir, int64_t idx, char *buf, int64_t max) {
    struct dirent **l = NULL; int n = dir ? scandir(dir, &l, file_not_dots, alphasort) : -1;
    if (n < 0 || !buf || idx < 0 || idx >= n || max < 1) { for (int i = 0; i < n; i++) free(l[i]); free(l); return -1; }
    snprintf(buf, (size_t)max + 1, "%s", l[idx]->d_name); int64_t len = (int64_t)strlen(buf);
    for (int i = 0; i < n; i++) free(l[i]);
    free(l); return len;
}
