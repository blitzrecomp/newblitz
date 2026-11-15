/* NB Remote's platform layer on the PSP (see nbr.h), for the EBOOT and the VSH plugin alike: sceIo files, sceNetInet sockets
 * called directly (no libcglue, so the plugin needs no newlib), scePower and the RTC. NBR_NO_NET=1 (the launcher-only plugin)
 * leaves every net import out: its sockets just fail. */
#include "nbr.h"
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <psppower.h>
#include <psprtc.h>
#include <string.h>

void (*nbr_console_fn)(const char* line) = 0;   /* the EBOOT points this at printf (PPSSPP and PSPLink show it) */

uint64_t nbr_now_us(void) { return sceKernelGetSystemTimeWide(); }
void nbr_sleep_ms(int ms) { sceKernelDelayThread((SceUInt)ms * 1000); }
uint32_t nbr_low_digits(void) { return sceKernelGetSystemTimeLow() % 10000000u; }
long long nbr_wall_clock(void) {
    u64 tick = 0;
    if (sceRtcGetCurrentTick(&tick) < 0) return 0;
    const u64 epoch = 62135596800ull * 1000000ull;   /* 0001-01-01 to 1970-01-01, in microseconds */
    return tick > epoch ? (long long)((tick - epoch) / 1000000ull) : 0;
}
void nbr_console(const char* line) { if (nbr_console_fn) nbr_console_fn(line); }

int nbr_open(const char* path, int mode) {
    int fl = mode == 0 ? PSP_O_RDONLY : mode == 1 ? (PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC) : (PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND);
    SceUID fd = sceIoOpen(path, fl, 0777);
    return fd < 0 ? -1 : (int)fd;
}
int nbr_read(int fd, void* buf, int n) { return sceIoRead(fd, buf, (SceSize)n); }
int nbr_write(int fd, const void* buf, int n) { return sceIoWrite(fd, buf, (SceSize)n); }
long long nbr_seek(int fd, long long off, int whence) { return sceIoLseek(fd, off, whence == 2 ? PSP_SEEK_END : PSP_SEEK_SET); }
void nbr_close(int fd) { sceIoClose(fd); }
long long nbr_file_size(const char* path) {
    SceIoStat st;
    memset(&st, 0, sizeof st);
    if (sceIoGetstat(path, &st) < 0 || FIO_S_ISDIR(st.st_mode)) return -1;
    return (long long)st.st_size;
}
int nbr_remove(const char* path) { return sceIoRemove(path) < 0 ? -1 : 0; }

/* sceIoRename on the Memory Stick: the target must not exist. If the firmware won't take the target with its device name, the
 * path without it; as a last resort a copy and a delete (an EBOOT is a few MB). */
int nbr_rename(const char* from, const char* to) {
    if (nbr_file_size(to) >= 0 || nbr_is_dir(to)) return -1;
    int rc = sceIoRename(from, to);
    if (rc >= 0) return 0;
    const char* colon = strchr(to, ':');
    if (colon && sceIoRename(from, colon + 1) >= 0) return 0;
    nbr_log("sceIoRename %s -> %s: 0x%08X; copying instead", from, to, (unsigned)rc);
    static char buf[16 * 1024];
    int in = nbr_open(from, 0);
    if (in < 0) return -1;
    int out = nbr_open(to, 1);
    if (out < 0) { nbr_close(in); return -1; }
    int ok = 1;
    for (;;) {
        int n = nbr_read(in, buf, sizeof buf);
        if (n < 0) { ok = 0; break; }
        if (n == 0) break;
        if (nbr_write(out, buf, n) != n) { ok = 0; break; }
    }
    nbr_close(in);
    nbr_close(out);
    if (!ok) { nbr_remove(to); return -1; }
    nbr_remove(from);
    return 0;
}
int nbr_mkdir(const char* path) { return sceIoMkdir(path, 0777) < 0 ? -1 : 0; }
int nbr_is_dir(const char* path) {
    SceUID d = sceIoDopen(path);
    if (d < 0) return 0;
    sceIoDclose(d);
    return 1;
}
int nbr_list_dir(const char* path, nbr_dir_fn fn, void* ctx) {
    SceUID d = sceIoDopen(path);
    if (d < 0) return -1;
    static SceIoDirent e;
    for (;;) {
        memset(&e, 0, sizeof e);
        if (sceIoDread(d, &e) <= 0) break;
        fn(e.d_name, FIO_S_ISDIR(e.d_stat.st_mode) != 0, (long long)e.d_stat.st_size, ctx);
    }
    sceIoDclose(d);
    return 0;
}
long long nbr_free_bytes(const char* path) {
    (void)path;
    struct { u32 max_clusters, free_clusters, max_sectors, sector_size, sectors_per_cluster; } info;
    void* argp = &info;
    memset(&info, 0, sizeof info);
    if (sceIoDevctl("ms0:", 0x02425818, &argp, sizeof argp, NULL, 0) < 0) return -1;
    return (long long)info.free_clusters * info.sector_size * info.sectors_per_cluster;
}

