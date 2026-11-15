// Blitz Builder → another device (2026-10-28): New Blitz sent from this computer to a PS5, an iPhone or an Android
// device, with the stored ROM and this computer's game setup (Season and Rules), so the device starts ready to play.
//
// The device builds come with the app: Resources/devices (`make device-kits`, then `make app-modern`):
//   ps5/PPSA99961/        the title folder; ps5/launch.elf, the payload that asks the console to start it
//   ios/NewBlitz.app.zip  the app signed for this Mac's team (zipped: the Mac app's own deep signing would re-sign it);
//                         only with DEVICE_KITS_IOS=1, as Apple's signing data names the developer. A private build.
//   ios/NewBlitz.ipa      the public app's iPhone app, signed by no one (make device-kits BLITZ_CONTENT=public): Blitz
//                         Builder saves it to Downloads with the ROM and the setup beside it, for the player to install
//                         with their own Apple ID (AltStore, SideStore, Sideloadly; docs/IOS_SIDELOAD.md). Any computer.
//   android/NewBlitz.apk
// How each goes, as the tools in tools/ do it by hand:
//   PS5      FTP (etaHEN's server, port 1337, anonymous) to /data/homebrew/<title> (unchanged files skipped by size),
//            the ROM to /data/blitzrecomp, the setup to its app folder; then launch.elf to the ELF loader (port 9021).
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
#include "../game/season_pak.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "librecomp/game.hpp"
#include "recompui/recompui.h"
#include "util/file.h"
#include "os_shell.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace blitz {
    void post_status(const std::string& msg);  // notices.cpp
}

#if defined(BLITZ_PLATFORM_DESKTOP)  // a computer sends; a phone or console is what's sent to
namespace {
    namespace fs = std::filesystem;
    std::atomic<bool> busy{false};

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

