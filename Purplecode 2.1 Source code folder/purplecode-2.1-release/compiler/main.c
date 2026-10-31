#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include "lexer.h"
#include "parser.h"
#include "codegen.h"
#include "elfemit.h"
#include "ast.h"

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        perror("fseek");
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) {
        perror("reading input size");
        fclose(f);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    if (ferror(f)) {
        perror("reading input");
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[n] = '\0';
    fclose(f);
    return buf;
}

/* Search globally installed Purple Libs after checking the importing source's
 * directory. PURPLECODE_LIB_DIR accepts colon-separated additional roots. */
static int is_regular_file(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}
static int find_named_recursive(const char *root, const char *base, char *out, size_t outsz, int depth) {
    if (depth > 12) return 0;
    DIR *d = opendir(root);
    if (!d) return 0;
    struct dirent *de;
    int found = 0;
    while (!found && (de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..") || !strcmp(de->d_name, ".git")) continue;
        char path[PATH_MAX];
        int n = snprintf(path, sizeof path, "%s/%s", root, de->d_name);
        if (n < 0 || (size_t)n >= sizeof path) continue;
        struct stat st;
        if (lstat(path, &st) != 0 || S_ISLNK(st.st_mode)) continue;
        if (S_ISREG(st.st_mode) && !strcmp(de->d_name, base)) {
            if (snprintf(out, outsz, "%s", path) < (int)outsz) found = 1;
        } else if (S_ISDIR(st.st_mode)) {
            found = find_named_recursive(path, base, out, outsz, depth + 1);
        }
    }
    closedir(d);
    return found;
}
static int resolve_library_path(const char *requested, const char *source_dir, char *out, size_t outsz) {
    if (requested[0] == '/') {
        if (is_regular_file(requested) && snprintf(out, outsz, "%s", requested) < (int)outsz) return 1;
        return 0;
    }
    char candidate[PATH_MAX];
    if (snprintf(candidate, sizeof candidate, "%s/%s", source_dir, requested) < (int)sizeof candidate &&
        is_regular_file(candidate) && snprintf(out, outsz, "%s", candidate) < (int)outsz) return 1;
    if (snprintf(candidate, sizeof candidate, "./%s", requested) < (int)sizeof candidate &&
        is_regular_file(candidate) && snprintf(out, outsz, "%s", candidate) < (int)outsz) return 1;
    const char *env = getenv("PURPLECODE_LIB_DIR");
    char *paths = env ? strdup(env) : NULL;
    const char *defaults[] = { NULL, "/usr/local/share/purplecode/libs", "/usr/share/purplecode/libs" };
    const char *home = getenv("HOME");
    char userroot[PATH_MAX] = "";
    if (home) snprintf(userroot, sizeof userroot, "%s/.local/share/purplecode/libs", home);
    defaults[0] = userroot[0] ? userroot : "/nonexistent";
    const char *base = strrchr(requested, '/'); base = base ? base + 1 : requested;
    if (paths) {
        char *save = NULL;
        for (char *root = strtok_r(paths, ":", &save); root; root = strtok_r(NULL, ":", &save)) {
            if (!*root) continue;
            if (snprintf(candidate, sizeof candidate, "%s/%s", root, requested) < (int)sizeof candidate &&
                is_regular_file(candidate) && snprintf(out, outsz, "%s", candidate) < (int)outsz) { free(paths); return 1; }
            if (find_named_recursive(root, base, out, outsz, 0)) { free(paths); return 1; }
        }
        free(paths);
    }
    for (size_t i = 0; i < sizeof defaults / sizeof defaults[0]; ++i) {
        const char *root = defaults[i];
        if (!root || !*root) continue;
        if (snprintf(candidate, sizeof candidate, "%s/%s", root, requested) < (int)sizeof candidate &&
            is_regular_file(candidate) && snprintf(out, outsz, "%s", candidate) < (int)outsz) return 1;
        if (find_named_recursive(root, base, out, outsz, 0)) return 1;
    }
    return 0;
}
static char **g_static_libs = NULL;
static size_t g_static_lib_count = 0;
static int append_static_libs(const char *value) {
    char *copy = strdup(value);
    if (!copy) return 0;
    char *save = NULL;
    for (char *item = strtok_r(copy, ", \t", &save); item; item = strtok_r(NULL, ", \t", &save)) {
        if (!*item) continue;
        for (const unsigned char *p = (const unsigned char *)item; *p; ++p) {
            if (!(isalnum(*p) || *p == '_' || *p == '+' || *p == '-')) {
                fprintf(stderr, "Purplecode: invalid STATIC_LIBS entry '%s' (use library names, not linker flags).\n", item);
                free(copy); return 0;
            }
        }
        int duplicate = 0;
        for (size_t i = 0; i < g_static_lib_count; ++i) if (!strcmp(g_static_libs[i], item)) duplicate = 1;
        if (duplicate) continue;
        char **next = realloc(g_static_libs, (g_static_lib_count + 1) * sizeof *next);
        if (!next) { free(copy); return 0; }
        g_static_libs = next;
        g_static_libs[g_static_lib_count] = strdup(item);
        if (!g_static_libs[g_static_lib_count]) { free(copy); return 0; }
        g_static_lib_count++;
    }
    free(copy);
    return 1;
}
static void free_static_libs(void) {
    for (size_t i = 0; i < g_static_lib_count; ++i) free(g_static_libs[i]);
    free(g_static_libs); g_static_libs = NULL; g_static_lib_count = 0;
}

static int join_path(char *out, size_t cap, const char *base, const char *suffix) {
    size_t a = strlen(base), b = strlen(suffix);
    if (a + b + 1 > cap) return 0;
    memcpy(out, base, a); memcpy(out + a, suffix, b + 1); return 1;
}
static char *find_runtime_source(const char *argv0) {
    const char *candidates[4]; char paths[4][PATH_MAX]; size_t count = 0;
    candidates[count++] = "runtime/purple_rt.c";
    char exe[PATH_MAX]; ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (len > 0) {
        exe[len] = 0; char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = 0;
            if (join_path(paths[count], sizeof paths[count], exe, "/runtime/purple_rt.c")) { candidates[count] = paths[count]; count++; }
            if (join_path(paths[count], sizeof paths[count], exe, "/../share/purplecode/runtime/purple_rt.c")) { candidates[count] = paths[count]; count++; }
        }
    }
    (void)argv0;
    for (size_t i = 0; i < count; ++i) if (is_regular_file(candidates[i])) return strdup(candidates[i]);
    return NULL;
}
static int run_static_linker(const char *assembly, const char *output, const char *argv0) {
    char *runtime = find_runtime_source(argv0);
    if (!runtime) {
        fprintf(stderr, "Purplecode --static: cannot locate runtime/purple_rt.c; install the runtime or run from the source tree.\n");
        return 1;
    }
    size_t base_count = 16;
    char **args = calloc(base_count + g_static_lib_count + 1, sizeof *args);
    if (!args) { free(runtime); return 1; }
    size_t n = 0;
    args[n++] = "gcc"; args[n++] = "-static"; args[n++] = "-no-pie";
    args[n++] = "-x"; args[n++] = "assembler"; args[n++] = (char *)assembly;
    args[n++] = "-x"; args[n++] = "c"; args[n++] = runtime;
    for (size_t i = 0; i < g_static_lib_count; ++i) {
        size_t len = strlen(g_static_libs[i]) + 3;
        args[n] = malloc(len);
        if (!args[n]) { for (size_t j = 9; j < n; ++j) free(args[j]); free(args); free(runtime); return 1; }
        snprintf(args[n++], len, "-l%s", g_static_libs[i]);
    }
    args[n++] = "-lpng"; args[n++] = "-ljpeg"; args[n++] = "-lssl"; args[n++] = "-lcrypto"; args[n++] = "-lz"; args[n++] = "-lm"; args[n++] = "-ldl";
    args[n++] = "-o"; args[n++] = (char *)output; args[n] = NULL;
    pid_t pid = fork();
    if (pid < 0) { perror("fork gcc"); for (size_t j = 9; j < n; ++j) free(args[j]); free(args); free(runtime); return 1; }
    if (pid == 0) { execvp(args[0], args); perror("exec gcc"); _exit(127); }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) { if (errno == EINTR) continue; perror("waitpid"); free(runtime); return 1; }
    for (size_t j = 9; j < n; ++j) if (j < n && args[j] && args[j][0] == '-' && args[j][1] == 'l' && j < 9 + g_static_lib_count) free(args[j]);
    free(args); free(runtime);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "Purplecode --static: static link failed. Install static development archives for libpng, libjpeg-turbo, libc and other runtime dependencies; no dynamic fallback was attempted.\n");
        return 1;
    }
    return 0;
}

