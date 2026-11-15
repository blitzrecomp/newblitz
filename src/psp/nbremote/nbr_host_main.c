/* nbremote-host: NB Remote's core on the Mac, for tests against tools/psp/mock_worker.py (make psp-remote-test).
 *   nbremote-host --stick <dir> [--secs N]      polls for N seconds (default 30) as <dir>/PSP/GAME/NBREMOTE would, printing
 *                                                "LAUNCH <path>" and "MENU" where the PSP would leave; the pid goes to stdout
 *                                                first ("PID psp-remote-<digits>") so a test can aim commands at it. */
#include "nbr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_big[1024 * 1024];
static char g_chunk[256 * 1024 + 4096];

int main(int argc, char** argv) {
    const char* stick = NULL;
    int secs = 30;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--stick") && i + 1 < argc) stick = argv[++i];
        else if (!strcmp(argv[i], "--secs") && i + 1 < argc) secs = atoi(argv[++i]);
    }
    if (!stick) { fprintf(stderr, "usage: nbremote-host --stick <dir> [--secs N]\n"); return 2; }
    static char app[512], games[512];
    snprintf(games, sizeof games, "%s/PSP/GAME", stick);
    snprintf(app, sizeof app, "%s/NBREMOTE", games);
    nbr_opts o;
    memset(&o, 0, sizeof o);
    o.app_dir = app;
    o.games_dir = games;
    o.form = "host";
    o.build = "host";
    o.log_name = "remote.log";
    o.big = g_big;
    o.big_cap = sizeof g_big;
    o.chunk = g_chunk;
    o.chunk_cap = sizeof g_chunk;
    const char* cl = getenv("NBR_CHUNK");   /* tests: smaller pieces */
    o.chunk_len = cl ? atoi(cl) : 256 * 1024;
    o.can_menu = 1;
    int ok = nbr_init(&o);
    printf("PID %s\n", nbr_pid());
    fflush(stdout);
    if (!ok) { fprintf(stderr, "no Worker channel (net.cfg)\n"); return 1; }
    uint64_t end = nbr_now_us() + (uint64_t)secs * 1000000ull;
    while (nbr_now_us() < end) {
        int a = nbr_step();
        if (a == NBR_ACT_LAUNCH) nbr_do_launch();
        else if (a == NBR_ACT_MENU) { printf("MENU\n"); fflush(stdout); }
        nbr_sleep_ms(100);
    }
    return 0;
}
