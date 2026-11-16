// Blitz Builder → another device (2026-10-28): New Blitz sent from this computer to a PS5, an iPhone or an Android
// device, with the stored ROM and this computer's game setup (Season and Rules), so the device starts ready to play.
//
// The device builds come with the app: Resources/devices (`make device-kits`, then `make app-modern`):
//   ps5/PPSA99961/        the title folder; ps5/launch.elf, the payload that asks the console to start it
//   ios/NewBlitz.app.zip  the app signed for this Mac's team (zipped: the Mac app's own deep signing would re-sign it);
//                         only with DEVICE_KITS_IOS=1, as Apple's signing data names the developer. A private build.
//   ios/NewBlitz.ipa      the public app's iPhone app, signed by no one (make device-kits BLITZ_CONTENT=public): Blitz
//                         Builder saves a copy to Downloads with the ROM and the setup inside, for the player to install
//                         with their own Apple ID (Sideloadly is recommended; AltStore, SideStore; docs/IOS_SIDELOAD.md). Any computer.
//   android/NewBlitz.apk
// How each goes, as the tools in tools/ do it by hand:
//   PS5      FTP (etaHEN's server, port 1337, anonymous) to /data/homebrew/<title> (unchanged files skipped by the
//            console's manifest), the ROM to /data/blitzrecomp if it has none, the setup to its app folder only if
//            it isn't set up; then launch.elf to the ELF loader (port 9021). ps5_send.h has the details.
//   iPhone   Xcode's devicectl (a Mac with Xcode): install, then the ROM into the app's Documents (imported at start)
//            and the setup into its app folder. The first USB-connected or paired device.
//   Android  adb (the Android SDK's platform-tools): install, then the ROM into the app's files folder. On a Mac, Windows
//            or Linux: adb from ANDROID_HOME / ANDROID_SDK_ROOT, the SDK's usual folder there, or the PATH.
// The 2026 Season pak goes along when it's installed here (paks/season2026.nbpak, season_pak.cpp; 2026-11-13): into
// the device's drop folder beside the ROM, where the app installs it at its next start (paks_at_launch), so a public
// device kit (no licensed content inside) plays the same season with no download of its own.
// Everything runs on a thread of its own; toasts say how it went.
#include "blitz_config.h"
#include "blitz_platform.h"
#include "net_compat.h"
#include "adb_parse.h"
#include "../game/season_pak.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "librecomp/game.hpp"
#include "recompui/recompui.h"
#include "util/file.h"
#include "os_shell.h"
#include "miniz.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace blitz {
    void post_status(const std::string& msg);  // notices.cpp
}

#if defined(BLITZ_PLATFORM_DESKTOP)  // a computer sends; a phone or console is what's sent to
#include "ps5_send.h"
namespace {
    namespace fs = std::filesystem;
    std::atomic<bool> busy{false};
    // How the last send went, for first run's screens (firstrun.cpp): 0 none yet, 1 running, 2 done, 3 failed; the
    // last thing said about it; and the folder a save went to (Open Folder).
    std::atomic<int> state{0};
    std::mutex said_mutex;
    std::string said;
    fs::path saved_folder;

    // A toast, and the line the setup screen shows.
    void say(const std::string& text) {
        {
            std::lock_guard lock{said_mutex};
            said = text;
        }
        blitz::post_status(text);
    }

    fs::path kits() { return recompui::file::get_program_path() / "devices"; }

    // The stored ROM (Load ROM's copy).
    fs::path stored_rom() {
        std::u8string id = blitz::game_id;
        return recomp::get_config_path() / (id + u8".z64");
    }

    // The game setup a device takes along: Season (the game, rosters, lineups ...) and Rules.
    std::vector<fs::path> setup_files() {
        std::vector<fs::path> out;
        for (const char* f : {"season.json", "rules.json"}) {
            fs::path p = recomp::get_config_path() / f;
            std::error_code ec;
            if (fs::is_regular_file(p, ec)) out.push_back(p);
        }
        return out;
    }

    // The installed 2026 Season pak's own file, to send along; empty without one (or in a build that carries the
    // season itself: its device kits do too).
    fs::path season_pak_file() {
        std::error_code ec;
        fs::path p = blitz::paks_dir() / (std::string(blitz::season_pak_id) + ".nbpak");
        return blitz::season_pak_installed() && fs::is_regular_file(p, ec) ? p : fs::path{};
    }

