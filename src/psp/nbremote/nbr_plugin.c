/* NB Remote, Form B: the ARK VSH plugin (docs/PSP_PORT.md "NB Remote"). Off unless the team adds its PLUGINS.TXT line; it only reads
 * the Memory Stick and starts one EBOOT, and it fails soft: anything odd is logged to PSP/GAME/NBREMOTE/logs/plugin.log and the
 * XMB is left alone.
 *
 * nbremote.prx (the one to enable): the always-there launcher. ~15 s after the XMB comes up it starts NB Remote
 * (PSP/GAME/NBREMOTE/EBOOT.PBP), which does the Wi-Fi; the games come back to NB Remote when they exit (home=remote), so the
 * PSP stays reachable for as long as it is on. It is for a PSP nobody is holding, so it stays in the XMB instead when
 *   - any button is pressed (or held) during the countdown: someone is using the XMB (entering USB mode, a dialog, starting
 *     a game), and a launch then could pull the XMB out from under them,
 *   - NB Remote left the XMB-hold marker (data/xmb_hold: its START button, HOME > Exit, or the menu command), once,
 *   - it has started NB Remote 3 times in the last 5 minutes (a crash loop), or the RTC can't be read (no guard, no start),
 *   - NBREMOTE/EBOOT.PBP is missing or isn't a PBP,
 *   - net.cfg says plugin_autostart=off, or plugin_dry_run=1 (then it only logs what it would have done).
 * No Wi-Fi of its own: the XMB owns the PSP's net stack (its Network column loads and terms it), and no ARK or PRO VSH plugin we
 * found does background Wi-Fi in the XMB (ARK's FTP server lives in its Custom Launcher app), so this build imports no net
 * library at all (NBR_NO_NET).
 *
 * nbremote_net.prx (experimental, NBR_PLUGIN_NET=1): the same launcher, plus, when it stays in the XMB and net.cfg says
 * plugin_net=on, the whole command core polling from the XMB (it tries sceUtilityLoadNetModule and sceNetInit there; if the
 * libraries aren't there or refuse, it logs that and stops). Untested on hardware; not for everyday use.
 *
 * Settings (PSP/GAME/NBREMOTE/data/net.cfg, written by make psp-deploy): plugin_autostart=on|off, plugin_delay_s=15,
 * plugin_dry_run=0|1, plugin_net=off|on (the net build only). */
#include "nbr.h"
#include <string.h>
#if NBR_HOST
/* the Mac's dry run of the decision logic (make psp-remote-test): NBR_MS0=<folder> stands for ms0:, NBR_HELD=1 holds R */
#include <stdlib.h>
typedef unsigned SceSize;
typedef int SceUID;
typedef struct { unsigned TimeStamp, Buttons; unsigned char Lx, Ly, Rsrv[6]; } SceCtrlData;
#define PSP_CTRL_RTRIGGER 0x200
static int sceCtrlPeekBufferPositive(SceCtrlData* d, int n) { (void)n; d->Buttons = getenv("NBR_HELD") ? PSP_CTRL_RTRIGGER : 0; return 1; }
#define NBR_SETTLE_MS 0
static const char* ms0(void) { const char* r = getenv("NBR_MS0"); return r ? r : "."; }
#else
#include <pspctrl.h>
#include <pspkernel.h>
static const char* ms0(void) { return "ms0:"; }
/* BRANDING: PSP module name */
PSP_MODULE_INFO("NBRemotePlugin", PSP_MODULE_USER, 1, 0);
#endif
#if NBR_PLUGIN_NET
#include "nbr_net.h"
#endif

#ifndef NBR_BUILD
#define NBR_BUILD "0 unknown"
#endif
#ifndef NBR_SETTLE_MS
#define NBR_SETTLE_MS 2000
#endif
/* the buttons a user-mode read reports: SELECT START, the D-pad, L R, TRIANGLE CIRCLE CROSS SQUARE (not HOLD, HOME and the
 * other kernel-only bits) */
#define NBR_ANY_BUTTON 0xF3F9u

#if NBR_PLUGIN_NET
static char g_big[32 * 1024];
static char g_chunk[64 * 1024 + 4096];
#else
static char g_big[4096];
#endif
static volatile int g_stop;
static char g_app[160], g_games[160];

static int file_exists(const char* path) { return nbr_file_size(path) >= 0; }
static const char* app_file(const char* rel) {
    static char path[2][200];
    static int k;
    k ^= 1;
    nbr_fmt(path[k], sizeof path[k], "%s/%s", g_app, rel);
    return path[k];
}

/* data/plugin_launches: the RTC second of each start; 3 in 5 minutes and it stops starting NB Remote */
static int launch_guard(void) {
    static char text[512], kept[512];
    const char* path = app_file("data/plugin_launches");
    long long now = nbr_wall_clock();
    if (now <= 0) return -2;   /* no clock, no guard: don't start at all */
    int n = 0, fd = nbr_open(path, 0);
    if (fd >= 0) { n = nbr_read(fd, text, sizeof text - 1); nbr_close(fd); }
    if (n < 0) n = 0;
    text[n] = 0;
    nbr_buf k;
    nbr_buf_init(&k, kept, sizeof kept);
    int recent = 0;
    for (char* p = text; *p;) {
        char* e = p;
        while (*e && *e != '\n') e++;
        long long v = 0;
        for (char* c = p; c < e && *c >= '0' && *c <= '9'; c++) v = v * 10 + (*c - '0');
        if (v > 0 && now > 0 && now - v < 300 && now >= v) { recent++; nbr_bprintf(&k, "%lld\n", v); }
        p = *e ? e + 1 : e;
    }
    if (recent >= 3) return -1;
    nbr_bprintf(&k, "%lld\n", now);
    fd = nbr_open(path, 1);
    if (fd >= 0) { nbr_write(fd, kept, k.len); nbr_close(fd); }
    return recent;
}

