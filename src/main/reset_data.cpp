// Settings → General → Reset Data and Restore a Backup (team, 2026-11-15).
//
// Reset Data asks first ("Reset Data?", with Keep my ROM ticked): Back Up and Reset (the default) makes a dated zip of
// the app folder, then resets; Reset doesn't back up; Cancel. A reset clears the app folder (settings, saves, setup,
// the season pak) except backups/, the logs and, when kept, the stored ROM; and the shared folder that remembers which
// game opens next (experience.cpp). Then the app starts again the way Main Menu does (main.cpp restart_fresh), on the
// launcher, where the first run comes up. A phone or console can't start itself again: it closes, and the next start
// is a first run.
//
// Restore a Backup (shown once there's one) lists the newest few by date, confirms, then puts the backup's files back
// in place of the folder's (the ROM stays) and starts again the same way.
//
// Where backups go: a computer's Documents/New Blitz Backups (os_shell.cpp documents_folder; BLITZ_DOCUMENTS_DIR for
// tests); a phone or the PS5, backups/ in the app's own folder, which a reset leaves alone. A backup holds the folder
// but the ROM (the player's own file, kept beside it), the logs and crash dumps.
//
// The clearing itself happens with the game stopped: the choice writes reset_pending in the app folder and the app
// quits; main.cpp clears (or restores) on the way out, after the game loop has ended, and again at the next start if
// a reset_pending is still there (the app was killed in between). Every step prints a [reset] line.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "blitz_config.h"
#include "blitz_platform.h"
#include "firstrun_screen.h"
#include "miniz.h"
#include "os_shell.h"
#include "recompui/recompui.h"
#include "ultramodern/ultramodern.hpp"
#include "util/file.h"

namespace blitz {
    void restart_fresh();       // main.cpp
    void show_launcher();       // main.cpp
    void experience_reset();    // experience.cpp
    void post_status(const std::string& msg);  // notices.cpp
}

namespace {
    namespace fs = std::filesystem;
    using blitz::firstrun_ui::Screen;
    using blitz::firstrun_ui::Action;
    using blitz::firstrun_ui::Check;

    // BRANDING: the backup folder and file names
    constexpr const char* backups_name = "New Blitz Backups";
    constexpr const char* backup_prefix = "New Blitz Backup ";
    constexpr const char* marker_name = "reset_pending";

    std::string utf8(const fs::path& p) {
        std::u8string s = p.u8string();
        return std::string(s.begin(), s.end());
    }

    fs::path from_utf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

