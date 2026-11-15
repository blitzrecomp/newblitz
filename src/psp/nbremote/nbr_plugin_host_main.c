/* nbremote-plugin-host: the VSH plugin's decision logic (nbr_plugin.c) on the Mac, a dry run of what the plugin would do in
 * the XMB: NBR_MS0=<folder laid out as a stick>, NBR_HELD=1 holds R. Prints "LAUNCH <path>" where the PSP would start an
 * EBOOT (tools/psp/nbremote_test.sh). */
int nbr_plugin_thread(unsigned args, void* argp);
int main(void) { return nbr_plugin_thread(0, 0); }