#if NBR_PLUGIN_NET
static void net_mode(void) {
    if (strcmp(nbr_cfg("plugin_net"), "on") != 0) return;
    nbr_log("plugin_net=on: EXPERIMENTAL: polling the Worker from the XMB");
    nbr_net_start(nbr_cfg_int("wifi_profile", 1), 1);
    while (!g_stop) {
        int st = nbr_net_state();
        if (st == NBR_NET_UNAVAILABLE) { nbr_log("no Wi-Fi from the XMB: the net mode stops (the launcher part is unaffected)"); return; }
        int a = nbr_step();
        if (a == NBR_ACT_LAUNCH) nbr_do_launch();
        nbr_sleep_ms(250);
    }
    nbr_net_stop();
}
#endif

int nbr_plugin_thread(SceSize args, void* argp) {
    (void)args; (void)argp;
    nbr_fmt(g_games, sizeof g_games, "%s/PSP/GAME", ms0());
    nbr_fmt(g_app, sizeof g_app, "%s/NBREMOTE", g_games);
    nbr_opts o;
    memset(&o, 0, sizeof o);
    o.app_dir = g_app;
    o.games_dir = g_games;
    o.form = "plugin";
    o.build = NBR_BUILD;
    o.log_name = "plugin.log";
    o.big = g_big;
    o.big_cap = sizeof g_big;
#if NBR_PLUGIN_NET
    o.chunk = g_chunk;
    o.chunk_cap = sizeof g_chunk;
    o.chunk_len = 64 * 1024;
#endif
    o.can_menu = 0;
    /* ARK starts VSH plugins while the XMB is still booting (before sceMediaSync): let it finish before touching the stick */
    for (int i = 0; i < NBR_SETTLE_MS / 250 && !g_stop; i++) nbr_sleep_ms(250);
    if (g_stop || !nbr_is_dir(g_app)) return 0;   /* no NB Remote on this stick: nothing to do, nothing written */
    nbr_init(&o);
    int delay = nbr_cfg_int("plugin_delay_s", 15);
    if (delay < 3) delay = 3;
    if (delay > 120) delay = 120;
    const char* autostart = nbr_cfg("plugin_autostart");
    int dry = nbr_cfg_int("plugin_dry_run", 0);
    nbr_log("plugin (%s build) in the XMB: autostart %s, delay %d s%s", NBR_PLUGIN_NET ? "net" : "launcher", *autostart ? autostart : "(unset: off)",
            delay, dry ? ", DRY RUN" : "");
    int launch = strcmp(autostart, "on") == 0;
    /* the marker counts for this XMB visit whatever happens next, so it's used up first (a button press below won't leave it
     * behind to keep a later, unattended boot in the XMB) */
    if (launch && file_exists(app_file("data/xmb_hold"))) {
        nbr_remove(app_file("data/xmb_hold"));
        nbr_log("NB Remote asked for the XMB (data/xmb_hold): staying this once");
        launch = 0;
    }
    for (int i = 0; launch && i < delay * 4 && !g_stop; i++) {
        nbr_sleep_ms(250);
        SceCtrlData pad;
        memset(&pad, 0, sizeof pad);
        if (sceCtrlPeekBufferPositive(&pad, 1) > 0 && (pad.Buttons & NBR_ANY_BUTTON)) {
            nbr_log("a button was pressed (0x%04X): someone is using the XMB, staying", (unsigned)(pad.Buttons & NBR_ANY_BUTTON));
            launch = 0;
        }
    }
    if (g_stop) return 0;
    if (launch && nbr_pbp_check(app_file("EBOOT.PBP")) < 0) { nbr_log("NBREMOTE/EBOOT.PBP is missing or not a PBP: staying"); launch = 0; }
    if (launch) {
        int recent = launch_guard();
        if (recent == -2) { nbr_log("the RTC can't be read, so the crash-loop guard can't work: staying in the XMB"); launch = 0; }
        else if (recent < 0) { nbr_log("NB Remote was started 3 times in the last 5 minutes: staying in the XMB (a crash loop?)"); launch = 0; }
    }
    if (launch && dry) { nbr_log("dry run: would start %s now", app_file("EBOOT.PBP")); launch = 0; }
    if (launch) {
        nbr_prepare_run("remote");
        nbr_do_launch();   /* returns only if the launch failed (logged) */
    }
#if NBR_PLUGIN_NET
    net_mode();
#endif
    return 0;
}

#if !NBR_HOST
static SceUID g_thread = -1;

int module_start(SceSize args, void* argp) {
    (void)args; (void)argp;
    /* low priority, small stack: the XMB comes first */
    g_thread = sceKernelCreateThread("nbremote_plugin", nbr_plugin_thread, 0x70, NBR_PLUGIN_NET ? 48 * 1024 : 16 * 1024, PSP_THREAD_ATTR_USER, NULL);
    if (g_thread >= 0) sceKernelStartThread(g_thread, args, argp);
    return 0;
}

int module_stop(SceSize args, void* argp) {
    (void)args; (void)argp;
    g_stop = 1;
    return 0;
}
#endif