    std::string lower(std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

    // A stored ROM, the player's own file (the app folder's top level: <game_id>.z64; an Android drop's blitz.z64 or zip).
    bool is_rom(const fs::path& name) {
        std::string ext = lower(name.extension().string());
        return ext == ".z64" || ext == ".n64" || ext == ".v64" || ext == ".zip";
    }

    bool is_log(const fs::path& name) { return lower(name.extension().string()) == ".log"; }

    // What a backup leaves out, by the name at the app folder's top: the backups, the ROM, the logs, crash dumps, and
    // the marks of a moment.
    bool backup_skips(const fs::path& top, bool is_dir) {
        std::string n = top.filename().string();
        if (n == "backups" || n == "dumps" || n == marker_name || n == "picking.tmp" || n == "open_launcher") return true;
        if (!is_dir && (is_rom(top) || is_log(top))) return true;
        return false;
    }

    std::string read_all(const fs::path& p) {
        std::ifstream in(p, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    bool write_all(const fs::path& p, const void* data, size_t size) {
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out.write(static_cast<const char*>(data), (std::streamsize)size);
        return (bool)out;
    }

    // "Nov 15, 2026, 2:03 PM", from a backup's name ("New Blitz Backup 2026-11-15 14.03.22.zip").
    std::string backup_date(const fs::path& zip) {
        int y, mo, d, h, mi, s;
        std::string name = zip.filename().string();
        if (name.rfind(backup_prefix, 0) != 0 ||
            sscanf(name.c_str() + strlen(backup_prefix), "%d-%d-%d %d.%d.%d", &y, &mo, &d, &h, &mi, &s) != 6)
            return name;
        static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        char out[64];
        snprintf(out, sizeof(out), "%s %d, %d, %d:%02d %s", months[std::clamp(mo, 1, 12) - 1], d, y, h % 12 == 0 ? 12 : h % 12, mi,
                 h < 12 ? "AM" : "PM");
        return out;
    }

    fs::path app_folder() { return recompui::file::get_app_folder_path(); }
}

namespace blitz {
    // Where Reset Data puts its backups: Documents/New Blitz Backups on a computer, backups/ in the app folder elsewhere.
    std::filesystem::path reset_backups_folder() {
        if (blitz::platform::desktop) {
            fs::path docs = blitz::documents_folder();
            if (!docs.empty()) return docs / backups_name;
        }
        return app_folder() / "backups";
    }

    // The backups there, newest first.
    std::vector<std::filesystem::path> reset_backups() {
        std::vector<fs::path> out;
        std::error_code ec;
        for (fs::directory_iterator it{reset_backups_folder(), ec}, end; !ec && it != end; it.increment(ec)) {
            std::string n = it->path().filename().string();
            if (n.rfind(backup_prefix, 0) == 0 && lower(it->path().extension().string()) == ".zip" && it->is_regular_file(ec))
                out.push_back(it->path());
        }
        std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b) { return a.filename() > b.filename(); });
        return out;
    }

    bool reset_backups_exist() { return !reset_backups().empty(); }

    // A dated zip of the app folder (backup_skips leaves out the ROM, the logs and the dumps). Its path, or empty with why.
    std::filesystem::path reset_make_backup(std::string& why) {
        fs::path app = app_folder(), dir = reset_backups_folder();
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) {
            why = "Couldn't make the folder " + utf8(dir) + " (" + ec.message() + ").";
            fprintf(stderr, "[reset] backup: %s\n", why.c_str());
            return {};
        }
        char stamp[32] = {};
        std::time_t now = std::time(nullptr);
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H.%M.%S", std::localtime(&now));
        fs::path zip_path = dir / (std::string(backup_prefix) + stamp + ".zip");
        mz_zip_archive zip{};
        if (!mz_zip_writer_init_heap(&zip, 0, 1 << 20)) {
            why = "Couldn't start the backup.";
            return {};
        }
        bool ok = true;
        int files = 0;
        size_t bytes = 0;
        for (fs::recursive_directory_iterator it{app, fs::directory_options::skip_permission_denied, ec}, end; ok && !ec && it != end; it.increment(ec)) {
            fs::path rel = it->path().lexically_relative(app);
            std::error_code fec;
            bool dir_entry = it->is_directory(fec);
            if (it.depth() == 0 && backup_skips(rel, dir_entry)) {
                if (dir_entry) it.disable_recursion_pending();
                continue;
            }
            if (dir_entry || !it->is_regular_file(fec)) continue;
            if (it->file_size(fec) > (64u << 20)) {  // nothing of the player's is this big: a cache
                fprintf(stderr, "[reset] backup: left out %s (%llu bytes)\n", utf8(rel).c_str(), (unsigned long long)it->file_size(fec));
                continue;
            }
            std::string data = read_all(it->path());
            std::u8string generic = rel.generic_u8string();
            std::string name(generic.begin(), generic.end());
            ok = mz_zip_writer_add_mem(&zip, name.c_str(), data.data(), data.size(), MZ_DEFAULT_COMPRESSION);
            files++;
            bytes += data.size();
        }
        if (ec) fprintf(stderr, "[reset] backup: reading the folder stopped: %s\n", ec.message().c_str());
        // What it is, for whoever opens the zip.
        std::string about = std::string("New Blitz ") + BLITZ_VERSION + " (build " BLITZ_BUILD ") on " + blitz::platform::name +
                            ", " + stamp + ".\nSettings, saves and setup from the app folder. The ROM isn't in it.\n"
                            "Settings > Saves > Restore a Backup puts it back.\n";
        ok = ok && mz_zip_writer_add_mem(&zip, "New Blitz Backup.txt", about.data(), about.size(), MZ_DEFAULT_COMPRESSION);
        void* buf = nullptr;
        size_t size = 0;
        ok = ok && mz_zip_writer_finalize_heap_archive(&zip, &buf, &size);
        mz_zip_writer_end(&zip);
        ok = ok && write_all(zip_path, buf, size);
        if (buf) mz_free(buf);
        if (!ok) {
            fs::remove(zip_path, ec);
            why = "Couldn't write the backup in " + utf8(dir) + ".";
            fprintf(stderr, "[reset] backup failed: %s\n", why.c_str());
            return {};
        }
        fprintf(stderr, "[reset] backup made: %s (%d files, %zu bytes, zip %zu bytes)\n", utf8(zip_path).c_str(), files, bytes, size);
        return zip_path;
    }

