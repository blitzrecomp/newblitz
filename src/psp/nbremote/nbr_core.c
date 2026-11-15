/* NB Remote's command core (see nbr.h): net.cfg, the signed-line check, the verbs, the poller. */
#include "nbr.h"
#include <string.h>

#define MAX_CFG 20
#define LOG_CAP (256 * 1024)
#define FILE_MAX (20 * 1024 * 1024)
#define POLL_FAST_US (2 * 1000000ull)
#define FAST_FOR_US (120 * 1000000ull)

static nbr_opts g;
static struct { char k[24]; char v[200]; } g_cfg[MAX_CFG];
static int g_ncfg;
static char g_base[200];          /* logs_base: http://<host>/<PSP_KEY> (never logged) */
static char g_key[96];            /* cmd_key */
static char g_pid[32];
static long long g_last_seq = -1;
static uint64_t g_t0, g_next_poll, g_fast_until;
static nbr_counters g_count;
static int g_action;
static char g_action_path[160];
static char g_poll[8192];         /* the mailbox's reply: command lines */
static char g_small[2048];        /* small replies (stored, meta) */
static struct { long long size; char hex[65]; } g_hash[8];

/* ---- small helpers ------------------------------------------------------------------------------------------------------ */
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int upper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

static int ci_find(const char* hay, const char* needle) {
    int n = (int)strlen(needle);
    for (const char* p = hay; *p; p++) {
        int k = 0;
        while (k < n && p[k] && lower(p[k]) == lower(needle[k])) k++;
        if (k == n) return 1;
    }
    return 0;
}

static int parse_ll(const char* s, long long* out) {
    long long v = 0;
    int digits = 0;
    if (!*s) return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || ++digits > 18) return -1;
        v = v * 10 + (*s - '0');
    }
    *out = v;
    return 0;
}

static int safe_name(const char* n) {
    int len = (int)strlen(n);
    if (len < 1 || len > 60 || n[0] == '.') return 0;
    for (int i = 0; i < len; i++) {
        char c = n[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) return 0;
    }
    return 1;
}

uint32_t nbr_uptime_ms(void) { return (uint32_t)((nbr_now_us() - g_t0) / 1000); }

void nbr_uptime_text(char* out, int n) {
    uint32_t ms = nbr_uptime_ms();
    nbr_fmt(out, n, "%u.%03u", (unsigned)(ms / 1000), (unsigned)(ms % 1000));
}

static void utc_text(long long t, char* out, int n) {
    if (t <= 0) { nbr_fmt(out, n, "unknown"); return; }
    long long days = t / 86400, rem = t % 86400;
    /* civil from days (Howard Hinnant's algorithm) */
    long long z = days + 719468, era = z / 146097, doe = z - era * 146097;
    long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long long mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    nbr_fmt(out, n, "%04d-%02d-%02d %02d:%02d:%02dZ", (int)y, (int)m, (int)d, (int)(rem / 3600), (int)(rem / 60 % 60), (int)(rem % 60));
}

/* ---- the log: <app>/logs/<log_name>, 256 KB, one .1 kept; and the console ------------------------------------------------ */
void nbr_log(const char* fmt, ...) {
    char line[400];
    char up[24];
    nbr_uptime_text(up, sizeof up);
    int k = nbr_fmt(line, sizeof line, "[%10s] ", up);
    va_list ap;
    va_start(ap, fmt);
    nbr_vfmt(line + k, (int)sizeof line - k - 1, fmt, ap);
    va_end(ap);
    nbr_console(line);
    if (!g.app_dir) return;
    char path[200], old[200];
    nbr_fmt(path, sizeof path, "%s/logs/%s", g.app_dir, g.log_name ? g.log_name : "remote.log");
    long long size = nbr_file_size(path);
    if (size > LOG_CAP) {
        nbr_fmt(old, sizeof old, "%s.1", path);
        nbr_remove(old);
        nbr_rename(path, old);
    }
    int fd = nbr_open(path, 2);
    if (fd < 0) return;
    int len = (int)strlen(line);
    line[len] = '\n';
    nbr_write(fd, line, len + 1);
    nbr_close(fd);
}

/* ---- net.cfg ------------------------------------------------------------------------------------------------------------ */
static void load_cfg(void) {
    char path[200];
    nbr_fmt(path, sizeof path, "%s/data/net.cfg", g.app_dir);
    g_ncfg = 0;
    int fd = nbr_open(path, 0);
    if (fd < 0) return;
    static char text[4096];
    int n = nbr_read(fd, text, sizeof text - 1);
    nbr_close(fd);
    if (n <= 0) return;
    text[n] = 0;
    char* p = text;
    while (*p && g_ncfg < MAX_CFG) {
        char* e = p;
        while (*e && *e != '\n') e++;
        char* next = *e ? e + 1 : e;
        *e = 0;
        while (e > p && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        while (*p == ' ' || *p == '\t') p++;
        char* eq = strchr(p, '=');
        if (*p && *p != '#' && eq) {
            *eq = 0;
            char* k = p;
            char* v = eq + 1;
            char* ke = eq;
            while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) *--ke = 0;
            while (*v == ' ' || *v == '\t') v++;
            nbr_fmt(g_cfg[g_ncfg].k, sizeof g_cfg[g_ncfg].k, "%s", k);
            nbr_fmt(g_cfg[g_ncfg].v, sizeof g_cfg[g_ncfg].v, "%s", v);
            g_ncfg++;
        }
        p = next;
    }
}

