/* Purplecode 2.4 – Net.* (hosted): TCP, UDP, HTTP client, static HTTP server. Plain HTTP only (no TLS).
 * Buffers passed to Net.Recv* / Net.Resolve / Net.Peer must hold max+1 bytes. */
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

static int64_t g_net_status = 0;
static char g_net_peer[64] = "";
static void net_init(void) { static int d; if (!d) { signal(SIGPIPE, SIG_IGN); d = 1; } }

int64_t purple_net_connect(const char *host, int64_t port) {
    net_init(); if (!host) return -1;
    char ps[16]; snprintf(ps, sizeof ps, "%d", (int)port);
    struct addrinfo hints, *res = NULL, *a; memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, ps, &hints, &res) != 0) return -1;
    int fd = -1;
    for (a = res; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol); if (fd < 0) continue;
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res); return fd;
}
int64_t purple_net_listen(int64_t port) {
    net_init(); int fd = socket(AF_INET, SOCK_STREAM, 0); if (fd < 0) return -1;
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_ANY); sa.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(fd, 32) < 0) { close(fd); return -1; }
    return fd;
}
int64_t purple_net_accept(int64_t fd) { return accept((int)fd, NULL, NULL); }
/* 0 = timeout, -1 = error, otherwise the client fd */
int64_t purple_net_accepttimeout(int64_t fd, int64_t ms) {
    struct pollfd p = { (int)fd, POLLIN, 0 }; int r = poll(&p, 1, (int)ms);
    if (r < 0) return -1; if (r == 0) return 0;
    return accept((int)fd, NULL, NULL);
}
int64_t purple_net_sendbuf(int64_t fd, const char *buf, int64_t len) {
    int64_t tot = 0;
    while (tot < len) { ssize_t n = send((int)fd, buf + tot, (size_t)(len - tot), MSG_NOSIGNAL); if (n <= 0) return tot ? tot : -1; tot += n; }
    return tot;
}
int64_t purple_net_send(int64_t fd, const char *s) { return s ? purple_net_sendbuf(fd, s, (int64_t)strlen(s)) : -1; }
int64_t purple_net_recv(int64_t fd, char *buf, int64_t max) {
    if (!buf || max < 1) return -1;
    ssize_t n = recv((int)fd, buf, (size_t)max, 0); if (n < 0) return -1;
    buf[n] = 0; return n;
}
/* -2 = timeout */
int64_t purple_net_recvtimeout(int64_t fd, char *buf, int64_t max, int64_t ms) {
    struct pollfd p = { (int)fd, POLLIN, 0 }; int r = poll(&p, 1, (int)ms);
    if (r == 0) return -2; if (r < 0) return -1;
    return purple_net_recv(fd, buf, max);
}
/* Reads one line (without CR/LF). Returns length, -1 on error/EOF before any byte. */
int64_t purple_net_recvline(int64_t fd, char *buf, int64_t max) {
    if (!buf || max < 1) return -1;
    int64_t n = 0; char c;
    while (n < max) {
        ssize_t r = recv((int)fd, &c, 1, 0);
        if (r <= 0) { if (n == 0) return -1; break; }
        if (c == '\n') break; if (c != '\r') buf[n++] = c;
    }
    buf[n] = 0; return n;
}
int64_t purple_net_close(int64_t fd) { return close((int)fd); }

int64_t purple_net_udpopen(int64_t port) {
    net_init(); int fd = socket(AF_INET, SOCK_DGRAM, 0); if (fd < 0) return -1;
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one); setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_ANY); sa.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { close(fd); return -1; }
    return fd;
}
int64_t purple_net_udpsend(int64_t fd, const char *host, int64_t port, const char *s) {
    if (!host || !s) return -1;
    char ps[16]; snprintf(ps, sizeof ps, "%d", (int)port);
    struct addrinfo hints, *res = NULL; memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, ps, &hints, &res) != 0) return -1;
    ssize_t n = sendto((int)fd, s, strlen(s), 0, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res); return n;
}
int64_t purple_net_udprecv(int64_t fd, char *buf, int64_t max) {
    if (!buf || max < 1) return -1;
    struct sockaddr_in from; socklen_t fl = sizeof from;
    ssize_t n = recvfrom((int)fd, buf, (size_t)max, 0, (struct sockaddr *)&from, &fl); if (n < 0) return -1;
    buf[n] = 0; snprintf(g_net_peer, sizeof g_net_peer, "%s:%d", inet_ntoa(from.sin_addr), (int)ntohs(from.sin_port));
    return n;
}
int64_t purple_net_peer(char *buf, int64_t max) { if (!buf || max < 1) return -1; snprintf(buf, (size_t)max + 1, "%s", g_net_peer); return (int64_t)strlen(buf); }
int64_t purple_net_print(const char *s) { if (!s) return -1; fputs(s, stdout); fflush(stdout); return (int64_t)strlen(s); }
int64_t purple_net_resolve(const char *host, char *buf, int64_t max) {
    if (!host || !buf || max < 1) return -1;
    struct addrinfo hints, *res = NULL; memset(&hints, 0, sizeof hints); hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0) return -1;
    char ip[INET6_ADDRSTRLEN] = "";
    void *addr = res->ai_family == AF_INET ? (void *)&((struct sockaddr_in *)res->ai_addr)->sin_addr : (void *)&((struct sockaddr_in6 *)res->ai_addr)->sin6_addr;
    inet_ntop(res->ai_family, addr, ip, sizeof ip); freeaddrinfo(res);
    snprintf(buf, (size_t)max + 1, "%s", ip); return (int64_t)strlen(buf);
}
int64_t purple_net_hostname(char *buf, int64_t max) {
    if (!buf || max < 1) return -1;
    char h[256]; if (gethostname(h, sizeof h) != 0) return -1; h[sizeof h - 1] = 0;
    snprintf(buf, (size_t)max + 1, "%s", h); return (int64_t)strlen(buf);
}
int64_t purple_net_status(void) { return g_net_status; }