    std::string read_all(const fs::path& p) {
        std::ifstream in{p, std::ios::binary};
        return {std::istreambuf_iterator<char>(in), {}};
    }

    // --- Commands (iPhone, Android) ----------------------------------------------------------------------------------

    // A path as the UTF-8 the command lines below are made of.
    std::string utf8(const fs::path& p) {
        std::u8string u = p.u8string();
        return {u.begin(), u.end()};
    }

    // One argument of a command line: a POSIX shell's single quotes; on Windows, double quotes (CreateProcess's own
    // rules; no path there can hold a double quote).
    std::string quote(const std::string& s) {
#if defined(_WIN32)
        return "\"" + s + "\"";
#else
        std::string q = "'";
        for (char c : s) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
        return q + "'";
#endif
    }

    // Runs a command line; its output (stdout and stderr) and whether it succeeded.
    bool run(const std::string& cmd, std::string* out = nullptr) {
        fprintf(stderr, "[send] %s\n", cmd.c_str());
#if defined(_WIN32)
        // CreateProcess, not _popen: no console window flashes up over the game, and no cmd.exe between (its quoting).
        auto wide = [](const std::string& u) {
            std::wstring w(MultiByteToWideChar(CP_UTF8, 0, u.c_str(), -1, nullptr, 0), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, u.c_str(), -1, w.data(), (int)w.size());
            if (!w.empty()) w.pop_back();
            return w;
        };
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE rd = nullptr, wr = nullptr;
        if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = wr;
        si.hStdError = wr;
        PROCESS_INFORMATION pi{};
        std::wstring line = wide(cmd);
        BOOL started = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        DWORD start_error = started ? 0 : GetLastError();
        CloseHandle(wr);
        std::string text;
        if (started) {
            // Read until the command exits, not until the pipe closes: `adb devices` may start adb's server, which
            // inherits the pipe and outlives the command.
            auto drain = [&] {
                char buf[512];
                DWORD avail = 0, n = 0;
                while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0 &&
                       ReadFile(rd, buf, avail < sizeof(buf) ? avail : (DWORD)sizeof(buf), &n, nullptr) && n > 0)
                    text.append(buf, n);
            };
            while (WaitForSingleObject(pi.hProcess, 50) == WAIT_TIMEOUT) drain();
            drain();
        }
        CloseHandle(rd);
        DWORD rc = 1;
        if (started) {
            GetExitCodeProcess(pi.hProcess, &rc);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
        else text = "couldn't start it (error " + std::to_string(start_error) + ")";
        if (out) *out = text;
        if (rc != 0) fprintf(stderr, "[send] exit %lu: %s\n", (unsigned long)rc, text.c_str());
        return started && rc == 0;
#else
        FILE* p = popen((cmd + " 2>&1").c_str(), "r");
        if (!p) return false;
        std::string text;
        char buf[512];
        while (fgets(buf, sizeof(buf), p)) text += buf;
        int rc = pclose(p);
        if (out) *out = text;
        if (rc != 0) fprintf(stderr, "[send] exit %d: %s\n", rc, text.c_str());
        return rc == 0;
#endif
    }

    // BRANDING: kit file names
    fs::path ipa_kit() { return kits() / "ios" / "NewBlitz.ipa"; }