static const char* cfg_raw(const char* key) {
    for (int i = 0; i < g_ncfg; i++) if (strcmp(g_cfg[i].k, key) == 0) return g_cfg[i].v;
    return "";
}

const char* nbr_cfg(const char* key) {
    if (strcmp(key, "cmd_key") == 0 || strcmp(key, "logs_base") == 0 || strcmp(key, "hub_base") == 0) return "";
    return cfg_raw(key);
}

int nbr_cfg_int(const char* key, int fallback) {
    const char* v = cfg_raw(key);
    long long x;
    if (!*v || parse_ll(v, &x) < 0) return fallback;
    return (int)x;
}

/* ---- seq ---------------------------------------------------------------------------------------------------------------- */
static void seq_path(char* out, int n, const char* suffix) { nbr_fmt(out, n, "%s/data/cmd_seq%s", g.app_dir, suffix); }

static void load_seq(void) {
    /* cmd_seq, or the cmd_seq.tmp save_seq left if the power went between its remove and rename: the higher one, so a lost
     * file never lets an old signed line run again */
    static const char* const suffix[2] = {"", ".tmp"};
    for (int k = 0; k < 2; k++) {
        char path[200], text[32];
        seq_path(path, sizeof path, suffix[k]);
        int fd = nbr_open(path, 0);
        if (fd < 0) continue;
        int n = nbr_read(fd, text, sizeof text - 1);
        nbr_close(fd);
        if (n <= 0) continue;
        text[n] = 0;
        while (n && (text[n - 1] == '\n' || text[n - 1] == '\r' || text[n - 1] == ' ')) text[--n] = 0;
        long long v;
        if (parse_ll(text, &v) == 0 && v > g_last_seq) g_last_seq = v;
    }
}

static void save_seq(void) {
    char path[200], tmp[200], text[32];
    seq_path(path, sizeof path, "");
    seq_path(tmp, sizeof tmp, ".tmp");
    int n = nbr_fmt(text, sizeof text, "%lld\n", g_last_seq);
    int fd = nbr_open(tmp, 1);
    if (fd < 0) return;
    nbr_write(fd, text, n);
    nbr_close(fd);
    nbr_remove(path);
    nbr_rename(tmp, path);
}

/* ---- games -------------------------------------------------------------------------------------------------------------- */
static const struct { const char* game; const char* folder; } GAMES[] = {
    {"classic", "NBCLASSIC"}, {"modern", "NEWBLITZ"}, {"probe", "NBPROBE"}, {"remote", "NBREMOTE"}};
#define NGAMES 4

const char* nbr_game_folder(const char* game) {
    for (int i = 0; i < NGAMES; i++) if (strcmp(GAMES[i].game, game) == 0) return GAMES[i].folder;
    return NULL;
}

static int game_index(const char* game) {
    for (int i = 0; i < NGAMES; i++) if (strcmp(GAMES[i].game, game) == 0) return i;
    return -1;
}

static void game_file(char* out, int n, const char* folder, const char* rel) {
    nbr_fmt(out, n, "%s/%s/%s", g.games_dir, folder, rel);
}

/* sha256 of a file through the chunk buffer; hex into out (65). <0 if it can't be read. */
static int hash_file(const char* path, char* hex, long long* size_out) {
    if (!g.chunk || g.chunk_cap < 4096) return -1;
    int fd = nbr_open(path, 0);
    if (fd < 0) return -1;
    nbr_sha256 s;
    nbr_sha256_init(&s);
    long long total = 0;
    int piece = g.chunk_cap > 65536 ? 65536 : g.chunk_cap;
    for (;;) {
        int n = nbr_read(fd, g.chunk, piece);
        if (n < 0) { nbr_close(fd); return -1; }
        if (n == 0) break;
        nbr_sha256_update(&s, g.chunk, (size_t)n);
        total += n;
    }
    nbr_close(fd);
    uint8_t d[32];
    nbr_sha256_final(&s, d);
    nbr_hex(d, 32, hex);
    if (size_out) *size_out = total;
    return 0;
}

static const char* eboot_hash(int gi, long long size) {
    if (g_hash[gi].size == size && g_hash[gi].hex[0]) return g_hash[gi].hex;
    char path[200];
    game_file(path, sizeof path, GAMES[gi].folder, "EBOOT.PBP");
    long long got;
    if (hash_file(path, g_hash[gi].hex, &got) < 0 || got != size) { g_hash[gi].hex[0] = 0; g_hash[gi].size = -1; return "?"; }
    g_hash[gi].size = size;
    return g_hash[gi].hex;
}

static void forget_hash(int gi) { g_hash[gi].size = -1; g_hash[gi].hex[0] = 0; }

/* ---- Worker requests ---------------------------------------------------------------------------------------------------- */
static int worker(const char* method, const char* path, const void* body, int blen, char* out, int cap, nbr_http_res* r, int timeout_ms) {
    char url[400];
    nbr_fmt(url, sizeof url, "%s%s", g_base, path);
    return nbr_http(method, url, body, blen, out, cap, r, timeout_ms);
}

