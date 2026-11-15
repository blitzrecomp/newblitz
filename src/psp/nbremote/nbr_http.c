/* NB Remote's HTTP/1.0 client (see nbr.h): the same rules as src/psp/psp_http.cpp (Host, Content-Length, Connection: close;
 * never Accept-Encoding or Expect; a chunked reply refused; a reply short of its Content-Length fails), on the platform's
 * sockets, into the caller's buffer. No TLS: the logs Worker is reached over plain http://. */
#include "nbr.h"
#include <string.h>

#define HEADER_MAX 4096

static int parse_url(const char* url, char* host, int host_cap, int* port, const char** path) {
    const char* scheme = "http://";
    if (strncmp(url, scheme, 7) != 0) return -1;
    const char* h = url + 7;
    const char* slash = strchr(h, '/');
    const char* end = slash ? slash : h + strlen(h);
    const char* colon = NULL;
    for (const char* c = h; c < end; c++) if (*c == ':') colon = c;
    const char* hend = colon ? colon : end;
    if (hend - h <= 0 || hend - h >= host_cap) return -1;
    memcpy(host, h, (size_t)(hend - h));
    host[hend - h] = 0;
    *port = 80;
    if (colon) {
        int p = 0;
        for (const char* c = colon + 1; c < end; c++) {
            if (*c < '0' || *c > '9') return -1;
            p = p * 10 + (*c - '0');
        }
        *port = p;
    }
    *path = slash ? slash : "/";
    return *port > 0 && *port < 65536 ? 0 : -1;
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* the value of header name in hdr[0..n), or NULL; *vlen its length */
static const char* header(const char* hdr, int n, const char* name, int* vlen) {
    int nl = (int)strlen(name);
    int i = 0;
    while (i < n) {
        int e = i;
        while (e + 1 < n && !(hdr[e] == '\r' && hdr[e + 1] == '\n')) e++;
        if (e - i > nl && hdr[i + nl] == ':') {
            int k = 0;
            while (k < nl && lower(hdr[i + k]) == lower(name[k])) k++;
            if (k == nl) {
                int v = i + nl + 1;
                while (v < e && (hdr[v] == ' ' || hdr[v] == '\t')) v++;
                *vlen = e - v;
                return hdr + v;
            }
        }
        i = e + 2;
    }
    return NULL;
}

static void set_err(nbr_http_res* r, const char* what) {
    int n = (int)strlen(what);
    if (n > (int)sizeof r->err - 1) n = (int)sizeof r->err - 1;
    memcpy(r->err, what, (size_t)n);
    r->err[n] = 0;
}

int nbr_http(const char* method, const char* url, const void* body, int body_len, char* out, int out_cap, nbr_http_res* r,
             int timeout_ms) {
    char host[96], req[512];
    int port;
    const char* path;
    memset(r, 0, sizeof *r);
    r->content_length = -1;
    uint64_t t0 = nbr_now_us();
    if (parse_url(url, host, sizeof host, &port, &path) < 0) { set_err(r, "bad url"); return -1; }
    if ((int)strlen(path) > 300) { set_err(r, "url too long"); return -1; }
    uint32_t addr;
    if (nbr_resolve(host, &addr) < 0) { set_err(r, "dns failed"); return -1; }
    int s = nbr_tcp_connect(addr, port, timeout_ms);
    if (s < 0) { set_err(r, s == -2 ? "connect timeout" : "connect failed"); return -1; }
    nbr_buf b;
    nbr_buf_init(&b, req, sizeof req);
    nbr_bprintf(&b, "%s %s HTTP/1.0\r\nHost: %s", method, path, host);
    if (port != 80) nbr_bprintf(&b, ":%d", port);
    /* BRANDING: HTTP User-Agent */
    nbr_bputs(&b, "\r\nUser-Agent: NewBlitz-Remote/1\r\nConnection: close\r\n");
    if (strcmp(method, "GET") != 0 || body_len > 0)
        nbr_bprintf(&b, "Content-Length: %d\r\nContent-Type: application/octet-stream\r\n", body_len);
    nbr_bputs(&b, "\r\n");
    if (b.overflow) { nbr_tcp_close(s); set_err(r, "request too long"); return -1; }
    if (nbr_tcp_send(s, req, b.len, timeout_ms) < 0 || (body_len > 0 && nbr_tcp_send(s, body, body_len, timeout_ms) < 0)) {
        nbr_tcp_close(s);
        set_err(r, "send failed");
        return -1;
    }
    int have = 0, hend = -1;
    for (;;) {
        if (have >= out_cap) { nbr_tcp_close(s); set_err(r, "reply too large"); return -1; }
        int n = nbr_tcp_recv(s, out + have, out_cap - have, timeout_ms);
        if (n == 0) break;
        if (n < 0) { nbr_tcp_close(s); set_err(r, n == -2 ? "recv timeout" : "recv failed"); return -1; }
        int from = have > 3 ? have - 3 : 0;
        have += n;
        if (hend < 0) {
            for (int i = from; i + 3 < have; i++)
                if (out[i] == '\r' && out[i + 1] == '\n' && out[i + 2] == '\r' && out[i + 3] == '\n') { hend = i; break; }
            if (hend < 0 && have > HEADER_MAX) { nbr_tcp_close(s); set_err(r, "header too large"); return -1; }
            if (hend >= 0) {
                int vl;
                const char* te = header(out, hend + 2, "Transfer-Encoding", &vl);
                if (te) {
                    for (int i = 0; i + 7 <= vl; i++)
                        if (lower(te[i]) == 'c' && strncmp(te + i + 1, "hunked", 6) == 0) { nbr_tcp_close(s); set_err(r, "chunked reply refused"); return -1; }
                }
                const char* cl = header(out, hend + 2, "Content-Length", &vl);
                if (cl) {
                    long long v = 0;
                    for (int i = 0; i < vl && cl[i] >= '0' && cl[i] <= '9'; i++) v = v * 10 + (cl[i] - '0');
                    r->content_length = v;
                    if (v > out_cap - hend - 4) { nbr_tcp_close(s); set_err(r, "reply too large"); return -1; }
                }
            }
        }
        if (hend >= 0 && r->content_length >= 0 && have - hend - 4 >= r->content_length) break;
    }
    nbr_tcp_close(s);
    r->ms = (int)((nbr_now_us() - t0) / 1000);
    if (hend < 0) { set_err(r, "no header"); return -1; }
    /* "HTTP/1.x NNN" */
    if (hend < 12 || strncmp(out, "HTTP/1.", 7) != 0) { set_err(r, "bad status line"); return -1; }
    r->status = (out[9] - '0') * 100 + (out[10] - '0') * 10 + (out[11] - '0');
    int blen = have - hend - 4;
    if (r->content_length >= 0) {
        if (blen < r->content_length) { set_err(r, "short body"); r->status = 0; return -1; }
        blen = (int)r->content_length;
    }
    memmove(out, out + hend + 4, (size_t)blen);
    if (blen < out_cap) out[blen] = 0;
    r->body_len = blen;
    return 0;
}
