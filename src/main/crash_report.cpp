// Crash reports (docs/design/FIRST_RUN_V2.md §6.2; the team's answer §12.3): off unless the player ticks Send Crash Reports
// (first run's updates screen, or Settings → General). When on, the start after a crash sends one small text report,
// silently, in the background: no prompt, no toast. Settings → General → What Crash Reports Send shows exactly what
// went last (or what one would hold, made from this run's log).
//
// A crash is a [crash] block in the last run's log (app_log.cpp writes it on every platform: a signal's call stack, or
// Windows' exception and module+offset frames). The report holds:
//   - the app's version, build and commit; the platform, its OS version and model where the system says (Apple);
//   - the graphics line the renderer printed;
//   - the [crash] block and its call stack (offsets and the app's own file names only);
//   - the [firstrun] lines;
//   - the last 200 lines of the log.
// Every line has the player's home folder and account name taken out (~ and <user>), any file name ending in a ROM or
// pak extension made <file>, and addresses like 192.168.1.20 made <address>; at most 256 KB. Never the ROM, a save,
// a settings file or a minidump (they hold memory, which can hold the ROM).
//
// It goes to the season server's Worker (tools/rosters_hub_worker, the route POST /reports/crash; DEPLOY.md there),
// whose address the build carries as the season server's (Resources/paks/server, season_pak.cpp): the route below is
// the only part in the source. Couldn't connect: it tries again at the next start, three times at most.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

#include "blitz_config.h"
#include "blitz_host.h"
#include "blitz_platform.h"
#include "../game/season_pak.h"
#include "recompui/config.h"
#include "util/file.h"

namespace {
    namespace fs = std::filesystem;

    // The one address part in the source: the route on the season server (its base, without "paks/", from the build's
    // own file). BLITZ_CRASH_REPORT_URL replaces the whole address (tests).
    constexpr const char* crash_report_route = "reports/crash";
    constexpr size_t max_report = 256 * 1024;
    constexpr int log_lines = 200;
    constexpr int max_tries = 3;

    fs::path folder() { return recompui::file::get_app_folder_path() / "crash_reports"; }
    fs::path pending_file() { return folder() / "pending.txt"; }
    fs::path tries_file() { return folder() / "pending.tries"; }
    fs::path sent_file() { return folder() / "last_sent.txt"; }

    std::string read_file(const fs::path& p) {
        std::ifstream in{p, std::ios::binary};
        return {std::istreambuf_iterator<char>(in), {}};
    }