static int post_result(const char* name, const char* body, int len) {
    char path[120];
    nbr_http_res r;
    nbr_fmt(path, sizeof path, "/res/psp-%s", name);
    for (int attempt = 0; attempt < 3; attempt++) {
        if (worker("POST", path, body, len, g_small, sizeof g_small - 1, &r, 15000) == 0 && r.status == 200) return 0;
        nbr_log("res/psp-%s: status %d %s (try %d)", name, r.status, r.err, attempt + 1);
        nbr_sleep_ms(1000);
    }
    return -1;
}

/* ---- path rules for ls/get: under <games>/NB* or <games>/NEWBLITZ, no "..", never net.cfg -------------------------------- */
static int game_path(const char* arg, char* out, int n) {
    const char* rel = arg;
    const char* pre = "ms0:/PSP/GAME/";
    int pl = (int)strlen(pre), k = 0;
    while (k < pl && arg[k] && lower(arg[k]) == lower(pre[k])) k++;
    if (k == pl) rel = arg + pl;
    if (!*rel || rel[0] == '/' || strchr(rel, ':') || strchr(rel, '\\') || strstr(rel, "..")) return -1;
    if (ci_find(rel, "net.cfg")) return -1;
    char first[32];
    int i = 0;
    while (rel[i] && rel[i] != '/' && i < 31) { first[i] = (char)upper(rel[i]); i++; }
    first[i] = 0;
    if (!(strncmp(first, "NB", 2) == 0 || strcmp(first, "NEWBLITZ") == 0)) return -1;
    nbr_fmt(out, n, "%s/%s", g.games_dir, rel);
    int len = (int)strlen(out);
    while (len > 1 && out[len - 1] == '/') out[--len] = 0;
    return 0;
}

/* ---- verbs -------------------------------------------------------------------------------------------------------------- */
typedef int (*verb_fn)(int argc, char** argv, nbr_buf* out);

static int v_help(int argc, char** argv, nbr_buf* out) {
    (void)argc; (void)argv;
    nbr_bputs(out, "ping | stat | run <game> | update <game> <file> <sha256> <size> | rollback <game> | log <name> <game> [bytes] |\n"
                   "ls <name> [path] | get <name> <file> | sendlog | menu | help      <game>: classic modern probe remote\n");
    return 1;
}

static int v_ping(int argc, char** argv, nbr_buf* out) {
    (void)argc; (void)argv;
    char up[24];
    nbr_uptime_text(up, sizeof up);
    nbr_bprintf(out, "pong remote %s %s %s\n", g.form, g_pid, up);
    return 1;
}

static int v_stat(int argc, char** argv, nbr_buf* out) {
    (void)argc; (void)argv;
    char up[24], when[40];
    nbr_uptime_text(up, sizeof up);
    utc_text(nbr_wall_clock(), when, sizeof when);
    nbr_bprintf(out, "remote %s build %s pid %s uptime %s clock %s\n", g.form, g.build ? g.build : "?", g_pid, up, when);
    nbr_sys_describe(out);
    nbr_bputs(out, "wifi ");
    nbr_net_describe(out);
    nbr_bputs(out, "\n");
    char path[200], text[96];
    nbr_fmt(path, sizeof path, "%s/data/last_run", g.app_dir);
    int fd = nbr_open(path, 0);
    int n = fd >= 0 ? nbr_read(fd, text, sizeof text - 1) : 0;
    if (fd >= 0) nbr_close(fd);
    if (n > 0) { text[n] = 0; while (n && text[n - 1] == '\n') text[--n] = 0; nbr_bprintf(out, "last_run %s\n", text); }
    else nbr_bputs(out, "last_run none\n");
    for (int i = 0; i < NGAMES; i++) {
        char eb[200], bak[200];
        game_file(eb, sizeof eb, GAMES[i].folder, "EBOOT.PBP");
        game_file(bak, sizeof bak, GAMES[i].folder, "EBOOT.PBP.bak");
        long long size = nbr_file_size(eb), bsize = nbr_file_size(bak);
        if (size < 0) { nbr_bprintf(out, "game %-7s %-9s no EBOOT\n", GAMES[i].game, GAMES[i].folder); continue; }
        nbr_bprintf(out, "game %-7s %-9s EBOOT %lld bytes sha256 %.16s", GAMES[i].game, GAMES[i].folder, size, eboot_hash(i, size));
        if (bsize >= 0) nbr_bprintf(out, ", .bak %lld bytes", bsize);
        nbr_bputs(out, "\n");
    }
    nbr_bprintf(out, "polls %d (failed %d) cmds %d rejected %d uploads %d\n", g_count.polls, g_count.poll_fail, g_count.cmds,
                g_count.rejected, g_count.uploads);
    return 1;
}

int nbr_pbp_check(const char* path) {
    long long size = nbr_file_size(path);
    if (size < 0) return -1;
    unsigned char m[4] = {0xff, 0xff, 0xff, 0xff};
    int fd = nbr_open(path, 0);
    if (fd < 0) return -1;
    int n = nbr_read(fd, m, 4);
    nbr_close(fd);
    if (size < 40 || n != 4 || m[0] != 0 || m[1] != 'P' || m[2] != 'B' || m[3] != 'P') return -2;
    return 0;
}

