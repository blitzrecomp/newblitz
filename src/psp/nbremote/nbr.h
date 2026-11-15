/* NB Remote (docs/PSP_PORT.md "NB Remote"): the PSP's always-there remote control, shared by
 *   Form A, the "New Blitz Remote" EBOOT (PSP/GAME/NBREMOTE, nbr_eboot.c), and
 *   Form B, the ARK VSH plugin (nbremote.prx, nbr_plugin.c),
 * and built for the Mac too (nbr_plat_host.c, nbr_host_main.c) so the command logic is tested against tools/psp/mock_worker.py.
 *
 * Plain C99, no heap, no exceptions: every buffer is the caller's (nbr_opts). It never listens: it polls the logs Worker's psp
 * mailbox (GET <logs_base>/cmd?pid=psp-remote-<digits>) and posts results to <logs_base>/res/psp-<name>. A command line is
 * "<seq> <verb args> #<tag>", tag = the first 16 hex of HMAC-SHA256(cmd_key, "<seq>|<verb args>"), seq above the last one
 * (kept in <app>/data/cmd_seq): the same signing as the games (src/psp/psp_remote.cpp). Anything else is refused and logged.
 *
 * Verbs: ping; stat; run <game>; update <game> <file> <sha256> <size>; rollback <game>; log <name> <game> [bytes];
 *        ls <name> [path]; get <name> <file>; sendlog; menu; help.   <game>: classic | modern | probe | remote
 */