    // main.cpp: on the way out (when="quit", the game stopped) and at the next start (when="start", before anything
    // reads the folder), what Reset Data or Restore asked for (reset_pending).
    void reset_data_pending(const std::filesystem::path& app, const char* when) {
        std::error_code ec;
        const fs::path marker = app / marker_name;
        if (!fs::is_regular_file(marker, ec)) return;
        std::ifstream in(marker);
        std::string what, zip_line;
        std::getline(in, what);
        std::getline(in, zip_line);
        in.close();
        const bool restore = what.rfind("restore", 0) == 0;
        const bool keep_rom = restore || what.find("keep_rom=1") != std::string::npos;
        fprintf(stderr, "[reset] %s: %s%s%s\n", when, restore ? "restore " : "reset ", restore ? zip_line.c_str() : "",
                restore ? "" : keep_rom ? "keep_rom=1" : "keep_rom=0");
        // A restore reads its zip before anything goes: one that can't be read changes nothing.
        std::string zip_data;
        mz_zip_archive zip{};
        if (restore) {
            zip_data = read_all(from_utf8(zip_line));
            if (zip_data.empty() || !mz_zip_reader_init_mem(&zip, zip_data.data(), zip_data.size(), 0)) {
                fprintf(stderr, "[reset] restore: couldn't read %s: nothing changed\n", zip_line.c_str());
                fs::remove(marker, ec);
                return;
            }
        }
        // The folder's contents but backups/, the logs (this run's is open) and the ROM if kept.
        int removed = 0, failed = 0, kept = 0;
        std::vector<fs::path> entries;
        for (fs::directory_iterator it{app, ec}, end; !ec && it != end; it.increment(ec)) entries.push_back(it->path());
        for (const fs::path& p : entries) {
            fs::path name = p.filename();
            std::error_code dec;
            bool dir = fs::is_directory(p, dec);
            if (name == "backups" || (!dir && is_log(name)) || (keep_rom && !dir && is_rom(name))) {
                if (!dir && is_rom(name)) fprintf(stderr, "[reset] kept the ROM %s\n", utf8(name).c_str());
                kept++;
                continue;
            }
            fs::remove_all(p, dec);
            if (dec) {
                failed++;
                fprintf(stderr, "[reset] couldn't remove %s: %s\n", utf8(name).c_str(), dec.message().c_str());
            }
            else removed++;
        }
        fprintf(stderr, "[reset] cleared %s: %d removed, %d kept, %d failed\n", utf8(app).c_str(), removed, kept, failed);
        if (!restore) {
            blitz::experience_reset();
        }
        else {
            int written = 0, skipped = 0;
            for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); i++) {
                mz_zip_archive_file_stat st;
                if (!mz_zip_reader_file_stat(&zip, i, &st) || st.m_is_directory) continue;
                fs::path rel = from_utf8(st.m_filename).lexically_normal();
                std::string top = rel.empty() ? "" : utf8(*rel.begin());
                // Inside the folder only, and nothing a backup never holds.
                if (rel.empty() || rel.is_absolute() || rel.has_root_name() || top == ".." || utf8(rel) == "New Blitz Backup.txt" ||
                    backup_skips(*rel.begin(), std::distance(rel.begin(), rel.end()) > 1)) {
                    skipped++;
                    continue;
                }
                size_t size = 0;
                void* data = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
                fs::path to = app / rel;
                std::error_code dec;
                fs::create_directories(to.parent_path(), dec);
                bool ok = data != nullptr && write_all(to, data, size);
                if (data) mz_free(data);
                if (ok) written++;
                else fprintf(stderr, "[reset] restore: couldn't write %s\n", utf8(rel).c_str());
            }
            mz_zip_reader_end(&zip);
            fprintf(stderr, "[reset] restored %d files from %s (%d left out)\n", written, zip_line.c_str(), skipped);
        }
        fs::remove(marker, ec);
        fprintf(stderr, "[reset] done (%s)\n", when);
    }
}