static int run_elf_linker(const char *assembly, const char *output, const char *argv0) {
    char *runtime = find_runtime_source(argv0);
    if (!runtime) {
        fprintf(stderr, "Purplecode: cannot locate runtime/purple_rt.c; install the runtime or run from the source tree.\n");
        return 1;
    }

    /* Try several link recipes from richest to leanest so the compiler
     * works even when optional image/GUI libraries are missing. */
    const char *recipes[][24] = {
        /* Native Wayland with FreeType font rasterization. */
        { "gcc", "-no-pie", "-x", "assembler", assembly, "-x", "c", runtime,
          "-DPURPLE_HAS_WAYLAND", "-DPURPLE_HAS_FREETYPE", "-I/usr/include/freetype2", "-lwayland-client", "-lfreetype", "-lssl", "-lcrypto", "-ldl", "-lm", "-o", output },
        /* FreeType without a Wayland development package. */
        { "gcc", "-no-pie", "-x", "assembler", assembly, "-x", "c", runtime,
          "-DPURPLE_HAS_FREETYPE", "-I/usr/include/freetype2", "-lfreetype", "-lssl", "-lcrypto", "-ldl", "-lm", "-o", output },
        /* Native Wayland without optional font rasterizer. */
        { "gcc", "-no-pie", "-x", "assembler", assembly, "-x", "c", runtime,
          "-DPURPLE_HAS_WAYLAND", "-lwayland-client", "-lssl", "-lcrypto", "-ldl", "-lm", "-o", output },
        /* Full X11 + image libs. */
        { "gcc", "-no-pie", "-x", "assembler", assembly, "-x", "c", runtime,
          "-DPURPLE_HAS_X11", "-lX11", "-lpng", "-ljpeg", "-lssl", "-lcrypto", "-ldl", "-lm", "-o", output },
        /* X11 but no image libs. */
        { "gcc", "-no-pie", "-x", "assembler", assembly, "-x", "c", runtime,
          "-DPURPLE_HAS_X11", "-lX11", "-lssl", "-lcrypto", "-ldl", "-lm", "-o", output },
        /* No X11, with image libs. */
        { "gcc", "-no-pie", "-x", "assembler", assembly, "-x", "c", runtime,
          "-lpng", "-ljpeg", "-lssl", "-lcrypto", "-ldl", "-lm", "-o", output },
        /* Minimal hosted runtime. */
        { "gcc", "-no-pie", "-x", "assembler", assembly, "-x", "c", runtime,
          "-lssl", "-lcrypto", "-ldl", "-lm", "-o", output },
    };
    const int recipe_lens[] = { 19, 17, 16, 18, 16, 16, 14 };
    const char *recipe_names[] = {
        "Wayland+FreeType+OpenSSL", "FreeType+OpenSSL", "native Wayland+OpenSSL",
        "X11+png+jpeg+OpenSSL", "X11+OpenSSL", "png+jpeg+OpenSSL", "OpenSSL minimal"
    };

    for (int r = 0; r < 7; r++) {
        if ((r == 0 || r == 2) && access("/usr/include/wayland-client.h", R_OK) != 0) continue;
        if ((r == 0 || r == 1) && access("/usr/include/freetype2/ft2build.h", R_OK) != 0) continue;
        char *args[30];
        int n = 0;
        for (int i = 0; i < recipe_lens[r]; i++)
            args[n++] = (char *)recipes[r][i];
        args[n] = NULL;

        pid_t pid = fork();
        if (pid < 0) { perror("fork gcc"); free(runtime); return 1; }
        if (pid == 0) {
            /* silence gcc noise on failed attempts */
            if (r > 0) {
                freopen("/dev/null", "w", stderr);
            }
            execvp(args[0], args);
            perror("exec gcc");
            _exit(127);
        }
        int status = 0;
        while (waitpid(pid, &status, 0) < 0) {
            if (errno == EINTR) continue;
            perror("waitpid"); free(runtime); return 1;
        }
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            if (r > 0)
                fprintf(stderr, "Purplecode: note – linked with %s recipe (some optional features unavailable).\n", recipe_names[r]);
            free(runtime);
            return 0;
        }
    }

    free(runtime);
    fprintf(stderr, "Purplecode: native ELF link failed with every recipe. Is gcc installed?\n");
    return 1;
}