    std::vector<std::string> lines_of(const std::string& text) {
        std::vector<std::string> out;
        std::stringstream ss{text};
        std::string line;
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            out.push_back(line);
        }
        return out;
    }

    std::string endpoint() {
        if (const char* e = getenv("BLITZ_CRASH_REPORT_URL"); e != nullptr) return e;
        std::string server = blitz::pak_server();  // ".../paks/"
        if (server.empty()) return {};
        const std::string tail = "paks/";
        if (server.size() >= tail.size() && server.compare(server.size() - tail.size(), tail.size(), tail) == 0)
            server.resize(server.size() - tail.size());
        return server + crash_report_route;
    }

    void replace_all(std::string& s, const std::string& from, const std::string& to) {
        if (from.empty()) return;
        for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size())) s.replace(at, from.size(), to);
    }

    // The player's folders, account name, ROM and pak file names and addresses out of a line.
    std::string scrub(std::string line) {
        for (const char* var : {"HOME", "USERPROFILE"}) {
            const char* home = getenv(var);
            if (home != nullptr && strlen(home) > 1) replace_all(line, home, "~");
        }
        static const std::regex user_dirs(R"((/Users/|/home/|[A-Za-z]:\\Users\\|[A-Za-z]:/Users/)[^/\\\s"']+)");
        line = std::regex_replace(line, user_dirs, "~");
        for (const char* var : {"USER", "USERNAME", "LOGNAME"}) {
            const char* user = getenv(var);
            if (user != nullptr && strlen(user) >= 3) replace_all(line, user, "<user>");
        }
        // A line naming a ROM or pak file (names hold spaces and anything else): only its [tag] stays.
        static const std::regex rom_names(R"(\.(z64|n64|v64|nbpak|zip)\b)", std::regex::icase);
        if (std::regex_search(line, rom_names)) {
            size_t tag = line.rfind(']', line.find(' ') == std::string::npos ? line.size() : line.find(' '));
            line = (line.rfind('[', 0) == 0 && tag != std::string::npos ? line.substr(0, tag + 1) + " " : std::string()) +
                   "(a line naming a ROM or pak file, left out)";
        }
        static const std::regex ipv4(R"(\b\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}\b)");
        line = std::regex_replace(line, ipv4, "<address>");
        return line;
    }

    std::string system_line() {
        std::string out = std::string(blitz::platform::name);
#if defined(__APPLE__)
        auto sys = [](const char* name) {
            char buf[256] = {};
            size_t len = sizeof(buf) - 1;
            return sysctlbyname(name, buf, &len, nullptr, 0) == 0 ? std::string(buf) : std::string("?");
        };
        out += " " + sys("kern.osproductversion") + " (" + sys("kern.osversion") + "), " + sys("hw.model");
#endif
        return out;
    }

    // A report from a log's text. crash: whether it ended in a crash (else an example, for the Settings view).
    std::string build_report(const std::string& log, bool& crash) {
        std::vector<std::string> lines = lines_of(log);
        size_t crash_at = lines.size();
        for (size_t i = 0; i < lines.size(); i++) {
            if (lines[i].find("[crash]") != std::string::npos) {
                crash_at = i;
                break;
            }
        }
        crash = crash_at < lines.size();
        std::string graphics;
        std::vector<std::string> firstrun;
        for (const std::string& l : lines) {
            // RT64 prints "Device Name: Apple M3 Pro" (and its Vulkan devices' names on Windows, Linux and Android).
            if (graphics.empty() && (l.rfind("Device Name:", 0) == 0 || l.find("GPU:") != std::string::npos))
                graphics = l;
            if (l.find("[firstrun]") != std::string::npos && firstrun.size() < 80) firstrun.push_back(l);
        }
        // BRANDING: the crash report's first line
        std::string head = std::string("New Blitz crash report\n") +
                           "app: " BLITZ_VERSION " build " BLITZ_BUILD " (" BLITZ_COMMIT ")\n" +
                           "system: " + system_line() + "\n" +
                           "graphics: " + scrub(graphics) + "\n";
        std::string body = "--- crash ---\n";
        if (!crash) body += "(no crash in this log: an example of what a report holds)\n";
        else for (size_t i = crash_at; i < lines.size() && i < crash_at + 120; i++) body += scrub(lines[i]) + "\n";
        body += "--- first run ---\n";
        for (const std::string& l : firstrun) body += scrub(l) + "\n";
        body += "--- log, last lines ---\n";
        std::string tail;
        size_t from = lines.size() > (size_t)log_lines ? lines.size() - log_lines : 0;
        for (size_t i = from; i < lines.size(); i++) tail += scrub(lines[i]) + "\n";
        std::string out = head + body;
        // At most 256 KB: the log's tail gives way first, from its oldest end.
        if (out.size() + tail.size() > max_report) {
            size_t room = out.size() < max_report ? max_report - out.size() : 0;
            tail = tail.substr(tail.size() - room);
            size_t nl = tail.find('\n');
            if (nl != std::string::npos) tail = tail.substr(nl + 1);
        }
        out += tail;
        if (out.size() > max_report) out.resize(max_report);
        return out;
    }

    std::atomic<bool> sending{false};

    void send_pending() {
        if (sending.exchange(true)) return;
        std::thread([] {
            std::error_code ec;
            std::string body = read_file(pending_file());
            std::string url = endpoint();
            int tries = std::atoi(read_file(tries_file()).c_str()) + 1;
            int status = 0;
#if defined(BLITZ_PLATFORM_PS5)
            // No crash reports from the PS5 (docs/design/FIRST_RUN_V2.md §2.3), though its http_post works since Cloud
            // Sync needed one (2026-11-27).
            bool ok = false;
            url.clear();
#else
            bool ok = !body.empty() && !url.empty() && blitz::http_post(url, "text/plain; charset=utf-8", body, status);
#endif
            if (ok) {
                fs::rename(pending_file(), sent_file(), ec);
                fs::remove(tries_file(), ec);
                fprintf(stderr, "[crash-report] sent (%zu bytes)\n", body.size());
            }
            else if (url.empty()) {
                fprintf(stderr, "[crash-report] this build has no server to send to; kept\n");
            }
            else if (tries >= max_tries) {
                fs::remove(pending_file(), ec);
                fs::remove(tries_file(), ec);
                fprintf(stderr, "[crash-report] not sent (HTTP %d), the last of %d tries: dropped\n", status, max_tries);
            }
            else {
                std::ofstream(tries_file()) << tries;
                fprintf(stderr, "[crash-report] not sent (HTTP %d): again at the next start (%d of %d)\n", status, tries, max_tries);
            }
            sending = false;
        }).detach();
    }

    std::vector<std::string> blocks_of(const std::string& text) {
        std::vector<std::string> out;
        std::vector<std::string> lines = lines_of(text);
        std::string block;
        int n = 0;
        for (const std::string& l : lines) {
            block += l + "\n";
            if (++n == 12) {
                out.push_back(block);
                block.clear();
                n = 0;
            }
        }
        if (!block.empty()) out.push_back(block);
        return out;
    }
}

