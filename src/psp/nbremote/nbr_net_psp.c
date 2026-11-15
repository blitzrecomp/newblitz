/* NB Remote's Wi-Fi on the PSP (see nbr.h): the games' bring-up (src/psp/psp_net.cpp) in C. Its own thread waits for the WLAN
 * switch, loads the net modules, inits the stack, then tries the configured profile slot and every other saved one (the team's
 * Blitz profile is in slot 6, not 1) until one gets an address; after that it checks the link every 5 s and reconnects
 * (30 s after a failed try). Everything that fails is logged and retried: nothing here ends the program.
 * The plugin's experimental net mode (NBR_PLUGIN_NET) uses this too; there sceUtilityLoadNetModule may well refuse (the XMB
 * owns the net stack), which is logged and leaves the plugin a launcher. */
#include "nbr.h"
#include "nbr_net.h"
#include <pspkernel.h>
#include <pspnet.h>
#include <pspnet_apctl.h>
#include <psputility.h>
#include <psputility_netparam.h>
#include <pspwlan.h>
#include <string.h>

int sceNetInetInit(void);
int sceNetResolverInit(void);

static volatile int g_state = NBR_NET_OFF;
static volatile int g_stop;
static int g_profile = 1, g_load_modules = 1, g_thread = -1;
static char g_ip[20], g_ssid[40];
static int g_slot;

const char* nbr_net_state_name(void) {
    switch (g_state) {
        case NBR_NET_OFF: return "off";
        case NBR_NET_WLAN_OFF: return "WLAN switch off";
        case NBR_NET_LOADING: return "starting";
        case NBR_NET_CONNECTING: return "connecting";
        case NBR_NET_UP: return "up";
        case NBR_NET_FAILED: return "failed (retrying)";
        case NBR_NET_UNAVAILABLE: return "unavailable";
    }
    return "?";
}

int nbr_net_up(void) { return g_state == NBR_NET_UP; }
int nbr_net_state(void) { return g_state; }

void nbr_net_describe(nbr_buf* out) {
    if (g_state == NBR_NET_UP) nbr_bprintf(out, "up ip %s ssid \"%s\" profile %d", g_ip, g_ssid, g_slot);
    else nbr_bputs(out, nbr_net_state_name());
}

const char* nbr_net_ip(void) { return g_ip; }
const char* nbr_net_ssid(void) { return g_ssid; }
int nbr_net_slot(void) { return g_slot; }

static const char* apctl_name(int s) {
    switch (s) {
        case PSP_NET_APCTL_STATE_DISCONNECTED: return "disconnected";
        case PSP_NET_APCTL_STATE_SCANNING: return "scanning";
        case PSP_NET_APCTL_STATE_JOINING: return "joining";
        case PSP_NET_APCTL_STATE_GETTING_IP: return "getting_ip";
        case PSP_NET_APCTL_STATE_GOT_IP: return "got_ip";
        case PSP_NET_APCTL_STATE_EAP_AUTH: return "eap_auth";
        case PSP_NET_APCTL_STATE_KEY_EXCHANGE: return "key_exchange";
    }
    return "?";
}

static int init_stack(void) {
    if (g_load_modules) {
        int a = sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON), b = sceUtilityLoadNetModule(PSP_NET_MODULE_INET);
        nbr_log("net modules: common 0x%08X inet 0x%08X", (unsigned)a, (unsigned)b);
    }
    int n = sceNetInit(128 * 1024, 0x20, 4 * 1024, 0x20, 4 * 1024);
    int i = sceNetInetInit();
    int r = sceNetResolverInit();
    int c = sceNetApctlInit(0x8000, 0x30);
    nbr_log("sceNetInit 0x%08X sceNetInetInit 0x%08X sceNetResolverInit 0x%08X sceNetApctlInit 0x%08X", (unsigned)n, (unsigned)i,
            (unsigned)r, (unsigned)c);
    if ((unsigned)n == 0x8002013Au || (unsigned)c == 0x8002013Au) return -2;   /* the libraries aren't there (not linked) */
    return n < 0 || i < 0 || c < 0 ? -1 : 0;
}