static int valid_lib_name(const char *s) {
    if (!s || !(*s) || !(isalpha((unsigned char)*s) || *s == '_')) return 0;
    for (const unsigned char *p = (const unsigned char *)s + 1; *p; ++p)
        if (!(isalnum(*p) || *p == '_')) return 0;
    return 1;
}
static char *trim(char *s) {
    while (isspace((unsigned char)*s)) ++s;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) --e;
    *e = 0; return s;
}
static int valid_controlang(const char *s) {
    if (!s || !*s) return 0;
    int segment_start = 1;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (*p == '.') { if (segment_start) return 0; segment_start = 1; }
        else if (segment_start) { if (!(isalpha(*p) || *p == '_')) return 0; segment_start = 0; }
        else if (!(isalnum(*p) || *p == '_')) return 0;
    }
    return !segment_start;
}
static char *replace_token(const char *src, const char *from, const char *to, int percent_only) {
    size_t fl = strlen(from), tl = strlen(to), cap = strlen(src) + 1, used = 0;
    char *out = malloc(cap); if (!out) return NULL;
    for (size_t i = 0; src[i];) {
        size_t start = i; int match = 0;
        if (percent_only && src[i] == '%' && !strncmp(src+i+1, from, fl) &&
            !(isalnum((unsigned char)src[i+1+fl]) || src[i+1+fl] == '_')) { match = 1; i += fl + 1; }
        else if (!percent_only && !strncmp(src+i, from, fl) &&
                 (i == 0 || !(isalnum((unsigned char)src[i-1]) || src[i-1] == '_')) &&
                 src[i+fl] == '(') { match = 1; i += fl; }
        if (match) {
            size_t need = used + tl + strlen(src+i) + 1;
            if (need > cap) { cap = need * 2; char *q = realloc(out, cap); if (!q) { free(out); return NULL; } out=q; }
            memcpy(out+used,to,tl); used+=tl;
        } else {
            i = start + 1;
            if (used + 2 > cap) { cap *= 2; char *q=realloc(out,cap); if(!q){free(out);return NULL;} out=q; }
            out[used++]=src[start];
        }
    }
    out[used]=0; return out;
}
/* #uselib "library.pl" loads a real, validated metadata file at compile time.
 * Library paths are resolved relative to the source file, not the process cwd. */
