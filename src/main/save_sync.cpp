// Settings → General → Saves (docs/SAVE_SYNC.md): moving the player's Controller Pak saves and Advanced seasons between
// devices, three ways, on one menu:
//   - Cloud Sync (§7): turn it on here and join with its code on the other devices; saves go up after each save the game
//     makes and come down at launch and every few minutes while the game isn't running, encrypted on the device
//     (src/game/save_sync_cloud.h). Three Cloud Save Slots under one code. Needs the sync server's address
//     (Resources/sync/server, or BLITZ_SYNC_SERVER). The code is kept in the platform's secure storage (blitz_host.h
//     secure_put), else a file only this user can read.
//   - Sync over Wi-Fi (§7.6): one device shows a code and serves its saves for ten minutes; the other types the code
//     (src/main/save_sync_wifi.h). Both open, same network.
//   - Export Save File / Import Save File (phase 1, §6): one .nbsave file the player carries over.
// Cloud Sync and Wi-Fi are behind BLITZ_SAVE_SYNC_NET (CMakeLists.txt: on in every build, public ones too, since
// 1.0.5). On the PS5 only Cloud Sync is offered: the console can't share a file or be reached by another device's app.
//
// Every import, whichever way it came, goes through phase 1's checks (save_sync_core.h read_bundle) and is put in place
// by the restore mark, never under a running game: a folder whose pak the game has open gets its import at the next
// start (controller_pak.cpp pak_sync_apply). Whatever an import replaces goes to pak_history/ first, so Controller Pak
// Backups undoes it. Nothing is replaced unasked unless it carries on from this device's saves (and, for Cloud Sync,
// nothing is sent unasked unless it carries the cloud's on). Every step prints a [sync], [cloud] or [wifi sync] line.
// None of it touches a ROM made with Export ROM or a real N64 Controller Pak.
//
// Where files go and come from (Export / Import Save File):
//   - a computer: export to Documents/New Blitz Saves (shown in the file browser); import with the file dialog, or from
//     that folder's list;
//   - a phone: export to the app folder's exports/ (the Files app on an iPhone, a file manager on Android); import with
//     the system picker (open_file_dialog: the Files sheet on iOS, ACTION_OPEN_DOCUMENT on Android) or from exports/;
//     on iOS a .nbsave opened into the app (Share → New Blitz) comes here too (firstrun.cpp).
//
// The screens are first run's (firstrun_screen.cpp), like Controller Pak Backups': D-pad and A, a tap or a click, and
// Back or B to cancel. Every screen has a way out (Cancel, Keep This Device's, Turn Off, Stop).
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "blitz_config.h"
#include "blitz_host.h"
#include "blitz_platform.h"
#include "firstrun_screen.h"
#include "net_compat.h"
#include "os_shell.h"
#include "recompui/recompui.h"
#include "ultramodern/ultramodern.hpp"
#include "util/file.h"
#include "librecomp/game.hpp"
#include "../game/save_sync_cloud.h"
#include "save_sync_wifi.h"

namespace blitz {
    void restart_fresh();     // main.cpp
    void request_restart();   // main.cpp
    void show_launcher();     // main.cpp
    void post_status(const std::string& msg);  // notices.cpp
    std::vector<std::filesystem::path> content_dirs_newest_first();  // content_dirs.cpp
    // controller_pak.cpp: a staged import in `folder`, now if its pak isn't open, else at the next start.
    bool pak_sync_apply(const std::filesystem::path& folder, bool& needs_restart, std::string& why);
    bool pak_busy();  // controller_pak.cpp: the game is writing the pak right now
    uint64_t pak_change_count();                                   // controller_pak.cpp: one per Save
    bool pak_folder_open(const std::filesystem::path& folder);     // controller_pak.cpp
}

namespace {
    namespace fs = std::filesystem;
    namespace ss = blitz::savesync;
    namespace cl = blitz::savesync::cloud;
    namespace wf = blitz::savesync::wifi;
    using blitz::firstrun_ui::Screen;
    using Clock = std::chrono::steady_clock;

    // BRANDING: the export folder in Documents
    constexpr const char* documents_name = "New Blitz Saves";
    constexpr size_t listed = 3;  // bundles offered on Import's first screen
    constexpr const char* secret_name = "cloud_sync_code";

#if defined(BLITZ_PLATFORM_PS5)
    constexpr bool on_ps5 = true;
#else
    constexpr bool on_ps5 = false;
#endif
    constexpr bool has_picker = !on_ps5;
#if defined(BLITZ_SAVE_SYNC_NET)
    constexpr bool net_build = true;
#else
    constexpr bool net_build = false;
#endif
    // Which devices each way works on, and what none of them reaches (said on the screens).
    constexpr const char* file_devices = "Mac, Windows, Linux, iPhone and iPad, and Android (not the PS5)";
    constexpr const char* not_n64 = "None of this reaches a ROM made with Export ROM or a real N64's Controller Pak.";

    // Cloud Sync's timing: sent this long after the game's last Save (once it has stopped writing), looked for this often
    // while the game isn't running (the launcher: before a season is opened) and while it is (only to say newer saves
    // are waiting for the next start).
    constexpr auto push_quiet = std::chrono::seconds(20);
    constexpr auto look_idle = std::chrono::minutes(3);
    constexpr auto look_playing = std::chrono::minutes(10);

    enum Page {
        None, Menu, Message, Pick, Decide, Confirm, Restarting,
        CloudIntro, CloudWorking, CloudCode, CloudOn, CloudOff, CloudRetry, CloudSlots, CloudSlotConfirm,
        CloudTurnedOn, CloudPair, CloudPad,
        WifiMenu, WifiSending, WifiWorking,
    };
    // Where the saves being imported came from: the questions differ a little (Cloud Sync asks only what sync_once says to
    // ask; a file or Wi-Fi asks about older saves too).
    enum Source { FromFile, FromWifi, FromCloud };

    struct State {
        int page = None;
        std::string title, body;          // Message, CloudRetry
        std::vector<fs::path> found;      // Pick: bundles in the export folders, newest first
        ss::Plan plan;
        std::vector<size_t> questions;    // Decide: the folders to ask about, in order
        size_t q = 0;                     // Decide: which question
        size_t asking = 0;                // Decide: the folder being asked about
        bool take_settings = true;
        int source = FromFile;
        bool guard = false;               // Decide (Cloud Sync): this device's saves don't carry the cloud's on
        std::string retry_what;           // CloudRetry: "join", "wifi" or "wifi-address"
        bool offer_address = false;       // CloudRetry (Wi-Fi): nothing answered; offer to type the other's address
        std::string working;              // CloudWorking / WifiWorking: what's happening; WifiSending: the code
        std::string wifi_code;            // Get Saves: the code typed, for a typed address after
        std::vector<std::string> slots;   // CloudSlots: what each slot holds
        int slot_pick = 0;                // CloudSlotConfirm: the slot to switch to
        int cancel_to = None;             // CloudWorking / WifiWorking: where Cancel goes (nothing has changed then)
        std::string digits;               // CloudPad: the number typed so far
        std::string pad_note;             // CloudPad: why the last number didn't work
    } st;

    std::atomic<bool> request{false};
    std::atomic<bool> request_cloud{false};  // first run's "Playing on multiple devices?" Yes (save_sync_open_cloud)
    bool cloud_only = false;  // opened from that question: Cancel and Back close the screens instead of showing the Saves menu
    std::atomic<int> pressed{0};
    std::atomic<bool> dirty{false};
    enum Press {
        NoPress, Export, Import, Cancel, ChooseFile, PickFound, KeepMine, UseTheirs, ImportIt, Close, Ok,
        OpenCloud, OpenWifi, TurnOn, Join, SyncNow, ShowCode, TurnOff, TurnOffDelete, AskTurnOff, Retry, BackToMenu,
        WifiSend, WifiGet, WifiStop, TypeAddress, OpenSlots, PickSlot, SwitchSlot,
        AddDevice, NewNumber, PairCancel, ShowFullCode, OpenPad, PadKey, PadDelete, PadClear, PadBack, FullCode, CloseWorking,
    };
    int picked = 0;

    std::mutex file_mutex;
    fs::path opened_file;                 // from the file dialog or another app, for the next tick
    std::atomic<bool> file_waiting{false};

    void press(int what) {
        pressed = what;
        dirty = true;
    }

    std::string utf8(const fs::path& p) {
        std::u8string s = p.u8string();
        return std::string(s.begin(), s.end());
    }

    ss::Env env() {
        ss::Env e;
        e.app = recomp::get_config_path();
        e.version = BLITZ_VERSION;
        e.build = BLITZ_BUILD;
        e.platform = blitz::platform::name;
        e.pack_installed = [](const std::string& id) {
            std::error_code ec;
            if (fs::is_regular_file(recomp::get_config_path() / "packs" / id / "pack.json", ec)) return true;
            for (const fs::path& root : blitz::content_dirs_newest_first()) {
                if (fs::is_regular_file(root / "packs" / id / "pack.json", ec)) return true;
            }
            return false;
        };
        return e;
    }

    // ---- Cloud Sync's server, code and the background work -----------------------------------------------------------
    std::string trimmed(std::string s) {
        while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
        size_t a = 0;
        while (a < s.size() && std::isspace((unsigned char)s[a])) a++;
        return s.substr(a);
    }

