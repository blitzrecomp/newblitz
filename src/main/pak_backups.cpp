// Settings → General → Controller Pak Backups (1.0.4, after the team's Advanced season vanished from his pak on
// 2026-11-26): the dated copies controller_pak.cpp keeps of each pak (pak_history/), newest first, each with what it
// holds ("LAR, week 6"); pick one, confirm, and it's put back. The pak as it is now goes into the copies first, so a
// restore can be undone the same way.
//
// When: before the game has opened that pak this run (from the launcher), at once. With the game running, the game
// keeps the pak's file system in memory and may be part way through a write, so the file can't change under it: a
// mark goes in the pak's folder and the app restarts (a phone or console closes); the next start puts the copy back
// at its first pak access, before the game reads anything. Every step prints a [pak] line.
//
// The screens are first run's (firstrun_screen.cpp), like Restore a Backup's (reset_data.cpp): a controller's D-pad
// and A, a tap or a click, and Back or B to cancel.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "blitz_config.h"
#include "blitz_pak_backups.h"
#include "blitz_platform.h"
#include "firstrun_screen.h"
#include "recompui/recompui.h"
#include "ultramodern/ultramodern.hpp"

namespace blitz {
    void restart_fresh();     // main.cpp
    void request_restart();   // main.cpp
    void show_launcher();     // main.cpp
    void post_status(const std::string& msg);  // notices.cpp
}

namespace {
    using blitz::firstrun_ui::Screen;

    constexpr size_t per_page = 5;

    enum Page { None, List, Confirm, Restarting };
    struct State {
        int page = None;
        std::vector<blitz::PakBackup> list;
        size_t first = 0;      // the list's page
        int chosen = -1;
    } st;

    std::atomic<bool> request{false};
    std::atomic<int> pressed{0};
    std::atomic<bool> dirty{false};
    enum Press { NoPress, Pick, More, Cancel, ToList, RestoreIt, Close };
    int picked = 0;

    void press(int what) {
        pressed = what;
        dirty = true;
    }

    // "Nov 26, 1:53 PM"
    std::string when(std::time_t t) {
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        char out[48];
        snprintf(out, sizeof(out), "%s %d, %d:%02d %s", months[tm.tm_mon], tm.tm_mday, tm.tm_hour % 12 == 0 ? 12 : tm.tm_hour % 12, tm.tm_min,
                 tm.tm_hour < 12 ? "AM" : "PM");
        return out;
    }

    std::string label(const blitz::PakBackup& b) {
        std::string s = when(b.when) + " \xC2\xB7 " + b.what;
        if (!b.folder.empty()) s += " (" + b.folder + ")";
        return s;
    }

    // The launcher steps aside while these screens are up (it would take the D-pad from them), as for Reset Data.
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
            case List: {
                s.title = "Controller Pak Backups";
                s.body = "Copies of your Controller Pak from each time it changed, newest first. Pick one to put it back.";
                for (size_t i = st.first; i < st.list.size() && i < st.first + per_page; i++) {
                    int n = (int)i;
                    s.actions.push_back({label(st.list[i]), [n] { picked = n; press(Pick); }});
                }
                if (st.list.size() > st.first + per_page) s.actions.push_back({"Older", [] { press(More); }});
                s.actions.push_back({"Cancel", [] { press(Cancel); }});
                s.back = [] { press(Cancel); };
                s.back_button = false;
                break;
            }
            case Confirm: {
                const blitz::PakBackup& b = st.list[(size_t)st.chosen];
                bool running = ultramodern::is_game_started();
                s.title = "Restore this pak?";
                s.body = "Your Controller Pak goes back to " + when(b.when) + ": " + b.what +
                         ".\n\nThe pak you have now is kept as a backup too." +
                         (running ? blitz::restart_available ? " New Blitz restarts to do it." : " New Blitz closes to do it; open it again after."
                                  : "");
                s.actions = {
                    {"Restore", [] { press(RestoreIt); }},
                    {"Cancel", [] { press(ToList); }},
                };
                s.back = [] { press(ToList); };
                s.back_button = false;
                break;
            }
            case Restarting:
                s.title = "Ready to restore";
                s.body = blitz::restart_available ? "New Blitz restarts with that pak." : "New Blitz closes now. Open it again and that pak is back.";
                s.actions = {{blitz::restart_available ? "Restart" : "Close New Blitz", [] { press(Close); }}};
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
    // config.cpp's row, through firstrun.cpp's Settings request (Settings closes first).
    void pak_backups_open() { request = true; }

    // Every UI tick (firstrun.cpp builder_update).
    void pak_backups_update() {
        if (request.exchange(false)) {
            st = State{};
            st.list = pak_backups();
            fprintf(stderr, "[pak] backups: %zu to choose from\n", st.list.size());
            if (st.list.empty()) {
                post_status("No other Controller Pak copies yet");
                return;
            }
            st.page = List;
            dirty = true;
        }
        if (int p = pressed.exchange(0); p != NoPress) {
            switch (p) {
                case Pick:
                    if (picked >= 0 && picked < (int)st.list.size()) {
                        st.chosen = picked;
                        fprintf(stderr, "[pak] backups: picked %s (%s)\n", st.list[picked].file.filename().string().c_str(),
                                st.list[picked].what.c_str());
                        st.page = Confirm;
                    }
                    break;
                case More:
                    st.first += per_page;
                    break;
                case Cancel:
                    fprintf(stderr, "[pak] backups: cancelled\n");
                    st = State{};
                    break;
                case ToList:
                    st.page = List;
                    break;
                case RestoreIt: {
                    bool restart = false;
                    std::string why = pak_restore(st.list[(size_t)st.chosen], restart);
                    if (!why.empty()) {
                        post_status(why);
                        st = State{};
                    }
                    else if (restart) {
                        st.page = Restarting;
                    }
                    else {
                        post_status("Controller Pak restored");
                        st = State{};
                    }
                    break;
                }
                case Close:
                    blitz::firstrun_ui::hide();
                    st = State{};
                    if (blitz::restart_available) blitz::request_restart();
                    else blitz::restart_fresh();
                    return;
            }
        }
        if (!dirty.exchange(false)) return;
        show();
    }
}
