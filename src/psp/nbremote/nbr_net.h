/* NB Remote's Wi-Fi thread on the PSP (nbr_net_psp.c). */
#ifndef NBR_NET_H
#define NBR_NET_H
#ifdef __cplusplus
extern "C" {
#endif
enum { NBR_NET_OFF, NBR_NET_WLAN_OFF, NBR_NET_LOADING, NBR_NET_CONNECTING, NBR_NET_UP, NBR_NET_FAILED, NBR_NET_UNAVAILABLE };
void nbr_net_start(int profile, int load_modules);   /* its own thread; profile: net.cfg's wifi_profile, tried first */
void nbr_net_stop(void);
int nbr_net_state(void);
const char* nbr_net_state_name(void);
const char* nbr_net_ip(void);
const char* nbr_net_ssid(void);
int nbr_net_slot(void);
#ifdef __cplusplus
}
#endif
#endif