int nbr_prepare_run(const char* game) {
    const char* folder = nbr_game_folder(game);
    if (!folder) return -1;
    game_file(g_action_path, sizeof g_action_path, folder, "EBOOT.PBP");
    int pc = nbr_pbp_check(g_action_path);
    if (pc == -1) return -2;
    if (pc < 0) return -3;
    char path[200], text[96], when[40];
    utc_text(nbr_wall_clock(), when, sizeof when);
    int n = nbr_fmt(text, sizeof text, "%s at %s\n", game, when);
    nbr_fmt(path, sizeof path, "%s/data/last_run", g.app_dir);
    int fd = nbr_open(path, 1);
    if (fd >= 0) { nbr_write(fd, text, n); nbr_close(fd); }
    return 0;
}

static int v_run(int argc, char** argv, nbr_buf* out) {
    int rc = argc > 1 ? nbr_prepare_run(argv[1]) : -1;
    if (rc == -1) { nbr_bputs(out, "usage: run <classic|modern|probe|remote>\n"); return 0; }
    if (rc == -2) { nbr_bprintf(out, "run: no %s/EBOOT.PBP\n", nbr_game_folder(argv[1])); return 0; }
    if (rc == -3) { nbr_bprintf(out, "run: %s/EBOOT.PBP isn't a PBP (rollback %s?)\n", nbr_game_folder(argv[1]), argv[1]); return 0; }
    g_action = NBR_ACT_LAUNCH;
    nbr_bprintf(out, "launching %s (%s)\n", argv[1], g_action_path);
    return 1;
}

static int parse_meta(const char* text, long long* bytes, char* sha) {
    const char* b = strstr(text, "bytes=");
    const char* s = strstr(text, "sha256=");
    if (!b || !s) return -1;
    long long v = 0;
    for (b += 6; *b >= '0' && *b <= '9'; b++) v = v * 10 + (*b - '0');
    *bytes = v;
    s += 7;
    for (int i = 0; i < 64; i++) {
        char c = (char)lower(s[i]);
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return -1;
        sha[i] = c;
    }
    sha[64] = 0;
    return 0;
}

static void say(const char* what) {
    if (g.status) g.status(what);
}