static char *prepare_source(const char *src, const char *input_path) {
    char *copy = strdup(src); if (!copy) return NULL;
    char *out = calloc(strlen(src) + 1, 1); if (!out) { free(copy); return NULL; }
    size_t outlen = 0, outcap = strlen(src) + 1;
    typedef struct { char alias[256]; char target[512]; } LibAlias;
    LibAlias *aliases = NULL; size_t alias_count = 0, alias_capacity = 0;
    char *save = NULL;
    char *line = strtok_r(copy, "\n", &save);
    char source_dir[PATH_MAX];
    snprintf(source_dir, sizeof source_dir, "%s", input_path);
    char *slash = strrchr(source_dir, '/');
    if (slash) { if (slash == source_dir) slash[1] = 0; else *slash = 0; } else strcpy(source_dir, ".");
    while (line) {
        char *t = trim(line);
        if (!strncmp(t, "#uselib", 7) && isspace((unsigned char)t[7])) {
            char *q = strchr(t, '"'), *end = q ? strchr(q+1, '"') : NULL;
            if (!q || !end || *trim(end+1)) { fprintf(stderr,"Purplecode: malformed #uselib directive: %s\n", t); goto fail; }
            *end = 0;
            char libpath[PATH_MAX];
            if (!resolve_library_path(q + 1, source_dir, libpath, sizeof libpath)) {
                fprintf(stderr, "Purplecode: cannot find Purple Lib '%s'. Checked source directory, PURPLECODE_LIB_DIR, and standard user/system library roots.\n", q + 1);
                goto fail;
            }
            char *meta = read_file(libpath); if (!meta) { fprintf(stderr,"Purplecode: cannot load Purple Lib '%s'\n",libpath); goto fail; }
            char *mcopy=strdup(meta); if(!mcopy){free(meta);goto fail;}
            int started=0, ended=0, bad=0; char libname[256]="", controlang[512]="";
            int have_version=0, have_type=0, have_language=0; char version[128]="";
            char *libbody = NULL; size_t bodylen = 0;
            char *ms=NULL;
            for(char *ml=strtok_r(mcopy,"\n",&ms); ml; ml=strtok_r(NULL,"\n",&ms)) {
                char *mt=trim(ml);
                if (!started) { if(strcmp(mt,"PLStartMarekPCStartHeader")) { if(*mt) bad=1; continue; } started=1; continue; }
                if (!ended && !strcmp(mt,"PLEndMarekPCEndHeader")) { ended=1; continue; }
                if (ended) {
                    /* Everything after the closing metadata marker is executable
                     * Purplecode source and is compiled into the importing unit. */
                    size_t add = strlen(ml) + 1;
                    char *nb = realloc(libbody, bodylen + add + 1);
                    if (!nb) { bad=1; break; }
                    libbody = nb; memcpy(libbody + bodylen, ml, add - 1);
                    bodylen += add - 1; libbody[bodylen++] = '\n'; libbody[bodylen] = 0;
                    continue;
                }
                char *eq=strchr(mt,'='); if(!eq) { if(*mt) bad=1; continue; }
                *eq=0; char *key=trim(mt), *val=trim(eq+1);
                if(!strcmp(key,"NAME")) snprintf(libname,sizeof libname,"%s",val);
                else if(!strcmp(key,"VERSION")) { have_version=*val!=0; snprintf(version,sizeof version,"%s",val); }
                else if(!strcmp(key,"TYPE")) have_type=!strcmp(val,"Purple Lib");
                else if(!strcmp(key,"LANGUAGE")) have_language=!strcmp(val,"Purplecode");
                else if(!strcmp(key,"CONTROLANG")) snprintf(controlang,sizeof controlang,"%s",val);
                else if(!strcmp(key,"STATIC_LIBS")) { if(!append_static_libs(val)) bad=1; }
            }
            free(mcopy); free(meta);
            if(!started || !ended || bad || !valid_lib_name(libname) || !have_version || !have_type || !have_language || (controlang[0] && !valid_controlang(controlang))) {
                fprintf(stderr,"Purplecode: invalid Purple Lib metadata in %s (required: markers, NAME, VERSION, TYPE=Purple Lib, LANGUAGE=Purplecode; CONTROLANG must be dotted identifier)\n",libpath); goto fail;
            }
            if (controlang[0]) {
                if (alias_count == alias_capacity) {
                    size_t cap = alias_capacity ? alias_capacity * 2 : 16;
                    LibAlias *next = realloc(aliases, cap * sizeof *next);
                    if (!next) { free(libbody); goto fail; }
                    aliases = next; alias_capacity = cap;
                }
                snprintf(aliases[alias_count].alias,sizeof aliases[alias_count].alias,"%s",libname);
                snprintf(aliases[alias_count].target,sizeof aliases[alias_count].target,"%s",controlang); alias_count++;
            }
            if (libbody && bodylen) {
                size_t need = outlen + bodylen + 2;
                if (need > outcap) { outcap = need * 2; char *q = realloc(out, outcap); if (!q) { free(libbody); goto fail; } out=q; }
                memcpy(out + outlen, libbody, bodylen); outlen += bodylen; out[outlen] = 0;
            }
            free(libbody);
            printf("Purple Lib loaded: %s v%s from %s%s\n",libname,version,libpath,controlang[0]?" (Controlang registered)":"");
        } else {
            size_t n=strlen(line)+1;
            if(outlen+n+1>outcap){outcap=(outlen+n+1)*2;char*q=realloc(out,outcap);if(!q)goto fail;out=q;}
            memcpy(out+outlen,line,n-1);outlen+=n-1;out[outlen++]='\n';out[outlen]=0;
        }
        line=strtok_r(NULL,"\n",&save);
    }
    free(copy);
    for(size_t i=0;i<alias_count;i++) {
        char *next=replace_token(out,aliases[i].alias,aliases[i].target,1); if(!next){free(aliases);free(out);return NULL;} free(out); out=next;
        next=replace_token(out,aliases[i].alias,aliases[i].target,0); if(!next){free(aliases);free(out);return NULL;} free(out); out=next;
    }
    free(aliases);
    return out;
fail:
    free(aliases); free(copy); free(out); return NULL;
}