static int is_emu(void) {
    static int cached = -1;
    if (cached < 0) {
        int out = 0;
        int r = sceIoDevctl("kemulator:", 3, NULL, 0, &out, sizeof out);
        cached = (r >= 0 && out) ? 1 : 0;
    }
    return cached;
}

/* ---- sockets: sceNetInet direct (declared here so no newlib socket headers are needed) ---------------------------------- */
#if NBR_NO_NET
int nbr_resolve(const char* host, uint32_t* addr_be) { (void)host; (void)addr_be; return -1; }
int nbr_tcp_connect(uint32_t addr_be, int port, int timeout_ms) { (void)addr_be; (void)port; (void)timeout_ms; return -1; }
int nbr_tcp_send(int s, const void* buf, int n, int timeout_ms) { (void)s; (void)buf; (void)n; (void)timeout_ms; return -1; }
int nbr_tcp_recv(int s, void* buf, int n, int timeout_ms) { (void)s; (void)buf; (void)n; (void)timeout_ms; return -1; }
void nbr_tcp_close(int s) { (void)s; }
int nbr_net_up(void) { return 0; }
void nbr_net_describe(nbr_buf* out) { nbr_bputs(out, "none (the launcher plugin has no Wi-Fi of its own)"); }
#else
int sceNetInetSocket(int domain, int type, int protocol);
int sceNetInetSetsockopt(int s, int level, int name, const void* val, unsigned len);
int sceNetInetGetsockopt(int s, int level, int name, void* val, unsigned* len);
int sceNetInetConnect(int s, const void* addr, unsigned len);
int sceNetInetSend(int s, const void* buf, unsigned len, int flags);
int sceNetInetRecv(int s, void* buf, unsigned len, int flags);
int sceNetInetSelect(int n, void* rd, void* wr, void* ex, void* timeout);
int sceNetInetClose(int s);
int sceNetInetGetErrno(void);
int sceNetResolverCreate(int* rid, void* buf, unsigned len);
int sceNetResolverStartNtoA(int rid, const char* host, void* addr, unsigned timeout, int retry);
int sceNetResolverDelete(int rid);

typedef struct { uint32_t bits[8]; } fdset256;           /* the PSP's fd_set: 256 descriptors */
typedef struct { int32_t sec, usec; } inet_timeval;
typedef struct { uint8_t len, family; uint16_t port; uint32_t addr; uint8_t zero[8]; } inet_sockaddr_in;   /* BSD, with sin_len */

static int busy(int e) { return e == 11 || e == 35 || e == 36 || e == 115 || e == 119 || e == 0x77; }   /* EAGAIN / EINPROGRESS, both numberings */

static int wait_sock(int s, int write, int ms) {
    fdset256 set;
    memset(&set, 0, sizeof set);
    if (s < 0 || s >= 256) return -1;
    set.bits[s >> 5] |= 1u << (s & 31);
    inet_timeval tv = {ms / 1000, (ms % 1000) * 1000};
    return sceNetInetSelect(s + 1, write ? NULL : &set, write ? &set : NULL, NULL, &tv);
}

static int parse_quad(const char* s, uint32_t* be) {
    uint32_t parts[4];
    int n = 0;
    while (n < 4) {
        if (*s < '0' || *s > '9') return -1;
        uint32_t v = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (uint32_t)(*s++ - '0'); if (v > 255) return -1; }
        parts[n++] = v;
        if (n < 4) { if (*s != '.') return -1; s++; }
    }
    if (*s) return -1;
    uint8_t* b = (uint8_t*)be;   /* network order: the first part first in memory */
    for (int i = 0; i < 4; i++) b[i] = (uint8_t)parts[i];
    return 0;
}

int nbr_resolve(const char* host, uint32_t* addr_be) {
    if (parse_quad(host, addr_be) == 0) return 0;
    static char rbuf[1024];
    int rid = -1;
    if (sceNetResolverCreate(&rid, rbuf, sizeof rbuf) < 0) return -1;
    uint32_t a = 0;
    int rc = sceNetResolverStartNtoA(rid, host, &a, 2, 3);
    sceNetResolverDelete(rid);
    if (rc < 0 || !a) return -1;
    *addr_be = a;
    return 0;
}