static int v_update(int argc, char** argv, nbr_buf* out) {
    if (argc < 5) { nbr_bputs(out, "usage: update <classic|modern|probe|remote> <file> <sha256> <size>\n"); return 0; }
    int gi = game_index(argv[1]);
    if (gi < 0) { nbr_bprintf(out, "update: unknown game %s\n", argv[1]); return 0; }
    const char* name = argv[2];
    if (!safe_name(name)) { nbr_bputs(out, "update: bad file name ([A-Za-z0-9._-])\n"); return 0; }
    char want[65];
    if (strlen(argv[3]) != 64) { nbr_bputs(out, "update: the sha256 must be 64 hex digits\n"); return 0; }
    for (int i = 0; i < 64; i++) {
        char c = (char)lower(argv[3][i]);
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { nbr_bputs(out, "update: the sha256 must be 64 hex digits\n"); return 0; }
        want[i] = c;
    }
    want[64] = 0;
    long long size;
    if (parse_ll(argv[4], &size) < 0 || size < 1 || size > FILE_MAX) { nbr_bputs(out, "update: size must be 1 to 20 MB\n"); return 0; }
    if (!g.chunk || g.chunk_len < 1024) { nbr_bputs(out, "update: no download buffer in this build\n"); return 0; }
    const char* folder = GAMES[gi].folder;
    char dir[200], eb[200], bak[200], tmp[200];
    nbr_fmt(dir, sizeof dir, "%s/%s", g.games_dir, folder);
    game_file(eb, sizeof eb, folder, "EBOOT.PBP");
    game_file(bak, sizeof bak, folder, "EBOOT.PBP.bak");
    game_file(tmp, sizeof tmp, folder, "EBOOT.PBP.tmp");
    if (!nbr_is_dir(dir)) nbr_mkdir(dir);
    if (!nbr_is_dir(dir)) { nbr_bprintf(out, "update: can't make %s\n", dir); return 0; }
    int low = nbr_battery_low();
    if (low >= 0) {
        nbr_bprintf(out, "update: refused: the battery is at %d%% and not on the charger (plug it in; a power cut mid-update can leave no EBOOT)\n", low);
        return 0;
    }
    long long free_b = nbr_free_bytes(dir);
    if (free_b >= 0 && free_b < size + 1024 * 1024) {
        nbr_bprintf(out, "update: not enough space on the stick: %lld KB free, %lld KB needed (the file plus 1 MB)\n", free_b / 1024,
                    (size + 1024 * 1024) / 1024);
        return 0;
    }
    /* the store must hold exactly what the command names */
    char mpath[120];
    nbr_http_res r;
    nbr_fmt(mpath, sizeof mpath, "/file/%s/meta", name);
    if (worker("GET", mpath, NULL, 0, g_small, sizeof g_small - 1, &r, 15000) < 0 || r.status != 200) {
        nbr_bprintf(out, "update: the store has no %s (status %d %s)\n", name, r.status, r.err);
        return 0;
    }
    long long mbytes;
    char msha[65];
    if (parse_meta(g_small, &mbytes, msha) < 0) { nbr_bputs(out, "update: unreadable meta from the store\n"); return 0; }
    if (mbytes != size || strcmp(msha, want) != 0) {
        nbr_bprintf(out, "update: refused: the store's %s is %lld bytes sha256 %.16s, the command said %lld bytes %.16s\n", name, mbytes,
                    msha, size, want);
        return 0;
    }
    nbr_log("update %s: %s, %lld bytes, sha256 %.16s, into %s", argv[1], name, size, want, tmp);
    uint64_t t0 = nbr_now_us();
    nbr_remove(tmp);
    int fd = nbr_open(tmp, 1);
    if (fd < 0) { nbr_bprintf(out, "update: can't write %s\n", tmp); return 0; }
    nbr_sha256 s;
    nbr_sha256_init(&s);
    long long off = 0;
    int pieces = 0, retries = 0;
    long long next_say = 0;
    while (off < size) {
        int len = size - off < g.chunk_len ? (int)(size - off) : g.chunk_len;
        char path[160];
        nbr_fmt(path, sizeof path, "/file/%s?off=%lld&len=%d", name, off, len);
        int ok = 0;
        for (int attempt = 0; attempt < 4 && !ok; attempt++) {
            if (attempt) { retries++; nbr_sleep_ms(1000 * attempt); }
            if (worker("GET", path, NULL, 0, g.chunk, g.chunk_cap, &r, 20000) == 0 && r.status == 200 && r.body_len == len) ok = 1;
            else nbr_log("update %s: piece at %lld: status %d, %d of %d bytes %s (try %d)", argv[1], off, r.status, r.body_len, len, r.err, attempt + 1);
        }
        if (!ok) {
            nbr_close(fd);
            nbr_remove(tmp);
            nbr_bprintf(out, "update: the piece at %lld failed 4 times (status %d %s); nothing changed\n", off, r.status, r.err);
            return 0;
        }
        if (nbr_write(fd, g.chunk, len) != len) {
            nbr_close(fd);
            nbr_remove(tmp);
            nbr_bprintf(out, "update: writing %s failed at %lld (stick full?); nothing changed\n", tmp, off);
            return 0;
        }
        nbr_sha256_update(&s, g.chunk, (size_t)len);
        off += len;
        pieces++;
        if (off >= next_say || off == size) {
            char line[80];
            nbr_fmt(line, sizeof line, "update %s: %lld of %lld KB", argv[1], off / 1024, size / 1024);
            say(line);
            nbr_log("%s", line);
            next_say = off + 1024 * 1024;
        }
    }
    nbr_close(fd);
    uint8_t d[32];
    char got[65];
    nbr_sha256_final(&s, d);
    nbr_hex(d, 32, got);
    if (strcmp(got, want) != 0) {
        nbr_remove(tmp);
        nbr_log("update %s: REFUSED: downloaded sha256 %.16s, wanted %.16s", argv[1], got, want);
        nbr_bprintf(out, "update: refused: what arrived hashes to %.16s, not %.16s; nothing changed\n", got, want);
        return 0;
    }
    /* read the copy back off the stick before it replaces anything */
    long long back = -1;
    if (hash_file(tmp, got, &back) < 0 || back != size || strcmp(got, want) != 0) {
        nbr_remove(tmp);
        nbr_log("update %s: REFUSED: the copy on the stick reads back as %lld bytes %.16s", argv[1], back, got);
        nbr_bprintf(out, "update: refused: the copy on the stick reads back as %lld bytes sha256 %.16s; nothing changed\n", back, got);
        return 0;
    }
    if (nbr_pbp_check(tmp) < 0) {
        nbr_remove(tmp);
        nbr_bprintf(out, "update: refused: %s isn't a PBP (no \\0PBP header); nothing changed\n", name);
        return 0;
    }
    int had = nbr_file_size(eb) >= 0;
    if (had) {
        nbr_remove(bak);
        if (nbr_rename(eb, bak) != 0) {
            nbr_remove(tmp);
            nbr_bprintf(out, "update: can't move the old EBOOT.PBP aside; nothing changed\n");
            return 0;
        }
    }
    if (nbr_rename(tmp, eb) != 0) {
        if (had) nbr_rename(bak, eb);
        nbr_bprintf(out, "update: can't rename the new EBOOT into place; the old one is back\n");
        return 0;
    }
    forget_hash(gi);
    int ms = (int)((nbr_now_us() - t0) / 1000);
    nbr_log("update %s: done, %lld bytes in %d ms, %d pieces, %d retries", argv[1], size, ms, pieces, retries);
    nbr_bprintf(out, "updated %s: %s/EBOOT.PBP is %lld bytes sha256 %.16s (verified after writing), %d ms, %d pieces, %d retries; %s\n",
                argv[1], folder, size, want, ms, pieces, retries, had ? "the previous one is EBOOT.PBP.bak" : "there was no previous one");
    return 1;
}

