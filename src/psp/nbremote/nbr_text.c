/* NB Remote's text helpers (see nbr.h): a small printf and fixed buffers, so the core needs no libc printf (the VSH plugin
 * links ARK's small libansic, not newlib). */
#include "nbr.h"
#include <string.h>

typedef struct { char* out; int n, at; } sink;

static void put(sink* s, char c) {
    if (s->at < s->n - 1) s->out[s->at] = c;
    s->at++;
}

static void put_num(sink* s, unsigned long long v, int base, int upper, int neg, int width, int zero, int left) {
    char tmp[24];
    int k = 0;
    const char* dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do { tmp[k++] = dig[v % (unsigned)base]; v /= (unsigned)base; } while (v);
    int len = k + (neg ? 1 : 0);
    if (!left && !zero) while (width-- > len) put(s, ' ');
    if (neg) put(s, '-');
    if (!left && zero) while (width-- > len) put(s, '0');
    while (k) put(s, tmp[--k]);
    if (left) while (width-- > len) put(s, ' ');
}

int nbr_vfmt(char* out, int n, const char* f, va_list ap) {
    sink s = {out, n, 0};
    for (; *f; f++) {
        if (*f != '%') { put(&s, *f); continue; }
        f++;
        int left = 0, zero = 0, width = 0, prec = -1, lng = 0;
        for (;; f++) {
            if (*f == '-') left = 1;
            else if (*f == '0') zero = 1;
            else break;
        }
        if (*f == '*') { width = va_arg(ap, int); f++; }
        else while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        if (*f == '.') {
            f++;
            prec = 0;
            if (*f == '*') { prec = va_arg(ap, int); f++; }
            else while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0');
        }
        while (*f == 'l') { lng++; f++; }
        if (*f == 'z') { f++; }
        switch (*f) {
            case '%': put(&s, '%'); break;
            case 'c': put(&s, (char)va_arg(ap, int)); break;
            case 's': {
                const char* str = va_arg(ap, const char*);
                if (!str) str = "(null)";
                int len = (int)strlen(str);
                if (prec >= 0 && prec < len) len = prec;
                if (!left) while (width-- > len) put(&s, ' ');
                for (int i = 0; i < len; i++) put(&s, str[i]);
                if (left) while (width-- > len) put(&s, ' ');
                break;
            }
            case 'd': case 'i': {
                long long v = lng >= 2 ? va_arg(ap, long long) : lng == 1 ? (long long)va_arg(ap, long) : (long long)va_arg(ap, int);
                put_num(&s, v < 0 ? (unsigned long long)(-(v + 1)) + 1 : (unsigned long long)v, 10, 0, v < 0, width, zero, left);
                break;
            }
            case 'u': case 'x': case 'X': {
                unsigned long long v = lng >= 2 ? va_arg(ap, unsigned long long)
                                     : lng == 1 ? (unsigned long long)va_arg(ap, unsigned long) : (unsigned long long)va_arg(ap, unsigned);
                put_num(&s, v, *f == 'u' ? 10 : 16, *f == 'X', 0, width, zero, left);
                break;
            }
            case 'p': put(&s, '0'); put(&s, 'x'); put_num(&s, (unsigned long long)(uintptr_t)va_arg(ap, void*), 16, 0, 0, 8, 1, 0); break;
            case 0: f--; break;
            default: put(&s, '%'); put(&s, *f); break;
        }
    }
    if (n > 0) out[s.at < n ? s.at : n - 1] = 0;
    return s.at;
}

int nbr_fmt(char* out, int n, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = nbr_vfmt(out, n, fmt, ap);
    va_end(ap);
    return r;
}

void nbr_buf_init(nbr_buf* b, char* mem, int cap) {
    b->p = mem;
    b->len = 0;
    b->cap = cap;
    b->overflow = 0;
    if (cap > 0) mem[0] = 0;
}

void nbr_bput(nbr_buf* b, const void* data, int n) {
    if (n <= 0) return;
    if (b->len + n > b->cap - 1) { n = b->cap - 1 - b->len; b->overflow = 1; }
    if (n <= 0) return;
    memcpy(b->p + b->len, data, (size_t)n);
    b->len += n;
    b->p[b->len] = 0;
}

void nbr_bputs(nbr_buf* b, const char* s) { nbr_bput(b, s, (int)strlen(s)); }

void nbr_bprintf(nbr_buf* b, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int room = b->cap - b->len;
    int want = nbr_vfmt(b->p + b->len, room, fmt, ap);
    va_end(ap);
    if (want >= room) { b->overflow = 1; want = room > 0 ? room - 1 : 0; }
    b->len += want;
}