namespace blitz {
    bool crash_reports_on() {
        return std::get<bool>(recompui::config::get_general_config().get_option_value("crash_reports"));
    }

    void set_crash_reports(bool on) {
        recomp::config::Config& g = recompui::config::get_general_config();
        g.update_option_value("crash_reports", on);
        g.save_config();
        fprintf(stderr, "[crash-report] Send Crash Reports %s\n", on ? "on" : "off");
    }

    // main.cpp, once the settings are loaded: the last run crashed (its log, blitz.prev.log, ends in a [crash] block)
    // and reports are on: one report, kept in crash_reports/pending.txt until it's sent.
    void crash_report_at_start() {
        if (!crash_reports_on()) return;
        std::error_code ec;
        fs::path prev = recompui::file::get_app_folder_path() / "blitz.prev.log";
        std::string log = read_file(prev);
        // A crash on the way out (Settings → Restart and Main Menu end the app and start it again; the frontend's
        // shutdown can fault after the game has stopped, 2026-11-15) lost nothing: no report for it.
        size_t crash_at = log.find("[crash]");
        size_t closing = std::min(log.find("Unable to shutdown debugger plugin"), log.find("[firstrun] main menu: leaving"));
        if (crash_at != std::string::npos && closing < crash_at) {
            fprintf(stderr, "[crash-report] the last run crashed while closing: not reported\n");
        }
        else if (crash_at != std::string::npos) {
            bool crash = false;
            std::string report = build_report(log, crash);
            fs::create_directories(folder(), ec);
            std::ofstream(pending_file(), std::ios::binary) << report;
            fs::remove(tries_file(), ec);
            fprintf(stderr, "[crash-report] the last run crashed: a report waits to be sent (%zu bytes)\n", report.size());
        }
        if (fs::is_regular_file(pending_file(), ec)) send_pending();
    }

    // What Crash Reports Send (Settings, first run): the last report sent, or one made from this run's log as an
    // example. In blocks for the screen.
    std::vector<std::string> crash_report_preview(bool& sent) {
        std::error_code ec;
        sent = fs::is_regular_file(sent_file(), ec);
        if (sent) return blocks_of(read_file(sent_file()));
        bool crash = false;
        return blocks_of(build_report(read_file(recompui::file::get_app_folder_path() / "blitz.log"), crash));
    }
}
