// The PSP's Wi-Fi bring-up (docs/PSP_PORT.md §5.3 "Net startup"): WLAN switch, the net modules, sceNetInit / InetInit /
// ResolverInit / ApctlInit, then sceNetApctlConnect(profile) with every state change logged, until GOT_IP or 30 s. The netconf
// dialog is never used. start() does all of it on its own thread (priority 0x30) and never blocks boot; anything that fails is
// logged and the game carries on without the network. On the Mac (psp-host) the network is simply up.
#pragma once
#include <string>
#include <vector>

namespace pspnet {

enum State { NET_OFF, NET_WLAN_OFF, NET_LOADING, NET_CONNECTING, NET_UP, NET_FAILED };
const char* state_name(State s);

struct Report {
    int wlan_switch = -1;                 // 1 on, 0 off, -1 not asked
    int free_kb_before = 0, free_kb_after = 0, max_kb_after = 0;
    int pool = 0, pool_max = 0, pool_free = 0;   // sceNetGetMallocStat after init
    int rc_common = 0, rc_inet = 0, rc_netinit = 0, rc_inetinit = 0, rc_resolver = 0, rc_apctl = 0, rc_connect = 0;
    int init_ms = 0, ms_to_ip = -1, profile = 1;
    std::string ip, ssid, profile_name, error;
    std::vector<std::pair<int, int>> states;     // (apctl state, ms since connect)
};

bool init_stack(Report& r);                       // modules + sceNet*Init; idempotent
bool connect(int profile, int timeout_ms, Report& r);
// The preferred profile slot if it holds one, then every other saved slot (1-10) in turn, until one gets an IP.
bool connect_any(int preferred, Report& r);
void disconnect();
void start(int profile);                          // the background bring-up (retries the WLAN switch every 30 s)
State state();
bool up();
bool wait_up(int timeout_ms);
Report report();                                  // a copy of the last bring-up's numbers

}  // namespace pspnet