    // The public iPhone app into Downloads/New Blitz for iPhone: NewBlitz.ipa, and beside it a folder holding what the
    // app picks up from its Documents at start (main.cpp's drop folder): the ROM, this computer's setup, the season pak
    // and Blitz Builder's mark. Plus a README with the steps. No signing here: the player's own tool does that.
    bool save_ipa() {
        std::error_code ec;
        if (!fs::is_regular_file(ipa_kit(), ec)) {
            blitz::post_status("This build has no iPhone app");
            return false;
        }
        fs::path downloads = blitz::downloads_folder();
        if (downloads.empty()) {
            blitz::post_status("Couldn't find your Downloads folder");
            return false;
        }
        // BRANDING: the iPhone folder's name in Downloads
        fs::path out = downloads / "New Blitz for iPhone", phone = out / "Copy to the iPhone";
        fs::remove_all(phone, ec);  // last time's ROM and setup
        fs::create_directories(phone, ec);
        if (ec) {
            blitz::post_status("Couldn't make a folder in Downloads");
            return false;
        }
        auto copy = [&](const fs::path& from, const fs::path& to) {
            std::error_code e;
            fs::copy_file(from, to, fs::copy_options::overwrite_existing, e);
            if (e) fprintf(stderr, "[send] copy %s: %s\n", utf8(from).c_str(), e.message().c_str());
            return !e;
        };
        bool ok = copy(ipa_kit(), out / "NewBlitz.ipa");
        if (fs::is_regular_file(stored_rom(), ec)) ok = copy(stored_rom(), phone / "blitz.z64") && ok;
        for (const fs::path& f : setup_files()) ok = copy(f, phone / f.filename()) && ok;
        if (fs::path pak = season_pak_file(); !pak.empty()) ok = copy(pak, phone / pak.filename()) && ok;
        std::ofstream(phone / "builder_done") << "1\n";
        // BRANDING: the iPhone README's app name
        std::ofstream(out / "README.txt")
            << "New Blitz for iPhone and iPad\n"
               "=============================\n\n"
               "Apple lets you install apps you sign yourself. NewBlitz.ipa isn't signed by anyone: you sign it with your\n"
               "own Apple ID as you install it. A free Apple ID works; its apps need re-signing every 7 days (your\n"
               "saves stay). A paid developer account lasts a year.\n\n"
               "1. Install NewBlitz.ipa with one of these, on a Mac or a Windows PC:\n"
               "     AltStore    https://altstore.io\n"
               "     SideStore   https://sidestore.io\n"
               "     Sideloadly  https://sideloadly.io\n"
               "   Each asks for your Apple ID and signs the app for your phone. They can change the bundle ID if yours\n"
               "   must differ.\n\n"
               "2. On the phone: Settings > Privacy & Security > Developer Mode > On (the phone restarts), then\n"
               "   Settings > General > VPN & Device Management > your Apple ID > Trust.\n\n"
               "3. Give New Blitz your ROM and this setup: copy everything in \"Copy to the iPhone\" into New Blitz's\n"
               "   folder on the phone.\n"
               "     - On the phone: Files > On My iPhone > New Blitz (open New Blitz once first so the folder exists).\n"
               "       AirDrop or iCloud Drive gets the files onto the phone.\n"
               "     - From a Mac: Finder > your iPhone > Files > drag them onto New Blitz.\n"
               "     - From Windows: the Apple Devices app (or iTunes) > your iPhone > Files / File Sharing > New Blitz.\n"
               "   Then open New Blitz again: it takes the ROM, the setup and the 2026 Season (if it's there).\n"
               "   Or skip this and use Load ROM in the app.\n\n"
               "More: docs/IOS_SIDELOAD.md in the source, or the project's README.\n";
        fprintf(stderr, "[send] iPhone app saved in %s\n", utf8(out).c_str());
        blitz::post_status(ok ? "The iPhone app is in Downloads → New Blitz for iPhone"
                              : "The iPhone app is in Downloads, but the ROM or setup didn't copy");
        blitz::reveal_in_file_browser(out / "NewBlitz.ipa");
        return ok;
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
            blitz::post_status("The iPhone app isn't included here: build it with Xcode on your Mac (make ios-modern, then "
                               "tools/ios/install.sh modern)");
            return false;
        }
        std::string list;
        if (!run("xcrun devicectl list devices", &list)) {
            blitz::post_status("Sending to an iPhone needs Xcode on this Mac");
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
            blitz::post_status("No iPhone found: connect it (or pair it in Xcode) and unlock it");
            return false;
        }
        fs::path tmp = fs::temp_directory_path() / "newblitz-ios";
        fs::remove_all(tmp, ec);
        fs::create_directories(tmp, ec);
        // BRANDING: app name in Send to Device toasts (and below)
        blitz::post_status("Sending New Blitz to the iPhone…");
        if (!run("ditto -x -k " + quote(zip.string()) + " " + quote(tmp.string())) ||
            !run("xcrun devicectl device install app --device " + device + " " + quote((tmp / "NewBlitz.app").string()))) {
            blitz::post_status("Couldn't install on the iPhone (is it unlocked?)");
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
        blitz::post_status(ok ? "New Blitz is on the iPhone" : "New Blitz is on the iPhone, but the ROM or setup didn't copy");
        return ok;
    }
#else
    bool send_iphone() {  // devicectl is Xcode's: a Mac's only
        blitz::post_status("Sending straight to an iPhone needs a Mac with Xcode: save the iPhone app (.ipa) instead");
        return false;
    }
#endif

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

    bool send_android() {
        // BRANDING: kit file names
        fs::path apk = kits() / "android" / "NewBlitz.apk";
        std::error_code ec;
        if (!fs::is_regular_file(apk, ec)) {
            blitz::post_status("This build has no Android kit (make device-kits)");
            return false;
        }
        std::string adb = quote(adb_path()), devices;
        if (!run(adb + " devices", &devices) || devices.find("\tdevice") == std::string::npos) {
            blitz::post_status(devices.find("\tdevice") == std::string::npos && devices.find("List of devices") == std::string::npos
                                   ? "Sending to Android needs adb (Android's platform-tools) on this computer"
                                   : "No Android device found: connect it with USB debugging on (adb)");
            return false;
        }
        // BRANDING: app name in Send to Device toasts
        blitz::post_status("Sending New Blitz to the Android device…");
        if (!run(adb + " install -r " + quote(utf8(apk)))) {
            blitz::post_status("Couldn't install on the Android device");
            return false;
        }
        const std::string drop = "/sdcard/Android/data/com.blitzrecomp.newblitz/files/";  // android_stage.cmake
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
        blitz::post_status(ok ? "New Blitz is on the Android device" : "New Blitz is installed, but the ROM or setup didn't copy");
        return ok;
    }

