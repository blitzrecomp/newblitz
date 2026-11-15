// See psp_net.h.
#include "psp_net.h"
#include "psp_log.h"
#include "psp_sys.h"
#include <string.h>
#include <stdio.h>

#ifdef __PSP__
#include <pspkernel.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <pspnet_resolver.h>
#include <psputility.h>
#include <psputility_netparam.h>
#include <pspwlan.h>
#endif

namespace pspnet {
namespace {
volatile State g_state = NET_OFF;
pspsys::Mutex* g_mtx = nullptr;
Report g_report;
bool g_stack = false;
int g_thread = 0;
int g_profile = 1;

void set_state(State s) {
    if (g_state != s) PLOG("net", "state %s -> %s", state_name(g_state), state_name(s));
    g_state = s;
}
pspsys::Mutex& mtx() { if (!g_mtx) g_mtx = new pspsys::Mutex(); return *g_mtx; }

#ifdef __PSP__
const char* apctl_name(int s) {
    switch (s) {
        case PSP_NET_APCTL_STATE_DISCONNECTED: return "disconnected";
        case PSP_NET_APCTL_STATE_SCANNING: return "scanning";
        case PSP_NET_APCTL_STATE_JOINING: return "joining";
        case PSP_NET_APCTL_STATE_GETTING_IP: return "getting_ip";
        case PSP_NET_APCTL_STATE_GOT_IP: return "got_ip";
        case PSP_NET_APCTL_STATE_EAP_AUTH: return "eap_auth";
        case PSP_NET_APCTL_STATE_KEY_EXCHANGE: return "key_exchange";
        default: return "?";
    }
}
#endif

void net_thread(void*) {
    Report r;
#ifdef __PSP__
    bool said_off = false;
    for (;;) {
        r.wlan_switch = sceWlanGetSwitchState();
        if (r.wlan_switch == 1) break;
        if (!said_off) { PWARN("net", "WLAN switch off: no Wi-Fi this run unless it is switched on (checking every 30 s)"); said_off = true; }
        set_state(NET_WLAN_OFF);
        { pspsys::Lock l(mtx()); g_report = r; }
        pspsys::sleep_ms(30000);
    }
    if (said_off) PLOG("net", "WLAN switch on");
#endif
    set_state(NET_LOADING);
    if (!init_stack(r)) {
        set_state(NET_FAILED);
        { pspsys::Lock l(mtx()); g_report = r; }
        return;
    }
    set_state(NET_CONNECTING);
    bool ok = connect_any(g_profile, r);
    { pspsys::Lock l(mtx()); g_report = r; }
    set_state(ok ? NET_UP : NET_FAILED);
#ifdef __PSP__
    // Stay connected: a sleep and wake, the access point going away or a weak signal leave apctl disconnected, and nothing
    // reconnected it (the PSP-1001, 2026-11-04: the screen went dark, and the game ran on with Wi-Fi gone). Every 5 s; after
    // a failed try, 30 s. disconnect() (NET_OFF) ends it.
    for (;;) {
        pspsys::sleep_ms(g_state == NET_FAILED ? 30000 : 5000);
        if (g_state == NET_OFF) return;
        int st = -1;
        if (sceNetApctlGetState(&st) < 0) continue;
        if (st == PSP_NET_APCTL_STATE_GOT_IP) {
            if (g_state != NET_UP) set_state(NET_UP);
            continue;
        }
        if (st != PSP_NET_APCTL_STATE_DISCONNECTED && g_state == NET_CONNECTING) continue;   // apctl is still on its way
        PWARN("net", "Wi-Fi link lost (apctl %s): reconnecting", apctl_name(st));
        set_state(NET_CONNECTING);
        sceNetApctlDisconnect();
        pspsys::sleep_ms(500);
        Report again;
        again.wlan_switch = sceWlanGetSwitchState();
        bool back = again.wlan_switch == 1 && connect_any(g_profile, again);
        { pspsys::Lock l(mtx()); g_report = again; }
        if (g_state == NET_OFF) return;
        set_state(back ? NET_UP : NET_FAILED);
    }
#endif
}
}  // namespace

const char* state_name(State s) {
    switch (s) {
        case NET_OFF: return "off";
        case NET_WLAN_OFF: return "wlan_off";
        case NET_LOADING: return "loading";
        case NET_CONNECTING: return "connecting";
        case NET_UP: return "up";
        case NET_FAILED: return "failed";
    }
    return "?";
}

bool init_stack(Report& r) {
    if (g_stack) return true;
    uint64_t t0 = pspsys::now_us();
    r.free_kb_before = pspsys::free_kb();
#ifdef __PSP__
    r.rc_common = sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
    r.rc_inet = sceUtilityLoadNetModule(PSP_NET_MODULE_INET);
    PLOG("net", "net modules: common 0x%08X inet 0x%08X", (unsigned)r.rc_common, (unsigned)r.rc_inet);
    // 0x80110F02-ish "already loaded" codes are fine; a real failure shows up in sceNetInit below
    // The stack's threads above the game and the renderer (0x20, 0x18): at the SDK's usual 42/48 a busy game starved them
    // and a stuck renderer silenced the hang report (PSP-1001, 2026-11-04). They sleep unless there's traffic.
    r.rc_netinit = sceNetInit(128 * 1024, 0x15, 4 * 1024, 0x15, 4 * 1024);
    r.rc_inetinit = sceNetInetInit();
    r.rc_resolver = sceNetResolverInit();
    r.rc_apctl = sceNetApctlInit(0x8000, 0x16);
    PLOG("net", "sceNetInit 0x%08X sceNetInetInit 0x%08X sceNetResolverInit 0x%08X sceNetApctlInit 0x%08X", (unsigned)r.rc_netinit,
         (unsigned)r.rc_inetinit, (unsigned)r.rc_resolver, (unsigned)r.rc_apctl);
    SceNetMallocStat ms;
    memset(&ms, 0, sizeof ms);
    if (sceNetGetMallocStat(&ms) >= 0) { r.pool = ms.pool; r.pool_max = ms.maximum; r.pool_free = ms.free; }
    r.free_kb_after = pspsys::free_kb();
    r.max_kb_after = pspsys::max_free_kb();
    r.init_ms = (int)((pspsys::now_us() - t0) / 1000);
    PLOG("net", "free memory %d KB before the net modules, %d KB after (largest %d KB); net pool %d, max used %d, free %d; %d ms",
         r.free_kb_before, r.free_kb_after, r.max_kb_after, r.pool, r.pool_max, r.pool_free, r.init_ms);
    if (r.rc_netinit < 0 || r.rc_inetinit < 0 || r.rc_apctl < 0) {
        r.error = "net init failed";
        PERR("net", "net init failed: no Wi-Fi this run");
        return false;
    }
#else
    (void)t0;
#endif
    g_stack = true;
    return true;
}

// The PSP keeps up to 10 network profiles in numbered slots, and the Network Settings list isn't in slot order: deleting an
// old profile left slot 1 empty on the team's PSP (2026-11-04, sceNetApctlConnect(1) 0x80110601). So: the configured slot first
// if it holds a profile, then every other slot that does, until one gets an address. Each slot's name and SSID go to the log.
bool connect_any(int preferred, Report& r) {
#ifdef __PSP__
    int order[11];
    int n = 0;
    bool valid[11] = {};
    for (int id = 1; id <= 10; id++) {
        if (sceUtilityCheckNetParam(id) != 0) continue;
        valid[id] = true;
        netData name, ssid;
        memset(&name, 0, sizeof name);
        memset(&ssid, 0, sizeof ssid);
        sceUtilityGetNetParam(id, PSP_NETPARAM_NAME, &name);
        sceUtilityGetNetParam(id, PSP_NETPARAM_SSID, &ssid);
        name.asString[sizeof name.asString - 1] = 0;
        ssid.asString[sizeof ssid.asString - 1] = 0;
        PLOG("net", "profile slot %d: \"%s\" (SSID \"%s\")", id, name.asString, ssid.asString);
    }
    if (preferred >= 1 && preferred <= 10 && valid[preferred]) order[n++] = preferred;
    else if (preferred >= 1) PWARN("net", "net.cfg's wifi_profile %d holds no profile: trying the others", preferred);
    for (int id = 1; id <= 10; id++)
        if (valid[id] && id != preferred) order[n++] = id;
    if (n == 0) { r.error = "no network profiles"; PERR("net", "no saved network profiles (Settings > Network Settings)"); return false; }
    for (int i = 0; i < n; i++) {
        Report tryr = r;
        tryr.states.clear();
        if (connect(order[i], 30000, tryr)) { r = tryr; return true; }
        r = tryr;
        sceNetApctlDisconnect();
        pspsys::sleep_ms(500);
    }
    return false;
#else
    return connect(preferred > 0 ? preferred : 1, 30000, r);
#endif
}

bool connect(int profile, int timeout_ms, Report& r) {
    r.profile = profile;
#ifdef __PSP__
    uint64_t t0 = pspsys::now_us();
    r.rc_connect = sceNetApctlConnect(profile);
    PLOG("net", "sceNetApctlConnect(%d) 0x%08X", profile, (unsigned)r.rc_connect);
    if (r.rc_connect < 0) { r.error = "apctl connect refused"; PERR("net", "apctl connect refused (is profile %d set up?)", profile); return false; }
    int last = -1;
    bool seen_progress = false;
    for (;;) {
        int st = -1;
        int rc = sceNetApctlGetState(&st);
        int ms = (int)((pspsys::now_us() - t0) / 1000);
        if (rc < 0) { r.error = "apctl state error"; PERR("net", "sceNetApctlGetState 0x%08X", (unsigned)rc); return false; }
        if (st != last) {
            PLOG("net", "apctl %s at %d ms", apctl_name(st), ms);
            r.states.push_back({st, ms});
            last = st;
        }
        if (st == PSP_NET_APCTL_STATE_GOT_IP) { r.ms_to_ip = ms; break; }
        if (st != PSP_NET_APCTL_STATE_DISCONNECTED) seen_progress = true;
        else if (seen_progress) { r.error = "dropped back to disconnected"; PERR("net", "apctl fell back to disconnected (password? range?)"); return false; }
        if (ms > timeout_ms) { r.error = "timeout"; PERR("net", "no IP after %d ms", ms); sceNetApctlDisconnect(); return false; }
        pspsys::sleep_ms(50);
    }
    SceNetApctlInfo info;
    memset(&info, 0, sizeof info);
    if (sceNetApctlGetInfo(PSP_NET_APCTL_INFO_IP, &info) >= 0) r.ip = info.ip;
    memset(&info, 0, sizeof info);
    if (sceNetApctlGetInfo(PSP_NET_APCTL_INFO_SSID, &info) >= 0) r.ssid = std::string((const char*)info.ssid, strnlen((const char*)info.ssid, 32));
    memset(&info, 0, sizeof info);
    if (sceNetApctlGetInfo(PSP_NET_APCTL_INFO_PROFILE_NAME, &info) >= 0) r.profile_name = std::string(info.name, strnlen(info.name, 64));
    PLOG("net", "got IP %s on \"%s\" (profile %d \"%s\") in %d ms", r.ip.c_str(), r.ssid.c_str(), profile, r.profile_name.c_str(), r.ms_to_ip);
    return true;
#else
    (void)timeout_ms;
    r.ms_to_ip = 0;
    r.ip = "host";
    return true;
#endif
}

void disconnect() {
#ifdef __PSP__
    sceNetApctlDisconnect();
#endif
    set_state(NET_OFF);
}

void start(int profile) {
    if (g_thread) return;
    g_profile = profile > 0 ? profile : 1;
    g_thread = pspsys::thread_start("blitz_net", net_thread, nullptr, 64 * 1024, 0x17);
    if (!g_thread) { PERR("net", "net thread failed to start"); set_state(NET_FAILED); }
}

State state() { return g_state; }
bool up() { return g_state == NET_UP; }
bool wait_up(int timeout_ms) {
    uint64_t until = pspsys::now_us() + (uint64_t)timeout_ms * 1000;
    while (pspsys::now_us() < until) {
        if (g_state == NET_UP) return true;
        if (g_state == NET_FAILED || g_state == NET_WLAN_OFF) return false;
        pspsys::sleep_ms(50);
    }
    return g_state == NET_UP;
}
Report report() { pspsys::Lock l(mtx()); return g_report; }

}  // namespace pspnet