    // The sync server's address: BLITZ_SYNC_SERVER (tests: a local copy, tools/save_sync_worker/test/local_server.mjs),
    // else Resources/sync/server, which every build writes from build/save_sync_server (never in the source).
    std::string sync_server() {
        if (!net_build) return {};
        static const std::string s = [] {
            std::string v;
            if (const char* e = getenv("BLITZ_SYNC_SERVER"); e != nullptr) v = trimmed(e);
            else {
                std::vector<uint8_t> b = ss::read_file(recompui::file::get_program_path() / "sync" / "server");
                v = trimmed(std::string(b.begin(), b.end()));
            }
            if (v.rfind("https://", 0) != 0 && v.rfind("http://", 0) != 0) return std::string();
            if (v.back() != '/') v += '/';
            return v;
        }();
        return s;
    }
    bool cloud_available() { return net_build && !sync_server().empty(); }

    // Where the code is kept: the platform's secure storage (blitz_host.h secure_put: Keychain, Android Keystore, DPAPI,
    // the Secret Service), else a file in the app folder only this user can read. Read once a run and kept in memory (the
    // Mac may ask once whether New Blitz may read its Keychain item after an update).
    std::mutex secret_mutex;
    std::string secret_cached;
    cl::Secrets secrets() {
        cl::Secrets fallback = cl::file_secrets(recomp::get_config_path() / "cloud_sync_code");
        cl::Secrets s;
        s.kind = std::string(blitz::secure_kind()) != "none" ? blitz::secure_kind() : fallback.kind;
        s.put = [fallback](const std::string& v) {
            std::lock_guard lock{secret_mutex};
            if (blitz::secure_put(secret_name, v)) {
                fallback.erase();
                secret_cached = v;
                fprintf(stderr, "[cloud] the sync code is kept in %s\n", blitz::secure_kind());
                return true;
            }
            if (fallback.put(v)) {
                secret_cached = v;
                fprintf(stderr, "[cloud] no secure storage here: the sync code is kept in a file only this user can read\n");
                return true;
            }
            return false;
        };
        s.get = [fallback](std::string& v) {
            std::lock_guard lock{secret_mutex};
            if (!secret_cached.empty()) {
                v = secret_cached;
                return true;
            }
            if (blitz::secure_get(secret_name, v) || fallback.get(v)) {
                secret_cached = v;
                return true;
            }
            return false;
        };
        s.erase = [fallback] {
            std::lock_guard lock{secret_mutex};
            blitz::secure_erase(secret_name);
            fallback.erase();
            secret_cached.clear();
        };
        return s;
    }

    cl::State state_with_code() {
        cl::Secrets sec = secrets();
        return cl::load_state(recomp::get_config_path(), &sec);
    }
    cl::State state_quick() { return cl::load_state(recomp::get_config_path(), nullptr); }

    cl::Http http() {
        cl::Http h;
        h.get = [](const std::string& url, std::string& body, int& status) { return blitz::http_get(url, "", "", body, status); };
        h.post = [](const std::string& url, const std::string& body, int& status) {
            return blitz::http_post(url, "application/octet-stream", body, status);
        };
        h.get_auth = [](const std::string& url, const std::string& bearer, std::string& body, int& status) {
            return blitz::http_get(url, bearer, "", body, status);
        };
        return h;
    }

    // The job's cancel state: Cancel on its screen wins only before the job's first change to the device or the cloud
    // (cl::Hooks::commit); after that it finishes (the screen says so). Nothing is ever left half done.
    enum JobCancel { Running = 0, CancelPressed = 1, Committed = 2 };
    std::atomic<int> job_cancel{Running};

    bool job_commit() {
        int expect = Running;
        if (job_cancel.compare_exchange_strong(expect, Committed)) {
            dirty = true;  // the working screen loses its Cancel
            return true;
        }
        return expect == Committed;
    }

    cl::Hooks hooks(bool cancellable = true) {
        cl::Hooks h;
        h.folder_open = [](const fs::path& folder) { return blitz::pak_folder_open(folder); };
        h.apply = [](const fs::path& folder, bool& later, std::string& why) { return blitz::pak_sync_apply(folder, later, why); };
        h.busy = [] { return blitz::pak_busy(); };
        h.secrets = secrets();
        if (cancellable) h.commit = job_commit;
        return h;
    }

    // One piece of network work at a time, on its own thread; its outcome is picked up on the UI tick.
    std::atomic<bool> job_running{false};
    std::mutex job_mutex;
    bool job_done = false;
    cl::Outcome job_outcome;
    std::string job_kind;                 // "turn-on", "join", "pair-join", "pair-make", "sync", "sync-quiet",
                                          // "sync-quiet-look", "finish", "off", "slots", "switch", "wifi-get", "wifi-send"
    ss::Plan job_plan;                    // wifi-get: the read bundle
    std::string job_text;                 // wifi-send: the code; wifi-get / -send: why it failed
    bool job_flag = false;                // wifi-get: nothing answered (offer a typed address)
    std::vector<std::string> job_lines;   // slots: what each slot holds
    cl::Offer job_offer;                  // pair-make: the number made

    bool run_job(const std::string& kind, std::function<void()> work) {
        if (job_running.exchange(true)) return false;
        job_cancel = Running;
        {
            std::lock_guard lock{job_mutex};
            job_done = false;
            job_kind = kind;
        }
        std::thread([work] {
            try {
                work();
            } catch (const std::exception& e) {
                fprintf(stderr, "[cloud] stopped: %s\n", e.what());
                std::lock_guard lock{job_mutex};
                job_outcome = cl::Outcome{};
                job_outcome.result = cl::Result::Failed;
                job_outcome.message = "Something went wrong. Try again.";
            }
            {
                std::lock_guard lock{job_mutex};
                job_done = true;
            }
            job_running = false;
            dirty = true;
        }).detach();
        return true;
    }

    void finish_job(const cl::Outcome& o) {
        std::lock_guard lock{job_mutex};
        job_outcome = o;
    }

    // A question the background sync found while no one could be asked: kept for the Cloud Sync screen.
    bool ask_waiting = false;
    cl::Outcome asked;                    // the Ask outcome being answered (the screens set each folder's take)

    // Add a Device's number: shown on CloudPair until it's taken, it runs out, or the player leaves (then it's cancelled
    // on the server at once). A thread asks the server every few seconds whether the other device has taken it.
    std::mutex pair_mutex;
    cl::Offer pair_offer;                 // ok: a number is up
    Clock::time_point pair_until{};
    std::atomic<bool> pair_taken{false};
    std::atomic<unsigned> pair_gen{0};    // bumped when a number goes: its watcher stops

    // Digits typed on a keyboard while the number pad is up (firstrun_ui's typed, from SDL's event watch).
    std::mutex keys_mutex;
    std::string keys_typed;
    char pad_key = 0;                     // PadKey: the on-screen key pressed

    // The background timing.
    Clock::time_point next_look{};
    uint64_t changes_seen = 0;
    Clock::time_point changed_at{};
    bool push_due = false;
    bool started = false;

    std::string ago(std::time_t t) {
        if (t == 0) return "not yet";
        long s = (long)(std::time(nullptr) - t);
        if (s < 90) return "just now";
        if (s < 90 * 60) return std::to_string((s + 30) / 60) + " minutes ago";
        if (s < 36 * 3600) return std::to_string((s + 1800) / 3600) + " hours ago";
        return std::to_string((s + 43200) / 86400) + " days ago";
    }

    std::string slot_word(int slot) { return "Slot " + std::to_string(slot); }

    // Where an export goes, and the folders Import lists.
    fs::path export_folder() {
        if (blitz::platform::desktop) {
            fs::path docs = blitz::documents_folder();
            if (!docs.empty()) return docs / documents_name;
        }
        return recomp::get_config_path() / "exports";
    }

    std::vector<fs::path> find_bundles() {
        std::vector<std::pair<fs::file_time_type, fs::path>> all;
        std::vector<fs::path> dirs = {export_folder(), recomp::get_config_path() / "exports"};
        if (dirs[0] == dirs[1]) dirs.pop_back();
        for (const fs::path& dir : dirs) {
            std::error_code ec;
            for (fs::directory_iterator it{dir, ec}, end; !ec && it != end; it.increment(ec)) {
                std::error_code fec;
                if (it->path().extension() != ss::extension || !it->is_regular_file(fec)) continue;
                all.push_back({it->last_write_time(fec), it->path()});
            }
        }
        std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::vector<fs::path> out;
        for (const auto& [t, p] : all) out.push_back(p);
        return out;
    }

    void message(const std::string& title, const std::string& body) {
        st.page = Message;
        st.title = title;
        st.body = body;
    }

    // The launcher steps aside while these screens are up (it would take the D-pad from them), as for Pak Backups.
    void launcher_visible(bool on) {
        if (ultramodern::is_game_started()) return;
        recompui::ContextId launcher = recompui::get_launcher_context_id();
        if (launcher == recompui::ContextId::null()) return;
        if (on) blitz::show_launcher();
        else if (recompui::is_context_shown(launcher)) recompui::hide_context(launcher);
    }

    // A file or Wi-Fi import asks about saves both devices changed and older ones; Cloud Sync's questions come from
    // sync_once (save_sync_cloud.cpp: conflicts, the push guard, switching slots).
    bool asks(const ss::FolderPlan& f) { return f.verdict == ss::Verdict::Conflict || f.verdict == ss::Verdict::Older; }

    // What the confirm screen lists, and whether anything changes.
    bool anything_to_do() {
        for (const ss::FolderPlan& f : st.plan.folders) {
            if (f.take) return true;
        }
        // Cloud Sync brings league settings only along with saves (save_sync_cloud.cpp apply_pull).
        return st.source != FromCloud && !st.plan.settings.empty() && st.take_settings;
    }