namespace {
    // ---- the screens (firstrun_screen.cpp draws them; built on the UI tick, never from a button's own callback) -------

    enum Page { None, Ask, BackupFailed, Done, List, Confirm };
    struct State {
        int page = None;
        bool keep_rom = true;
        fs::path backup;           // the one just made
        std::string why;           // why a backup failed
        std::vector<fs::path> list;
        fs::path chosen;           // the backup to restore
        bool restoring = false;
    } st;

    std::atomic<int> request{0};   // Settings: 1 Reset Data, 2 Restore a Backup
    std::atomic<int> pressed{0};   // a button, done on the tick (below)
    std::atomic<bool> dirty{false};
    enum Press { NoPress, BackUpAndReset, ResetOnly, Cancel, Restart, ShowBackup, Pick, RestoreIt };
    int picked = 0;

    void press(int what) {
        pressed = what;
        dirty = true;
    }

    // reset_pending, then out: a computer restarts at once into the first run unless there's a backup to point at
    // (Done shows where it went); a phone or console says it closes first.
    bool write_marker(bool restore) {
        std::error_code ec;
        fs::create_directories(app_folder(), ec);
        std::ofstream out(app_folder() / marker_name, std::ios::trunc);
        if (restore) out << "restore\n" << utf8(st.chosen) << "\n";
        else out << "reset keep_rom=" << (st.keep_rom ? 1 : 0) << "\n";
        out.close();
        bool ok = (bool)out;
        fprintf(stderr, "[reset] %s asked%s%s: %s\n", restore ? "restore" : "reset", restore ? " from " : st.keep_rom ? ", ROM kept" : ", ROM cleared",
                restore ? utf8(st.chosen).c_str() : "", ok ? "the app ends to do it" : "couldn't write the mark");
        if (!ok) blitz::post_status("Couldn't write in New Blitz's folder: nothing was changed");
        return ok;
    }

    // True when the app is on its way out (nothing more to show).
    bool finish_or_restart() {
        if (blitz::app_restart_available && st.backup.empty()) {
            blitz::firstrun_ui::hide();
            st = State{};
            blitz::restart_fresh();
            return true;
        }
        st.page = Done;
        return false;
    }

    // The launcher steps aside while these screens are up (it would take the D-pad from them), as for first run.
    void launcher_visible(bool on) {
        if (ultramodern::is_game_started()) return;
        recompui::ContextId launcher = recompui::get_launcher_context_id();
        if (launcher == recompui::ContextId::null()) return;
        if (on) blitz::show_launcher();
        else if (recompui::is_context_shown(launcher)) recompui::hide_context(launcher);
    }