    // The public iPhone app into Downloads/New Blitz for iPhone: NewBlitz.ipa with the ROM, this computer's setup, the
    // season pak and Blitz Builder's mark inside the app itself, in Payload/<app>/setup/ (team, 2026-11-15: no separate
    // copy to the phone). The app takes them in at its first start (main.cpp, iOS). Made fresh from the kit's .ipa,
    // which stays without them: the .ipa a phone downloads itself has no ROM, and asks for one with the Files picker.
    // Plus a README with the steps. No signing here: the player's own tool signs the whole app, these files with it.
    bool save_ipa() {
        std::error_code ec;
        if (!fs::is_regular_file(ipa_kit(), ec)) {
            say("This build has no iPhone app");
            return false;
        }
        fs::path downloads = blitz::downloads_folder();
        if (const char* d = getenv("BLITZ_EXPORT_DIR")) downloads = d;  // dev runs
        if (downloads.empty()) {
            say("Couldn't find your Downloads folder");
            return false;
        }
        // BRANDING: the iPhone folder's name in Downloads
        fs::path out = downloads / "New Blitz for iPhone";
        fs::remove_all(out / "Copy to the iPhone", ec);  // an older save's separate folder
        fs::create_directories(out, ec);
        if (ec) {
            say("Couldn't make a folder in Downloads");
            return false;
        }
        mz_zip_archive in{}, zip{};
        if (!mz_zip_reader_init_file(&in, utf8(ipa_kit()).c_str(), 0)) {
            say("The iPhone app in this build can't be read");
            return false;
        }
        // The app's folder in the archive: Payload/<name>.app/
        std::string app;
        for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&in) && app.empty(); i++) {
            char name[1024];
            mz_zip_reader_get_filename(&in, i, name, sizeof(name));
            std::string n = name;
            size_t dot = n.find(".app/");
            if (n.rfind("Payload/", 0) == 0 && dot != std::string::npos) app = n.substr(0, dot + 5);
        }
        fs::path tmp = out / "NewBlitz.ipa.part", ipa = out / "NewBlitz.ipa";
        bool ok = !app.empty() && mz_zip_writer_init_file(&zip, utf8(tmp).c_str(), 0);
        for (mz_uint i = 0; ok && i < mz_zip_reader_get_num_files(&in); i++) {
            char name[1024];
            mz_zip_reader_get_filename(&in, i, name, sizeof(name));
            if (std::string(name).rfind(app + "setup/", 0) == 0) continue;  // none in a kit; never two copies
            ok = mz_zip_writer_add_from_zip_reader(&zip, &in, i);
        }
        auto add = [&](const fs::path& from, const std::string& name) {
            if (!ok) return;
            std::string data = read_all(from);
            ok = !data.empty() && mz_zip_writer_add_mem(&zip, (app + "setup/" + name).c_str(), data.data(), data.size(),
                                                         name.ends_with(".z64") || name.ends_with(".nbpak") ? MZ_NO_COMPRESSION : MZ_DEFAULT_COMPRESSION);
        };
        bool has_rom = fs::is_regular_file(stored_rom(), ec);
        if (has_rom) add(stored_rom(), "blitz.z64");
        for (const fs::path& f : setup_files()) add(f, f.filename().string());
        if (fs::path pak = season_pak_file(); !pak.empty()) add(pak, pak.filename().string());
        // The mark says when: an .ipa made again (another setup) is taken in again; the same one re-signed isn't.
        std::string mark = "1 " + std::to_string((long long)std::time(nullptr)) + "\n";
        if (ok) ok = mz_zip_writer_add_mem(&zip, (app + "setup/builder_done").c_str(), mark.data(), mark.size(), MZ_DEFAULT_COMPRESSION);
        if (ok) ok = mz_zip_writer_finalize_archive(&zip);
        mz_zip_writer_end(&zip);
        mz_zip_reader_end(&in);
        if (ok) {
            fs::remove(ipa, ec);
            fs::rename(tmp, ipa, ec);
            ok = !ec;
        }
        if (!ok) {
            fs::remove(tmp, ec);
            say("Couldn't make the iPhone app with your ROM inside");
            return false;
        }
        // BRANDING: the iPhone README's app name
        std::ofstream(out / "README.txt")
            << "New Blitz for iPhone and iPad\n"
               "=============================\n\n"
               "NewBlitz.ipa holds your ROM and this computer's setup. Don't share it.\n\n"
               "Apple lets you install apps you sign yourself. NewBlitz.ipa isn't signed by anyone: you sign it with your\n"
               "own Apple ID as you install it. A free Apple ID works. Its install lasts 7 days, and Sideloadly can\n"
               "renew it for you. A paid developer account lasts a year. A free one allows 3 sideloaded apps per device.\n\n"
               "Recommended: Sideloadly, https://sideloadly.io (Mac or Windows)\n"
               "1. Install Sideloadly. On Windows it needs Apple's iTunes and iCloud from apple.com (the web versions,\n"
               "   not the Microsoft Store ones).\n"
               "2. Plug the iPhone in with a cable.\n"
               "3. Drag NewBlitz.ipa into Sideloadly and sign in with your own Apple ID. You type the password into\n"
               "   Sideloadly. It's never shared with New Blitz.\n"
               "4. Before you start, turn on AutoRefresh: the circular-arrows button left of Start.\n"
               "5. On the iPhone: Settings > General > VPN & Device Management > trust your Apple ID. Turn on\n"
               "   Developer Mode if iOS asks.\n"
               "6. Turn on Wi-Fi sync for the phone. Mac: Finder > select the iPhone > \"Show this iPhone when on\n"
               "   Wi-Fi\". Windows: the same option in iTunes.\n"
               "7. Open New Blitz. It starts with your ROM and this setup, ready to play.\n\n"
               "With AutoRefresh on, Sideloadly renews the app in the background whenever the computer is on and\n"
               "the phone is on the same Wi-Fi. If it ever lapses, plug in and click Start again in Sideloadly.\n\n"
               "Other options: AltStore https://altstore.io (also refreshes over Wi-Fi from a computer), SideStore\n"
               "https://sidestore.io (refreshes on the phone, but setup is fiddlier).\n\n"
               "More: newblitz.com/start, or docs/IOS_SIDELOAD.md in the source.\n";
        fprintf(stderr, "[send] iPhone app saved in %s (%s)\n", utf8(out).c_str(), has_rom ? "with the ROM inside" : "no ROM stored");
        saved_folder = out;
        say("The iPhone app is in Downloads → New Blitz for iPhone");
        blitz::reveal_in_file_browser(ipa);
        return true;
    }

