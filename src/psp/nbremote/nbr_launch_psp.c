/* Starting an EBOOT from the Memory Stick in place of the running program: NB Remote's run verb, the games' restart and their
 * way home to NB Remote (src/psp/psp_remote.cpp), and the VSH plugin.
 * On a PSP with ARK (NBR_CFW_IMPORTS=1): ARK's sctrlKernelLoadExecVSHWithApitype(0x141, path) first, the call ARK's own xMenu
 * launcher makes for homebrew (apitype 0x141 = Memory Stick homebrew, key "game"); it is exported to user mode as
 * SystemCtrlForUser. Under emulation, or if that returns, the firmware's sceKernelLoadExec. Returns only if both failed. */
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <psploadexec.h>
#include <string.h>

#if NBR_CFW_IMPORTS
struct SceKernelLoadExecVSHParam;
int sctrlKernelLoadExecVSHWithApitype(int apitype, const char* file, struct SceKernelLoadExecVSHParam* param);
typedef struct {
    SceSize size, args;
    void* argp;
    const char* key;
    u32 vshmain_args_size;
    void* vshmain_args;
    char* configfile;
    u32 unk4, unk5;
} vsh_param;   /* SceKernelLoadExecVSHParam (psploadexec_kernel.h), spelled out so this file needs no kernel headers */
#endif

/* "\0PBP" at the start: a missing, empty or truncated EBOOT is never handed to LoadExec (which tears the running program down
 * first, so a failure there lands in the XMB, not back here) */
static int is_pbp(const char* path) {
    unsigned char m[4] = {0xff, 0xff, 0xff, 0xff};
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return 0;
    int n = sceIoRead(fd, m, 4);
    sceIoClose(fd);
    return n == 4 && m[0] == 0 && m[1] == 'P' && m[2] == 'B' && m[3] == 'P';
}

int nbr_launch_eboot(const char* path) {
    if (!is_pbp(path)) return (int)0x80010002;   /* SCE_ERROR_ERRNO_ENOENT-ish: not started */
    int emu = 0, out = 0;
    if (sceIoDevctl("kemulator:", 3, NULL, 0, &out, sizeof out) >= 0 && out) emu = 1;
    int rc = -1;
#if NBR_CFW_IMPORTS
    if (!emu) {
        vsh_param p;
        memset(&p, 0, sizeof p);
        p.size = sizeof p;
        p.args = strlen(path) + 1;
        p.argp = (void*)path;
        p.key = "game";
        rc = sctrlKernelLoadExecVSHWithApitype(0x141, path, (struct SceKernelLoadExecVSHParam*)&p);
        if (rc >= 0) return rc;   /* it took the launch (it doesn't come back if it worked): never a second LoadExec on top */
    }
#else
    (void)emu;
#endif
    struct SceKernelLoadExecParam q;
    memset(&q, 0, sizeof q);
    q.size = sizeof q;
    q.args = strlen(path) + 1;
    q.argp = (void*)path;
    q.key = NULL;
    int rc2 = sceKernelLoadExec(path, &q);
    return rc2 < 0 ? rc2 : rc;
}