static void usage(const char *prog) {
    fprintf(stderr, "Purplecode Compiler — 2.0 NextGen (NG)\n");
    fprintf(stderr, "Usage: %s <input.purple> [-o out] [options]\n", prog);
    fprintf(stderr, "\n");
    fprintf(stderr, "  Default behaviour (no flags):\n");
    fprintf(stderr, "    Compile + assemble + link → native executable.\n");
    fprintf(stderr, "    Intermediate assembly is never left on disk.\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -o <file>        Output executable name (default: basename of input, or a.out)\n");
    fprintf(stderr, "  -S, --asm        Emit assembly only (.s), do not link\n");
    fprintf(stderr, "  --static         Static link the executable\n");
    fprintf(stderr, "  --direct         Pure direct-to-ELF64 backend (no C runtime, limited subset)\n");
    fprintf(stderr, "  --dump-regalloc  Debug: print register allocation from --direct backend\n");
    fprintf(stderr, "  -h, --help       Show this help\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Examples:\n");
    fprintf(stderr, "  %s hello.purple                 # → ./hello   (ready to run)\n", prog);
    fprintf(stderr, "  %s game.purple -o mygame        # → ./mygame\n", prog);
    fprintf(stderr, "  %s foo.purple -S -o foo.s       # assembly only\n", prog);
}