#if defined(BLITZ_MACOS)
    bool send_iphone() {
        // BRANDING: kit file names
        fs::path zip = kits() / "ios" / "NewBlitz.app.zip";
        std::error_code ec;
        if (!fs::is_regular_file(zip, ec)) {
            // The usual case: the Mac app doesn't carry a signed iPhone build (Makefile, DEVICE_KITS_IOS).
            fprintf(stderr, "[send] no iPhone kit in %s: build it with Xcode (make ios-modern, then tools/ios/install.sh modern)\n",
                    kits().string().c_str());
            say("The iPhone app isn't included here: build it with Xcode on your Mac (make ios-modern, then "
                               "tools/ios/install.sh modern)");
            return false;
        }
        std::string list;
        if (!run("xcrun devicectl list devices", &list)) {
            say("Sending to an iPhone needs Xcode on this Mac");
            return false;
        }
        // The first real device that's reachable (USB or paired over Wi-Fi), by its id (8-16 hex).
        std::string device;
        size_t line_start = 0;
        while (line_start < list.size() && device.empty()) {
            size_t end = list.find('\n', line_start);
            std::string line = list.substr(line_start, end == std::string::npos ? std::string::npos : end - line_start);
            line_start = end == std::string::npos ? list.size() : end + 1;
            // "connected" (USB) or "available (paired)" (Wi-Fi), never "unavailable" or a simulator
            bool up = line.find("connected") != std::string::npos ||
                      (line.find("available") != std::string::npos && line.find("unavailable") == std::string::npos);
            if (!up || line.find("simulat") != std::string::npos) continue;
            for (size_t i = 0; i + 25 <= line.size(); i++) {
                auto hex = [&](size_t a, size_t n) {
                    for (size_t k = a; k < a + n; k++) if (!std::isxdigit((unsigned char)line[k])) return false;
                    return true;
                };
                if (hex(i, 8) && line[i + 8] == '-' && hex(i + 9, 16)) {
                    device = line.substr(i, 25);
                    break;
                }
            }
        }
        if (device.empty()) {
            say("No iPhone found: connect it (or pair it in Xcode) and unlock it");
            return false;
        }
        fs::path tmp = fs::temp_directory_path() / "newblitz-ios";
        fs::remove_all(tmp, ec);
        fs::create_directories(tmp, ec);
        // BRANDING: app name in Send to Device toasts (and below)
        say("Sending New Blitz to the iPhone…");
        if (!run("ditto -x -k " + quote(zip.string()) + " " + quote(tmp.string())) ||
            !run("xcrun devicectl device install app --device " + device + " " + quote((tmp / "NewBlitz.app").string()))) {
            say("Couldn't install on the iPhone (is it unlocked?)");
            return false;
        }
        const std::string bundle = "com.blitzrecomp.newblitz";  // platform/ios/ios_bundle.cmake
        auto copy_to = [&](const fs::path& src, const std::string& dst) {
            return run("xcrun devicectl device copy to --device " + device + " --domain-type appDataContainer --domain-identifier " +
                       bundle + " --source " + quote(src.string()) + " --destination " + quote(dst));
        };
        bool ok = !fs::is_regular_file(stored_rom(), ec) || copy_to(stored_rom(), "Documents/blitz.z64");
        // The setup goes into Documents with the ROM: a new install has no app folder until it first runs (devicectl
        // doesn't make one). The app moves them in at start (main.cpp); builder_done skips its Blitz Builder (config.cpp).
        for (const fs::path& f : setup_files()) ok = copy_to(f, "Documents/" + f.filename().string()) && ok;
        if (fs::path pak = season_pak_file(); !pak.empty()) ok = copy_to(pak, "Documents/" + pak.filename().string()) && ok;
        if (ok) {
            std::ofstream(tmp / "builder_done") << "1\n";
            ok = copy_to(tmp / "builder_done", "Documents/builder_done");
        }
        fs::remove_all(tmp, ec);
        say(ok ? "New Blitz is on the iPhone" : "New Blitz is on the iPhone, but the ROM or setup didn't copy");
        return ok;
    }
