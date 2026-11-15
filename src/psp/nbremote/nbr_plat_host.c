/* NB Remote's platform layer on the Mac (see nbr.h): POSIX files and sockets, so nbr_host_main.c can run the core against
 * tools/psp/mock_worker.py with a folder laid out like a Memory Stick. */
#include "nbr.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

uint64_t nbr_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000;
}
void nbr_sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }
uint32_t nbr_low_digits(void) { return (uint32_t)(nbr_now_us() % 10000000ull); }
long long nbr_wall_clock(void) { return (long long)time(NULL); }
void nbr_console(const char* line) { fprintf(stderr, "%s\n", line); }

int nbr_open(const char* path, int mode) {
    int fl = mode == 0 ? O_RDONLY : mode == 1 ? (O_WRONLY | O_CREAT | O_TRUNC) : (O_WRONLY | O_CREAT | O_APPEND);
    return open(path, fl, 0644);
}
int nbr_read(int fd, void* buf, int n) { return (int)read(fd, buf, (size_t)n); }
int nbr_write(int fd, const void* buf, int n) { return (int)write(fd, buf, (size_t)n); }
long long nbr_seek(int fd, long long off, int whence) { return (long long)lseek(fd, (off_t)off, whence == 2 ? SEEK_END : SEEK_SET); }
void nbr_close(int fd) { close(fd); }
long long nbr_file_size(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) return -1;
    return (long long)st.st_size;
}
int nbr_remove(const char* path) { return unlink(path) == 0 ? 0 : -1; }
int nbr_rename(const char* from, const char* to) {
    struct stat st;
    if (stat(to, &st) == 0) return -1;   /* as on FAT: the target must not exist */
    return rename(from, to) == 0 ? 0 : -1;
}
int nbr_mkdir(const char* path) { return mkdir(path, 0755) == 0 ? 0 : -1; }
int nbr_is_dir(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}
int nbr_list_dir(const char* path, nbr_dir_fn fn, void* ctx) {
    DIR* d = opendir(path);
    if (!d) return -1;
    struct dirent* e;
    while ((e = readdir(d))) {
        char full[1024];
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        if (stat(full, &st) != 0) continue;
        fn(e->d_name, S_ISDIR(st.st_mode), (long long)st.st_size, ctx);
    }
    closedir(d);
    return 0;
}
long long nbr_free_bytes(const char* path) {
    const char* fake = getenv("NBR_FREE_BYTES");   /* tests: pretend the stick is nearly full */
    if (fake) return atoll(fake);
    struct statvfs v;
    if (statvfs(path, &v) != 0) return -1;
    return (long long)v.f_bavail * (long long)v.f_frsize;
}

int nbr_resolve(const char* host, uint32_t* addr_be) {
    struct in_addr ia;
    if (inet_aton(host, &ia)) { *addr_be = ia.s_addr; return 0; }
    struct hostent* he = gethostbyname(host);
    if (!he || he->h_length != 4) return -1;
    memcpy(addr_be, he->h_addr_list[0], 4);
    return 0;
}

static int wait_fd(int s, short ev, int ms) {
    struct pollfd p = {s, ev, 0};
    return poll(&p, 1, ms);
}

int nbr_tcp_connect(uint32_t addr_be, int port, int timeout_ms) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    int one = 1;
#ifdef SO_NOSIGPIPE
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    (void)one;
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = addr_be;
    if (connect(s, (struct sockaddr*)&sa, sizeof sa) < 0) {
        if (errno != EINPROGRESS) { close(s); return -1; }
        if (wait_fd(s, POLLOUT, timeout_ms) <= 0) { close(s); return -2; }
        int err = 0;
        socklen_t len = sizeof err;
        getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err) { close(s); return -1; }
    }
    return s;
}
int nbr_tcp_send(int s, const void* buf, int n, int timeout_ms) {
    int sent = 0;
    while (sent < n) {
        ssize_t k = send(s, (const char*)buf + sent, (size_t)(n - sent), 0);
        if (k > 0) { sent += (int)k; continue; }
        if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && wait_fd(s, POLLOUT, timeout_ms) > 0) continue;
        return -1;
    }
    return sent;
}
int nbr_tcp_recv(int s, void* buf, int n, int timeout_ms) {
    for (;;) {
        ssize_t k = recv(s, buf, (size_t)n, 0);
        if (k >= 0) return (int)k;
        if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;
        if (wait_fd(s, POLLIN, timeout_ms) <= 0) return -2;
    }
}
void nbr_tcp_close(int s) { close(s); }

int nbr_net_up(void) { return 1; }
void nbr_net_describe(nbr_buf* out) { nbr_bputs(out, "up (the Mac's network)"); }
void nbr_sys_describe(nbr_buf* out) { nbr_bputs(out, "battery n/a (Mac)\nmemory n/a\nfirmware n/a\n"); }
int nbr_battery_low(void) { const char* v = getenv("NBR_BATTERY_LOW"); return v ? atoi(v) : -1; }   /* tests */
int nbr_launch(const char* eboot) {
    fprintf(stdout, "LAUNCH %s\n", eboot);
    fflush(stdout);
    return -1;
}
