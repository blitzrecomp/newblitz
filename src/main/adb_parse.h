// Send to Android over USB (device_send.cpp): what adb's answers mean, so each common failure gets plain words and a
// way forward (team, 2026-11-15: a downgrade only ever said "Couldn't install"). Pure text in, a verdict out, for
// tests/adb_parse_test.cpp.
#pragma once
#include <cctype>
#include <cstdlib>
#include <string>

namespace blitz::adb {
    enum class Problem {
        None,          // it worked
        NoAdb,         // adb isn't on this computer (or won't run)
        NoDevice,      // nothing connected, or USB debugging off
        Unauthorized,  // connected, the USB debugging prompt not allowed yet
        Offline,       // connected but not answering (replug, unlock)
        TooMany,       // more than one device: adb won't pick
        Downgrade,     // the device has a newer build than this computer carries
        Incompatible,  // installed from somewhere else (another signature): Android won't update it
        Storage,       // the device is out of space
        Other,         // anything else (code says what adb said)
    };

    struct Verdict {
        Problem problem = Problem::Other;
        long carried = -1, installed = -1;  // Downgrade: this computer's build and the device's (when adb says)
        std::string code;                   // the INSTALL_FAILED_... or error adb gave
    };

    inline const char* name(Problem p) {
        switch (p) {
            case Problem::None: return "ok";
            case Problem::NoAdb: return "no-adb";
            case Problem::NoDevice: return "no-device";
            case Problem::Unauthorized: return "unauthorized";
            case Problem::Offline: return "offline";
            case Problem::TooMany: return "more-than-one";
            case Problem::Downgrade: return "downgrade";
            case Problem::Incompatible: return "incompatible";
            case Problem::Storage: return "storage";
            default: return "other";
        }
    }

    // `adb devices`: None when a device is ready (exactly one). started: the command ran at all.
    inline Verdict devices(bool started, const std::string& out) {
        Verdict v;
        if (!started || out.find("List of devices") == std::string::npos) {
            v.problem = Problem::NoAdb;
            return v;
        }
        int ready = 0, unauthorized = 0, offline = 0;
        size_t at = out.find("List of devices");
        at = out.find('\n', at);
        while (at != std::string::npos && at < out.size()) {
            size_t end = out.find('\n', at + 1);
            std::string line = out.substr(at + 1, end == std::string::npos ? std::string::npos : end - at - 1);
            at = end;
            size_t tab = line.find('\t');
            if (tab == std::string::npos) continue;
            std::string state = line.substr(tab + 1);
            while (!state.empty() && (state.back() == '\r' || state.back() == ' ')) state.pop_back();
            if (state == "device") ready++;
            else if (state == "unauthorized") unauthorized++;
            else if (state == "offline" || state.rfind("no permissions", 0) == 0) offline++;
        }
        v.problem = ready > 1 ? Problem::TooMany : ready == 1 ? Problem::None : unauthorized ? Problem::Unauthorized
                  : offline ? Problem::Offline : Problem::NoDevice;
        return v;
    }

    // `adb install -r`'s output (and whether it exited 0).
    inline Verdict install(bool ok, const std::string& out) {
        Verdict v;
        auto has = [&](const char* s) { return out.find(s) != std::string::npos; };
        if (ok && !has("Failure [")) {
            v.problem = Problem::None;
            return v;
        }
        if (size_t f = out.find("INSTALL_"); f != std::string::npos) {
            size_t e = f;
            while (e < out.size() && (std::isupper((unsigned char)out[e]) || out[e] == '_')) e++;
            v.code = out.substr(f, e - f);
        }
        if (has("INSTALL_FAILED_VERSION_DOWNGRADE")) {
            v.problem = Problem::Downgrade;
            // "Update version code 660 is older than current 724" (older Androids: "Package Verification Result")
            if (size_t p = out.find("version code "); p != std::string::npos) v.carried = std::strtol(out.c_str() + p + 13, nullptr, 10);
            if (size_t p = out.find("than current "); p != std::string::npos) v.installed = std::strtol(out.c_str() + p + 13, nullptr, 10);
        }
        else if (has("INSTALL_FAILED_UPDATE_INCOMPATIBLE") || has("INSTALL_FAILED_SHARED_USER_INCOMPATIBLE") ||
                 has("signatures do not match"))
            v.problem = Problem::Incompatible;
        else if (has("INSTALL_FAILED_INSUFFICIENT_STORAGE") || has("not enough space") || has("No space left"))
            v.problem = Problem::Storage;
        else if (has("more than one device"))
            v.problem = Problem::TooMany;
        else if (has("unauthorized"))
            v.problem = Problem::Unauthorized;
        else if (has("device offline") || has("device not found"))
            v.problem = Problem::Offline;
        else if (has("no devices/emulators found"))
            v.problem = Problem::NoDevice;
        else if (has("command not found") || has("is not recognized"))
            v.problem = Problem::NoAdb;
        else
            v.problem = Problem::Other;
        if (v.code.empty() && v.problem != Problem::Other) v.code = name(v.problem);
        return v;
    }
}