#else
    bool send_iphone() {  // devicectl is Xcode's: a Mac's only
        say("Sending straight to an iPhone needs a Mac with Xcode: save the iPhone app (.ipa) instead");
        return false;
    }
#endif

    // BRANDING: kit file names
    fs::path apk_kit() { return kits() / "android" / "NewBlitz.apk"; }

    // Save the APK (first run's Android, without USB): Downloads/New Blitz for Android holds NewBlitz.apk, and beside
    // it "Copy to the device" with what the app picks up from its files folder at start (main.cpp's drop folder): the
    // ROM, this computer's setup, the season pak and Blitz Builder's mark. A README says the steps.
    bool save_apk() {
        std::error_code ec;
        if (!fs::is_regular_file(apk_kit(), ec)) {
            say("This build has no Android app");
            return false;
        }
        fs::path downloads = blitz::downloads_folder();
        if (const char* d = getenv("BLITZ_EXPORT_DIR")) downloads = d;  // dev runs
        if (downloads.empty()) {
            say("Couldn't find your Downloads folder");
            return false;
        }
        // BRANDING: the Android folder's name in Downloads
        fs::path out = downloads / "New Blitz for Android", device = out / "Copy to the device";
        fs::remove_all(device, ec);  // last time's ROM and setup
        fs::create_directories(device, ec);
        if (ec) {
            say("Couldn't make a folder in Downloads");
            return false;
        }
        auto copy = [&](const fs::path& from, const fs::path& to) {
            std::error_code e;
            fs::copy_file(from, to, fs::copy_options::overwrite_existing, e);
            if (e) fprintf(stderr, "[send] copy %s: %s\n", utf8(from).c_str(), e.message().c_str());
            return !e;
        };
        bool ok = copy(apk_kit(), out / "NewBlitz.apk");
        if (fs::is_regular_file(stored_rom(), ec)) ok = copy(stored_rom(), device / "blitz.z64") && ok;
        for (const fs::path& f : setup_files()) ok = copy(f, device / f.filename()) && ok;
        if (fs::path pak = season_pak_file(); !pak.empty()) ok = copy(pak, device / pak.filename()) && ok;
        std::ofstream(device / "builder_done") << "1\n";
        // BRANDING: the Android README's app name
        std::ofstream(out / "README.txt")
            << "New Blitz for Android\n"
               "=====================\n\n"
               "1. Copy NewBlitz.apk to the device (USB, a cloud folder, or a card) and open it there. Android asks you to\n"
               "   allow installs from that source.\n\n"
               "2. Open New Blitz once, then close it (that makes its folder).\n\n"
               "3. Copy everything in \"Copy to the device\" into New Blitz's folder on the device:\n"
               "     Android/data/com.blitzrecomp.newblitz/files\n"
               "   (a file manager, or Android File Transfer / Explorer over USB). It holds your ROM: don't share it.\n\n"
               "4. Open New Blitz again: it starts with your ROM, this setup and the 2026 Season (if it's there).\n";
        fprintf(stderr, "[send] Android app saved in %s\n", utf8(out).c_str());
        saved_folder = out;
        say(ok ? "The Android app is in Downloads → New Blitz for Android"
               : "The Android app is in Downloads, but the ROM or setup didn't copy");
        return ok;
    }

    // adb: the SDK's platform-tools where each computer's Android Studio puts them, else the PATH.
    std::string adb_path() {
#if defined(_WIN32)
        const char* exe = "adb.exe";
#else
        const char* exe = "adb";
#endif
        std::vector<fs::path> tries;
        for (const char* v : {"ANDROID_HOME", "ANDROID_SDK_ROOT"})
            if (const char* h = getenv(v); h && *h) tries.push_back(fs::path(h) / "platform-tools" / exe);
#if defined(_WIN32)
        if (const char* h = getenv("LOCALAPPDATA"); h && *h) tries.push_back(fs::path(h) / "Android" / "Sdk" / "platform-tools" / exe);
#elif defined(BLITZ_MACOS)
        if (const char* h = getenv("HOME"); h && *h) tries.push_back(fs::path(h) / "Library" / "Android" / "sdk" / "platform-tools" / exe);
#else
        if (const char* h = getenv("HOME"); h && *h) tries.push_back(fs::path(h) / "Android" / "Sdk" / "platform-tools" / exe);
#endif
        std::error_code ec;
        for (const fs::path& p : tries) if (fs::is_regular_file(p, ec)) return utf8(p);
        return "adb";
    }

    // The last Android send's problem (adb_parse.h's name: "downgrade", "incompatible", "no-device" ...), for the
    // Builder's failure screen to offer the way forward; "" when there was none.
    std::string android_problem;

    // In words, with what to do next (team, 2026-11-15: a downgrade only said "Couldn't install").
    // BRANDING: app name in Send to Android's messages
    std::string android_words(const blitz::adb::Verdict& v) {
        using P = blitz::adb::Problem;
        const std::string allow = "Connect the device by USB, unlock it, and tap Allow on the USB debugging prompt.";
        switch (v.problem) {
            case P::NoAdb:
                return "Sending over USB needs adb (Android's platform-tools) on this computer. How to get it: "
                       "newblitz.com/start, under Android. Or save the APK and copy it over yourself.";
            case P::NoDevice:
                return "No Android device found. " + allow +
                       " (USB debugging: Settings \xE2\x86\x92 About \xE2\x86\x92 tap Build number 7 times, then Developer options.)";
            case P::Unauthorized: return "The device hasn't allowed this computer yet. " + allow;
            case P::Offline: return "The device isn't answering. " + allow + " If it stays this way, unplug it and plug it in again.";
            case P::TooMany: return "More than one Android device is connected. Unplug all but the one to send to.";
            case P::Downgrade:
                if (v.installed > 0 && v.carried > 0)
                    return "This device already has a newer New Blitz (build " + std::to_string(v.installed) + ") than this "
                           "computer carries (" + std::to_string(v.carried) + "). Update New Blitz on this computer, or keep the "
                           "one on the device.";
                return "This device already has a newer New Blitz than this computer carries. Update New Blitz on this "
                       "computer, or keep the one on the device.";
            case P::Incompatible:
                return "The New Blitz on this device was installed from somewhere else, so Android won't update it. Remove it "
                       "and install this one? Its saves on the device will be lost.";
            case P::Storage: return "The device is out of space for New Blitz. Free up some space on it, then try again.";
            default:
                return "Couldn't install on the Android device" + (v.code.empty() ? std::string(".") : " (" + v.code + ").");
        }
    }

    bool android_failed(const blitz::adb::Verdict& v, const char* at) {
        {
            std::lock_guard lock{said_mutex};
            android_problem = blitz::adb::name(v.problem);
        }
        fprintf(stderr, "[send] android %s: problem=%s code=%s carried=%ld installed=%ld\n", at, blitz::adb::name(v.problem),
                v.code.c_str(), v.carried, v.installed);
        say(android_words(v));
        return false;
    }

    // replace: remove the device's New Blitz first (another signature: Android won't update it; its saves go too).
    bool send_android(bool replace) {
        {
            std::lock_guard lock{said_mutex};
            android_problem.clear();
        }
        // BRANDING: kit file names
        fs::path apk = kits() / "android" / "NewBlitz.apk";
        std::error_code ec;
        if (!fs::is_regular_file(apk, ec)) {
            say("This build has no Android kit (make device-kits)");
            return false;
        }
        std::string adb = quote(adb_path()), devices;
        bool started = run(adb + " devices", &devices);
        if (blitz::adb::Verdict v = blitz::adb::devices(started, devices); v.problem != blitz::adb::Problem::None)
            return android_failed(v, "devices");
        const std::string package = "com.blitzrecomp.newblitz";  // android_stage.cmake
        if (replace) {
            std::string out;
            bool removed = run(adb + " uninstall " + package, &out);
            fprintf(stderr, "[send] android: removed the device's New Blitz first (%s)\n", removed ? "done" : out.c_str());
        }
        // BRANDING: app name in Send to Device toasts
        say("Sending New Blitz to the Android device\xE2\x80\xA6");
        std::string out;
        bool installed = run(adb + " install -r " + quote(utf8(apk)), &out);
        if (blitz::adb::Verdict v = blitz::adb::install(installed, out); v.problem != blitz::adb::Problem::None)
            return android_failed(v, "install");
        const std::string drop = "/sdcard/Android/data/" + package + "/files/";
        bool ok = !fs::is_regular_file(stored_rom(), ec) || run(adb + " push " + quote(utf8(stored_rom())) + " " + drop + "blitz.z64");
        // The setup and Blitz Builder's mark, as to an iPhone: the app moves them into its folder at start (main.cpp).
        for (const fs::path& f : setup_files()) ok = run(adb + " push " + quote(utf8(f)) + " " + drop + f.filename().string()) && ok;
        if (fs::path pak = season_pak_file(); !pak.empty()) ok = run(adb + " push " + quote(utf8(pak)) + " " + drop + pak.filename().string()) && ok;
        if (ok) {
            fs::path mark = fs::temp_directory_path() / "newblitz-builder_done";
            std::ofstream(mark) << "1\n";
            ok = run(adb + " push " + quote(utf8(mark)) + " " + drop + "builder_done");
            fs::remove(mark, ec);
        }
        say(ok ? "New Blitz is on the Android device" : "New Blitz is installed, but the ROM or setup didn't copy");
        return ok;
    }

    // --- PS5 (ps5_send.h) ------------------------------------------------------------------------------------------

    blitz::ps5::Progress ps5_progress;

    bool send_ps5(const std::string& ip) {
        fs::path title = kits() / "ps5" / "PPSA99961", launch = kits() / "ps5" / "launch.elf";
        std::error_code ec;
        if (!fs::is_directory(title, ec) || !fs::is_regular_file(launch, ec)) {
            say("This build has no PS5 kit (make device-kits)");
            return false;
        }
        blitz::ps5::Job job;
        job.host = ip;
        if (const char* fp = getenv("BLITZ_PS5_FTP_PORT")) job.ftp_port = std::atoi(fp);  // dev: a stand-in server (tests)
        if (const char* lp = getenv("BLITZ_PS5_LOADER_PORT")) job.loader_port = std::atoi(lp);
        job.title_dir = title;
        job.launch_elf = launch;
        job.rom = stored_rom();
        job.rom_name = utf8(stored_rom().filename());
        job.pak = season_pak_file();
        job.setup = setup_files();
        // BRANDING: app name in Send to Device toasts; /data/blitzrecomp folder
        blitz::ps5::Result r = blitz::ps5::send(job, ps5_progress, [](const std::string& line) {
            fprintf(stderr, "[ps5] %s\n", line.c_str());
        });
        switch (r.stage) {
            case blitz::ps5::Result::Unreachable:
                say("Nothing answered at " + ip + ". Check the address (Settings \xE2\x86\x92 Network on the PS5) and that the "
                    "PS5 is on and on the same network.");
                return false;
            case blitz::ps5::Result::Refused:
                say("The PS5 at " + ip + " answered, but its FTP server didn't: is etaHEN running?");
                return false;
            case blitz::ps5::Result::CopyStopped:
                say("The PS5 copy stopped at " + r.stopped_at + ". Try again: what's already there is kept.");
                return false;
            default:
                break;
        }
        std::string text = !r.launched ? "New Blitz is on the PS5. Start it from the home screen."
                         : r.restarted == 0 ? "New Blitz is on the PS5. If the PS5 still shows the old game, close it and open New "
                                              "Blitz from the home screen."
                                            : "New Blitz is starting on your PS5.";
        if (!r.setup_ok) text += " The ROM or the setup didn't copy: send again to finish.";
        else if (r.set_up_already) text += " The PS5 kept its own Season and Rules settings.";
        say(text);
        return true;
    }

    void in_background(std::function<bool()> job) {
        if (busy.exchange(true)) {
            say("Already sending to a device");
            return;
        }
        state = 1;
        {
            std::lock_guard lock{said_mutex};
            said.clear();
        }
        std::thread([job] {
            bool ok = false;
            try {
                ok = job();
            }
            catch (const std::exception& e) {
                fprintf(stderr, "[send] %s\n", e.what());
                say("Sending stopped: " + std::string(e.what()));
            }
            busy = false;
            state = ok ? 2 : 3;
        }).detach();
    }

    fs::path ps5_address_file() { return recomp::get_config_path() / "devices_ps5_address.txt"; }
}