    void start_finish();

    // The next question from `from`, or the confirm screen.
    void next_question(size_t from) {
        if (from < st.questions.size()) {
            st.q = from;
            st.asking = st.questions[from];
            st.page = Decide;
            return;
        }
        if (st.source == FromCloud && !anything_to_do()) {
            start_finish();  // kept all of this device's: they go up
            return;
        }
        st.page = Confirm;
        if (!anything_to_do() && st.plan.settings.empty()) {
            bool kept = std::any_of(st.plan.folders.begin(), st.plan.folders.end(), [](const ss::FolderPlan& f) { return f.verdict != ss::Verdict::Same; });
            fprintf(stderr, "[sync] import: nothing to change\n");
            blitz::post_status(kept ? "Nothing imported: you kept your saves" : "Your saves already match");
            st = State{};
        }
    }

    void do_export() {
        if (blitz::pak_busy()) {
            blitz::post_status("The game is saving: try Export again in a moment");
            st = State{};
            return;
        }
        ss::Env e = env();
        ss::ExportResult r = ss::make_bundle(e);
        if (!r.ok) {
            blitz::post_status(r.why.empty() ? "Couldn't export your saves" : r.why);
            st = State{};
            return;
        }
        fs::path dir = export_folder();
        fs::path file = dir / ss::bundle_file_name(std::time(nullptr));
        if (!ss::write_file(file, r.zip)) {
            fprintf(stderr, "[sync] export: couldn't write %s\n", utf8(file).c_str());
            message("Couldn't export", "Your saves couldn't be written in " + utf8(dir) + ".");
            return;
        }
        fprintf(stderr, "[sync] exported to %s\n", utf8(file).c_str());
        std::string what = std::to_string(r.paks) + (r.paks == 1 ? " Controller Pak" : " Controller Paks");
        if (r.seasons > 0) what += " and " + std::to_string(r.seasons) + (r.seasons == 1 ? " season file" : " season files");
        std::string where = blitz::platform::desktop ? "It's in Documents \xE2\x86\x92 " + std::string(documents_name) + "."
                                                     : "It's in New Blitz's folder, in exports (the Files app on an iPhone, a file "
                                                       "manager on Android).";
        message("Saves exported", what + " in " + file.filename().string() + ".\n\n" + where +
                                      " Copy it to your other device and choose Import Save File there (" + file_devices +
                                      "). Save states stay on each device. " + not_n64);
        if (blitz::platform::file_browser) blitz::reveal_in_file_browser(file);
    }

    // A bundle read and checked: the questions, then the confirm screen. For Cloud Sync, `questions` are sync_once's.
    void take_plan(ss::Plan plan, int source, std::vector<size_t> questions = {}, bool guard = false) {
        st.plan = std::move(plan);
        st.source = source;
        st.guard = guard;
        if (!st.plan.ok) {
            message("Can't import these saves", st.plan.why + "\n\nNothing was changed.");
            return;
        }
        st.take_settings = true;  // their league settings, when they differ (the confirm screen's box)
        st.questions.clear();
        if (source == FromCloud) st.questions = std::move(questions);
        else {
            for (size_t i = 0; i < st.plan.folders.size(); i++) {
                if (asks(st.plan.folders[i])) st.questions.push_back(i);
            }
        }
        next_question(0);
    }

    void read_bundle_file(const fs::path& path) {
        bool ok = false;
        std::vector<uint8_t> data = ss::read_file(path, &ok);
        fprintf(stderr, "[sync] import: reading %s (%zu bytes)\n", path.filename().string().c_str(), data.size());
        ss::Plan plan = ok ? ss::read_bundle(env(), data) : ss::Plan{};
        if (!ok) plan.why = "That file couldn't be read.";
        take_plan(std::move(plan), FromFile);
    }

    void do_import() {
        if (st.source == FromCloud) {
            start_finish();
            return;
        }
        ss::Env e = env();
        std::string why;
        if (!ss::stage(e, st.plan, st.take_settings, why)) {
            message("Couldn't import", why + "\n\nNothing was changed.");
            return;
        }
        ss::record_import(e, st.plan);
        bool restart = st.take_settings && !st.plan.settings.empty();
        std::string failed;
        for (const fs::path& folder : ss::staged_folders(e, st.plan)) {
            bool later = false;
            std::string w;
            if (!blitz::pak_sync_apply(folder, later, w)) failed = w;
            restart = restart || later;
        }
        if (!failed.empty()) {
            message("Not all of it went in", failed + "\n\nNew Blitz tries again the next time it starts. Your saves from before are in "
                                                      "Controller Pak Backups.");
            return;
        }
        if (restart) {
            st.page = Restarting;
            return;
        }
        blitz::post_status("Saves imported");
        st = State{};
    }

    // ---- Cloud Sync's actions (each a job) ---------------------------------------------------------------------------
    // A job the player started from a screen: the working screen while it runs, and where Cancel goes (before the job's
    // first change, nothing has changed). False (and a toast) if another job is still running, so no screen is left
    // waiting on a job that never started.
    bool start_job(const std::string& kind, const std::string& what, int cancel_to, std::function<void()> work, int page = CloudWorking) {
        if (job_running) {
            blitz::post_status("Cloud Sync is busy: try again in a moment");
            return false;
        }
        if (!run_job(kind, std::move(work))) return false;
        st.page = page;
        st.working = what;
        st.cancel_to = cancel_to;
        return true;
    }

    // The background's rounds (never cancelled: nobody is looking at them).
    void start_sync(bool can_ask, const std::string& kind) {
        run_job(kind, [can_ask] {
            ss::Env e = env();
            cl::State s = state_with_code();
            finish_job(cl::sync_once(e, http(), sync_server(), s, hooks(false), can_ask));
        });
    }

    // Sync Now: its screen's Cancel stops it before anything is sent or taken.
    void start_sync_now() {
        start_job("sync", "Syncing\xE2\x80\xA6", CloudOn, [] {
            ss::Env e = env();
            cl::State s = state_with_code();
            finish_job(cl::sync_once(e, http(), sync_server(), s, hooks(), true));
        });
    }

    void start_turn_on() {
        start_job("turn-on", "Turning Cloud Sync on\xE2\x80\xA6", CloudIntro, [] {
            ss::Env e = env();
            cl::State s = state_with_code();
            finish_job(cl::turn_on(e, http(), sync_server(), s, hooks()));
        });
    }

    void start_join(const std::string& typed) {
        start_job("join", "Looking for your saves\xE2\x80\xA6", CloudPad, [typed] {
            ss::Env e = env();
            cl::State s = state_with_code();
            finish_job(cl::join(e, http(), sync_server(), s, hooks(), typed));
        });
    }

    // Join with the 6-digit number: the code comes from the server (sealed with the number), never typed or shown.
    bool start_pair_join(const std::string& number) {
        return start_job("pair-join", "Joining\xE2\x80\xA6", CloudPad, [number] {
            cl::Claimed c = cl::claim_pairing(http(), sync_server(), number);
            if (!c.ok) {
                cl::Outcome o;
                o.result = c.status == 0 || c.status == 429 || c.status >= 500 ? cl::Result::Offline : cl::Result::Failed;
                o.message = c.message;
                finish_job(o);
                return;
            }
            ss::Env e = env();
            cl::State s = state_with_code();
            finish_job(cl::join(e, http(), sync_server(), s, hooks(), c.code, c.slot));
        });
    }

    void start_finish() {
        asked.plan = st.plan;
        if (!st.take_settings) asked.plan.settings.clear();  // the box unticked: saves only
        const bool joining = !asked.pending.code.empty();
        start_job("finish", "Syncing\xE2\x80\xA6", joining ? CloudIntro : CloudOn, [] {
            ss::Env e = env();
            cl::State s = state_with_code();
            finish_job(cl::finish_pull(e, http(), sync_server(), s, hooks(), asked));
        });
    }

    void start_turn_off(bool delete_cloud) {
        start_job("off", delete_cloud ? "Turning off and deleting the cloud copies\xE2\x80\xA6" : "Turning off\xE2\x80\xA6", CloudOn, [delete_cloud] {
            cl::Outcome o;
            // All or nothing: Cancel counts only before the first step.
            if (!job_commit()) {
                o.result = cl::Result::Cancelled;
                finish_job(o);
                return;
            }
            ss::Env e = env();
            cl::State s = state_with_code();
            o.result = cl::Result::Off;
            if (delete_cloud && !s.code.empty()) {
                for (int slot = 1; slot <= cl::slot_count; slot++) {
                    int status = cl::remove(http(), sync_server(), cl::derive(s.code, slot));
                    if (status != 200) o.message = "The cloud copies couldn't all be deleted now (" + std::to_string(status) + "). They go by themselves after 90 days.";
                }
            }
            secrets().erase();
            cl::store_state(e.app, cl::State{});
            std::error_code ec;
            fs::remove_all(e.app / "cloud_sync", ec);  // the synced copies
            fprintf(stderr, "[cloud] turned off%s\n", delete_cloud ? " (cloud copies deleted)" : "");
            finish_job(o);
        });
    }