static int connect_slot(int slot) {
    int rc = sceNetApctlConnect(slot);
    nbr_log("sceNetApctlConnect(%d) 0x%08X", slot, (unsigned)rc);
    if (rc < 0) return -1;
    uint64_t t0 = nbr_now_us();
    int last = -1, progress = 0;
    for (;;) {
        int st = -1;
        if (sceNetApctlGetState(&st) < 0) return -1;
        int ms = (int)((nbr_now_us() - t0) / 1000);
        if (st != last) { nbr_log("apctl %s at %d ms", apctl_name(st), ms); last = st; }
        if (st == PSP_NET_APCTL_STATE_GOT_IP) break;
        if (st != PSP_NET_APCTL_STATE_DISCONNECTED) progress = 1;
        else if (progress) return -1;
        if (ms > 30000 || g_stop) { sceNetApctlDisconnect(); return -1; }
        nbr_sleep_ms(100);
    }
    union SceNetApctlInfo info;
    memset(&info, 0, sizeof info);
    if (sceNetApctlGetInfo(PSP_NET_APCTL_INFO_IP, &info) >= 0) nbr_fmt(g_ip, sizeof g_ip, "%.16s", info.ip);
    memset(&info, 0, sizeof info);
    if (sceNetApctlGetInfo(PSP_NET_APCTL_INFO_SSID, &info) >= 0) nbr_fmt(g_ssid, sizeof g_ssid, "%.32s", (const char*)info.ssid);
    g_slot = slot;
    nbr_log("got IP %s on \"%s\" (profile %d) in %d ms", g_ip, g_ssid, slot, (int)((nbr_now_us() - t0) / 1000));
    return 0;
}

static int connect_any(void) {
    int order[11], n = 0, valid[11];
    memset(valid, 0, sizeof valid);
    for (int id = 1; id <= 10; id++) {
        if (sceUtilityCheckNetParam(id) != 0) continue;
        valid[id] = 1;
        netData name, ssid;
        memset(&name, 0, sizeof name);
        memset(&ssid, 0, sizeof ssid);
        sceUtilityGetNetParam(id, PSP_NETPARAM_NAME, &name);
        sceUtilityGetNetParam(id, PSP_NETPARAM_SSID, &ssid);
        name.asString[sizeof name.asString - 1] = 0;
        ssid.asString[sizeof ssid.asString - 1] = 0;
        nbr_log("profile slot %d: \"%s\" (SSID \"%s\")", id, name.asString, ssid.asString);
    }
    if (g_profile >= 1 && g_profile <= 10 && valid[g_profile]) order[n++] = g_profile;
    for (int id = 1; id <= 10; id++) if (valid[id] && id != g_profile) order[n++] = id;
    if (!n) { nbr_log("no saved network profiles (Settings > Network Settings)"); return -1; }
    for (int i = 0; i < n && !g_stop; i++) {
        if (connect_slot(order[i]) == 0) return 0;
        sceNetApctlDisconnect();
        nbr_sleep_ms(500);
    }
    return -1;
}

static int net_thread(SceSize args, void* argp) {
    (void)args; (void)argp;
    int said = 0;
    while (!g_stop && sceWlanGetSwitchState() != 1) {
        if (!said) { nbr_log("WLAN switch off: waiting for it (checked every 5 s)"); said = 1; }
        g_state = NBR_NET_WLAN_OFF;
        nbr_sleep_ms(5000);
    }
    g_state = NBR_NET_LOADING;
    int init = init_stack();
    while (init == -1 && !g_stop) {   /* a failed init: try again in 30 s */
        g_state = NBR_NET_FAILED;
        nbr_sleep_ms(30000);
        init = init_stack();
    }
    if (init == -2) {
        nbr_log("the net libraries aren't available here: no Wi-Fi");
        g_state = NBR_NET_UNAVAILABLE;
        return 0;
    }
    while (!g_stop) {
        if (sceWlanGetSwitchState() != 1) {
            if (g_state != NBR_NET_WLAN_OFF) nbr_log("WLAN switch off");
            g_state = NBR_NET_WLAN_OFF;
            nbr_sleep_ms(5000);
            continue;
        }
        int st = -1;
        if (g_state == NBR_NET_UP && sceNetApctlGetState(&st) >= 0 && st == PSP_NET_APCTL_STATE_GOT_IP) { nbr_sleep_ms(5000); continue; }
        if (g_state == NBR_NET_UP) { nbr_log("Wi-Fi link lost (apctl %s): reconnecting", apctl_name(st)); sceNetApctlDisconnect(); nbr_sleep_ms(500); }
        g_state = NBR_NET_CONNECTING;
        if (connect_any() == 0) g_state = NBR_NET_UP;
        else { g_state = NBR_NET_FAILED; nbr_sleep_ms(30000); }
    }
    return 0;
}

void nbr_net_start(int profile, int load_modules) {
    if (g_thread >= 0) return;
    g_profile = profile;
    g_load_modules = load_modules;
    g_stop = 0;
    g_thread = sceKernelCreateThread("nbr_net", net_thread, 0x30, 32 * 1024, PSP_THREAD_ATTR_USER, NULL);
    if (g_thread >= 0) sceKernelStartThread(g_thread, 0, NULL);
    else nbr_log("net thread: 0x%08X", (unsigned)g_thread);
}

void nbr_net_stop(void) {
    g_stop = 1;
    if (g_state == NBR_NET_UP || g_state == NBR_NET_CONNECTING) sceNetApctlDisconnect();
}