    // --- FTP (PS5) ------------------------------------------------------------------------------------------------------

    class Ftp {
        int fd = -1;
        std::string host;

        static int dial(const std::string& host, int port) {
            int s = blitz::net::open_tcp();
            if (s < 0) return -1;
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = htons((uint16_t)port);
            if (inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1 || ::connect(s, (sockaddr*)&a, sizeof(a)) != 0) {
                blitz::net::close_socket(s);
                return -1;
            }
            blitz::net::set_timeouts(s, 30000);
            return s;
        }

        bool send_all(int s, const char* p, size_t n) {
            while (n > 0) {
                ssize_t k = blitz::net::send_some(s, p, n);
                if (k <= 0) return false;
                p += k;
                n -= (size_t)k;
            }
            return true;
        }

        // The server's reply code (multi-line replies read to their end); 0 on a broken connection.
        int reply(std::string* text = nullptr) {
            std::string line, first;
            for (;;) {
                line.clear();
                char c;
                while (blitz::net::recv_some(fd, &c, 1) == 1) {
                    if (c == '\n') break;
                    if (c != '\r') line += c;
                }
                if (line.size() < 3) return 0;
                if (first.empty()) first = line;
                if (line.size() >= 4 && line.compare(0, 3, first, 0, 3) == 0 && line[3] == ' ') break;
                if (first.size() >= 4 && first[3] != '-') break;
            }
            if (text) *text = first;
            return std::atoi(first.substr(0, 3).c_str());
        }

    public:
        int cmd(const std::string& c, std::string* text = nullptr) {
            std::string line = c + "\r\n";
            if (!send_all(fd, line.data(), line.size())) return 0;
            return reply(text);
        }

        bool open(const std::string& h, int port) {
            host = h;
            fd = dial(h, port);
            if (fd < 0 || reply() != 220) return false;
            int r = cmd("USER anonymous");
            if (r == 331) r = cmd("PASS blitz@");
            return r == 230 && cmd("TYPE I") == 200;
        }

        void mkdirs(const std::string& path) {
            std::string at;
            size_t i = 1;
            while (i <= path.size()) {
                size_t j = path.find('/', i);
                at = path.substr(0, j == std::string::npos ? path.size() : j);
                cmd("MKD " + at);
                if (j == std::string::npos) break;
                i = j + 1;
            }
        }

        long size_of(const std::string& path) {
            std::string t;
            int r = cmd("SIZE " + path, &t);
            static bool told = false;
            if (r != 213 && r != 550 && !told) {
                told = true;
                fprintf(stderr, "[send] SIZE answered %d (%s): every file is sent\n", r, t.c_str());
            }
            return r == 213 ? std::atol(t.c_str() + 4) : -1;
        }