    // What each slot holds, for the slots screen: its saves' summary, from the copy this device kept or the server.
    void start_slots() {
        start_job("slots", "Looking at your slots\xE2\x80\xA6", CloudOn, [] {
            ss::Env e = env();
            cl::State s = state_with_code();
            std::vector<std::string> lines;
            bool offline = false;
            for (int slot = 1; slot <= cl::slot_count; slot++) {
                cl::Keys k = cl::derive(s.code, slot);
                cl::Head h = cl::head(http(), sync_server(), k);
                std::string line = slot_word(slot) + (slot == s.slot ? " (this device)" : "") + ": ";
                if (!h.ok) {
                    offline = true;
                    lines.push_back(line + "can't be reached now");
                    continue;
                }
                if (h.rev == 0) {
                    lines.push_back(line + "empty");
                    continue;
                }
                cl::Bytes zip = cl::synced_copy(e.app, slot, h.rev);
                if (zip.empty()) {
                    int status = 0;
                    cl::Opened op = cl::open(k, cl::fetch(http(), sync_server(), k, status));
                    if (op.ok) {
                        zip = std::move(op.plain);
                        cl::keep_synced_copy(e.app, slot, h.rev, zip);
                    }
                }
                ss::Summary sum = ss::summarize(zip);
                if (!sum.ok) {
                    lines.push_back(line + "saves that can't be read");
                    continue;
                }
                std::string what;
                for (const std::string& f : sum.folders) what += (what.empty() ? "" : "; ") + f;
                lines.push_back(line + what + " (" + sum.device + ", " + sum.made + ")");
            }
            std::lock_guard lock{job_mutex};
            job_lines = lines;
            job_outcome = cl::Outcome{};
            job_outcome.result = offline ? cl::Result::Offline : cl::Result::UpToDate;
        });
    }

    // Switching slots: this device syncs with `slot` from now on. Nothing on the device, or in that slot, is replaced
    // without a question; until it goes through (or the questions are answered), the device stays on its old slot.
    void start_switch(int slot) {
        start_job("switch", "Switching to " + slot_word(slot) + "\xE2\x80\xA6", CloudSlots, [slot] {
            ss::Env e = env();
            cl::State s = state_with_code();
            finish_job(cl::switch_slot(e, http(), sync_server(), s, hooks(), slot));
        });
    }

    // ---- Add a Device ------------------------------------------------------------------------------------------------
    void stop_pairing(bool cancel_on_server) {
        cl::Offer o;
        {
            std::lock_guard lock{pair_mutex};
            o = pair_offer;
            pair_offer = cl::Offer{};
        }
        pair_gen++;
        pair_taken = false;
        if (cancel_on_server && o.ok) std::thread([o] { cl::cancel_pairing(http(), sync_server(), o); }).detach();
    }

    bool pairing_up() {
        std::lock_guard lock{pair_mutex};
        return pair_offer.ok;
    }

