// BlitzRecomp's starter (docs/design/ONE_APP.md): Contents/MacOS/BlitzRecomp. Picks the game to open and execs its
// engine, Contents/MacOS/Classic or Contents/MacOS/Modern, with that engine's resources (BLITZ_RESOURCES).
//
// Which game: BLITZ_ENGINE (dev) > the last one played (~/Library/Application Support/BlitzRecomp/experience,
// written by the engines) > the one with a ROM stored (Modern if only Modern has one) > Classic. An engine that
// isn't in the bundle (a Classic-only build) is never picked. Dev: BLITZ_APP_FOLDER_CLASSIC / _MODERN give each
// engine its own app folder (APP_FOLDER_PATH), BLITZ_EXPERIENCE_DIR moves the experience file.
#include <errno.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int exists(const char* p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static void app_support(char* out, size_t n, const char* leaf) {
    const char* home = getenv("HOME");
    snprintf(out, n, "%s/Library/Application Support/%s", home ? home : "/tmp", leaf);
}

int main(int argc, char** argv) {
    (void)argc;
    char exe[PATH_MAX], real[PATH_MAX];
    uint32_t size = sizeof(exe);
    if (_NSGetExecutablePath(exe, &size) != 0 || realpath(exe, real) == NULL) return 1;
    char* slash = strrchr(real, '/');
    if (slash == NULL) return 1;
    *slash = 0;  // real = Contents/MacOS

    char classic[PATH_MAX], modern[PATH_MAX];
    snprintf(classic, sizeof classic, "%s/Classic", real);
    snprintf(modern, sizeof modern, "%s/Modern", real);
    int have_classic = exists(classic), have_modern = exists(modern);

    const char* pick = NULL;
    const char* forced = getenv("BLITZ_ENGINE");
    if (forced && (strcmp(forced, "Classic") == 0 || strcmp(forced, "Modern") == 0)) pick = forced;
    if (pick == NULL) {
        char path[PATH_MAX], line[32] = {0};
        const char* dir = getenv("BLITZ_EXPERIENCE_DIR");
        if (dir) snprintf(path, sizeof path, "%s/experience", dir);
        // BRANDING: app folder names (user-visible)
        else app_support(path, sizeof path, "BlitzRecomp/experience");
        FILE* f = fopen(path, "r");
        if (f) {
            if (fgets(line, sizeof line, f)) line[strcspn(line, "\r\n")] = 0;
            fclose(f);
        }
        if (strcmp(line, "Modern") == 0) pick = "Modern";
        else if (strcmp(line, "Classic") == 0) pick = "Classic";
    }
    if (pick == NULL) {
        char rom[PATH_MAX];
        const char* mf = getenv("BLITZ_APP_FOLDER_MODERN");
        if (mf) snprintf(rom, sizeof rom, "%s/blitz2000.teamplus.n64.us.z64", mf);
        else app_support(rom, sizeof rom, "Blitz2026Recompiled/blitz2000.teamplus.n64.us.z64");
        char crom[PATH_MAX];
        const char* cf = getenv("BLITZ_APP_FOLDER_CLASSIC");
        if (cf) snprintf(crom, sizeof crom, "%s/blitz2000.n64.us.1.1.z64", cf);
        else app_support(crom, sizeof crom, "BlitzRecompiled/blitz2000.n64.us.1.1.z64");
        pick = exists(rom) && !exists(crom) ? "Modern" : "Classic";
    }
    if (strcmp(pick, "Modern") == 0 && !have_modern) pick = "Classic";
    if (strcmp(pick, "Classic") == 0 && !have_classic) pick = "Modern";

    char res[PATH_MAX];
    snprintf(res, sizeof res, "%s/../Resources/%s", real, pick);
    setenv("BLITZ_RESOURCES", res, 1);
    const char* folder = getenv(strcmp(pick, "Modern") == 0 ? "BLITZ_APP_FOLDER_MODERN" : "BLITZ_APP_FOLDER_CLASSIC");
    if (folder) setenv("APP_FOLDER_PATH", folder, 1);
    argv[0] = strcmp(pick, "Modern") == 0 ? modern : classic;
    execv(argv[0], argv);
    // BRANDING: name in the starter's error
    fprintf(stderr, "BlitzRecomp: couldn't start %s: %s\n", argv[0], strerror(errno));
    return 1;
}