    void show() {
        Screen s;
        switch (st.page) {
            case Ask:
                s.title = "Reset Data?";
                s.body = "This clears New Blitz's settings, saves and setup, then starts over from the first run.";
                s.checks.push_back(Check{"Keep my ROM", "", [] { return st.keep_rom; }, [](bool v) { st.keep_rom = v; }});
                s.actions = {
                    {"Back Up and Reset", [] { press(BackUpAndReset); }},
                    {"Reset", [] { press(ResetOnly); }},
                    {"Cancel", [] { press(Cancel); }},
                };
                s.back = [] { press(Cancel); };
                s.back_button = false;
                break;
            case BackupFailed:
                s.title = "Couldn't make the backup";
                s.body = st.why + "\n\nNothing was reset.";
                s.actions = {
                    {"Reset without a Backup", [] { press(ResetOnly); }},
                    {"Cancel", [] { press(Cancel); }},
                };
                s.focus = 1;
                s.back = [] { press(Cancel); };
                s.back_button = false;
                break;
            case Done: {
                std::string where;
                if (!st.backup.empty()) {
                    // BRANDING: the backup folder's name
                    where = blitz::platform::desktop ? "Your backup is in Documents \xE2\x86\x92 New Blitz Backups: " + st.backup.filename().string() + "."
                                                     : "Your backup is in New Blitz's folder, in backups: " + st.backup.filename().string() + ".";
                    where += " Settings \xE2\x86\x92 Saves \xE2\x86\x92 Restore a Backup brings it back.\n\n";
                }
                s.title = st.restoring ? "Ready to restore" : !st.backup.empty() ? "Backed up" : "Ready to start over";
                s.body = where + (blitz::app_restart_available
                                      ? std::string(st.restoring ? "New Blitz restarts with your backup." : "New Blitz restarts into the first run.")
                                      : std::string(st.restoring ? "New Blitz closes now. Open it again and your backup is back."
                                                                 : "New Blitz closes now. Open it again and the first run begins."));
                s.actions = {{blitz::app_restart_available ? "Restart" : "Close New Blitz", [] { press(Restart); }}};
                if (!st.backup.empty() && blitz::platform::file_browser) s.actions.push_back({"Show Backup", [] { press(ShowBackup); }});
                break;
            }
            case List: {
                s.title = "Restore a Backup";
                s.body = "Pick one, newest first. Restoring replaces your current settings and saves.";
                for (size_t i = 0; i < st.list.size() && i < 4; i++) {
                    int n = (int)i;
                    s.actions.push_back({backup_date(st.list[i]), [n] { picked = n; press(Pick); }});
                }
                s.actions.push_back({"Cancel", [] { press(Cancel); }});
                s.back = [] { press(Cancel); };
                s.back_button = false;
                break;
            }
            case Confirm:
                s.title = "Restore this backup?";
                s.body = "This replaces your current settings and saves with the backup from " + backup_date(st.chosen) + ". Your ROM stays.";
                s.actions = {
                    {"Restore", [] { press(RestoreIt); }},
                    {"Cancel", [] { press(Cancel); }},
                };
                s.back = [] { press(Cancel); };
                s.back_button = false;
                break;
            default:
                blitz::firstrun_ui::hide();
                launcher_visible(true);
                return;
        }
        launcher_visible(false);
        blitz::firstrun_ui::show(s);
    }
}

namespace blitz {
    // config.cpp's rows, through firstrun.cpp's Settings request (Settings closes first): 1 Reset Data, 2 Restore.
    void reset_data_open(bool restore) { request = restore ? 2 : 1; }

    // Every UI tick (firstrun.cpp builder_update).
    void reset_data_update() {
        if (int r = request.exchange(0); r != 0) {
            st = State{};
            if (r == 1) {
                fprintf(stderr, "[reset] asked: Reset Data?\n");
                st.page = Ask;
            }
            else {
                st.list = reset_backups();
                fprintf(stderr, "[reset] restore: %zu backup(s) in %s\n", st.list.size(), utf8(reset_backups_folder()).c_str());
                if (st.list.empty()) {
                    blitz::post_status("No backups yet");
                    return;
                }
                st.page = List;
            }
            dirty = true;
        }
        if (int p = pressed.exchange(0); p != NoPress) {
            switch (p) {
                case BackUpAndReset:
                    fprintf(stderr, "[reset] answered: Back Up and Reset (keep ROM %s)\n", st.keep_rom ? "yes" : "no");
                    st.backup = reset_make_backup(st.why);
                    if (st.backup.empty()) st.page = BackupFailed;
                    else if (write_marker(false) && finish_or_restart()) return;
                    break;
                case ResetOnly:
                    fprintf(stderr, "[reset] answered: Reset (no backup, keep ROM %s)\n", st.keep_rom ? "yes" : "no");
                    st.backup.clear();
                    if (write_marker(false) && finish_or_restart()) return;
                    break;
                case Cancel:
                    fprintf(stderr, "[reset] answered: Cancel\n");
                    st = State{};
                    break;
                case Restart:
                    blitz::firstrun_ui::hide();
                    st = State{};
                    blitz::restart_fresh();
                    return;
                case ShowBackup:
                    blitz::reveal_in_file_browser(st.backup);
                    return;  // the screen stays
                case Pick:
                    if (picked >= 0 && picked < (int)st.list.size()) {
                        st.chosen = st.list[picked];
                        fprintf(stderr, "[reset] restore: picked %s\n", utf8(st.chosen.filename()).c_str());
                        st.page = Confirm;
                    }
                    break;
                case RestoreIt:
                    fprintf(stderr, "[reset] answered: Restore\n");
                    st.restoring = true;
                    if (write_marker(true) && finish_or_restart()) return;
                    break;
            }
        }
        if (!dirty.exchange(false)) return;
        show();
    }
}
