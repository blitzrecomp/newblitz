// Registers a title folder with the PS5's app database so the home screen lists it (tools/ps5/deploy.sh REGISTER=1;
// docs/PS5_HOMEBREW.md). An ELF payload for etaHEN's loader on port 9021, built with the payload SDK. The same call
// as the SDK's samples/install_app, pointed at /data/homebrew instead of /user/app. Experimental.
#include <stdio.h>

int sceAppInstUtilInitialize(void);
int sceAppInstUtilAppInstallTitleDir(const char* title_id, const char* dir, void* reserved);

int main(void) {
    int err = sceAppInstUtilInitialize();
    if (err) {
        printf("sceAppInstUtilInitialize: 0x%x\n", err);
        return 1;
    }
    err = sceAppInstUtilAppInstallTitleDir(BLITZ_TITLE_ID, "/data/homebrew/", 0);
    printf("register %s: 0x%x\n", BLITZ_TITLE_ID, err);
    return err ? 1 : 0;
}
