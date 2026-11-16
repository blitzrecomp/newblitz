// Blitz Builder's Send to PS5 (device_send.cpp), the part with no app around it, so tests/ps5_send_test.cpp can run it
// against a local FTP server: etaHEN's FTP client, what to send and what to skip, the progress line, and the launch.
//
// What goes (2026-11-15, from the team's first full test):
//   the title folder   /data/homebrew/PPSA99961: a file goes when it changed. The console keeps a manifest of what was
//                      sent (.newblitz_manifest there: SHA-256, size, path), so a changed file of the same size (an
//                      eboot.bin, a team logo) still goes. Without one (a console last sent to by 0.9.x) everything
//                      goes once.
//   the ROM            /data/blitzrecomp/blitz.z64, only when the console has none (stored, or waiting there).
//   the season pak     /data/blitzrecomp/<pak>.nbpak, when it differs from the one installed there.
//   the setup          season.json, rules.json and builder_done, only for a console that isn't set up yet (no
//                      general.json there): a player's own console settings are never replaced. Into the drop folder
//                      beside the ROM, never the app folder: the game takes them in at its next start, writing its own
//                      copies. A file etaHEN's FTP writes belongs to root, and the game can't write over it (mode 777
//                      or not; 2026-11-15: its saves stuck as rules.json.temp, and Save & Quit then hung).
// Every connection gives up after a few seconds (a wrong address used to wait 75 s).
#pragma once
#include "net_compat.h"
#include "../common/sha256.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace blitz::ps5 {
    namespace fs = std::filesystem;

    // ---- the progress line ---------------------------------------------------------------------------------------

    // "18.4" (decimal megabytes, as a Mac or Windows shows file sizes)
    inline std::string mb(uint64_t bytes) {
        char s[32];
        snprintf(s, sizeof(s), "%.1f", bytes / 1e6);
        return s;
    }

    // Bytes per second over the last stretch of a send (the speed etaHEN's FTP gives swings from 0.3 MB/s to 40 KB/s).
    class RateWindow {
        std::deque<std::pair<double, uint64_t>> samples;  // (seconds, bytes so far)
        double span;
    public:
        explicit RateWindow(double seconds = 15.0) : span(seconds) {}
        void add(double t, uint64_t bytes) {
            samples.emplace_back(t, bytes);
            while (samples.size() > 2 && samples.front().first < t - span) samples.pop_front();
        }
        // -1 until it has a few seconds to go on
        double rate() const {
            if (samples.size() < 2) return -1;
            double dt = samples.back().first - samples.front().first;
            if (dt < 3.0) return -1;
            return (double)(samples.back().second - samples.front().second) / dt;
        }
    };

    struct Snapshot {
        int files_done = 0, files_total = 0;
        uint64_t bytes_done = 0, bytes_total = 0;
        double elapsed = 0;        // seconds since the first byte went
        double recent_rate = -1;   // RateWindow::rate()
    };

    // Seconds left, or -1 while it isn't worth saying: the first 10 s, or while the recent speed and the whole send's
    // average disagree by more than half (the speed is still swinging).
    inline int eta_seconds(const Snapshot& s) {
        if (s.elapsed < 10 || s.bytes_total == 0 || s.bytes_done == 0 || s.recent_rate <= 0) return -1;
        if (s.bytes_done >= s.bytes_total) return 0;
        double average = s.bytes_done / s.elapsed;
        if (average <= 0 || std::fabs(s.recent_rate - average) > 0.5 * average) return -1;
        double rate = (s.recent_rate + average) / 2;
        return (int)std::ceil((double)(s.bytes_total - s.bytes_done) / rate);
    }

    inline std::string eta_text(int seconds) {
        if (seconds < 0) return "";
        if (seconds < 60) return "less than a minute left";
        if (seconds >= 3600) return "over an hour left";
        int minutes = (seconds + 30) / 60;
        return "about " + std::to_string(minutes) + (minutes == 1 ? " minute left" : " minutes left");
    }

    // "Sending 12 of 23 files · 18.4 of 45.0 MB · about 3 minutes left" (the file under way is the 12th).
    inline std::string progress_text(const Snapshot& s) {
        int at = std::min(s.files_done + 1, s.files_total);
        std::string line = "Sending " + std::to_string(at) + " of " + std::to_string(s.files_total) +
                           (s.files_total == 1 ? " file" : " files") + " \xC2\xB7 " + mb(s.bytes_done) + " of " + mb(s.bytes_total) + " MB";
        std::string eta = eta_text(eta_seconds(s));
        return eta.empty() ? line : line + " \xC2\xB7 " + eta;
    }

    // What the send thread is doing, for the screen (device_send_progress) and the log.
    struct Progress {
        enum Phase { Idle, Connecting, Checking, Sending, Starting };
        std::mutex m;
        int phase = Idle;
        Snapshot snap;
        RateWindow window;
        std::chrono::steady_clock::time_point started;

        void set_phase(int p) {
            std::lock_guard lock{m};
            phase = p;
        }
        void begin(int files, uint64_t bytes) {
            std::lock_guard lock{m};
            phase = Sending;
            snap = Snapshot{0, files, 0, bytes, 0, -1};
            window = RateWindow{};
            started = std::chrono::steady_clock::now();
            window.add(0, 0);
        }
        void add_bytes(uint64_t n) {
            std::lock_guard lock{m};
            snap.bytes_done += n;
            snap.elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            window.add(snap.elapsed, snap.bytes_done);
        }
        void file_done() {
            std::lock_guard lock{m};
            snap.files_done++;
        }
        Snapshot snapshot() {
            std::lock_guard lock{m};
            Snapshot s = snap;
            if (phase == Sending) {
                s.elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
                window.add(s.elapsed, s.bytes_done);  // a stall counts too
                s.recent_rate = window.rate();
            }
            return s;
        }
        // The line under "Sending to your PS5…"
        std::string text() {
            int p;
            {
                std::lock_guard lock{m};
                p = phase;
            }
            switch (p) {
                case Connecting: return "Connecting to the PS5\xE2\x80\xA6";
                case Checking: return "Checking what's on the PS5\xE2\x80\xA6";
                case Sending: return progress_text(snapshot());
                case Starting: return "Starting New Blitz on the PS5\xE2\x80\xA6";  // BRANDING: app name
                default: return "";
            }
        }
    };

    // ---- FTP (etaHEN's server: anonymous, passive) -----------------------------------------------------------------

    class Ftp {
        int fd = -1;
        std::string host;
        std::set<std::string> made;  // folders made (or found) this session: MKD once each
        int connect_ms = 5000;

    public:
        int last_error = 0;  // dial's: ETIMEDOUT (nothing answered), ECONNREFUSED (nothing listening), ...

        int dial(int port) {
            int s = blitz::net::open_tcp();
            if (s < 0) return -1;
            blitz::net::harden(s);
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = htons((uint16_t)port);
            if (inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1) {
                last_error = EINVAL;
                blitz::net::close_socket(s);
                return -1;
            }
            last_error = blitz::net::connect_within(s, a, connect_ms);
            if (last_error != 0) {
                blitz::net::close_socket(s);
                return -1;
            }
            blitz::net::set_timeouts(s, 30000);
            return s;
        }

    private:
        static bool send_all(int s, const char* p, size_t n) {
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

        // A passive data connection, or -1.
        int data_connection() {
            std::string t;
            if (cmd("PASV", &t) != 227) return -1;
            size_t l = t.find('(');
            int v[6] = {0};
            if (l == std::string::npos || sscanf(t.c_str() + l + 1, "%d,%d,%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return -1;
            return dial(v[4] * 256 + v[5]);  // the console's own address (the one PASV names may be another of its own)
        }

    public:
        ~Ftp() { close(); }

        int cmd(const std::string& c, std::string* text = nullptr) {
            if (fd < 0) return 0;
            std::string line = c + "\r\n";
            if (!send_all(fd, line.data(), line.size())) return 0;
            return reply(text);
        }

        bool open(const std::string& h, int port, int timeout_ms = 5000) {
            host = h;
            connect_ms = timeout_ms;
            fd = dial(port);
            if (fd < 0) return false;
            blitz::net::set_recv_timeout(fd, 10000);  // a port that takes the connection but never greets
            bool ok = reply() == 220;
            blitz::net::set_recv_timeout(fd, 30000);
            if (!ok) return false;
            int r = cmd("USER anonymous");
            if (r == 331) r = cmd("PASS blitz@");
            return r == 230 && cmd("TYPE I") == 200;
        }

        bool is_open() const { return fd >= 0; }

        void mkdirs(const std::string& path) {
            size_t i = 1;
            while (i <= path.size()) {
                size_t j = path.find('/', i);
                std::string at = path.substr(0, j == std::string::npos ? path.size() : j);
                if (made.insert(at).second) cmd("MKD " + at);
                if (j == std::string::npos) break;
                i = j + 1;
            }
        }

        // A file's size; -1 when it isn't there; -2 when the server won't say (no SIZE).
        long long size_of(const std::string& path) {
            std::string t;
            int r = cmd("SIZE " + path, &t);
            if (r == 213) return std::atoll(t.c_str() + 4);
            return r == 550 ? -1 : -2;
        }

        // The names in a folder (NLST), for a server without SIZE.
        bool list(const std::string& dir, std::vector<std::string>& names) {
            int d = data_connection();
            if (d < 0) return false;
            int r = cmd("NLST " + dir);
            if (r != 150 && r != 125) {
                blitz::net::close_socket(d);
                return r == 550 || r == 450;  // no such folder: nothing in it
            }
            std::string all;
            char buf[4096];
            ssize_t n;
            while ((n = blitz::net::recv_some(d, buf, sizeof(buf))) > 0) all.append(buf, (size_t)n);
            blitz::net::close_socket(d);
            reply();
            size_t at = 0;
            while (at < all.size()) {
                size_t end = all.find('\n', at);
                std::string name = all.substr(at, end == std::string::npos ? std::string::npos : end - at);
                at = end == std::string::npos ? all.size() : end + 1;
                while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) name.pop_back();
                if (size_t slash = name.rfind('/'); slash != std::string::npos) name = name.substr(slash + 1);
                if (!name.empty()) names.push_back(name);
            }
            return true;
        }

        bool exists(const std::string& path) {
            long long s = size_of(path);
            if (s != -2) return s >= 0;
            size_t slash = path.rfind('/');
            std::vector<std::string> names;
            if (slash == std::string::npos || !list(path.substr(0, slash), names)) return false;
            return std::find(names.begin(), names.end(), path.substr(slash + 1)) != names.end();
        }

        // A small file's contents (the manifest); false when it isn't there.
        bool get(const std::string& path, std::string& out, size_t max_bytes = 4 << 20) {
            int d = data_connection();
            if (d < 0) return false;
            int r = cmd("RETR " + path);
            if (r != 150 && r != 125) {
                blitz::net::close_socket(d);
                return false;
            }
            out.clear();
            char buf[8192];
            ssize_t n;
            while ((n = blitz::net::recv_some(d, buf, sizeof(buf))) > 0 && out.size() <= max_bytes) out.append(buf, (size_t)n);
            blitz::net::close_socket(d);
            return reply() == 226 && out.size() <= max_bytes;
        }

        // Up under a temporary name, then renamed into place (a cut-short send never leaves half a file). The bytes
        // come from read (false when there are no more) and are reported to sent as they go.
        bool put(const std::string& path, const std::function<bool(std::string&)>& read,
                 const std::function<void(size_t)>& sent = nullptr) {
            int d = data_connection();
            if (d < 0) return false;
            int r = cmd("STOR " + path + ".part");
            bool ok = r == 150 || r == 125;
            std::string chunk;
            while (ok && read(chunk)) {
                ok = send_all(d, chunk.data(), chunk.size());
                if (ok && sent) sent(chunk.size());
            }
            blitz::net::close_socket(d);
            if (r == 150 || r == 125) {
                if (reply() != 226) ok = false;
            }
            if (!ok) return false;
            cmd("DELE " + path);
            return cmd("RNFR " + path + ".part") == 350 && cmd("RNTO " + path) == 250;
        }

        bool put_data(const std::string& path, const std::string& data) {
            bool given = false;
            return put(path, [&](std::string& chunk) {
                if (given) return false;
                given = true;
                chunk = data;
                return true;
            });
        }

        bool put_file(const std::string& path, const fs::path& from, const std::function<void(size_t)>& sent) {
            std::ifstream in{from, std::ios::binary};
            if (!in) return false;
            return put(path, [&](std::string& chunk) {
                chunk.resize(64 * 1024);
                in.read(chunk.data(), (std::streamsize)chunk.size());
                chunk.resize((size_t)in.gcount());
                return !chunk.empty();
            }, sent);
        }

        void close() {
            if (fd >= 0) {
                cmd("QUIT");
                blitz::net::close_socket(fd);
                fd = -1;
            }
            made.clear();
        }
    };

    // ---- the manifest --------------------------------------------------------------------------------------------

    struct Entry {
        std::string sha;
        long long size = -1;
    };
    using Manifest = std::map<std::string, Entry>;  // by the console's path

    inline const char* manifest_header = "newblitz-manifest 1";

    inline bool parse_manifest(const std::string& text, Manifest& out) {
        out.clear();
        size_t at = 0;
        bool header = false;
        while (at < text.size()) {
            size_t end = text.find('\n', at);
            std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
            at = end == std::string::npos ? text.size() : end + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            if (!header) {
                if (line != manifest_header) return false;
                header = true;
                continue;
            }
            // <sha256> <size> <path> (the path may hold spaces)
            size_t a = line.find(' '), b = a == std::string::npos ? a : line.find(' ', a + 1);
            if (b == std::string::npos || a != 64) continue;
            out[line.substr(b + 1)] = Entry{line.substr(0, a), std::atoll(line.substr(a + 1, b - a - 1).c_str())};
        }
        return header;
    }

    inline std::string manifest_text(const Manifest& m) {
        std::string s = std::string(manifest_header) + "\n";
        for (const auto& [path, e] : m) s += e.sha + " " + std::to_string(e.size) + " " + path + "\n";
        return s;
    }

    inline std::string file_sha256(const fs::path& p) {
        std::ifstream in{p, std::ios::binary};
        blitzsha::Sha256 h;
        std::vector<char> buf(256 * 1024);
        while (in) {
            in.read(buf.data(), (std::streamsize)buf.size());
            if (in.gcount() > 0) h.update(buf.data(), (size_t)in.gcount());
        }
        uint8_t out[32];
        h.final(out);
        return blitzsha::hex(out, 32);
    }

    // ---- the send ------------------------------------------------------------------------------------------------

    struct Job {
        std::string host;
        int ftp_port = 1337, loader_port = 9021, connect_ms = 5000;
        fs::path title_dir;             // the kit's PPSA99961 folder
        fs::path launch_elf;            // the payload that asks the console to start it (empty: don't)
        fs::path rom;                   // this computer's stored ROM (empty or missing: none)
        std::string rom_name;           // its name in the console's app folder (<game id>.z64)
        fs::path pak;                   // the installed season pak (empty: none)
        std::vector<fs::path> setup;    // season.json, rules.json
        std::string remote_title = "/data/homebrew/PPSA99961";
        std::string remote_drop = "/data/blitzrecomp";                         // BRANDING: PS5 folder
        std::string remote_app = "/data/blitzrecomp/Blitz2026Recompiled";
        double start_wait = 20;         // seconds to watch for the game starting again
    };

    struct Result {
        enum Stage { Ok, NoKit, Unreachable, Refused, CopyStopped };
        int stage = Ok;
        std::string stopped_at;         // CopyStopped: the file
        bool setup_ok = true;           // the ROM, pak and setup went (or weren't needed)
        bool set_up_already = false;    // the console had its own setup: kept
        bool launched = false;          // the loader took the payload
        int restarted = -1;             // -1 can't tell, 0 the old copy is still running, 1 it started again
        int files_sent = 0;
        uint64_t bytes_sent = 0;
        double seconds = 0;
    };

    using Log = std::function<void(const std::string&)>;

    // Sends the payload to the ELF loader; whether it took it.
    inline bool send_payload(const std::string& host, int port, const fs::path& elf_path, int connect_ms, const Log& log) {
        std::ifstream in{elf_path, std::ios::binary};
        std::string elf{std::istreambuf_iterator<char>(in), {}};
        if (elf.empty()) return false;
        int s = blitz::net::open_tcp();
        if (s < 0) return false;
        blitz::net::harden(s);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)port);
        int err = inet_pton(AF_INET, host.c_str(), &a.sin_addr) == 1 ? blitz::net::connect_within(s, a, connect_ms) : EINVAL;
        bool ok = err == 0;
        if (ok) {
            blitz::net::set_timeouts(s, 10000);
            const char* p = elf.data();
            size_t n = elf.size();
            while (n > 0) {
                ssize_t k = blitz::net::send_some(s, p, n);
                if (k <= 0) { ok = false; break; }
                p += k;
                n -= (size_t)k;
            }
        }
        if (!ok) log("the loader at " + host + ":" + std::to_string(port) + " didn't take the payload (error " + std::to_string(err ? err : errno) + ")");
        blitz::net::close_socket(s);
        return ok;
    }

    inline Result send(const Job& job, Progress& progress, const Log& log) {
        Result res;
        auto t0 = std::chrono::steady_clock::now();
        std::error_code ec;
        progress.set_phase(Progress::Connecting);
        Ftp ftp;
        if (!ftp.open(job.host, job.ftp_port, job.connect_ms)) {
            res.stage = ftp.last_error == ECONNREFUSED ? Result::Refused : Result::Unreachable;
            log("no FTP at " + job.host + ":" + std::to_string(job.ftp_port) + " (" +
                (ftp.last_error == ETIMEDOUT ? std::string("nothing answered in ") + std::to_string(job.connect_ms / 1000) + " s"
                 : ftp.last_error == ECONNREFUSED ? std::string("refused: nothing listening")
                 : ftp.last_error ? "error " + std::to_string(ftp.last_error) : std::string("no FTP greeting")) +
                ")");
            progress.set_phase(Progress::Idle);
            return res;
        }
        progress.set_phase(Progress::Checking);

        // What's there.
        const std::string manifest_path = job.remote_title + "/.newblitz_manifest";
        Manifest old;
        std::string text;
        bool have_manifest = ftp.get(manifest_path, text) && parse_manifest(text, old);
        if (!have_manifest) log("no manifest on the console yet: every file of the app goes once");
        res.set_up_already = ftp.exists(job.remote_app + "/general.json");
        // The running game's log, to see whether it starts again after the launch: a new start moves blitz.log to
        // blitz.prev.log (app_log.cpp).
        long long log_before = ftp.size_of(job.remote_app + "/blitz.log"), prev_before = ftp.size_of(job.remote_app + "/blitz.prev.log");

        struct Item {
            fs::path from;
            std::string data;      // or these bytes (builder_done)
            std::string to;
            uint64_t size = 0;
            std::string sha;       // for the manifest ("" none)
            std::vector<std::string> also;  // more manifest paths the same bytes stand for (the pak once installed)
            bool title = false, counted = true;
        };
        std::vector<Item> items;
        Manifest keep;  // what the manifest can still vouch for
        int skipped = 0;

        // The app.
        std::vector<fs::path> files;
        for (fs::recursive_directory_iterator it{job.title_dir, ec}, end; !ec && it != end; it.increment(ec))
            if (it->is_regular_file(ec)) files.push_back(it->path());
        std::sort(files.begin(), files.end());
        for (const fs::path& f : files) {
            std::string rel = fs::relative(f, job.title_dir, ec).generic_string();
            std::string dst = job.remote_title + "/" + rel;
            uint64_t size = (uint64_t)fs::file_size(f, ec);
            std::string sha = file_sha256(f);
            auto at = old.find(dst);
            if (at != old.end() && at->second.sha == sha && at->second.size == (long long)size && ftp.size_of(dst) == (long long)size) {
                keep[dst] = at->second;
                skipped++;
                continue;
            }
            items.push_back(Item{f, "", dst, size, sha, {}, true, true});
        }
        if (skipped) log(std::to_string(skipped) + " app files unchanged on the console: skipped");

        // The ROM, only where the console has none.
        if (!job.rom.empty() && fs::is_regular_file(job.rom, ec)) {
            uint64_t size = (uint64_t)fs::file_size(job.rom, ec);
            if (!job.rom_name.empty() && ftp.exists(job.remote_app + "/" + job.rom_name))
                log("ROM skipped: the console has its own stored");
            else if (ftp.size_of(job.remote_drop + "/blitz.z64") == (long long)size)
                log("ROM skipped: already there, waiting to be taken in");
            else
                items.push_back(Item{job.rom, "", job.remote_drop + "/blitz.z64", size, "", {}, false, true});
        }

        // The season pak, when the console's differs.
        if (!job.pak.empty() && fs::is_regular_file(job.pak, ec)) {
            std::string name = job.pak.filename().string();
            std::string installed = job.remote_app + "/paks/" + name, waiting = job.remote_drop + "/" + name;
            uint64_t size = (uint64_t)fs::file_size(job.pak, ec);
            std::string sha = file_sha256(job.pak);
            auto same = [&](const std::string& path) {
                long long there = ftp.size_of(path);
                if (there != (long long)size) return false;
                auto at = old.find(path);
                return at == old.end() || at->second.sha == sha;  // not in the manifest: the same size will do
            };
            if (same(installed)) {
                log("season pak skipped: the console has this one installed");
                keep[installed] = Entry{sha, (long long)size};
            }
            else if (same(waiting)) {
                log("season pak skipped: already there, waiting to be installed");
                keep[waiting] = keep[installed] = Entry{sha, (long long)size};
            }
            else items.push_back(Item{job.pak, "", waiting, size, sha, {installed}, false, true});
        }

        // The setup, for a console that isn't set up yet.
        if (res.set_up_already) {
            std::string names;
            for (const fs::path& f : job.setup) names += (names.empty() ? "" : ", ") + f.filename().string();
            log("kept the console's own setup (" + job.remote_app + "/general.json is there): " +
                (names.empty() ? std::string("nothing") : names + " and builder_done") + " not sent");
        }
        else {
            for (const fs::path& f : job.setup)
                if (fs::is_regular_file(f, ec))
                    items.push_back(Item{f, "", job.remote_drop + "/" + f.filename().string(), (uint64_t)fs::file_size(f, ec), "", {}, false, true});
            items.push_back(Item{{}, "1\n", job.remote_drop + "/builder_done", 2, "", {}, false, false});  // no Blitz Builder there (config.cpp)
        }

        int count = 0;
        uint64_t total = 0;
        for (const Item& i : items)
            if (i.counted) {
                count++;
                total += i.size;
            }
        log("to send: " + std::to_string(count) + " files, " + mb(total) + " MB");
        progress.begin(count, total);
        auto t_send = std::chrono::steady_clock::now();

        // The manifest first says only what still holds, so a send cut short can't vouch for a half-changed app.
        bool app_changes = std::any_of(items.begin(), items.end(), [](const Item& i) { return i.title; });
        if (app_changes && have_manifest) ftp.put_data(manifest_path, manifest_text(keep));

        for (const Item& i : items) {
            ftp.mkdirs(fs::path(i.to).parent_path().generic_string());
            auto sent = [&](size_t n) { progress.add_bytes(n); };
            bool ok = i.from.empty() ? ftp.put_data(i.to, i.data) : ftp.put_file(i.to, i.from, sent);
            if (!ok) {
                log("stopped at " + i.to);
                if (i.title) {
                    res.stage = Result::CopyStopped;
                    res.stopped_at = fs::relative(i.from, job.title_dir, ec).generic_string();
                    progress.set_phase(Progress::Idle);
                    return res;
                }
                res.setup_ok = false;
                if (!ftp.is_open()) break;
                continue;
            }
            if (!i.sha.empty()) {
                keep[i.to] = Entry{i.sha, (long long)i.size};
                for (const std::string& a : i.also) keep[a] = keep[i.to];
            }
            if (i.counted) {
                progress.file_done();
                res.files_sent++;
                res.bytes_sent += i.size;
            }
        }
        if (!ftp.put_data(manifest_path, manifest_text(keep))) log("couldn't write the manifest (the next send sends everything again)");
        res.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_send).count();
        {
            char line[96];
            snprintf(line, sizeof(line), "sent %.1f MB in %.0f s (%d files)", res.bytes_sent / 1e6, res.seconds, res.files_sent);
            log(line);
        }

        // Start it.
        if (!job.launch_elf.empty()) {
            progress.set_phase(Progress::Starting);
            res.launched = send_payload(job.host, job.loader_port, job.launch_elf, job.connect_ms, log);
            // Did it start again? Only worth asking where a game had a log to move (it has run here before).
            if (res.launched && log_before >= 0) {
                res.restarted = 0;
                auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds((int)(job.start_wait * 1000));
                while (std::chrono::steady_clock::now() < until) {
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    long long log_now = ftp.size_of(job.remote_app + "/blitz.log");
                    long long prev_now = ftp.size_of(job.remote_app + "/blitz.prev.log");
                    if (log_now == -2 && prev_now == -2) {  // the connection went: can't tell
                        res.restarted = -1;
                        break;
                    }
                    if (prev_now != prev_before || (log_now >= 0 && log_now < log_before)) {
                        res.restarted = 1;
                        break;
                    }
                }
                log(res.restarted == 1 ? "the game started again"
                    : res.restarted == 0 ? "the game's log didn't move: the copy already running stayed"
                                         : "lost the FTP connection: can't tell whether the game started again");
            }
        }
        ftp.close();
        res.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        progress.set_phase(Progress::Idle);
        return res;
    }
}