    // Every few seconds while the number is up: has the other device taken it?
    void watch_pairing(const cl::Offer& o) {
        const unsigned gen = pair_gen;
        const Clock::time_point until = pair_until;
        std::thread([o, gen, until] {
            while (pair_gen == gen && Clock::now() < until) {
                for (int i = 0; i < 30 && pair_gen == gen; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (pair_gen != gen) return;
                bool live = true;
                // Not live any more, well before it runs out: the other device took it.
                if (cl::pairing_live(http(), sync_server(), o, live) && !live && Clock::now() < until - std::chrono::seconds(2)) {
                    if (pair_gen == gen) {
                        pair_taken = true;
                        dirty = true;
                    }
                    return;
                }
            }
            dirty = true;  // ran out: the screen says so
        }).detach();
    }

    // A new number (New Number: the one before stops working at once).
    void start_pair_make() {
        cl::Offer before;
        {
            std::lock_guard lock{pair_mutex};
            before = pair_offer;
        }
        pair_gen++;  // the old number's watcher stops
        start_job("pair-make", "Getting a number\xE2\x80\xA6", CloudOn, [before] {
            cl::State s = state_with_code();
            cl::Offer o = cl::offer_pairing(http(), sync_server(), s.code, s.slot, before.ok ? &before : nullptr);
            std::lock_guard lock{job_mutex};
            job_offer = o;
            job_outcome = cl::Outcome{};
            job_outcome.result = o.ok ? cl::Result::UpToDate : cl::Result::Failed;
            job_outcome.message = o.message;
        });
    }

    void open_join_keyboard() {
        recompui::open_keyboard("Sync code (24 letters and numbers)", "", 29, true, [](const std::string& typed) {
            if (cl::normalize_code(typed).empty()) {
                st.page = CloudRetry;
                st.retry_what = "join";
                st.offer_address = false;
                st.title = "That isn't a sync code";
                st.body = "A sync code has 24 letters and numbers, like ABCD-EFGH-JKMN-PQRS-TVWX-YZ12. It's on the other device: "
                          "Settings \xE2\x86\x92 Saves \xE2\x86\x92 Cloud Sync \xE2\x86\x92 Show Full Code.";
            }
            else start_join(typed);
            dirty = true;
        });
    }

    void open_pad() {
        st.page = CloudPad;
        st.digits.clear();
        {
            std::lock_guard lock{keys_mutex};
            keys_typed.clear();
        }
    }

    // A key on the pad (or the keyboard): six digits and it joins.
    void pad_digit(char d) {
        if (st.page != CloudPad || st.digits.size() >= cl::pair_digits) return;
        st.digits += d;
        st.pad_note.clear();
        // Busy (a background sync running): the last digit comes off, so it can be typed again in a moment.
        if (st.digits.size() == cl::pair_digits && !start_pair_join(st.digits)) st.digits.pop_back();
    }

    // ---- Sync over Wi-Fi's actions -----------------------------------------------------------------------------------
    void start_wifi_send() {
        if (blitz::pak_busy()) {
            blitz::post_status("The game is saving: try again in a moment");
            return;
        }
        start_job("wifi-send", "Getting your saves ready\xE2\x80\xA6", WifiMenu, [] {
            ss::ExportResult r = ss::make_bundle(env());
            std::string code, why;
            if (!r.ok) why = r.why.empty() ? "Couldn't gather your saves." : r.why;
            else if (!wf::start_sending(r.zip, blitz::net::local_address(), code, why)) {}
            std::lock_guard lock{job_mutex};
            job_text = code.empty() ? why : code;
            job_outcome = cl::Outcome{};
            job_outcome.result = code.empty() ? cl::Result::Failed : cl::Result::UpToDate;
        }, WifiWorking);
    }

    void start_wifi_get(const std::string& typed, const std::string& address) {
        if (start_job("wifi-get", "Getting the saves\xE2\x80\xA6", WifiMenu, [typed, address] {
                wf::Got g = address.empty() ? wf::fetch(typed, blitz::net::local_address()) : wf::fetch_at(typed, address);
                ss::Plan plan;
                if (g.ok) plan = ss::read_bundle(env(), g.zip);
                std::lock_guard lock{job_mutex};
                job_outcome = cl::Outcome{};
                job_outcome.result = g.ok ? cl::Result::Pulled : cl::Result::Failed;
                job_text = g.why;
                job_flag = g.no_answer;
                job_plan = std::move(plan);
            }, WifiWorking))
            st.wifi_code = typed;
    }

    void open_wifi_keyboard() {
        recompui::open_keyboard("The code on the other device (12 letters and numbers)", "", 14, true, [](const std::string& typed) {
            wf::Code c;
            if (!wf::parse_code(typed, c)) {
                st.page = CloudRetry;
                st.retry_what = "wifi";
                st.offer_address = false;
                st.title = "That isn't the code";
                st.body = "The code has 12 letters and numbers, like ABCD-EFGH-JKMN. It's on the other device's Send My Saves screen.";
            }
            else start_wifi_get(typed, "");
            dirty = true;
        });
    }

    void open_address_keyboard() {
        recompui::open_keyboard("The other device's address (under its code)", "", 15, false, [](const std::string& typed) {
            start_wifi_get(st.wifi_code, trimmed(typed));
            dirty = true;
        });
    }

    // ---- what a finished job means for the screens -------------------------------------------------------------------
    void show_questions(const cl::Outcome& o) {
        asked = o;
        ask_waiting = false;
        take_plan(o.plan, FromCloud, o.asked, o.guard);
        dirty = true;
    }

    // The slot the questions are about: a switch's new slot, else the one this device syncs with.
    int asked_slot() { return asked.pending.slot != 0 ? asked.pending.slot : state_quick().slot; }

    void cloud_outcome(const std::string& kind, const cl::Outcome& o) {
        const bool on_screen = st.page == CloudWorking;
        const bool quiet = kind == "sync-quiet";                       // after a save, maybe mid-game: never a screen
        const bool background = quiet || kind == "sync-quiet-look";    // nobody asked: errors only on the Cloud Sync screen
        static uint64_t told_waiting = 0;                              // "waiting for the next start" said once per revision
        if (kind == "off" && o.result == cl::Result::Off) ask_waiting = false;  // no questions left for a device that's off
        switch (o.result) {
            case cl::Result::Cancelled:
                return;  // the player cancelled before anything changed: the screen already went back
            case cl::Result::Ask:
                if (on_screen || (!quiet && !ultramodern::is_game_started() && !blitz::firstrun_ui::shown())) show_questions(o);
                else if (o.pending.slot != 0) {
                    // A join's or a switch's questions with nobody looking (the screen was closed): nothing was kept.
                    blitz::post_status(o.pending.switching ? "Cloud Sync: still syncing with " + slot_word(state_quick().slot)
                                                           : "Cloud Sync: not joined. Nothing changed.");
                }
                else {
                    asked = o;
                    ask_waiting = true;
                    blitz::post_status("Cloud Sync: a save needs your choice. Settings \xE2\x86\x92 Saves \xE2\x86\x92 Cloud Sync");
                }
                return;
            case cl::Result::Pulled:
                blitz::post_status("Cloud Sync: " + o.message);
                break;
            case cl::Result::Waiting:
                if (kind == "finish" || kind == "join" || kind == "pair-join" || kind == "switch") {
                    if (o.message.find("starts again") != std::string::npos) {
                        st.page = Restarting;  // what the player chose goes in at the next start: restart now (phase 1's way)
                        dirty = true;
                        return;
                    }
                }
                if (!o.message.empty() && (!background || told_waiting != o.server_rev)) blitz::post_status("Cloud Sync: " + o.message);
                told_waiting = o.server_rev;
                break;
            case cl::Result::Offline:
            case cl::Result::Failed:
                if (!background && !on_screen && !o.message.empty()) blitz::post_status("Cloud Sync: " + o.message);
                break;
            default: break;
        }
        if (!on_screen) return;
        // The screen that started it.
        if (kind == "turn-on") {
            if (o.result == cl::Result::Offline || o.result == cl::Result::Failed || o.result == cl::Result::Busy) {
                message("Cloud Sync is off", (o.message.empty() ? std::string("The game is saving.") : o.message) +
                                                 "\n\nNothing changed. Try again in a moment.");
                return;
            }
            st.page = CloudTurnedOn;
        }
        else if (kind == "join" || kind == "pair-join") {
            if (o.result == cl::Result::Failed || o.result == cl::Result::Offline || o.result == cl::Result::Busy) {
                if (kind == "pair-join") {
                    // Back to the pad, the number cleared, saying why. Nothing on this device changed.
                    open_pad();
                    st.pad_note = o.message.empty() ? "The game is saving: try again in a moment." : o.message;
                    return;
                }
                st.page = CloudRetry;
                st.retry_what = "join";
                st.offer_address = false;
                st.title = o.result == cl::Result::Offline ? "Can't reach the sync server" : "No saves for that code";
                st.body = o.message + "\n\nNothing on this device changed.";
                return;
            }
            message("Cloud Sync is on", (o.result == cl::Result::Pulled ? o.message + ". " : std::string()) +
                                            "From now on your saves go up after each save and come down when New Blitz starts.");
        }
        else if (kind == "off") {
            message("Cloud Sync is off", (o.message.empty() ? std::string() : o.message + "\n\n") +
                                             "Your saves stay on this device. To sync again, turn it on, or join with a number.");
        }
        else if (kind == "finish" && !asked.pending.code.empty() && o.result != cl::Result::Pushed && o.result != cl::Result::Pulled &&
                 o.result != cl::Result::UpToDate) {
            message("Not joined", (o.message.empty() ? std::string() : o.message + "\n\n") + "Nothing on this device changed. Try again.");
        }
        else if (kind == "finish" && !asked.pending.code.empty()) {
            message("Cloud Sync is on", (o.result == cl::Result::Pulled ? o.message + ". " : std::string()) +
                                            "From now on your saves go up after each save and come down when New Blitz starts.");
        }
        else {
            st.page = CloudOn;
            if (kind == "switch" && o.result != cl::Result::Pushed && o.result != cl::Result::Pulled && o.result != cl::Result::UpToDate &&
                o.result != cl::Result::Waiting)
                blitz::post_status("Cloud Sync: still syncing with " + slot_word(state_quick().slot) + (o.message.empty() ? "" : ". " + o.message));
            else if (o.result == cl::Result::Pushed) blitz::post_status("Cloud Sync: your saves are up to date");
            else if (o.result == cl::Result::UpToDate) blitz::post_status("Cloud Sync: everything matches");
            else if ((o.result == cl::Result::Failed || o.result == cl::Result::Offline || o.result == cl::Result::Waiting) && !o.message.empty())
                blitz::post_status("Cloud Sync: " + o.message);
        }
    }

    void job_finished() {
        std::string kind;
        cl::Outcome o;
        ss::Plan plan;
        std::string text;
        bool flag = false;
        std::vector<std::string> lines;
        cl::Offer offer;
        {
            std::lock_guard lock{job_mutex};
            if (!job_done) return;
            job_done = false;
            kind = job_kind;
            o = job_outcome;
            plan = std::move(job_plan);
            text = job_text;
            flag = job_flag;
            lines = job_lines;
            offer = job_offer;
            job_offer = cl::Offer{};
        }
        const bool cancelled = job_cancel == CancelPressed;
        if (kind == "wifi-send") {
            if (cancelled) wf::stop_sending();  // the player left meanwhile
            else if (o.result == cl::Result::Failed) message("Can't send your saves", text);
            else if (st.page == WifiWorking) {
                st.page = WifiSending;
                st.working = text;  // the code
            }
            else wf::stop_sending();
            dirty = true;
            return;
        }
        if (kind == "wifi-get") {
            if (cancelled || st.page != WifiWorking) return;
            if (o.result == cl::Result::Failed) {
                st.page = CloudRetry;
                st.retry_what = "wifi";
                st.offer_address = flag;
                st.title = "Couldn't get the saves";
                st.body = text + (flag ? "\n\nOr type the address the other device shows under its code." : "");
            }
            else take_plan(std::move(plan), FromWifi);
            dirty = true;
            return;
        }
        if (kind == "slots") {
            if (cancelled || st.page != CloudWorking) return;
            st.slots = lines;
            st.page = CloudSlots;
            dirty = true;
            return;
        }
        if (kind == "pair-make") {
            if (cancelled || st.page != CloudWorking) {
                // Cancelled while it was being made: gone from the server at once.
                if (offer.ok) std::thread([offer] { cl::cancel_pairing(http(), sync_server(), offer); }).detach();
                return;
            }
            if (!offer.ok) {
                message("Can't add a device now", o.message + "\n\nOr use Show Full Code and type that code on the other device.");
                dirty = true;
                return;
            }
            {
                std::lock_guard lock{pair_mutex};
                pair_offer = offer;
            }
            pair_taken = false;
            pair_until = Clock::now() + std::chrono::seconds(cl::pair_seconds) - std::chrono::seconds(3);  // a little early
            watch_pairing(offer);
            st.page = CloudPair;
            dirty = true;
            return;
        }
        if (cancelled) {
            // Cancelled before anything changed (a join, turn-on or switch was put back as it was). An ordinary sync's
            // questions wait for later.
            fprintf(stderr, "[cloud] %s: cancelled (%s)\n", kind.c_str(), cl::result_name(o.result));
            if (kind == "finish" && asked.pending.slot == 0) ask_waiting = true;
            if (o.result == cl::Result::Ask && o.pending.slot == 0) {
                asked = o;
                ask_waiting = true;
            }
            dirty = true;
            return;
        }
        cloud_outcome(kind, o);
        dirty = true;
    }

    std::atomic<bool> sync_asked{false};  // Settings → Saves → Sync Now

    // Every UI tick, with Cloud Sync on: a look at launch, sending after the game saves, looking again now and then.
    void cloud_background() {
        if (!cloud_available()) return;
        static bool on = false;
        static Clock::time_point state_read{};
        const auto now = Clock::now();
        if (!started || now - state_read > std::chrono::seconds(5)) {
            on = state_quick().on;
            state_read = now;
        }
        if (!on) {
            started = true;
            if (sync_asked.exchange(false)) blitz::post_status("Cloud Sync is off: Saves \xE2\x86\x92 Open to turn it on");
            return;
        }
        // Settings → Saves → Sync Now (config.cpp): a sync at once, its outcome in a toast.
        if (sync_asked.exchange(false)) {
            if (job_running) blitz::post_status("Cloud Sync is already syncing");
            else {
                blitz::post_status("Syncing\xE2\x80\xA6");
                push_due = false;
                start_sync(true, "sync-now-row");
                return;
            }
        }
        if (!started) {
            started = true;
            changes_seen = blitz::pak_change_count();
            next_look = now;  // at launch, before a season is opened
        }
        const uint64_t changes = blitz::pak_change_count();
        if (changes != changes_seen) {
            changes_seen = changes;
            changed_at = now;
            push_due = true;
        }
        if (job_running || recompui::is_keyboard_open()) return;
        if (push_due && now - changed_at >= push_quiet && !blitz::pak_busy()) {
            push_due = false;
            next_look = now + (ultramodern::is_game_started() ? look_playing : look_idle);
            start_sync(false, "sync-quiet");
            return;
        }
        if (now >= next_look && st.page == None) {
            next_look = now + (ultramodern::is_game_started() ? look_playing : look_idle);
            start_sync(true, "sync-quiet-look");
        }
    }

    // ---- the screens -------------------------------------------------------------------------------------------------
    // The question for one folder: both sides with team, week, when each was saved and which device; the cloud's side
    // named by its slot.
    void decide_screen(Screen& s, const std::string& device) {
        const ss::FolderPlan& f = st.plan.folders[st.asking];
        auto when = [](const std::string& w) { return w.empty() ? std::string() : ", saved " + w; };
        if (st.source != FromCloud) {
            if (f.verdict == ss::Verdict::Older) {
                s.title = "These saves are older";
                s.body = f.label + "\n\nThis device" + when(f.mine_when) + ": " + f.mine + "\n" + device + when(f.theirs_when) + " (older): " +
                         f.theirs + "\n\nThis device has saved since. Keep yours, or use the older ones anyway (yours go to Controller Pak "
                                    "Backups first).";
                s.actions = {{"Keep Mine", [] { press(KeepMine); }}, {"Use Theirs Anyway", [] { press(UseTheirs); }}, {"Cancel", [] { press(Cancel); }}};
            }
            else {
                s.title = "Both devices changed these saves";
                s.body = f.label + "\n\nThis device" + when(f.mine_when) + ": " + f.mine + "\n" + device + when(f.theirs_when) + ": " + f.theirs +
                         "\n\nPick which to keep. If you use theirs, yours go to Controller Pak Backups first.";
                s.actions = {{"Keep Mine", [] { press(KeepMine); }}, {"Use Theirs", [] { press(UseTheirs); }}, {"Cancel", [] { press(Cancel); }}};
            }
            return;
        }
        const int slot = asked_slot();
        const bool switching = asked.pending.switching;
        const bool joining = !asked.pending.code.empty();
        const std::string cloud = switching ? slot_word(slot) : "The cloud (" + slot_word(slot) + ")";
        const std::string mine_dev = ss::device_label(env());
        std::string why;
        switch (f.verdict) {
            case ss::Verdict::Conflict: why = "Both changed since they last matched."; break;
            case ss::Verdict::Newer:
                why = st.guard ? "The cloud's are newer than this device's (a backup put back here?)." : cloud + "'s carry on from this device's.";
                break;
            case ss::Verdict::New: why = "Only the cloud has saves here: keeping this device's leaves them out of the cloud."; break;
            case ss::Verdict::Older: why = "This device's carry on from " + cloud + "'s: keeping them puts them in " + slot_word(slot) + " in place of what it holds."; break;
            default: break;
        }
        s.title = switching ? slot_word(slot) + " holds other saves"
                  : joining ? "This device has saves of its own"
                  : st.guard ? "These saves don't carry on from the cloud's"
                             : "Both devices changed these saves";
        if (joining) why = "Joining: choose which to keep. Cancel leaves this device as it was, not joined.";
        else if (switching) why += " Cancel stays on " + slot_word(state_quick().slot) + ".";
        s.body = f.label + "\n\nThis device (" + mine_dev + when(f.mine_when) + "): " + f.mine + "\n" + cloud + " (" +
                 (st.plan.device.empty() ? "another device" : st.plan.device) + when(f.theirs_when) + "): " + f.theirs + "\n\n" + why +
                 " Whichever you don't keep stays in Controller Pak Backups here, or as the cloud's previous copy.";
        s.actions = {{"Keep This Device's", [] { press(KeepMine); }},
                     {switching ? "Use " + slot_word(slot) + "'s" : "Use the Cloud's", [] { press(UseTheirs); }},
                     {"Cancel", [] { press(Cancel); }}};
    }

    void show() {
        Screen s;
        const std::string device = st.plan.device.empty() ? "The other device" : st.plan.device;
        auto cancel_only = [&] {
            s.back = [] { press(Cancel); };
            s.back_button = false;
        };
        switch (st.page) {
            case Menu: {
                s.title = "Saves";
                if (on_ps5) {
                    s.body = "Your Controller Pak saves and Advanced seasons, on your other devices. On the PS5 only Cloud Sync works: "
                             "the console can't share a save file or be reached over Wi-Fi by another device. Save states stay on each "
                             "device. " + std::string(not_n64);
                    if (!cloud_available()) s.note = "This build has no Cloud Sync.";
                }
                else
                    s.body = "Your Controller Pak saves and Advanced seasons, on your other devices. Save states stay on each device. " +
                             std::string(not_n64);
                if (cloud_available()) s.actions.push_back({state_quick().on ? "Cloud Sync: On" : "Cloud Sync", [] { press(OpenCloud); }});
                if (net_build && !on_ps5) s.actions.push_back({"Sync over Wi-Fi", [] { press(OpenWifi); }});
                if (!on_ps5) {
                    s.actions.push_back({"Export Save File", [] { press(Export); }});
                    s.actions.push_back({"Import Save File", [] { press(Import); }});
                }
                s.actions.push_back({"Cancel", [] { press(Cancel); }});
                cancel_only();
                break;
            }
            case Message:
                s.title = st.title;
                s.body = st.body;
                s.actions = {{"OK", [] { press(Ok); }}};
                s.back = [] { press(Ok); };
                s.back_button = false;
                break;
            case Pick: {
                s.title = "Import Save File";
                s.body = std::string("Choose a New Blitz saves file (.nbsave) made with Export Save File. Works on ") + file_devices + ".";
                if (has_picker) s.actions.push_back({"Choose a File", [] { press(ChooseFile); }});
                for (size_t i = 0; i < st.found.size() && i < listed; i++) {
                    int n = (int)i;
                    s.actions.push_back({st.found[i].filename().string(), [n] { picked = n; press(PickFound); }});
                }
                s.actions.push_back({"Cancel", [] { press(Cancel); }});
                cancel_only();
                break;
            }
            case Decide:
                decide_screen(s, device);
                cancel_only();
                break;
            case Confirm: {
                s.title = "Use these saves?";
                std::string body = "From " + device + (st.plan.made.empty() ? "" : ", " + st.plan.made) + ":";
                for (const ss::FolderPlan& f : st.plan.folders) {
                    body += "\n" + f.label + ": ";
                    if (f.take) body += f.theirs;
                    else if (f.verdict == ss::Verdict::Same) body += "already the same";
                    else body += "keeping yours";
                }
                bool running = ultramodern::is_game_started();
                body += "\n\nWhat it replaces goes to Controller Pak Backups first.";
                if (running || (st.source != FromCloud && !st.plan.settings.empty() && st.take_settings))
                    body += blitz::restart_available ? " New Blitz may restart to finish." : " New Blitz may close to finish; open it again after.";
                s.body = body;
                if (!st.plan.settings.empty()) {
                    s.checks.push_back({"Also use their Season and Rules settings",
                                        "The league these seasons were played in. From the next start.",
                                        [] { return st.take_settings; }, [](bool on) { st.take_settings = on; }});
                }
                s.actions = {
                    {"Use These Saves", [] { press(ImportIt); }},
                    {"Cancel", [] { press(Cancel); }},
                };
                cancel_only();
                break;
            }
            case Restarting:
                s.title = "Ready to finish";
                s.body = blitz::restart_available ? "New Blitz restarts to finish putting your saves in place."
                                                  : "New Blitz closes now. Open it again and your saves are there.";
                s.actions = {{blitz::restart_available ? "Restart" : "Close New Blitz", [] { press(Close); }}};
                break;
            case CloudIntro:
                s.title = "Cloud Sync";
                s.body = "Keeps your saves the same on all your devices (Mac, Windows, Linux, iPhone and iPad, Android and the PS5): "
                         "they go up after each save and come down when New Blitz starts.\n\nTurn it on here. Then, on the device that "
                         "has it on, Add a Device shows a 6-digit number: choose Join here and type it. Your saves are locked before "
                         "they leave this device, so the server can't read them. No account, no name. Save states are never synced. " +
                         std::string(not_n64);
                s.actions = {
                    {"Turn On Cloud Sync", [] { press(TurnOn); }},
                    {"Join", [] { press(OpenPad); }},
                    {"Cancel", [] { press(BackToMenu); }},
                };
                s.back = [] { press(BackToMenu); };
                s.back_button = false;
                break;
            case CloudWorking:
            case WifiWorking:
                s.title = st.page == CloudWorking ? "Cloud Sync" : "Sync over Wi-Fi";
                if (job_cancel == Committed) {
                    // Past the point where Cancel could leave everything as it was: it finishes by itself.
                    s.body = "Finishing\xE2\x80\xA6";
                    s.note = "Too far along to cancel: it finishes by itself. Close only hides this screen.";
                    s.actions = {{"Close", [] { press(CloseWorking); }}};
                    s.back = [] { press(CloseWorking); };
                    s.back_button = false;
                }
                else {
                    s.body = st.working;
                    s.note = "Cancel stops it. Nothing on this device changes.";
                    s.actions = {{"Cancel", [] { press(Cancel); }}};
                    cancel_only();
                }
                break;
            case CloudTurnedOn:
                s.title = "Cloud Sync is on";
                s.body = "Your saves are in the cloud, locked with this device's code. From now on they go up after each save and come "
                         "down when New Blitz starts.\n\nTo add your other device: Add a Device shows a 6-digit number to type there.";
                s.actions = {{"Add a Device", [] { press(AddDevice); }}, {"Done", [] { press(OpenCloud); }}};
                s.back = [] { press(OpenCloud); };
                s.back_button = false;
                break;
            case CloudPair: {
                cl::Offer o;
                {
                    std::lock_guard lock{pair_mutex};
                    o = pair_offer;
                }
                const int left = (int)std::chrono::duration_cast<std::chrono::seconds>(pair_until - Clock::now()).count();
                s.title = "Add a Device";
                s.body = "On your other device: Settings \xE2\x86\x92 General \xE2\x86\x92 Saves \xE2\x86\x92 Cloud Sync \xE2\x86\x92 "
                         "Join, and type this number:";
                s.big = cl::group_pair_number(o.number);
                if (left > 0) {
                    char t[16];
                    std::snprintf(t, sizeof t, "%d:%02d", left / 60, left % 60);
                    s.note = std::string("Works once, for ") + t + " more. Keep this screen up until the other device says it's on.";
                    s.actions = {{"Cancel", [] { press(PairCancel); }}, {"New Number", [] { press(NewNumber); }},
                                 {"Show Full Code", [] { press(ShowFullCode); }}};
                }
                else {
                    s.big = "--- ---";
                    s.note = "That number ran out (each lasts 2 minutes). New Number makes another.";
                    s.actions = {{"New Number", [] { press(NewNumber); }}, {"Show Full Code", [] { press(ShowFullCode); }},
                                 {"Cancel", [] { press(PairCancel); }}};
                }
                s.back = [] { press(PairCancel); };
                s.back_button = false;
                break;
            }
            case CloudPad: {
                s.title = "Join Cloud Sync";
                s.body = "On your other device: Saves \xE2\x86\x92 Cloud Sync \xE2\x86\x92 Add a Device. Type its 6-digit number.";
                std::string shown;
                for (size_t i = 0; i < cl::pair_digits; i++) {
                    if (i == 3) shown += ' ';
                    shown += i < st.digits.size() ? st.digits[i] : '_';
                }
                s.big = shown;
                s.note = st.pad_note;
                for (char d : std::string("123456789")) s.pad.push_back({std::string(1, d), [d] { pad_key = d; press(PadKey); }});
                s.pad.push_back({"Clear", [] { press(PadClear); }});
                s.pad.push_back({"0", [] { pad_key = '0'; press(PadKey); }});
                s.pad.push_back({"Delete", [] { press(PadDelete); }});
                s.typed = [](char d) {
                    std::lock_guard lock{keys_mutex};
                    keys_typed += d;
                    dirty = true;
                };
                s.actions = {{"Join with the Full Code", [] { press(FullCode); }}, {"Cancel", [] { press(OpenCloud); }}};
                s.focus = 0;
                s.back = [] { press(PadBack); };  // Backspace or B: one digit back, then out
                s.back_button = false;
                break;
            }
            case CloudCode: {
                cl::State cs = state_with_code();
                const std::string grouped = cl::group_code(cs.code);
                s.title = "Your full sync code";
                s.body = grouped + "\n\nFor a device that can't type the 6-digit number (or isn't on at the same time): Settings "
                                   "\xE2\x86\x92 General \xE2\x86\x92 Saves \xE2\x86\x92 Cloud Sync \xE2\x86\x92 Join \xE2\x86\x92 "
                                   "Join with the Full Code, and type this.\n\nKeep it to yourself: anyone with the code can read and "
                                   "change your saves.";
                s.qr_url = grouped;
                s.qr_caption = "The same code";
                s.actions = {{"Done", [] { press(OpenCloud); }}};
                s.back = [] { press(OpenCloud); };
                s.back_button = false;
                break;
            }
            case CloudOn: {
                cl::State cs = state_quick();
                s.title = "Cloud Sync is on";
                std::string body = "Syncing with " + slot_word(cs.slot) + ". Last synced: " + ago(cs.cur().last_sync) +
                                   (cs.cur().last_device.empty() ? "" : " (last saves in from " + cs.cur().last_device + ")") + ".";
                if (!cs.last_error.empty()) body += "\n\n" + cs.last_error;
                body += "\n\nYour saves go up after each save and come down when New Blitz starts. Save states are never synced.";
                s.body = body;
                if (ask_waiting) s.actions.push_back({"Choose Which Saves to Keep", [] { press(SyncNow); }});
                else s.actions.push_back({"Sync Now", [] { press(SyncNow); }});
                s.actions.push_back({"Add a Device", [] { press(AddDevice); }});
                s.actions.push_back({"Cloud Save Slots", [] { press(OpenSlots); }});
                s.actions.push_back({"Show Full Code", [] { press(ShowCode); }});
                s.actions.push_back({"Turn Off", [] { press(AskTurnOff); }});
                s.actions.push_back({"Back", [] { press(BackToMenu); }});
                s.back = [] { press(BackToMenu); };
                s.back_button = false;
                break;
            }
            case CloudSlots: {
                const int cur = state_quick().slot;
                s.title = "Cloud Save Slots";
                std::string body = "Three slots under one code, each with its own Controller Pak and season saves: keep separate seasons, "
                                   "or a backup. Save states are never synced. This device syncs with " + slot_word(cur) + ".\n";
                for (const std::string& l : st.slots) body += "\n" + l;
                s.body = body;
                for (int slot = 1; slot <= cl::slot_count; slot++) {
                    if (slot == cur) continue;
                    s.actions.push_back({"Use " + slot_word(slot), [slot] { picked = slot; press(PickSlot); }});
                }
                s.actions.push_back({"Back", [] { press(OpenCloud); }});
                s.back = [] { press(OpenCloud); };
                s.back_button = false;
                break;
            }
            case CloudSlotConfirm: {
                const int cur = state_quick().slot;
                s.title = "Sync with " + slot_word(st.slot_pick) + "?";
                s.body = "This device will sync with " + slot_word(st.slot_pick) + " from now on; " + slot_word(cur) +
                         " keeps what it holds.\n\nIf " + slot_word(st.slot_pick) + " holds other saves, you choose before anything on "
                         "this device or in the slot is replaced. An empty slot gets this device's saves. Save states are never synced.";
                s.actions = {{"Switch to " + slot_word(st.slot_pick), [] { press(SwitchSlot); }}, {"Cancel", [] { press(OpenSlots); }}};
                s.back = [] { press(OpenSlots); };
                s.back_button = false;
                break;
            }
            case CloudOff:
                s.title = "Turn off Cloud Sync?";
                s.body = "Your saves stay on this device, and your other devices keep syncing with each other.\n\nTurn Off and Delete "
                         "also removes the copies on the server, all three slots (a device still syncing puts its own back). Either way "
                         "they go by themselves after 90 days with no saves.";
                s.actions = {
                    {"Turn Off", [] { press(TurnOff); }},
                    {"Turn Off and Delete", [] { press(TurnOffDelete); }},
                    {"Cancel", [] { press(OpenCloud); }},
                };
                s.back = [] { press(OpenCloud); };
                s.back_button = false;
                break;
            case CloudRetry:
                s.title = st.title;
                s.body = st.body;
                s.actions = {{"Try Again", [] { press(Retry); }}};
                if (st.offer_address) s.actions.push_back({"Type Its Address", [] { press(TypeAddress); }});
                s.actions.push_back({"Cancel", [] { press(Cancel); }});
                cancel_only();
                break;
            case WifiMenu:
                s.title = "Sync over Wi-Fi";
                s.body = std::string("Open New Blitz on both devices, on the same Wi-Fi, and share before you switch your play session. Works "
                                     "between ") + file_devices + ".\n\nOn the device with the saves you want: Send My Saves. On the other: "
                                     "Get Saves. Save states stay on each device. " + not_n64;
                s.actions = {
                    {"Send My Saves", [] { press(WifiSend); }},
                    {"Get Saves", [] { press(WifiGet); }},
                    {"Cancel", [] { press(BackToMenu); }},
                };
                s.back = [] { press(BackToMenu); };
                s.back_button = false;
                break;
            case WifiSending: {
                wf::SendState ws = wf::send_state();
                int left = wf::seconds_left();
                const std::string addr = wf::sending_address();
                s.title = "Send my saves";
                s.body = "On the other device: Settings \xE2\x86\x92 General \xE2\x86\x92 Saves \xE2\x86\x92 Sync over Wi-Fi \xE2\x86\x92 "
                         "Get Saves, and type this code:\n\n" + wf::group_code(st.working) +
                         (addr.empty() ? std::string() : "\n\nIf it asks for this device's address: " + addr) +
                         "\n\nKeep this screen up until it's there.";
                if (ws == wf::SendState::Fetched) s.note = "Sent. The other device has your saves; you can stop now.";
                else if (ws == wf::SendState::TooManyTries) s.note = "Stopped: too many wrong codes were tried.";
                else if (ws == wf::SendState::TimedOut) s.note = "Stopped after ten minutes. Send again if you need to.";
                else s.note = "Open for " + std::to_string((left + 59) / 60) + (left > 60 ? " more minutes." : " more minute.");
                s.actions = {{ws == wf::SendState::Fetched ? "Done" : "Stop", [] { press(WifiStop); }}};
                s.back = [] { press(WifiStop); };
                s.back_button = false;
                break;
            }
            default:
                blitz::firstrun_ui::hide();
                launcher_visible(true);
                return;
        }
        s.large = true;  // every Saves screen: phone-sized words and buttons (the owner's iPhone, 2026-11-27)
        launcher_visible(false);
        blitz::firstrun_ui::show(s);
    }

    void take_file(const fs::path& path) {
        {
            std::lock_guard lock{file_mutex};
            opened_file = path;
        }
        file_waiting = true;
        dirty = true;
    }

    void open_cloud_page() {
        st = State{};
        st.page = state_quick().on ? CloudOn : CloudIntro;
    }
}

namespace blitz {
    // config.cpp's row, through firstrun.cpp's Settings request (Settings closes first).
    void save_sync_open() { request = true; }
    // First run's "Playing on multiple devices?" (firstrun.cpp, src/game/cloud_ask.h): its Yes opens Cloud Sync's own
    // screen (Turn On, or Join with a 6-digit number), with no Saves menu behind it. Whether to ask at all: this build
    // has Cloud Sync and it's off here.
    void save_sync_open_cloud() { request_cloud = true; }
    bool save_sync_cloud_available() { return cloud_available(); }
    bool save_sync_cloud_on() { return cloud_available() && state_quick().on; }
    // A Saves screen is up (or about to be): first run waits for them to close before it goes on.
    bool save_sync_showing() { return st.page != None || request.load() || request_cloud.load(); }
    // Settings → Saves → Sync Now (config.cpp): Cloud Sync now, without leaving Settings.
    void save_sync_now() {
#if defined(BLITZ_SAVE_SYNC_NET)
        sync_asked = true;
#else
        blitz::post_status("Cloud Sync isn't in this build");
#endif
    }