/* ---- HTTP client (HTTP/1.0, Connection: close, no TLS, no redirects) ---- */
#define HTTP_MAX_RESPONSE (16u * 1024u * 1024u)   /* 16 MiB hard limit */
#define HTTP_READ_TIMEOUT_SEC 30

static int64_t http_do(const char *host, int64_t port, const char *req, const char *outfile) {
    int fd = (int)purple_net_connect(host, port); if (fd < 0) return -1;

    /* Apply read timeout to prevent hanging forever */
    struct timeval tv = { .tv_sec = HTTP_READ_TIMEOUT_SEC, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    if (purple_net_send(fd, req) < 0) { close(fd); return -1; }

    size_t cap = 65536, len = 0;
    char *b = malloc(cap);
    if (!b) { close(fd); return -1; }

    for (;;) {
        if (len + 4096 + 1 > cap) {
            if (cap >= HTTP_MAX_RESPONSE) {
                free(b); close(fd); return -1;   /* response too large */
            }
            size_t ncap = cap * 2;
            if (ncap > HTTP_MAX_RESPONSE) ncap = HTTP_MAX_RESPONSE;
            char *nb = realloc(b, ncap);
            if (!nb) { free(b); close(fd); return -1; }
            b = nb;
            cap = ncap;
        }
        ssize_t n = recv(fd, b + len, cap - len - 1, 0);
        if (n <= 0) break;
        len += (size_t)n;
    }
    close(fd);
    b[len] = 0;

    g_net_status = (len > 12 && !strncmp(b, "HTTP/", 5)) ? atoi(b + 9) : 0;
    char *body = strstr(b, "\r\n\r\n");
    size_t off = body ? (size_t)(body - b) + 4 : len;
    size_t blen = len - off;

    if (outfile && *outfile && strcmp(outfile, "-")) {
        FILE *f = fopen(outfile, "wb");
        if (f) { fwrite(b + off, 1, blen, f); fclose(f); }
        else { free(b); return -1; }
    } else {
        fwrite(b + off, 1, blen, stdout);
        fflush(stdout);
    }
    free(b);
    return (int64_t)blen;
}
int64_t purple_net_httpget(const char *host, int64_t port, const char *path, const char *outfile) {
    if (!host || !path) return -1; char req[2048];
    snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: Purplecode/2.4\r\nConnection: close\r\n\r\n", path, host);
    return http_do(host, port, req, outfile);
}
int64_t purple_net_httppost(const char *host, int64_t port, const char *path, const char *body, const char *outfile) {
    if (!host || !path || !body) return -1;
    size_t cap = strlen(body) + 2048; char *req = malloc(cap); if (!req) return -1;
    snprintf(req, cap, "POST %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: Purplecode/2.4\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s", path, host, strlen(body), body);
    int64_t r = http_do(host, port, req, outfile); free(req); return r;
}

/* HTTPS client: OpenSSL TLS 1.2+ with system trust store and hostname verification. */
int64_t purple_net_httpsget(const char *host, const char *path, const char *outfile) {
    if (!host || !*host || !path || !*path) return -1;
    int fd=(int)purple_net_connect(host,443); if(fd<0) return -1;
    SSL_CTX *ctx=SSL_CTX_new(TLS_client_method()); if(!ctx){close(fd);return -1;}
    SSL_CTX_set_min_proto_version(ctx,TLS1_2_VERSION);
    SSL_CTX_set_verify(ctx,SSL_VERIFY_PEER,NULL);
    if(SSL_CTX_set_default_verify_paths(ctx)!=1){SSL_CTX_free(ctx);close(fd);return -1;}
    SSL *ssl=SSL_new(ctx); if(!ssl){SSL_CTX_free(ctx);close(fd);return -1;}
    SSL_set_fd(ssl,fd); SSL_set_tlsext_host_name(ssl,host);
    X509_VERIFY_PARAM *vp=SSL_get0_param(ssl); X509_VERIFY_PARAM_set1_host(vp,host,0);
    if(SSL_connect(ssl)!=1 || SSL_get_verify_result(ssl)!=X509_V_OK){SSL_free(ssl);SSL_CTX_free(ctx);close(fd);return -1;}
    char req[4096]; int qn=snprintf(req,sizeof req,"GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: Purplecode/2.4\r\nAccept: */*\r\nConnection: close\r\n\r\n",path,host);
    if(qn<0 || (size_t)qn>=sizeof req){SSL_shutdown(ssl);SSL_free(ssl);SSL_CTX_free(ctx);close(fd);return -1;}
    int sent=0;while(sent<qn){int wr=SSL_write(ssl,req+sent,qn-sent);if(wr<=0){SSL_shutdown(ssl);SSL_free(ssl);SSL_CTX_free(ctx);close(fd);return -1;}sent+=wr;}
    size_t cap=65536,len=0; char *buf=malloc(cap); if(!buf){SSL_shutdown(ssl);SSL_free(ssl);SSL_CTX_free(ctx);close(fd);return -1;}
    for(;;){ if(len+8192+1>cap){if(cap>=64u*1024u*1024u){free(buf);buf=NULL;break;}cap*=2;char *nb=realloc(buf,cap);if(!nb){free(buf);buf=NULL;break;}buf=nb;}
        int n=SSL_read(ssl,buf+len,(int)(cap-len-1)); if(n<=0)break;len+=(size_t)n; }
    SSL_shutdown(ssl);SSL_free(ssl);SSL_CTX_free(ctx);close(fd); if(!buf)return -1;buf[len]=0;
    g_net_status=(len>12&&!strncmp(buf,"HTTP/",5))?atoi(buf+9):0;
    char *body=strstr(buf,"\r\n\r\n");size_t off=body?(size_t)(body-buf)+4:len, blen=len-off;char *payload=buf+off,*decoded=NULL;
    if(body && strcasestr(buf,"Transfer-Encoding: chunked")){
        decoded=malloc(blen+1);if(!decoded){free(buf);return -1;}size_t pos=0,outn=0;
        while(pos<blen){char *line=memmem(payload+pos,blen-pos,"\r\n",2);if(!line)break;char num[32];size_t ln=(size_t)(line-(payload+pos));if(ln>=sizeof num)break;memcpy(num,payload+pos,ln);num[ln]=0;unsigned long chunk=strtoul(num,NULL,16);pos+=ln+2;if(!chunk)break;if(chunk>blen-pos)break;memcpy(decoded+outn,payload+pos,chunk);outn+=chunk;pos+=(size_t)chunk;if(pos+2<=blen&&payload[pos]=='\r'&&payload[pos+1]=='\n')pos+=2;else break;}
        decoded[outn]=0;payload=decoded;blen=outn;
    }
    if(outfile&&*outfile&&strcmp(outfile,"-")){FILE*f=fopen(outfile,"wb");if(!f){free(decoded);free(buf);return -1;}size_t wr=fwrite(payload,1,blen,f);fclose(f);if(wr!=blen){free(decoded);free(buf);return -1;}}
    else {fwrite(payload,1,blen,stdout);fflush(stdout);} free(decoded);free(buf);return (int64_t)blen;
}


/* Safely open path relative to dirfd without following any symlinks (intermediate or final).
 * Walks one component at a time with O_NOFOLLOW. Returns fd >=0 or -1.
 * final_flags are additional flags for the last component (e.g. 0 or O_DIRECTORY). */
static int openat_nofollow_walk(int dirfd, const char *rel, int final_flags) {
    if (!rel || !*rel) return -1;
    char path[PATH_MAX];
    size_t len = strlen(rel);
    if (len >= sizeof path) return -1;
    memcpy(path, rel, len + 1);

    int cur = dirfd;
    int owns = 0; /* whether cur must be closed by us */
    char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char *slash = strchr(p, '/');
        int is_last = (slash == NULL);
        if (slash) *slash = '\0';

        int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW;
        if (!is_last)
            flags |= O_DIRECTORY;
        else
            flags |= (final_flags & ~(O_RDONLY | O_CLOEXEC | O_NOFOLLOW)); /* keep forced flags */

        int next = openat(cur, p, flags);
        if (owns) close(cur);
        if (next < 0) {
            if (slash) *slash = '/';
            return -1;
        }
        cur = next;
        owns = 1;
        if (slash) {
            *slash = '/';
            p = slash + 1;
        } else {
            break;
        }
    }
    if (!owns) return -1; /* empty path after stripping */
    return cur;
}