/* Derive a sensible default executable name from the input path. */
static const char *default_exe_name(const char *input) {
    static char buf[PATH_MAX];
    const char *base = strrchr(input, '/');
    base = base ? base + 1 : input;
    size_t len = strlen(base);
    /* Strip .purple or .purplebc */
    if (len > 7 && !strcmp(base + len - 7, ".purple")) {
        snprintf(buf, sizeof buf, "%.*s", (int)(len - 7), base);
        return buf;
    }
    if (len > 9 && !strcmp(base + len - 9, ".purplebc")) {
        snprintf(buf, sizeof buf, "%.*s", (int)(len - 9), base);
        return buf;
    }
    /* Fallback */
    snprintf(buf, sizeof buf, "%s", base[0] ? base : "a.out");
    return buf;
}

int main(int argc, char **argv) {
    const char *input = NULL;
    const char *output = NULL;
    int output_set = 0, static_mode = 0, elf_mode = 1 /* DEFAULT: always native executable */, 
        direct_mode = 0, dump_regalloc = 0, asm_only = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output = argv[++i]; output_set = 1;
        } else if (strcmp(argv[i], "--static") == 0) {
            static_mode = 1; elf_mode = 1; asm_only = 0;
        } else if (strcmp(argv[i], "--elf") == 0) {
            /* kept for backward compatibility; now the default */
            elf_mode = 1; asm_only = 0;
        } else if (strcmp(argv[i], "-S") == 0 || strcmp(argv[i], "--asm") == 0) {
            asm_only = 1; elf_mode = 0; static_mode = 0;
        } else if (strcmp(argv[i], "--direct") == 0) {
            direct_mode = 1; elf_mode = 0; asm_only = 0;
        } else if (strcmp(argv[i], "--dump-regalloc") == 0) {
            dump_regalloc = 1;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]); return 0;
        } else if (argv[i][0] != '-') {
            if (input) { fprintf(stderr, "Purplecode: only one input file is supported.\n"); return 1; }
            input = argv[i];
        } else {
            usage(argv[0]); return 1;
        }
    }
    if (!input) { usage(argv[0]); return 1; }

    if (direct_mode) {
        if (!output_set) output = "a.out";
        elf_mode = 0;
        static_mode = 0;
        asm_only = 0;
    } else if (asm_only) {
        if (!output_set) output = "out.s";
        elf_mode = 0;
    } else {
        /* Default path: native executable */
        elf_mode = 1;
        if (!output_set) output = default_exe_name(input);
    }

    char *raw_src = read_file(input);
    if (!raw_src) return 1;
    char *src = prepare_source(raw_src, input);
    free(raw_src);
    if (!src) return 1;

    Lexer lexer; lexer_init(&lexer, src);
    Parser parser; parser_init(&parser, &lexer);
    ASTNode *prog = parse_program(&parser);
    if (parser.had_error) {
        fprintf(stderr, "Compilation failed due to parse errors.\n");
        ast_free(prog); parser_free(&parser); lexer_free(&lexer); free(src); return 1;
    }

    if (direct_mode) {
        ElfEmit em;
        elfemit_init(&em);
        em.dump_regalloc = dump_regalloc;
        int ok = elfemit_program(&em, prog);
        if (ok) ok = elfemit_write_elf(&em, output);
        int failed = em.had_error || !ok;
        elfemit_free(&em);
        ast_free(prog); parser_free(&parser); lexer_free(&lexer); free(src); free_static_libs();
        if (failed) {
            fprintf(stderr, "Purplecode --direct: compilation failed.\n");
            unlink(output);
            return 1;
        }
        printf("Purplecode: compiled %s -> %s (direct ELF64, no intermediate .s/.c, syscall runtime)\n", input, output);
        return 0;
    }

    char temp_path[] = "/tmp/purplecode-asm-XXXXXX";
    const char *asm_path = output;
    int temp_fd = -1;
    if (elf_mode) {
        temp_fd = mkstemp(temp_path);
        if (temp_fd < 0) { perror("mkstemp"); ast_free(prog); parser_free(&parser); lexer_free(&lexer); free(src); return 1; }
        asm_path = temp_path;
    }
    FILE *out = temp_fd >= 0 ? fdopen(temp_fd, "w") : fopen(asm_path, "w");
    if (!out) {
        perror(asm_path); if (temp_fd >= 0) { close(temp_fd); unlink(temp_path); }
        ast_free(prog); parser_free(&parser); lexer_free(&lexer); free(src); return 1;
    }
    CodeGen cg; codegen_init(&cg, out); codegen_program(&cg, prog);
    bool semantic_error = cg.had_error; codegen_free(&cg);
    if (fclose(out) != 0) { perror("writing output"); semantic_error = true; }
    if (semantic_error) {
        remove(asm_path); fprintf(stderr, "Compilation failed due to type errors.\n");
        ast_free(prog); parser_free(&parser); lexer_free(&lexer); free(src); return 1;
    }
    int rc = 0;
    if (elf_mode) {
        rc = static_mode ? run_static_linker(asm_path, output, argv[0]) : run_elf_linker(asm_path, output, argv[0]);
        unlink(asm_path);   /* never leave intermediate assembly on disk */
        if (rc != 0) {
            unlink(output);
            fprintf(stderr, "Purplecode: failed to produce executable\n");
        } else {
            printf("Purplecode: %s → %s  (native executable, ready to run)\n", input, output);
        }
    } else {
        printf("Purplecode: %s → %s  (assembly only)\n", input, output);
    }
    ast_free(prog); parser_free(&parser); lexer_free(&lexer); free(src); free_static_libs();
    return rc;
}