static int v_rollback(int argc, char** argv, nbr_buf* out) {
    int gi = argc > 1 ? game_index(argv[1]) : -1;
    if (gi < 0) { nbr_bputs(out, "usage: rollback <classic|modern|probe|remote>\n"); return 0; }
    char eb[200], bak[200], old[200];
    game_file(eb, sizeof eb, GAMES[gi].folder, "EBOOT.PBP");
    game_file(bak, sizeof bak, GAMES[gi].folder, "EBOOT.PBP.bak");
    game_file(old, sizeof old, GAMES[gi].folder, "EBOOT.PBP.old");
    long long bsize = nbr_file_size(bak);
    if (bsize < 0) { nbr_bprintf(out, "rollback: %s has no EBOOT.PBP.bak\n", GAMES[gi].folder); return 0; }
    nbr_remove(old);
    int had = nbr_file_size(eb) >= 0;
    if (had && nbr_rename(eb, old) != 0) { nbr_bputs(out, "rollback: can't move EBOOT.PBP aside; nothing changed\n"); return 0; }
    if (nbr_rename(bak, eb) != 0) {
        if (had) nbr_rename(old, eb);
        nbr_bputs(out, "rollback: can't rename EBOOT.PBP.bak; nothing changed\n");
        return 0;
    }
    if (had) nbr_rename(old, bak);
    forget_hash(gi);
    nbr_log("rollback %s: EBOOT.PBP is the previous one (%lld bytes)", argv[1], bsize);
    nbr_bprintf(out, "rolled back %s: EBOOT.PBP is now the previous %lld bytes sha256 %.16s; %s\n", argv[1], bsize, eboot_hash(gi, bsize),
                had ? "the one it replaced is the new EBOOT.PBP.bak" : "there was no EBOOT.PBP before");
    return 1;
}

static int read_tail(const char* path, long long want, nbr_buf* out, int whole_only) {
    long long size = nbr_file_size(path);
    if (size < 0) return -1;
    int room = out->cap - 1 - out->len;
    if (whole_only && size > room) return -2;
    if (want > room) want = room;
    if (want > size) want = size;
    int fd = nbr_open(path, 0);
    if (fd < 0) return -1;
    nbr_seek(fd, size - want, 0);
    int got = 0;
    while (got < want) {
        int n = nbr_read(fd, out->p + out->len + got, (int)(want - got));
        if (n <= 0) break;
        got += n;
    }
    nbr_close(fd);
    out->len += got;
    out->p[out->len] = 0;
    return 0;
}

static int v_log(int argc, char** argv, nbr_buf* out) {
    if (argc < 3) { nbr_bputs(out, "usage: log <name> <classic|modern|probe|remote|plugin> [bytes]\n"); return 0; }
    char path[200];
    if (strcmp(argv[2], "remote") == 0) nbr_fmt(path, sizeof path, "%s/logs/%s", g.app_dir, g.log_name ? g.log_name : "remote.log");
    else if (strcmp(argv[2], "plugin") == 0) nbr_fmt(path, sizeof path, "%s/logs/plugin.log", g.app_dir);
    else {
        const char* folder = nbr_game_folder(argv[2]);
        if (!folder) { nbr_bprintf(out, "log: unknown game %s\n", argv[2]); return 0; }
        game_file(path, sizeof path, folder, "logs/blitz.log");
    }
    long long want = 256 * 1024;
    if (argc > 3 && parse_ll(argv[3], &want) < 0) want = 256 * 1024;
    if (read_tail(path, want, out, 0) < 0) { nbr_bprintf(out, "log: can't read %s\n", path); return 0; }
    return 1;
}

typedef struct { nbr_buf* out; int filter; int count; } ls_ctx;
static void ls_one(const char* name, int is_dir, long long size, void* vctx) {
    ls_ctx* c = (ls_ctx*)vctx;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return;
    if (c->filter) {
        char up[16];
        int i = 0;
        while (name[i] && i < 15) { up[i] = (char)upper(name[i]); i++; }
        up[i] = 0;
        if (!(strncmp(up, "NB", 2) == 0 || strcmp(up, "NEWBLITZ") == 0)) return;
    }
    c->count++;
    if (is_dir) nbr_bprintf(c->out, "%10s  %s/\n", "-", name);
    else nbr_bprintf(c->out, "%10lld  %s\n", size, name);
}

static int v_ls(int argc, char** argv, nbr_buf* out) {
    char path[200];
    ls_ctx c = {out, 0, 0};
    if (argc < 3 || strcmp(argv[2], "/") == 0) { nbr_fmt(path, sizeof path, "%s", g.games_dir); c.filter = 1; }
    else if (game_path(argv[2], path, sizeof path) < 0) { nbr_bputs(out, "ls: only under ms0:/PSP/GAME/NB* or NEWBLITZ\n"); return 0; }
    if (nbr_list_dir(path, ls_one, &c) != 0) { nbr_bprintf(out, "ls: can't open %s\n", path); return 0; }
    return 1;
}

static int v_get(int argc, char** argv, nbr_buf* out) {
    char path[200];
    if (argc < 3 || game_path(argv[2], path, sizeof path) < 0) { nbr_bputs(out, "usage: get <name> <file under NB*/ or NEWBLITZ/> (never net.cfg)\n"); return 0; }
    int r = read_tail(path, 0x7fffffff, out, 1);
    if (r == -2) { nbr_bprintf(out, "get: %s is %lld bytes, more than this build sends (%d)\n", path, nbr_file_size(path), out->cap - 1); return 0; }
    if (r < 0) { nbr_bprintf(out, "get: can't read %s\n", path); return 0; }
    return 1;
}