    // A .nbsave opened into the app from another (iOS Share → New Blitz; firstrun.cpp): straight to its import. The
    // file is read at once (the caller removes its copy after).
    void save_sync_open_file(const std::filesystem::path& path) {
        if (on_ps5) return;
        st = State{};
        read_bundle_file(path);
        dirty = true;
    }

    // main.cpp, at start before the settings are read: league settings an import staged.
    void save_sync_at_start(const std::filesystem::path& app) {
        int n = ss::apply_staged_settings(app);
        if (n > 0) fprintf(stderr, "[sync] %d settings file(s) from an import put in place\n", n);
    }

    // Every UI tick (firstrun.cpp builder_update).
    void save_sync_update() {
        // Dev: BLITZ_OPEN_SAVES=menu|cloud|on|pad opens that screen once at start (to look at its sizes with
        // tools/run_dev.sh's screenshots; BLITZ_PHONE_UI=1 for a phone's).
        static bool dev_checked = false;
        static int dev_ticks = 0;  // once the UI is up (as first run waits: the launcher, then a second of frames)
        if (!dev_checked && recompui::get_launcher_context_id() != recompui::ContextId::null() && ++dev_ticks > 90) {
            dev_checked = true;
            if (const char* e = getenv("BLITZ_OPEN_SAVES"); e != nullptr) {
                const std::string w = e;
                st = State{};
                st.page = w == "pad" ? CloudPad : w == "cloud" ? CloudIntro : w == "on" ? CloudOn : Menu;
                if (w == "pad") st.digits = "48";
                dirty = true;
            }
        }
        if (request.exchange(false)) {
            st = State{};
            st.page = Menu;
            cloud_only = false;
            dirty = true;
            fprintf(stderr, "[sync] opened\n");
        }
        if (request_cloud.exchange(false)) {
            open_cloud_page();
            cloud_only = true;
            dirty = true;
            fprintf(stderr, "[sync] opened on Cloud Sync (from the multiple-devices question)\n");
        }
        job_finished();
        cloud_background();
        if (file_waiting.exchange(false)) {
            fs::path path;
            {
                std::lock_guard lock{file_mutex};
                path = opened_file;
            }
            if (st.page == Pick) read_bundle_file(path);
            dirty = true;
        }
        // The Wi-Fi screen counts down (and shows when the other device has the saves).
        static Clock::time_point ticked{};
        if (st.page == WifiSending && Clock::now() - ticked > std::chrono::seconds(1)) {
            ticked = Clock::now();
            dirty = true;
        }
        if (int p = pressed.exchange(0); p != NoPress) {
            switch (p) {
                case Export:
                    if (!on_ps5) do_export();
                    break;
                case Import:
                    if (on_ps5) break;
                    st.found = find_bundles();
                    st.page = Pick;
                    break;
                case ChooseFile:
                    recompui::file::open_file_dialog([](bool success, const fs::path& path) {
                        if (success) take_file(path);
                    });
                    break;
                case PickFound:
                    if (picked >= 0 && picked < (int)st.found.size()) read_bundle_file(st.found[(size_t)picked]);
                    break;
                case KeepMine:
                case UseTheirs: {
                    ss::FolderPlan& f = st.plan.folders[st.asking];
                    f.take = p == UseTheirs;
                    fprintf(stderr, "[sync] import: %s: %s\n", f.folder.empty() ? "saves" : f.folder.c_str(), f.take ? "use theirs" : "keep this device's");
                    next_question(st.q + 1);
                    break;
                }
                case ImportIt:
                    if (!anything_to_do() && st.source != FromCloud) {
                        blitz::post_status("Your saves already match");
                        st = State{};
                    }
                    else do_import();
                    break;
                case Cancel:
                    if (st.page == CloudWorking || st.page == WifiWorking) {
                        // Cancel on a working screen: only while nothing has changed (after that the screen offers Close).
                        int expect = Running;
                        if (job_running && !job_cancel.compare_exchange_strong(expect, CancelPressed) && expect == Committed) break;
                        fprintf(stderr, "[sync] cancelled while working: nothing changed\n");
                        const int to = st.cancel_to;
                        if (to == CloudPad) open_pad();
                        else if (to == WifiMenu) {
                            st = State{};
                            st.page = WifiMenu;
                        }
                        else if (to == CloudOn || to == CloudIntro || to == CloudSlots) open_cloud_page();
                        else st = State{};
                        blitz::post_status("Cancelled. Nothing changed.");
                        break;
                    }
                    fprintf(stderr, "[sync] cancelled\n");
                    if (st.source == FromCloud && (st.page == Decide || st.page == Confirm)) {
                        // A join's or a switch's questions: nothing was kept before them, so nothing to undo.
                        if (asked.pending.switching) blitz::post_status("Still syncing with " + slot_word(state_quick().slot) + ". Nothing changed.");
                        else if (asked.pending.slot != 0) blitz::post_status("Not joined. Nothing on this device changed.");
                        else ask_waiting = true;  // asked again later
                    }
                    st = State{};
                    break;
                case CloseWorking:
                    st = State{};  // the job finishes by itself; its outcome is a toast
                    break;
                case Ok:
                    st = State{};
                    break;
                case BackToMenu:
                    st = State{};
                    if (!cloud_only) st.page = Menu;
                    break;
                case Close:
                    blitz::firstrun_ui::hide();
                    st = State{};
                    if (blitz::restart_available) blitz::request_restart();
                    else blitz::restart_fresh();
                    return;
                case OpenCloud:
                    open_cloud_page();
                    break;
                case TurnOn:
                    start_turn_on();
                    break;
                case Join:
                case FullCode:
                    open_join_keyboard();
                    break;
                case OpenPad:
                    open_pad();
                    st.pad_note.clear();
                    break;
                case PadKey:
                    pad_digit(pad_key);
                    break;
                case PadDelete:
                    if (!st.digits.empty()) st.digits.pop_back();
                    break;
                case PadClear:
                    st.digits.clear();
                    break;
                case PadBack:
                    if (!st.digits.empty()) st.digits.pop_back();
                    else open_cloud_page();
                    break;
                case AddDevice:
                case NewNumber:
                    start_pair_make();
                    break;
                case PairCancel:
                    stop_pairing(true);
                    open_cloud_page();
                    break;
                case ShowFullCode:
                    stop_pairing(true);
                    st.page = CloudCode;
                    break;
                case SyncNow:
                    if (job_running) blitz::post_status("Cloud Sync is already syncing");
                    else if (ask_waiting && asked.result == cl::Result::Ask) show_questions(asked);
                    else start_sync_now();
                    break;
                case ShowCode:
                    st.page = CloudCode;
                    break;
                case OpenSlots:
                    start_slots();
                    break;
                case PickSlot:
                    st.slot_pick = picked;
                    st.page = CloudSlotConfirm;
                    break;
                case SwitchSlot:
                    start_switch(st.slot_pick);
                    break;
                case AskTurnOff:
                    st.page = CloudOff;
                    break;
                case TurnOff:
                case TurnOffDelete:
                    start_turn_off(p == TurnOffDelete);
                    break;
                case Retry:
                    if (st.retry_what == "wifi") open_wifi_keyboard();
                    else open_join_keyboard();
                    break;
                case TypeAddress:
                    open_address_keyboard();
                    break;
                case OpenWifi:
                    if (!on_ps5) st.page = WifiMenu;
                    break;
                case WifiSend:
                    start_wifi_send();
                    break;
                case WifiGet:
                    open_wifi_keyboard();
                    break;
                case WifiStop:
                    wf::stop_sending();
                    st = State{};
                    st.page = WifiMenu;
                    break;
            }
        }
        // Digits typed on a keyboard while the number pad is up.
        {
            std::string keys;
            {
                std::lock_guard lock{keys_mutex};
                keys.swap(keys_typed);
            }
            for (char c : keys) pad_digit(c);
            if (!keys.empty()) dirty = true;
        }
        // Add a Device's number: taken by the other device, counting down, or cancelled the moment its screen goes.
        if (pairing_up()) {
            static Clock::time_point counted{};
            if (pair_taken) {
                stop_pairing(false);
                if (st.page == CloudPair) {
                    message("Your other device has it", "It's joining now: from now on its saves and this device's sync. Add more "
                                                        "devices the same way.");
                }
                dirty = true;
            }
            else if (st.page != CloudPair) stop_pairing(true);
            else if (Clock::now() - counted > std::chrono::seconds(1)) {
                counted = Clock::now();
                // The countdown: only its words change (a redraw lit up the default button each second). The screen is
                // drawn again when the number runs out, for its other buttons.
                const int left = (int)std::chrono::duration_cast<std::chrono::seconds>(pair_until - Clock::now()).count();
                char t[16];
                std::snprintf(t, sizeof t, "%d:%02d", left / 60, left % 60);
                if (left <= 0 || !blitz::firstrun_ui::update_note(std::string("Works once, for ") + t +
                                                                   " more. Keep this screen up until the other device says it's on.")) {
                    dirty = true;
                }
            }
        }
        // A keyboard up for a code or an address (open_join_keyboard...): left on top. Drawing this page now put it over the
        // keyboard, which then couldn't be typed on (the team's Surface Duo, 2026-11-27); it's drawn when the keyboard closes.
        if (recompui::is_keyboard_open()) return;
        if (!dirty.exchange(false)) return;
        show();
    }
}