/* ---- Static HTTP server: Net.HttpServe(port, dir, max_requests) – 0 = forever ---- */
static const char *http_mime(const char *p) {
    const char *e = strrchr(p, '.'); if (!e) return "application/octet-stream";
    static const char *t[][2] = {{".html","text/html; charset=utf-8"},{".htm","text/html; charset=utf-8"},{".css","text/css"},{".js","text/javascript"},{".json","application/json"},{".txt","text/plain; charset=utf-8"},{".png","image/png"},{".jpg","image/jpeg"},{".jpeg","image/jpeg"},{".gif","image/gif"},{".svg","image/svg+xml"},{".ico","image/x-icon"},{".wasm","application/wasm"},{".ppm","image/x-portable-pixmap"}};
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (!strcmp(e, t[i][0])) return t[i][1];
    return "application/octet-stream";
}
static void http_reply(int c, int code, const char *msg, const char *mime, const char *body, size_t n, int head) {
    char h[512]; int hl = snprintf(h, sizeof h, "HTTP/1.0 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nServer: Purplecode/2.4\r\nConnection: close\r\n\r\n", code, msg, mime, n);
    purple_net_sendbuf(c, h, hl); if (!head && n) purple_net_sendbuf(c, body, (int64_t)n);
}
int64_t purple_net_httpserve(int64_t port, const char *dir, int64_t maxreq) {
    if (!dir) return -1;
    int ls = (int)purple_net_listen(port); if (ls < 0) return -1;

    /* Open the root directory once – all file access is relative to this fd */
    int dirfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirfd < 0) { close(ls); return -1; }

    /* Read timeout for accepted clients so a slow/hanging client cannot freeze the single-threaded server */
    const int CLIENT_RECV_TIMEOUT_SEC = 15;

    int64_t served = 0;
    while (maxreq <= 0 || served < maxreq) {
        int c = accept(ls, NULL, NULL); if (c < 0) break;

        /* Prevent one slow client from hanging the whole server */
        struct timeval tv = { .tv_sec = CLIENT_RECV_TIMEOUT_SEC, .tv_usec = 0 };
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

        char rq[8192]; size_t got = 0;
        while (got < sizeof rq - 1) {
            ssize_t n = recv(c, rq + got, sizeof rq - 1 - got, 0);
            if (n <= 0) break;
            got += (size_t)n;
            rq[got] = 0;
            if (strstr(rq, "\r\n\r\n")) break;
        }
        rq[got] = 0;
        char m[8] = "", u[1024] = "";
        sscanf(rq, "%7s %1023s", m, u);
        int head = !strcmp(m, "HEAD");
        char *q = strchr(u, '?'); if (q) *q = 0;

        if (strcmp(m, "GET") && !head) {
            http_reply(c, 405, "Method Not Allowed", "text/plain", "405\n", 4, 0);
        } else if (strstr(u, "..") || u[0] != '/' || strchr(u, '\\')) {
            /* Reject path traversal attempts early */
            http_reply(c, 400, "Bad Request", "text/plain", "400\n", 4, head);
        } else {
            /* Strip leading '/' so path is relative to dirfd */
            const char *rel = u + 1;
            if (!*rel) rel = "index.html";   /* bare "/" → index.html */

            /* Walk every path component with O_NOFOLLOW so intermediate directory
             * symlinks cannot escape the served root (openat + O_NOFOLLOW only
             * protects the final component). */
            int fd = openat_nofollow_walk(dirfd, rel, 0);
            if (fd < 0) {
                /* If it was a directory, try index.html inside it */
                int dfd = openat_nofollow_walk(dirfd, rel, O_DIRECTORY);
                if (dfd >= 0) {
                    fd = openat(dfd, "index.html", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
                    close(dfd);
                }
            }

            if (fd < 0) {
                http_reply(c, 404, "Not Found", "text/plain", "404 not found\n", 14, head);
                printf("%s %s -> 404\n", m, u);
            } else {
                struct stat st;
                if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
                    close(fd);
                    http_reply(c, 404, "Not Found", "text/plain", "404 not found\n", 14, head);
                    printf("%s %s -> 404\n", m, u);
                } else {
                    size_t sz = (size_t)st.st_size;
                    char *data = malloc(sz + 1);
                    if (data && read(fd, data, sz) == (ssize_t)sz) {
                        http_reply(c, 200, "OK", http_mime(rel), data, sz, head);
                        printf("%s %s -> 200 (%zu bytes)\n", m, u, sz);
                    } else {
                        http_reply(c, 500, "Server Error", "text/plain", "500\n", 4, head);
                    }
                    free(data);
                    close(fd);
                }
            }
        }
        fflush(stdout);
        close(c);
        served++;
    }
    close(dirfd);
    close(ls);
    return served;
}
