// Send to Android's reading of adb (src/main/adb_parse.h), from adb's real answers: `make test-adb-parse`.
#include "../src/main/adb_parse.h"

#include <cstdio>

using namespace blitz::adb;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

int main() {
    // adb devices
    CHECK(devices(true, "List of devices attached\nR5CT1234ABC\tdevice\n\n").problem == Problem::None);
    CHECK(devices(true, "* daemon not running; starting now at tcp:5037\n* daemon started successfully\nList of devices attached\n"
                        "R5CT1234ABC\tdevice\r\n\r\n").problem == Problem::None);
    CHECK(devices(true, "List of devices attached\n\n").problem == Problem::NoDevice);
    CHECK(devices(true, "List of devices attached\nR5CT1234ABC\tunauthorized\n\n").problem == Problem::Unauthorized);
    CHECK(devices(true, "List of devices attached\nR5CT1234ABC\toffline\n\n").problem == Problem::Offline);
    CHECK(devices(true, "List of devices attached\nR5CT1234ABC\tno permissions (missing udev rules? user is in the plugdev group); "
                        "see [http://developer.android.com/tools/device.html]\n").problem == Problem::Offline);
    CHECK(devices(true, "List of devices attached\nA\tdevice\nB\tdevice\n").problem == Problem::TooMany);
    CHECK(devices(true, "List of devices attached\nA\tunauthorized\nB\tdevice\n").problem == Problem::None);
    CHECK(devices(false, "sh: adb: command not found\n").problem == Problem::NoAdb);
    CHECK(devices(false, "couldn't start it (error 2)").problem == Problem::NoAdb);
    CHECK(devices(false, "").problem == Problem::NoAdb);

    // adb install -r: the team's, 2026-11-15
    Verdict v = install(false, "Performing Streamed Install\nadb: failed to install /Applications/New Blitz.app/Contents/Resources/"
                               "devices/android/NewBlitz.apk: Failure [INSTALL_FAILED_VERSION_DOWNGRADE: Downgrade detected: "
                               "Update version code 660 is older than current 724]\n");
    CHECK(v.problem == Problem::Downgrade);
    CHECK(v.carried == 660 && v.installed == 724);
    CHECK(v.code == "INSTALL_FAILED_VERSION_DOWNGRADE");
    v = install(false, "Failure [INSTALL_FAILED_VERSION_DOWNGRADE]\n");
    CHECK(v.problem == Problem::Downgrade && v.carried == -1 && v.installed == -1);

    v = install(false, "Performing Streamed Install\nadb: failed to install NewBlitz.apk: Failure [INSTALL_FAILED_UPDATE_INCOMPATIBLE: "
                       "Package com.blitzrecomp.newblitz signatures do not match previously installed version; ignoring!]\n");
    CHECK(v.problem == Problem::Incompatible && v.code == "INSTALL_FAILED_UPDATE_INCOMPATIBLE");
    CHECK(install(false, "Failure [INSTALL_FAILED_UPDATE_INCOMPATIBLE: Existing package com.blitzrecomp.newblitz signatures do "
                         "not match newer version; ignoring!]").problem == Problem::Incompatible);

    v = install(false, "Performing Streamed Install\nadb: failed to install NewBlitz.apk: Failure [INSTALL_FAILED_INSUFFICIENT_STORAGE: "
                       "Failed to override installation location]\n");
    CHECK(v.problem == Problem::Storage);
    CHECK(install(false, "adb: failed to install NewBlitz.apk: Failure [INSTALL_FAILED_INSUFFICIENT_STORAGE]").problem == Problem::Storage);

    CHECK(install(false, "adb: no devices/emulators found\n").problem == Problem::NoDevice);
    CHECK(install(false, "adb: device unauthorized.\nThis adb server's $ADB_VENDOR_KEYS is not set\n").problem == Problem::Unauthorized);
    CHECK(install(false, "adb: device offline\n").problem == Problem::Offline);
    CHECK(install(false, "adb: more than one device/emulator\n").problem == Problem::TooMany);
    CHECK(install(false, "sh: adb: command not found\n").problem == Problem::NoAdb);

    v = install(false, "adb: failed to install NewBlitz.apk: Failure [INSTALL_PARSE_FAILED_NO_CERTIFICATES: Failed to collect certificates]\n");
    CHECK(v.problem == Problem::Other && v.code == "INSTALL_PARSE_FAILED_NO_CERTIFICATES");

    CHECK(install(true, "Performing Streamed Install\nSuccess\n").problem == Problem::None);
    CHECK(install(true, "Success\n").problem == Problem::None);
    // Some adb versions exit 0 on a failed install.
    CHECK(install(true, "Failure [INSTALL_FAILED_VERSION_DOWNGRADE]\n").problem == Problem::Downgrade);

    printf(failures ? "%d FAILED\n" : "adb_parse: all passed\n", failures);
    return failures ? 1 : 0;
}