        // A file up under a temporary name, then renamed into place (a cut-short send never leaves half a file).
        bool put(const std::string& data, const std::string& path) {
            std::string t;
            if (cmd("PASV", &t) != 227) return false;
            size_t l = t.find('(');
            int v[6] = {0};
            if (l == std::string::npos || sscanf(t.c_str() + l + 1, "%d,%d,%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
            int data_fd = dial(host, v[4] * 256 + v[5]);
            if (data_fd < 0) return false;
            int r = cmd("STOR " + path + ".part");
            bool ok = (r == 150 || r == 125) && send_all(data_fd, data.data(), data.size());
            blitz::net::close_socket(data_fd);
            if (!ok || reply() != 226) return false;
            cmd("DELE " + path);
            return cmd("RNFR " + path + ".part") == 350 && cmd("RNTO " + path) == 250;
        }

        void close() {
            if (fd >= 0) {
                cmd("QUIT");
                blitz::net::close_socket(fd);
                fd = -1;
            }
        }
        ~Ftp() { close(); }
    };

    bool send_ps5(const std::string& ip) {
        fs::path title = kits() / "ps5" / "PPSA99961", launch = kits() / "ps5" / "launch.elf";
        std::error_code ec;
        if (!fs::is_directory(title, ec) || !fs::is_regular_file(launch, ec)) {
            blitz::post_status("This build has no PS5 kit (make device-kits)");
            return false;
        }
        Ftp ftp;
        const char* fp = getenv("BLITZ_PS5_FTP_PORT");  // dev: a stand-in server (tests)
        const char* lp = getenv("BLITZ_PS5_LOADER_PORT");
        if (!ftp.open(ip, fp ? std::atoi(fp) : 1337)) {
            blitz::post_status("Couldn't reach the PS5 at " + ip + " (is etaHEN running?)");
            return false;
        }
        // BRANDING: app name in Send to Device toasts; /data/blitzrecomp folder
        blitz::post_status("Sending New Blitz to the PS5…");
        const std::string remote = "/data/homebrew/PPSA99961";
        int sent = 0;
        for (fs::recursive_directory_iterator it{title, ec}, end; !ec && it != end; it.increment(ec)) {
            std::string rel = fs::relative(it->path(), title, ec).generic_string();
            if (it->is_directory(ec)) {
                ftp.mkdirs(remote + "/" + rel);
                continue;
            }
            std::string dst = remote + "/" + rel;
            ftp.mkdirs(fs::path(dst).parent_path().generic_string());
            auto size = (long)fs::file_size(it->path(), ec);
            if (it->path().filename() != "eboot.bin" && ftp.size_of(dst) == size) continue;  // already there
            if (!ftp.put(read_all(it->path()), dst)) {
                blitz::post_status("The PS5 copy stopped at " + rel);
                return false;
            }
            sent++;
        }
        ftp.mkdirs("/data/blitzrecomp/Blitz2026Recompiled");
        if (fs::is_regular_file(stored_rom(), ec) && ftp.size_of("/data/blitzrecomp/blitz.z64") != (long)fs::file_size(stored_rom(), ec))
            ftp.put(read_all(stored_rom()), "/data/blitzrecomp/blitz.z64");
        bool setup_ok = true;
        if (fs::path pak = season_pak_file(); !pak.empty()) {  // installed by the app at its next start
            std::string dst = "/data/blitzrecomp/" + pak.filename().string();
            if (ftp.size_of(dst) != (long)fs::file_size(pak, ec)) setup_ok = ftp.put(read_all(pak), dst);
        }
        for (const fs::path& f : setup_files()) setup_ok = ftp.put(read_all(f), "/data/blitzrecomp/Blitz2026Recompiled/" + f.filename().string()) && setup_ok;
        if (setup_ok) ftp.put("1\n", "/data/blitzrecomp/Blitz2026Recompiled/builder_done");  // no Blitz Builder there (config.cpp)
        ftp.close();
        fprintf(stderr, "[send] PS5: %d files sent\n", sent);

        // Start it: the launch payload to the ELF loader.
        int s = blitz::net::open_tcp();
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)(lp ? std::atoi(lp) : 9021));
        bool started = s >= 0 && inet_pton(AF_INET, ip.c_str(), &a.sin_addr) == 1 && ::connect(s, (sockaddr*)&a, sizeof(a)) == 0;
        if (started) {
            std::string elf = read_all(launch);
            const char* p = elf.data();
            size_t n = elf.size();
            while (n > 0) {
                ssize_t k = blitz::net::send_some(s, p, n);
                if (k <= 0) { started = false; break; }
                p += k;
                n -= (size_t)k;
            }
        }
        if (!started) fprintf(stderr, "[send] the loader at %s:%d didn't take the payload (errno %d)\n", ip.c_str(), lp ? std::atoi(lp) : 9021, errno);
        if (s >= 0) blitz::net::close_socket(s);
        blitz::post_status(started ? "New Blitz is on the PS5 and starting" : "New Blitz is on the PS5: start it from the home screen");
        return true;
    }

    void in_background(std::function<bool()> job) {
        if (busy.exchange(true)) {
            blitz::post_status("Already sending to a device");
            return;
        }
        std::thread([job] {
            try {
                job();
            }
            catch (const std::exception& e) {
                fprintf(stderr, "[send] %s\n", e.what());
                blitz::post_status("Sending stopped: " + std::string(e.what()));
            }
            busy = false;
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
    void send_to_android() { in_background(send_android); }
}
#else
namespace blitz {
    bool device_kits_available() { return false; }
    bool iphone_kit_available() { return false; }
    bool iphone_ipa_available() { return false; }
    void save_iphone_ipa() {}
    std::string ps5_last_address() { return {}; }
    void send_to_ps5(const std::string&) {}
    void send_to_iphone() {}
    void send_to_android() {}
}
#endif
