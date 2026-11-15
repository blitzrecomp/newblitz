/* NB Remote, Form A: the "New Blitz Remote" EBOOT (PSP/GAME/NBREMOTE; docs/PSP_PORT.md "NB Remote"). User mode. It brings up
 * Wi-Fi, polls the logs Worker for signed commands (nbr_core.c) and shows what it's doing on the debug screen; it runs until a
 * command (or a button) starts a game, and the games come back to it when they exit (net.cfg home=remote).
 *   X  New Blitz Classic   TRIANGLE  New Blitz   SQUARE  the probe   START  back to the XMB (and stay there)
 * The system is kept awake (scePowerTick, suspend only: the screen may still dim and go dark) so Wi-Fi stays up. */
#include "nbr.h"
#include "nbr_net.h"
#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspkernel.h>
#include <psppower.h>
#include <stdio.h>
#include <string.h>

/* BRANDING: PSP module name */
PSP_MODULE_INFO("NBRemote", PSP_MODULE_USER, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(1024);

#ifndef NBR_BUILD
#define NBR_BUILD "0 unknown"
#endif

extern void (*nbr_console_fn)(const char* line);

static char g_big[1024 * 1024];
static char g_chunk[256 * 1024 + 4096];
static char g_app[160], g_games[160];
static char g_doing[96] = "starting";
static volatile int g_leaving;
static int g_has_worker;

static void console(const char* line) { printf("%s\n", line); }
static void status(const char* what) { nbr_fmt(g_doing, sizeof g_doing, "%.90s", what); }

static void leave_to_xmb(const char* why) {
    g_leaving = 1;
    nbr_mark_xmb_hold();
    nbr_log("back to the XMB (%s)", why);
    sceKernelExitGame();
}

static int exit_callback(int a, int b, void* c) {
    (void)a; (void)b; (void)c;
    leave_to_xmb("HOME");
    return 0;
}

static int callback_thread(SceSize args, void* argp) {
    (void)args; (void)argp;
    int cb = sceKernelCreateCallback("nbr_exit", exit_callback, NULL);
    sceKernelRegisterExitCallback(cb);
    sceKernelSleepThreadCB();
    return 0;
}

static void launch(const char* game) {
    int rc = nbr_prepare_run(game);
    if (rc < 0) { status(rc == -2 ? "that game isn't on the stick" : rc == -3 ? "that EBOOT isn't a PBP (rollback?)" : "unknown game"); return; }
    g_leaving = 1;
    status("launching");
    nbr_do_launch();
    g_leaving = 0;
    status("the launch failed (see logs/remote.log)");
}

static int poll_thread(SceSize args, void* argp) {
    (void)args; (void)argp;
    for (;;) {
        if (!g_leaving) {
            int a = nbr_step();
            if (a == NBR_ACT_LAUNCH) {
                g_leaving = 1;
                status("launching");
                nbr_do_launch();
                g_leaving = 0;
                status("the launch failed (see logs/remote.log)");
            } else if (a == NBR_ACT_MENU) {
                nbr_sleep_ms(300);
                leave_to_xmb("menu command");
            }
        }
        nbr_sleep_ms(200);
    }
    return 0;
}

static void line(int y, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void line(int y, const char* fmt, ...) {
    char text[80];
    va_list ap;
    va_start(ap, fmt);
    int n = nbr_vfmt(text, 69, fmt, ap);
    va_end(ap);
    if (n > 68) n = 68;
    while (n < 68) text[n++] = ' ';
    text[68] = 0;
    pspDebugScreenSetXY(0, y);
    pspDebugScreenPuts(text);
}

static void draw(void) {
    nbr_counters c = nbr_get_counters();
    char up[24];
    nbr_uptime_text(up, sizeof up);
    int pct = scePowerGetBatteryLifePercent();
    pspDebugScreenSetTextColor(0xFF40C0FF);
    /* BRANDING: NB Remote screen title */
    line(1, " New Blitz Remote                                  build %s", NBR_BUILD);
    pspDebugScreenSetTextColor(0xFFFFFFFF);
    line(2, " ------------------------------------------------------------------");
    if (nbr_net_state() == NBR_NET_UP) line(4, " Wi-Fi     up  %s  \"%s\"  (profile %d)", nbr_net_ip(), nbr_net_ssid(), nbr_net_slot());
    else line(4, " Wi-Fi     %s", nbr_net_state_name());
    if (g_has_worker) line(5, " Worker    %s  polls %d (failed %d)", nbr_pid(), c.polls, c.poll_fail);
    else line(5, " Worker    off: no data/net.cfg with logs_base and cmd_key (make psp-deploy)");
    line(6, " Commands  %d run, %d refused", c.cmds, c.rejected);
    line(7, " Last      %s", c.last_cmd[0] ? c.last_cmd : "-");
    line(8, " Result    %.56s", c.last_result[0] ? c.last_result : "-");
    line(9, " Doing     %s", g_doing);
    if (pct >= 0) line(10, " Battery   %d%%%s", pct, scePowerIsBatteryCharging() == 1 ? " charging" : scePowerIsPowerOnline() == 1 ? " on the charger" : "");
    else line(10, " Battery   none%s", scePowerIsPowerOnline() == 1 ? " (on the charger)" : "");
    line(11, " Up        %s s", up);
    line(13, " ------------------------------------------------------------------");
    line(14, " X  New Blitz Classic     TRIANGLE  New Blitz     SQUARE  Probe");
    line(15, " START  back to the XMB (and stay there)");
}

static void find_dirs(int argc, char** argv) {
    /* argv[0]: ms0:/PSP/GAME/NBREMOTE/EBOOT.PBP (PPSSPP: ms0:/PSP/GAME/NBREMOTE//EBOOT.PBP) */
    nbr_fmt(g_app, sizeof g_app, "%s", argc > 0 && argv[0] ? argv[0] : "ms0:/PSP/GAME/NBREMOTE/EBOOT.PBP");
    char* slash = strrchr(g_app, '/');
    if (slash) *slash = 0;
    int n = (int)strlen(g_app);
    while (n > 0 && g_app[n - 1] == '/') g_app[--n] = 0;
    if (!strchr(g_app, '/')) nbr_fmt(g_app, sizeof g_app, "ms0:/PSP/GAME/NBREMOTE");
    nbr_fmt(g_games, sizeof g_games, "%s", g_app);
    slash = strrchr(g_games, '/');
    if (slash) *slash = 0;
}

int main(int argc, char** argv) {
    nbr_console_fn = console;
    find_dirs(argc, argv);
    int cbt = sceKernelCreateThread("nbr_callbacks", callback_thread, 0x11, 4096, PSP_THREAD_ATTR_USER, NULL);
    if (cbt >= 0) sceKernelStartThread(cbt, 0, NULL);
    pspDebugScreenInit();
    pspDebugScreenClear();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);

    nbr_opts o;
    memset(&o, 0, sizeof o);
    o.app_dir = g_app;
    o.games_dir = g_games;
    o.form = "eboot";
    o.build = NBR_BUILD;
    o.log_name = "remote.log";
    o.big = g_big;
    o.big_cap = sizeof g_big;
    o.chunk = g_chunk;
    o.chunk_cap = sizeof g_chunk;
    o.chunk_len = 256 * 1024;
    o.can_menu = 1;
    o.status = status;
    g_has_worker = nbr_init(&o);
    nbr_log("app %s, games %s", g_app, g_games);
    nbr_net_start(nbr_cfg_int("wifi_profile", 1), 1);
    int pt = sceKernelCreateThread("nbr_poll", poll_thread, 0x30, 64 * 1024, PSP_THREAD_ATTR_USER, NULL);
    if (pt >= 0) sceKernelStartThread(pt, 0, NULL);
    status(g_has_worker ? "ready" : "no Worker channel");

    /* a button already down when NB Remote starts (the × that confirmed a game's HOME > Exit, still held through the launch)
     * doesn't count: only a press after it is let go, or a held × would start Classic again straight away */
    unsigned prev;
    {
        SceCtrlData pad;
        memset(&pad, 0, sizeof pad);
        sceCtrlPeekBufferPositive(&pad, 1);
        prev = pad.Buttons;
    }
    uint64_t next_tick = 0, next_draw = 0;
    for (;;) {
        SceCtrlData pad;
        memset(&pad, 0, sizeof pad);
        sceCtrlPeekBufferPositive(&pad, 1);
        unsigned now_b = pad.Buttons, pressed = now_b & ~prev;
        prev = now_b;
        if (!g_leaving) {
            if (pressed & PSP_CTRL_START) leave_to_xmb("START");
            else if (pressed & PSP_CTRL_CROSS) launch("classic");
            else if (pressed & PSP_CTRL_TRIANGLE) launch("modern");
            else if (pressed & PSP_CTRL_SQUARE) launch("probe");
        }
        uint64_t t = nbr_now_us();
        if (t >= next_tick) { next_tick = t + 5000000ull; scePowerTick(PSP_POWER_TICK_SUSPEND); }
        if (t >= next_draw) { next_draw = t + 500000ull; draw(); }
        sceDisplayWaitVblankStart();
    }
    return 0;
}