static int v_sendlog(int argc, char** argv, nbr_buf* out) {
    (void)argc; (void)argv;
    char path[200], up[160];
    nbr_fmt(path, sizeof path, "%s/logs/%s", g.app_dir, g.log_name ? g.log_name : "remote.log");
    /* the log goes up from the big buffer; out is that buffer, so the reply is written after */
    out->len = 0;
    if (read_tail(path, out->cap - 256, out, 0) < 0 || out->len == 0) { out->len = 0; nbr_bputs(out, "sendlog: no log yet\n"); return 0; }
    const char* dev = cfg_raw("device");
    nbr_fmt(up, sizeof up, "/up?device=%s-remote&kind=logs", *dev && safe_name(dev) ? dev : "psp1001");
    nbr_http_res r;
    int len = out->len;
    int rc = worker("POST", up, out->p, len, g_small, sizeof g_small - 1, &r, 20000);
    out->len = 0;
    out->p[0] = 0;
    if (rc < 0 || r.status != 200) { nbr_bprintf(out, "sendlog: upload failed: status %d %s\n", r.status, r.err); return 0; }
    g_count.uploads++;
    while (r.body_len && (g_small[r.body_len - 1] == '\n' || g_small[r.body_len - 1] == '\r')) g_small[--r.body_len] = 0;
    nbr_bprintf(out, "sendlog: %d bytes uploaded as %s\n", len, g_small);
    return 1;
}

void nbr_mark_xmb_hold(void) {
    char path[200];
    nbr_fmt(path, sizeof path, "%s/data/xmb_hold", g.app_dir);
    int fd = nbr_open(path, 1);
    if (fd >= 0) { nbr_write(fd, "1\n", 2); nbr_close(fd); }
}

static int v_menu(int argc, char** argv, nbr_buf* out) {
    (void)argc; (void)argv;
    if (!g.can_menu) { nbr_bputs(out, "menu: already in the XMB\n"); return 1; }
    nbr_mark_xmb_hold();
    g_action = NBR_ACT_MENU;
    nbr_bputs(out, "back to the XMB (data/xmb_hold set: the plugin leaves the XMB alone once)\n");
    return 1;
}

static const struct { const char* name; verb_fn fn; int named; } VERBS[] = {
    {"ping", v_ping, 0}, {"stat", v_stat, 0}, {"run", v_run, 0}, {"update", v_update, 0}, {"rollback", v_rollback, 0},
    {"log", v_log, 1}, {"ls", v_ls, 1}, {"get", v_get, 1}, {"sendlog", v_sendlog, 0}, {"menu", v_menu, 0}, {"help", v_help, 0}};
#define NVERBS ((int)(sizeof VERBS / sizeof VERBS[0]))

/* ---- the signed line ---------------------------------------------------------------------------------------------------- */
/* "<seq> <verb args> #<tag>" -> the verb args (in place), or NULL and why */
static char* verify(char* line, const char** why) {
    int n = (int)strlen(line);
    while (n && (line[n - 1] == ' ' || line[n - 1] == '\r' || line[n - 1] == '\t')) line[--n] = 0;
    while (*line == ' ' || *line == '\t') { line++; n--; }
    char* hash = NULL;
    for (int i = n - 2; i >= 0; i--) if (line[i] == ' ' && line[i + 1] == '#') { hash = line + i; break; }
    char* sp = strchr(line, ' ');
    if (!hash || !sp || sp >= hash) { *why = "unsigned"; return NULL; }
    *sp = 0;
    *hash = 0;
    const char* seq = line;
    char* body = sp + 1;
    while (*body == ' ') body++;
    char* be = hash;
    while (be > body && be[-1] == ' ') *--be = 0;
    const char* tag = hash + 2;
    long long v;
    if (parse_ll(seq, &v) < 0) { *why = "bad seq"; return NULL; }
    if (!g_key[0]) { *why = "no cmd_key in net.cfg"; return NULL; }
    static char msg[600];
    int ml = nbr_fmt(msg, sizeof msg, "%s|%s", seq, body);
    if (ml >= (int)sizeof msg) { *why = "too long"; return NULL; }
    uint8_t d[32];
    char hex[65];
    nbr_hmac_sha256(g_key, strlen(g_key), msg, (size_t)ml, d);
    nbr_hex(d, 32, hex);
    if (strlen(tag) != 16 || strncmp(hex, tag, 16) != 0) { *why = "bad tag"; return NULL; }
    if (v <= g_last_seq) { *why = "replayed or old seq"; return NULL; }
    g_last_seq = v;
    save_seq();
    return body;
}