int nbr_tcp_connect(uint32_t addr_be, int port, int timeout_ms) {
    int s = sceNetInetSocket(2 /* AF_INET */, 1 /* SOCK_STREAM */, 0);
    if (s < 0) return -1;
    int one = 1;
    sceNetInetSetsockopt(s, 0xffff /* SOL_SOCKET */, 0x1009 /* SO_NONBLOCK */, &one, sizeof one);
    inet_sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.len = sizeof sa;
    sa.family = 2;
    sa.port = (uint16_t)(((port & 0xff) << 8) | ((port >> 8) & 0xff));
    sa.addr = addr_be;
    if (sceNetInetConnect(s, &sa, sizeof sa) < 0) {
        int e = sceNetInetGetErrno();
        if (!busy(e)) { sceNetInetClose(s); return -1; }
        if (wait_sock(s, 1, timeout_ms) <= 0) { sceNetInetClose(s); return -2; }
        int err = 0;
        unsigned len = sizeof err;
        if (sceNetInetGetsockopt(s, 0xffff, 0x1007 /* SO_ERROR */, &err, &len) < 0) err = 0;   /* PPSSPP may not answer */
        if (err) { sceNetInetClose(s); return -1; }
    }
    return s;
}

int nbr_tcp_send(int s, const void* buf, int n, int timeout_ms) {
    int sent = 0;
    uint64_t until = nbr_now_us() + (uint64_t)timeout_ms * 1000;
    while (sent < n) {
        int k = sceNetInetSend(s, (const char*)buf + sent, (unsigned)(n - sent), 0);
        if (k > 0) { sent += k; until = nbr_now_us() + (uint64_t)timeout_ms * 1000; continue; }
        if (k < 0 && busy(sceNetInetGetErrno()) && nbr_now_us() < until) { wait_sock(s, 1, 200); continue; }
        return -1;
    }
    return sent;
}

int nbr_tcp_recv(int s, void* buf, int n, int timeout_ms) {
    uint64_t until = nbr_now_us() + (uint64_t)timeout_ms * 1000;
    for (;;) {
        int k = sceNetInetRecv(s, buf, (unsigned)n, 0);
        if (k >= 0) return k;
        if (!busy(sceNetInetGetErrno())) return -1;
        if (nbr_now_us() >= until) return -2;
        wait_sock(s, 0, 200);
    }
}

void nbr_tcp_close(int s) { sceNetInetClose(s); }
#endif

/* ---- stat lines --------------------------------------------------------------------------------------------------------- */
#if NBR_CFW_IMPORTS
int sctrlHENGetVersion(void);
int sctrlHENGetMinorVersion(void);
#endif

int nbr_battery_low(void) {
    int pct = scePowerGetBatteryLifePercent();
    if (pct < 0 || pct >= 10 || scePowerIsPowerOnline() == 1) return -1;   /* no battery reading (on AC), plenty, or charging */
    return pct;
}

void nbr_sys_describe(nbr_buf* out) {
    int pct = scePowerGetBatteryLifePercent();
    int charging = scePowerIsBatteryCharging(), online = scePowerIsPowerOnline(), mv = scePowerGetBatteryVolt();
    if (pct >= 0) {
        nbr_bprintf(out, "battery %d%% %s%s", pct, charging == 1 ? "charging" : "not charging", online == 1 ? " (on the charger)" : " (on battery)");
        if (mv > 0 && mv < 10000) nbr_bprintf(out, ", %d mV", mv);
        nbr_bputs(out, "\n");
    }
    else nbr_bprintf(out, "battery none (0x%08X)%s\n", (unsigned)pct, online == 1 ? " (on the charger)" : "");
    nbr_bprintf(out, "memory free_kb=%d max_kb=%d\n", (int)(sceKernelTotalFreeMemSize() / 1024), (int)(sceKernelMaxFreeMemSize() / 1024));
    unsigned fw = (unsigned)sceKernelDevkitVersion();
    nbr_bprintf(out, "firmware %u.%u%u (0x%08X)%s", fw >> 24, (fw >> 16) & 0xff, (fw >> 8) & 0xff, fw, is_emu() ? " emulator" : "");
#if NBR_CFW_IMPORTS
    if (!is_emu()) nbr_bprintf(out, " cfw 0x%08X.%d", (unsigned)sctrlHENGetVersion(), sctrlHENGetMinorVersion());
#endif
    nbr_bputs(out, "\n");
}

int nbr_launch_eboot(const char* path);   /* nbr_launch_psp.c */
int nbr_launch(const char* eboot) { return nbr_launch_eboot(eboot); }