namespace blitz {
    // Whether this app carries device kits (Builder offers "Another device" only then).
    bool device_kits_available() {
        std::error_code ec;
        return fs::is_directory(kits(), ec);
    }

    // Whether this computer can send the iPhone app straight to a phone: a Mac app built with DEVICE_KITS_IOS=1 (signed
    // for its developer's team, so never a public one) on a Mac with Xcode's devicectl.
    bool iphone_kit_available() {
#if defined(BLITZ_MACOS)
        std::error_code ec;
        return fs::is_regular_file(kits() / "ios" / "NewBlitz.app.zip", ec);
#else
        return false;
#endif
    }

    // Whether the kits hold the unsigned iPhone app (the public build's) for Save the iPhone app (.ipa).
    bool iphone_ipa_available() {
        std::error_code ec;
        return fs::is_regular_file(ipa_kit(), ec);
    }

    // The public build's Android app is in the kits (Save the APK; Send over USB too).
    bool android_kit_available() {
        std::error_code ec;
        return fs::is_regular_file(apk_kit(), ec);
    }

    int device_send_state() { return state.load(); }
    // The line under "Sending to your PS5…" (connecting, checking, "Sending 12 of 23 files · ..."); "" otherwise.
    std::string device_send_progress() { return ps5_progress.text(); }
    std::string device_send_message() {
        std::lock_guard lock{said_mutex};
        return said;
    }
    std::filesystem::path device_send_folder() { return saved_folder; }