static int run_command(char* cmd) {
    char* argv[12];
    int argc = 0;
    static char copy[600];
    nbr_fmt(copy, sizeof copy, "%s", cmd);
    char* p = copy;
    while (*p && argc < 12) {
        while (*p == ' ' || *p == '\t') *p++ = 0;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    nbr_buf out;
    nbr_buf_init(&out, g.big, g.big_cap);
    const char* res = "error";
    int ok = 0;
    g_action = NBR_ACT_NONE;
    if (argc == 0) nbr_bputs(&out, "empty command\n");
    else {
        int i;
        for (i = 0; i < NVERBS; i++) if (strcmp(VERBS[i].name, argv[0]) == 0) break;
        if (i == NVERBS) nbr_bprintf(&out, "unknown verb: %s (help lists them)\n", argv[0]);
        else if (VERBS[i].named && (argc < 2 || !safe_name(argv[1])))
            nbr_bprintf(&out, "%s: the first argument is the result's name ([A-Za-z0-9._-])\n", argv[0]);
        else {
            char doing[80];
            nbr_fmt(doing, sizeof doing, "%.70s", cmd);
            say(doing);
            ok = VERBS[i].fn(argc, argv, &out);
            res = !ok ? "error" : VERBS[i].named ? argv[1] : argv[0];
        }
    }
    if (out.overflow) nbr_log("%s: the reply was cut at %d bytes", argc ? argv[0] : "?", out.len);
    if (!ok) g_action = NBR_ACT_NONE;
    int posted = post_result(res, out.p, out.len);
    nbr_fmt(g_count.last_cmd, sizeof g_count.last_cmd, "%.60s", cmd);
    nbr_fmt(g_count.last_result, sizeof g_count.last_result, "%s%.*s", ok ? "" : "FAILED: ", 80,
            ok ? (out.len ? out.p : "") : out.p);
    for (char* c = g_count.last_result; *c; c++) if (*c == '\n') { *c = 0; break; }
    nbr_log("%s -> res/psp-%s (%d bytes)%s%s", cmd, res, out.len, ok ? "" : " FAILED", posted == 0 ? "" : " (the reply didn't post)");
    if (!ok) nbr_log("  %.200s", out.p);
    say(ok ? "ready" : "ready (last command failed)");
    return g_action;
}

int nbr_handle_line(const char* raw) {
    static char line[700];
    nbr_fmt(line, sizeof line, "%s", raw);
    const char* why = "";
    char* cmd = verify(line, &why);
    if (!cmd) {
        g_count.rejected++;
        /* the line itself isn't logged: a bad line is not ours, and could be anything */
        nbr_log("refused a command line (%s)", why);
        return NBR_ACT_NONE;
    }
    g_count.cmds++;
    return run_command(cmd);
}

/* ---- poller ------------------------------------------------------------------------------------------------------------- */
int nbr_init(const nbr_opts* o) {
    g = *o;
    g_t0 = nbr_now_us();
    for (int i = 0; i < 8; i++) forget_hash(i);
    char dir[200];
    nbr_fmt(dir, sizeof dir, "%s/logs", g.app_dir);
    nbr_mkdir(dir);
    nbr_fmt(dir, sizeof dir, "%s/data", g.app_dir);
    nbr_mkdir(dir);
    nbr_fmt(g_pid, sizeof g_pid, "psp-remote-%u", (unsigned)(nbr_low_digits() % 10000000u));
    load_cfg();
    load_seq();
    nbr_fmt(g_base, sizeof g_base, "%s", cfg_raw("logs_base"));
    int bl = (int)strlen(g_base);
    while (bl && g_base[bl - 1] == '/') g_base[--bl] = 0;
    nbr_fmt(g_key, sizeof g_key, "%s", cfg_raw("cmd_key"));
    nbr_log("NB Remote (%s) build %s starting: pid %s, net.cfg %s, logs_base %s, cmd_key %s, last seq %lld", g.form,
            g.build ? g.build : "?", g_pid, g_ncfg ? "present" : "missing", g_base[0] ? "set" : "none", g_key[0] ? "set" : "none",
            g_last_seq);
    g_next_poll = 0;
    return g_base[0] && g_key[0] && strncmp(g_base, "http://", 7) == 0;
}

const char* nbr_pid(void) { return g_pid; }
nbr_counters nbr_get_counters(void) { return g_count; }
const char* nbr_action_path(void) { return g_action_path; }

int nbr_step(void) {
    if (!g_base[0] || !g_key[0] || !nbr_net_up()) return NBR_ACT_NONE;
    uint64_t now = nbr_now_us();
    if (now < g_next_poll) return NBR_ACT_NONE;
    char path[80];
    nbr_fmt(path, sizeof path, "/cmd?pid=%s", g_pid);
    nbr_http_res r;
    g_count.polls++;
    int rc = worker("GET", path, NULL, 0, g_poll, sizeof g_poll - 1, &r, 10000);
    now = nbr_now_us();
    uint64_t idle = (uint64_t)nbr_cfg_int("poll_s", 10) * 1000000ull;   /* net.cfg's poll_s: 10 s unless a test says less */
    if (idle < 1000000ull) idle = 1000000ull;
    g_next_poll = now + (now < g_fast_until ? (idle < POLL_FAST_US ? idle : POLL_FAST_US) : idle);
    if (rc < 0 || r.status != 200) {
        g_count.poll_fail++;
        if (g_count.poll_fail < 5 || g_count.poll_fail % 30 == 0) nbr_log("cmd poll failed: status %d %s", r.status, r.err);
        return NBR_ACT_NONE;
    }
    if (g_count.polls == 1) nbr_log("first poll answered in %d ms", r.ms);
    int action = NBR_ACT_NONE;
    char* p = g_poll;
    while (*p && action == NBR_ACT_NONE) {
        char* e = p;
        while (*e && *e != '\n') e++;
        char* next = *e ? e + 1 : e;
        *e = 0;
        if (*p) {
            g_fast_until = nbr_now_us() + FAST_FOR_US;
            g_next_poll = nbr_now_us() + POLL_FAST_US;
            action = nbr_handle_line(p);
        }
        p = next;
    }
    if (action != NBR_ACT_NONE && *p) nbr_log("lines after a run or menu command are dropped (send them again)");
    return action;
}

void nbr_do_launch(void) {
    nbr_log("launching %s", g_action_path);
    int rc = nbr_launch(g_action_path);
    nbr_log("launch of %s failed: 0x%08X", g_action_path, (unsigned)rc);
}