#ifndef NBR_H
#define NBR_H
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ---- text into a fixed buffer ------------------------------------------------------------------------------------------- */
typedef struct {
    char* p;
    int len, cap;
    int overflow;          /* set once something didn't fit */
} nbr_buf;
void nbr_buf_init(nbr_buf* b, char* mem, int cap);
void nbr_bput(nbr_buf* b, const void* data, int n);
void nbr_bputs(nbr_buf* b, const char* s);
void nbr_bprintf(nbr_buf* b, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
/* A small printf: %% %c %s %d %i %u %x %X %p, the l and ll sizes, the - and 0 flags, a width, and .N / .* for strings. No floats
 * (the plugin has no float printf). Always 0-terminates when n > 0; returns the length it wanted. */
int nbr_vfmt(char* out, int n, const char* fmt, va_list ap);
int nbr_fmt(char* out, int n, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

/* ---- SHA-256 / HMAC (nbr_sha256.c; FIPS 180-4, RFC 2104) ----------------------------------------------------------------- */
typedef struct {
    uint32_t h[8];
    uint8_t buf[64];
    uint64_t len;
    uint32_t fill;
} nbr_sha256;
void nbr_sha256_init(nbr_sha256* s);
void nbr_sha256_update(nbr_sha256* s, const void* data, size_t n);
void nbr_sha256_final(nbr_sha256* s, uint8_t out[32]);
void nbr_hmac_sha256(const void* key, size_t key_len, const void* msg, size_t msg_len, uint8_t out[32]);
void nbr_hex(const uint8_t* bytes, int n, char* out); /* 2n hex digits + 0 */

/* ---- HTTP/1.0 client (nbr_http.c) --------------------------------------------------------------------------------------- */
typedef struct {
    int status;            /* 0: no reply (see err) */
    int body_len;
    long long content_length; /* -1 if not sent */
    int ms;
    char err[64];
} nbr_http_res;
/* The whole reply lands in out (headers are dropped; the body starts at out[0]); out must hold the body plus the headers
 * (4 KB). A body that doesn't fit, a chunked reply, or a reply cut short of its Content-Length fails. */
int nbr_http(const char* method, const char* url, const void* body, int body_len, char* out, int out_cap, nbr_http_res* r,
             int timeout_ms);

/* ---- what the platform gives (nbr_plat_psp.c on the PSP, nbr_plat_host.c on the Mac) ------------------------------------- */
uint64_t nbr_now_us(void);
void nbr_sleep_ms(int ms);
uint32_t nbr_low_digits(void);           /* the pid's digits: system time % 10000000 */
long long nbr_wall_clock(void);          /* UTC seconds since 1970 from the RTC, 0 if unknown */
void nbr_console(const char* line);      /* stdout (PPSSPP, PSPLink) or nothing */

int nbr_open(const char* path, int mode); /* mode: 0 read, 1 write (truncate), 2 append; <0 on failure */
int nbr_read(int fd, void* buf, int n);
int nbr_write(int fd, const void* buf, int n);
long long nbr_seek(int fd, long long off, int whence); /* whence 0 set, 2 end; the new offset */
void nbr_close(int fd);
long long nbr_file_size(const char* path); /* -1 if missing */
int nbr_remove(const char* path);          /* 0 ok */
int nbr_rename(const char* from, const char* to); /* 0 ok; the target must not exist (FAT) */
int nbr_mkdir(const char* path);
int nbr_is_dir(const char* path);
typedef void (*nbr_dir_fn)(const char* name, int is_dir, long long size, void* ctx);
int nbr_list_dir(const char* path, nbr_dir_fn fn, void* ctx); /* 0 ok */
long long nbr_free_bytes(const char* path); /* free space on that path's device, -1 if unknown */

int nbr_resolve(const char* host, uint32_t* addr_be);
int nbr_tcp_connect(uint32_t addr_be, int port, int timeout_ms); /* a socket, <0 on failure */
int nbr_tcp_send(int s, const void* buf, int n, int timeout_ms); /* all of it or <0 */
int nbr_tcp_recv(int s, void* buf, int n, int timeout_ms);       /* >0 bytes, 0 closed, <0 error or timeout */
void nbr_tcp_close(int s);

int nbr_net_up(void);
void nbr_net_describe(nbr_buf* out);     /* "up ip 1.2.3.4 ssid \"x\" profile 6" */
void nbr_sys_describe(nbr_buf* out);     /* battery, charging, memory, firmware lines for stat */
int nbr_battery_low(void);               /* the battery's percent if it is under 10% and not on the charger, else -1 */
int nbr_launch(const char* eboot);       /* start that EBOOT in place of this program; returns only if it failed (<0) */

/* ---- the remote itself (nbr_core.c) -------------------------------------------------------------------------------------- */
enum { NBR_ACT_NONE = 0, NBR_ACT_LAUNCH, NBR_ACT_MENU };

typedef struct {
    const char* app_dir;       /* "ms0:/PSP/GAME/NBREMOTE": data/net.cfg, data/cmd_seq, logs/ */
    const char* games_dir;     /* "ms0:/PSP/GAME": NBCLASSIC, NEWBLITZ, NBPROBE, NBREMOTE */
    const char* form;          /* "eboot", "plugin", "host" */
    const char* build;         /* build number and commit, for stat */
    const char* log_name;      /* "remote.log" (logs/<name>, 256 KB, one .1 kept) */
    char* big;                 /* replies and file reads: log, get, ls, stat (and the poll reply) */
    int big_cap;
    char* chunk;               /* update downloads: one ranged piece plus its headers */
    int chunk_cap;
    int chunk_len;             /* the piece asked for (at most 256 KB, and chunk_cap - 4 KB) */
    int can_menu;              /* the EBOOT can go back to the XMB; the plugin is in it already */
    void (*status)(const char* what); /* the screen's "doing" line (the EBOOT), or NULL */
} nbr_opts;

typedef struct {
    int polls, poll_fail, cmds, rejected, uploads;
    char last_cmd[64], last_result[96];
} nbr_counters;

/* Reads data/net.cfg (logs_base, cmd_key, device, wifi_profile, plus the plugin's keys), the seq, opens the log.
 * Returns 1 if the Worker channel can run (logs_base and cmd_key set), 0 if not (logged). */
int nbr_init(const nbr_opts* o);
const char* nbr_cfg(const char* key);     /* a net.cfg value, "" if absent (never the keys: those read as "") */
int nbr_cfg_int(const char* key, int fallback);
const char* nbr_pid(void);                /* psp-remote-<digits> */
void nbr_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
uint32_t nbr_uptime_ms(void);
void nbr_uptime_text(char* out, int n);   /* "123.456" (no float printf in the plugin) */

/* One step of the poller: polls the mailbox if it is time (10 s idle, 2 s for 2 minutes after a command) and the net is up,
 * runs what came, posts the results. Returns the action a command asked for (NBR_ACT_*) once its reply has been sent; the
 * caller does it (nbr_do_action). */
int nbr_step(void);
/* Runs one command line exactly as the Worker would have delivered it ("<seq> <verb args> #<tag>"): the tests' way in. */
int nbr_handle_line(const char* raw);
const char* nbr_action_path(void);        /* NBR_ACT_LAUNCH: the EBOOT */
/* The run verb without a command (the EBOOT's buttons, the plugin): sets the action path and data/last_run. 0 ok, -1 unknown
 * game, -2 no EBOOT there, -3 it isn't a PBP. */
int nbr_prepare_run(const char* game);
/* 0 if path is a PBP (starts "\0PBP", at least its 40-byte header), -1 missing, -2 not a PBP: nothing is started or installed
 * that would fail to load (a truncated copy, a wrong FILE=). */
int nbr_pbp_check(const char* path);
/* NBR_ACT_LAUNCH: nbr_launch(path) (and the log flushed first). Returns only if it failed. */
void nbr_do_launch(void);
void nbr_mark_xmb_hold(void);             /* data/xmb_hold: the plugin leaves the XMB alone next time */
nbr_counters nbr_get_counters(void);

/* Game names and folders: classic NBCLASSIC, modern NEWBLITZ, probe NBPROBE, remote NBREMOTE. NULL if unknown. */
const char* nbr_game_folder(const char* game);

#ifdef __cplusplus
}
#endif
#endif