    std::string ps5_last_address() {
        std::string a = read_all(ps5_address_file());
        while (!a.empty() && std::isspace((unsigned char)a.back())) a.pop_back();
        return a.empty() ? "192.168.0." : a;
    }

    void send_to_ps5(const std::string& ip) {
        std::ofstream(ps5_address_file()) << ip;
        in_background([ip] { return send_ps5(ip); });
    }
    void send_to_iphone() { in_background(send_iphone); }
    void save_iphone_ipa() { in_background(save_ipa); }
    void save_android_apk() { in_background(save_apk); }
    void send_to_android() { in_background([] { return send_android(false); }); }
    void send_to_android_replacing() { in_background([] { return send_android(true); }); }
    std::string device_send_problem() {
        std::lock_guard lock{said_mutex};
        return busy ? std::string() : android_problem;
    }
}
#else
namespace blitz {
    bool device_kits_available() { return false; }
    bool iphone_kit_available() { return false; }
    bool iphone_ipa_available() { return false; }
    bool android_kit_available() { return false; }
    void save_iphone_ipa() {}
    void save_android_apk() {}
    int device_send_state() { return 3; }
    std::string device_send_message() { return "This device can't send to another."; }
    std::string device_send_progress() { return {}; }
    std::filesystem::path device_send_folder() { return {}; }
    std::string ps5_last_address() { return {}; }
    void send_to_ps5(const std::string&) {}
    void send_to_iphone() {}
    void send_to_android() {}
    void send_to_android_replacing() {}
    std::string device_send_problem() { return {}; }
}
#endif
