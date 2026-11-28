// Settings → Add-Ons (docs/design/ONE_APP.md): everything a player adds to the game, in one place. What's installed
// (roster packs, uniform sets, art, announcer calls, rules files), Add… to install a file, a folder or a .zip (the
// kind is recognised), and Remove. The launcher's Add-Ons item opens this tab. Both apps; each lists what it uses.
//
// What Add… recognises (recursively, so a zip of a folder of several works):
//   - a folder with pack.json                     -> packs/<folder>
//   - a folder shaped like the app folder         -> its packs/ uniforms/ art/ announcer/ rules/ merged in
//   - <TEAM>/<set>/ with set.json and .wms parts  -> uniforms/<TEAM>/<set>   (TEAM a franchise id: DEN, GB …)
//   - a set folder with set.json, inside a TEAM folder or named in the zip as TEAM/…
//   - .wms                                        -> art/files
//   - .wbk / .ptr / .bfx                          -> announcer
//   - a rules .json (Rules → Save Setup to File)  -> rules
//   - a pak (.nbpak, or a .zip with pak.json at its root)  -> paks/<id> (season_pak.cpp), as the player's own
// PNG uniform art isn't game format yet: it's reported, with how to convert it (tools/import_uniform_sets.py).
//
// The paks (1.0.6): listed first, each installed one with Remove, then each the server posts that isn't in, with Get.
// They install and update themselves while content updates are on (season_pak.cpp); Check, top right, syncs now, with
// the download's progress under the buttons, and says "Up to date." when nothing's new. A pak added by hand is the
// player's own: never updated or replaced by itself.
// Open Folder (1.0.6): a computer's file browser; on a phone, the Files app (iPhone: Documents, the drop folder; Android:
// the app folder, through BlitzFolder.java), or where the folder is when it can't open. Not on the PS5 (FTP).
#include "../main/os_shell.h"
#include "blitz_config.h"
#include "blitz_host.h"
#include "blitz_platform.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "SDL2/SDL.h"
#include "json/json.hpp"
#include "miniz.h"
#include "recompui/config.h"
#include "recompui/recompui.h"
#include "librecomp/game.hpp"
#include "ultramodern/ultramodern.hpp"
#include "elements/ui_button.h"
#include "elements/ui_label.h"
#include "util/file.h"
#include "../main/ui_kit.h"
#include "season_pak.h"
#include <mutex>

namespace fs = std::filesystem;

namespace blitz {
    void request_restart();  // main.cpp: Settings → Restart
    void export_rom_dialog();  // export_rom.cpp
    void firstrun_settings_row(int which);  // firstrun.cpp: 6 Blitz Builder → N64 or emulator
    void post_status(const std::string& msg);
#if BLITZ_MODERN
    void original_settings_visibility();  // adv_mode.cpp: the settings that need the 2026 Season
    void season_arrived();  // adv_mode.cpp: Game turns to New Blitz once the season is added
#endif
}

namespace {
    const std::set<std::string> franchises = {"ARI", "ATL", "BAL", "BUF", "CAR", "CHI", "CIN", "CLE", "DAL", "DEN", "DET",
                                              "GB", "HOU", "IND", "JAX", "KC", "LV", "LAC", "LAR", "MIA", "MIN", "NE", "NO",
                                              "NYG", "NYJ", "PHI", "PIT", "SF", "SEA", "TB", "TEN", "WAS"};
    const char* const parts[] = {"packs", "uniforms", "art", "announcer", "rules"};

    fs::path app() { return recomp::get_config_path(); }

    std::string lower(std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }
    std::string upper(std::string s) {
        for (char& c : s) c = (char)std::toupper((unsigned char)c);
        return s;
    }

    // ---- what's installed ----

    struct Item {
        std::string kind;    // SEASON, LEAGUE, PAK, ROSTERS, UNIFORMS, ART, ANNOUNCER, RULES
        std::string name;
        std::string detail;
        fs::path path;       // what Remove deletes
        std::string pak;     // a pak's id: Remove goes through season_pak.cpp
        bool get = false;    // a posted pak that isn't in: Get instead of Remove
    };

    // What a thread (a sync's end, the picker, the Files app) has to tell the open tab (process_event shows it).
    std::mutex message_mutex;
    bool message_due = false;
    std::string message_text;
    void tell(const std::string& m) {
        std::lock_guard lock{message_mutex};
        message_due = true;
        message_text = m;
    }

    // The 2026 Season asked for by hand (Get, Check, Add…): once it's in, Game turns to New Blitz (season_arrived). An
    // automatic download leaves Game as the player set it.
    std::atomic<bool> season_wanted{false};

    std::string megabytes(uint64_t n) {
        char buf[32];
        snprintf(buf, sizeof(buf), n < (1u << 20) ? "%.2f MB" : "%.1f MB", n / 1048576.0);
        return buf;
    }

    std::string pak_kind(const std::string& id, const std::string& kind) {
        if (id == blitz::season_pak_id) return "SEASON";
        return kind.empty() ? "PAK" : upper(kind);
    }

    // The paks: each installed one (the server's or the player's own), then each the server posts that isn't in.
    void scan_paks(std::vector<Item>& out) {
        std::vector<blitz::PostedPak> posted = blitz::posted_paks();
        auto posted_of = [&](const std::string& id) -> const blitz::PostedPak* {
            for (const auto& p : posted) if (p.id == id) return &p;
            return nullptr;
        };
        for (const blitz::InstalledPak& p : blitz::installed_paks()) {
            const blitz::PostedPak* pp = posted_of(p.id);
            std::string what = !p.summary.empty() ? p.summary : pp && !pp->summary.empty() ? pp->summary
                             : p.id == blitz::season_pak_id ? "rosters, lineups, uniforms, team art, Houston and Pro Bowl" : "";
            std::string detail = (p.version.empty() ? std::string("") : "version " + p.version) + (what.empty() || p.version.empty() ? "" : ": ") + what;
            if (pp && pp->by_hand) detail += " (added by hand: not updated by itself)";
            else if (pp && pp->waiting) detail += " (an update is downloaded: in at the next start)";
            else if (pp && pp->newer) detail += " (version " + pp->version + " is out: Check gets it)";
            else if (!pp && p.id != blitz::season_pak_id) detail += detail.empty() ? "added by hand" : " (added by hand)";
            out.push_back({pak_kind(p.id, pp ? pp->kind : ""), p.title, detail, p.dir, p.id, false});
        }
        for (const blitz::PostedPak& p : posted) {
            if (p.installed) continue;
            std::string detail = p.summary.empty() ? std::string("") : p.summary + ". ";
            if (p.too_new) detail += "Needs a newer New Blitz.";
            else if (p.waiting) detail += "Downloaded: in at the next start.";
            else if (p.removed) detail += "Removed. Get brings it back (" + megabytes(p.size) + ").";
            else if (p.wifi) detail += "Waits for Wi-Fi (" + megabytes(p.size) + "): Get downloads it now.";
            else detail += "Not in yet (" + megabytes(p.size) + ")" + (blitz::content_updates_on() ? ": it downloads by itself, or Get it now." : ".");
            Item it{pak_kind(p.id, p.kind), p.title, detail, {}, p.id, !p.too_new && !p.waiting};
            out.push_back(it);
        }
    }

    std::vector<Item> scan() {
        std::vector<Item> out;
        std::error_code ec;
#if BLITZ_MODERN
        scan_paks(out);
#endif
        auto dirs = [&](const fs::path& p) {
            std::vector<fs::path> v;
            if (fs::is_directory(p, ec)) {
                for (const auto& e : fs::directory_iterator(p, ec)) if (e.is_directory(ec)) v.push_back(e.path());
            }
            std::sort(v.begin(), v.end());
            return v;
        };
        auto count_files = [&](const fs::path& p, std::set<std::string> exts) {
            int n = 0;
            if (fs::is_directory(p, ec)) {
                for (const auto& e : fs::recursive_directory_iterator(p, ec)) {
                    if (e.is_regular_file(ec) && exts.count(lower(e.path().extension().string()))) n++;
                }
            }
            return n;
        };
        for (const fs::path& d : dirs(app() / "packs")) {
            std::ifstream f{d / "pack.json"};
            if (!f) continue;
            std::string title = d.filename().string(), detail;
            try {
                nlohmann::json j = nlohmann::json::parse(f);
                title = j.value("title", title);
                if (j.contains("teams")) detail = std::to_string(j["teams"].size()) + " teams";
                if (j.contains("league")) detail += (detail.empty() ? "" : ", ") + std::string("a league");
            }
            catch (...) {
                detail = "pack.json can't be read";
            }
            out.push_back({"ROSTERS", title, detail.empty() ? d.filename().string() : detail, d});
        }
        int sets = 0, teams = 0;
        for (const fs::path& t : dirs(app() / "uniforms")) {
            int n = (int)dirs(t).size();
            if (n > 0) {
                sets += n;
                teams++;
            }
        }
        if (sets > 0) {
            out.push_back({"UNIFORMS", std::to_string(sets) + " uniform sets", "for " + std::to_string(teams) + (teams == 1 ? " team" : " teams") + ", cycled with △ at team select", app() / "uniforms"});
        }
        if (int n = count_files(app() / "art" / "files", {".wms"})) {
            out.push_back({"ART", std::to_string(n) + " images", "logos, banners, helmets and more, in place of the game's own", app() / "art" / "files"});
        }
        if (int n = count_files(app() / "announcer", {".wbk"})) {
            out.push_back({"ANNOUNCER", std::to_string(n) + (n == 1 ? " sound bank" : " sound banks"), "team name calls", app() / "announcer"});
        }
        if (fs::is_directory(app() / "rules", ec)) {
            std::vector<fs::path> files;
            for (const auto& e : fs::directory_iterator(app() / "rules", ec)) {
                if (e.is_regular_file(ec) && lower(e.path().extension().string()) == ".json") files.push_back(e.path());
            }
            std::sort(files.begin(), files.end());
            for (const fs::path& p : files) out.push_back({"RULES", p.stem().string(), "in Rules → Quick Setup", p});
        }
        return out;
    }

    // ---- installing ----

    struct Report {
        std::vector<std::string> done, skipped;
        void add(const std::string& s) { done.push_back(s); }
        void skip(const std::string& s) { skipped.push_back(s); }
    };

    bool copy_tree(const fs::path& from, const fs::path& to) {
        std::error_code ec;
        fs::create_directories(to, ec);
        fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        return !ec;
    }

    bool has_files(const fs::path& dir, const char* ext) {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (e.is_regular_file(ec) && lower(e.path().extension().string()) == ext) return true;
        }
        return false;
    }

    // A uniform set folder (set.json and game-format parts) for team `team`.
    void install_set(const fs::path& dir, const std::string& team, Report& r) {
        if (!has_files(dir, ".wms")) {
            if (has_files(dir, ".png")) {
                r.skip(team + " " + dir.filename().string() + ": PNG art, not converted yet (tools/import_uniform_sets.py)");
            }
            return;
        }
        if (copy_tree(dir, app() / "uniforms" / team / dir.filename())) r.add(team + " uniform set " + dir.filename().string());
    }

    void install_file(const fs::path& p, Report& r) {
        std::string ext = lower(p.extension().string());
        std::error_code ec;
        auto put = [&](const fs::path& folder, const std::string& what) {
            fs::create_directories(folder, ec);
            fs::copy_file(p, folder / p.filename(), fs::copy_options::overwrite_existing, ec);
            if (!ec) r.add(what + " " + p.filename().string());
        };
        if (ext == ".wms") put(app() / "art" / "files", "art");
        else if (ext == ".wbk" || ext == ".ptr" || ext == ".bfx") put(app() / "announcer", "announcer");
        else if (ext == ".json") {
            std::ifstream f{p};
            try {
                nlohmann::json j = nlohmann::json::parse(f);
                if (p.filename() == "pack.json") return;  // handled with its folder
                if (j.is_object() && (j.contains("codes") || j.contains("rules") || j.contains("preset"))) put(app() / "rules", "rules");
            }
            catch (...) {
            }
        }
    }

    void install_dir(const fs::path& dir, Report& r, int depth = 0) {
        std::error_code ec;
        if (depth > 6) return;
        if (fs::exists(dir / "pack.json", ec)) {
            if (copy_tree(dir, app() / "packs" / dir.filename())) r.add("roster pack " + dir.filename().string());
            return;
        }
        // Shaped like the app folder: merge its parts.
        bool shaped = false;
        for (const char* part : parts) {
            if (fs::is_directory(dir / part, ec)) {
                shaped = true;
                if (std::string(part) == "packs") {
                    for (const auto& e : fs::directory_iterator(dir / part, ec)) if (e.is_directory(ec)) install_dir(e.path(), r, depth + 1);
                }
                else if (std::string(part) == "uniforms") install_dir(dir / part, r, depth + 1);
                else if (copy_tree(dir / part, app() / part)) r.add(std::string(part) + " folder");
            }
        }
        if (shaped) return;
        std::string name = upper(dir.filename().string());
        if (fs::exists(dir / "set.json", ec)) {
            std::string team = upper(dir.parent_path().filename().string());
            if (franchises.count(team)) install_set(dir, team, r);
            else r.skip(dir.filename().string() + ": a uniform set, but not inside a team folder (DEN, GB, …)");
            return;
        }
        if (franchises.count(name)) {  // TEAM/<set>/...
            for (const auto& e : fs::directory_iterator(dir, ec)) {
                if (e.is_directory(ec) && fs::exists(e.path() / "set.json", ec)) install_set(e.path(), name, r);
            }
            return;
        }
        std::vector<fs::path> entries;
        for (const auto& e : fs::directory_iterator(dir, ec)) entries.push_back(e.path());
        std::sort(entries.begin(), entries.end());
        for (const fs::path& e : entries) {
            if (fs::is_directory(e, ec)) install_dir(e, r, depth + 1);
            else install_file(e, r);
        }
    }

    // A .zip unpacked to a temporary folder (removed afterwards).
    bool unzip(const fs::path& zip_path, const fs::path& to) {
        mz_zip_archive zip{};
        if (!mz_zip_reader_init_file(&zip, zip_path.string().c_str(), 0)) return false;
        bool ok = true;
        for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); i++) {
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
            std::string name = st.m_filename;
            // Nothing outside the temporary folder: no "..", no absolute paths.
            if (name.empty() || name[0] == '/' || name.find("..") != std::string::npos || name.rfind("__MACOSX", 0) == 0) continue;
            fs::path out = to / name;
            std::error_code ec;
            if (mz_zip_reader_is_file_a_directory(&zip, i)) {
                fs::create_directories(out, ec);
                continue;
            }
            fs::create_directories(out.parent_path(), ec);
            if (!mz_zip_reader_extract_to_file(&zip, i, out.string().c_str(), 0)) ok = false;
        }
        mz_zip_reader_end(&zip);
        return ok;
    }

    Report install(const std::vector<fs::path>& paths) {
        Report r;
        std::error_code ec;
        for (const fs::path& p : paths) {
            if (fs::is_directory(p, ec)) install_dir(p, r);
            else if (blitz::is_pak_file(p)) {  // a pak (the 2026 Season, a league): into paks/, checked (season_pak.cpp)
                std::string error, id;
                bool had_season = blitz::season_content();
                if (blitz::pak_install_by_hand(p, id, error)) {
                    std::string title = id;
                    for (const blitz::InstalledPak& ip : blitz::installed_paks()) if (ip.id == id) title = ip.title + (ip.version.empty() ? "" : " (" + ip.version + ")");
                    r.add(title);
                    if (id == blitz::season_pak_id && !had_season) season_wanted = true;
                }
                else r.skip(p.filename().string() + ": " + error);
            }
            else if (lower(p.extension().string()) == ".zip") {
                fs::path tmp = fs::temp_directory_path(ec) / ("blitz-addon-" + std::to_string(std::rand()));
                if (unzip(p, tmp)) install_dir(tmp, r);
                else r.skip(p.filename().string() + ": the zip can't be read");
                fs::remove_all(tmp, ec);
            }
            else install_file(p, r);
        }
        return r;
    }
}

namespace recompui {
    namespace {
        class AddOnsPanel;
        AddOnsPanel* live = nullptr;  // the open tab, for callbacks that come back later (the picker)

        // Where Open Folder's folder is, as a phone's Files app names it (Android/data/<package>/files).
        std::string folder_words() {
            std::string p = app().string();
            size_t at = p.find("Android/data/");
            return at == std::string::npos ? p : p.substr(at);
        }

        class AddOnsPanel : public Element {
            ContextId context;
            Element* list = nullptr;
            Label* status = nullptr;
            Element* progress = nullptr;  // the sync's line: what it's doing, and a bar for a download
            Label* progress_text = nullptr;
            Element* progress_fill = nullptr;
            Button* restart = nullptr;
            Button* check = nullptr;
            bool was_busy = false;

            // A sync by hand (Check, a row's Get): the tab follows it and says how it went.
            static void sync_now(const std::vector<std::string>& only) {
                if (std::find(only.begin(), only.end(), blitz::season_pak_id) != only.end() || only.empty()) {
                    if (!blitz::season_content()) season_wanted = true;
                }
                auto done = [](const blitz::PakSyncResult& r) {
                    std::string m = r.summary();
                    bool any = !r.added.empty() || !r.updated.empty();
                    if (any) m += ultramodern::is_game_started() ? " Restart to use it." : " It loads when the game starts.";
                    tell(m);
                };
                if (blitz::pak_sync_start(true, only, done)) {
                    if (live) live->refresh_later("");
                    return;
                }
                // The automatic check is running (it starts with the app): this one goes after it.
                std::thread([only, done] {
                    while (!blitz::pak_sync_start(true, only, done)) std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }).detach();
            }

            void row(const Item& it) {
                Element* box = context.create_element<Element>(list, 0, "div", false);
                blitz::ui::PanelOptions po;
                po.grid = false;
                Element* card = blitz::ui::panel(context, box, po);
                card->set_display(Display::Flex);
                card->set_flex_direction(FlexDirection::Row);
                card->set_align_items(AlignItems::Center);
                card->set_gap(18.0f);
                card->set_padding_left(18.0f);
                card->set_padding_right(18.0f);
                card->set_padding_top(10.0f);
                card->set_padding_bottom(10.0f);
                box->set_as_navigation_container(NavigationType::GridRow);
                Label* kind = context.create_element<Label>(card, it.kind, theme::Typography::LabelXS);
                kind->set_color(Color{143, 239, 245, 255});
                kind->set_min_width(150.0f);
                kind->set_flex_shrink(0.0f);
                Element* text = context.create_element<Element>(card, 0, "div", false);
                text->set_display(Display::Flex);
                text->set_flex_direction(FlexDirection::Column);
                text->set_flex_grow(1.0f);
                text->set_flex_shrink(1.0f);
                Label* name = context.create_element<Label>(text, upper(it.name), theme::Typography::Header3);
                name->set_font_size(36.0f);
                Label* detail = context.create_element<Label>(text, it.detail, theme::Typography::Body);
                detail->set_font_size(26.0f);
                detail->set_color(Color{184, 196, 218, 255});
                if (it.get) {
                    // A posted pak that isn't in: Get downloads it now (and brings back one the player removed).
                    Button* get = context.create_element<Button>(card, "Get", ButtonStyle::Primary, ButtonSize::Card);
                    get->set_flex_shrink(0.0f);
                    std::string id = it.pak;
                    get->add_pressed_callback([id]() { sync_now({id}); });
                    return;
                }
                Button* remove = context.create_element<Button>(card, "Remove", ButtonStyle::Tertiary, ButtonSize::Card);
                remove->set_flex_shrink(0.0f);
                fs::path target = it.path;
                std::string pak = it.pak;
                // Confirmed on the button itself (a prompt can't open over Settings from the launcher): the first
                // press asks "Sure?", a second within a few seconds removes.
                remove->add_pressed_callback([remove, target, pak]() {
                    static Button* armed = nullptr;
                    static uint32_t armed_at = 0;
                    uint32_t now = SDL_GetTicks();
                    if (armed != remove || now - armed_at > 4000) {
                        if (armed != nullptr && armed != remove) armed->set_text("Remove");
                        armed = remove;
                        armed_at = now;
                        remove->set_text("Sure?");
                        return;
                    }
                    armed = nullptr;
                    std::error_code ec;
                    std::string error;
                    if (!pak.empty()) {
                        if (!blitz::pak_remove(pak, error)) ec = std::make_error_code(std::errc::io_error);
                    }
                    else fs::remove_all(target, ec);
                    if (!error.empty()) error = "Couldn't remove it: " + error;
                    else if (ec) error = "Couldn't remove it: " + ec.message();
                    fprintf(stderr, "[addons] removed %s\n", pak.empty() ? target.string().c_str() : pak.c_str());
                    if (live) live->refresh_later(!error.empty() ? error : std::string("Removed.") + (ultramodern::is_game_started() ? " Restart to see the change." : ""));
                });
            }

            bool refresh_due = false;
            std::string refresh_message;

            // The sync's line, every update while one runs.
            void show_progress() {
                blitz::PakSyncProgress p = blitz::pak_sync_progress();
                bool busy = p.busy || blitz::pak_sync_busy();
                if (busy) {
                    std::string line = p.doing.empty() ? std::string("Checking for new paks…") : p.doing;
                    if (p.total > 0) line += "  " + megabytes(p.got) + " of " + megabytes(p.total);
                    progress_text->set_text(line);
                    float pct = p.total > 0 ? std::min(100.0f, 100.0f * (float)p.got / (float)p.total) : 0.0f;
                    progress_fill->set_width(pct, Unit::Percent);
                }
                progress->set_display(busy ? Display::Flex : Display::None);
                check->set_text(busy ? "Checking…" : "Check");
                if (was_busy && !busy) refresh_due = true;  // what it brought shows in the list
                was_busy = busy;
            }

        protected:
            std::string_view get_type_name() override { return "BlitzAddOns"; }
            void process_event(const Event& e) override {
                if (e.type != EventType::Update) return;
                {
                    std::lock_guard lock{message_mutex};
                    if (message_due) {
                        message_due = false;
                        refresh_message = message_text;
                        refresh_due = true;
                    }
                }
#if BLITZ_MODERN
                show_progress();
#endif
                if (refresh_due) {
                    refresh_due = false;
                    refresh(refresh_message);
                }
                queue_update();
            }

        public:
            // From a button's own callback: rebuilding the list then would delete the button mid-event (it crashed),
            // so the list is rebuilt on the next update. (Another thread: tell().)
            void refresh_later(const std::string& message) {
                refresh_message = message;
                refresh_due = true;
            }

            void refresh(const std::string& message) {
                bool opened = context.open_if_not_already();
#if BLITZ_MODERN
                if (season_wanted && blitz::season_content()) {
                    season_wanted = false;
                    blitz::season_arrived();  // New Blitz, from the next start
                }
                blitz::original_settings_visibility();  // an install or a removal shows or hides what needs the season
#endif
                list->clear_children();
                auto items = scan();
                if (items.empty()) {
                    Label* none = context.create_element<Label>(list, "Nothing added yet. Add… takes a roster pack, uniform sets, art, announcer calls, a rules file or a pak (.nbpak): a file, a folder or a .zip. Check gets the paks New Blitz posts.", theme::Typography::Body);
                    none->set_font_size(28.0f);
                    none->set_color(Color{184, 196, 218, 255});
                }
                for (const Item& it : items) row(it);
                status->set_text(message);
                status->set_display(message.empty() ? Display::None : Display::Block);
                // Restart where the message calls for one ("Restart to use it", "in at the next start"), not for "Up to date."
                bool wants = message.find("Restart") != std::string::npos || message.find("next start") != std::string::npos;
                restart->set_display(!wants || !ultramodern::is_game_started() || !blitz::restart_available ? Display::None : Display::Flex);
                if (opened) context.close();
            }

            AddOnsPanel(ResourceId rid, Element* parent) : Element(rid, parent, Events(EventType::Update), "div", false) {
                context = get_current_context();
                live = this;
                set_display(Display::Flex);
                set_flex_direction(FlexDirection::Column);
                set_padding(28.0f);
                set_gap(14.0f);
                set_height(100.0f, Unit::Percent);
                set_overflow_y(Overflow::Auto);
                set_width(100.0f, Unit::Percent);
                set_as_navigation_container(NavigationType::GridCol);

                Element* top = context.create_element<Element>(this, 0, "div", false);
                top->set_display(Display::Flex);
                top->set_flex_direction(FlexDirection::Row);
                top->set_align_items(AlignItems::Center);
                top->set_flex_wrap(FlexWrap::Wrap);  // buttons keep their labels' width (ui_button.cpp): a narrow window wraps them
                top->set_gap(14.0f);
                top->set_as_navigation_container(NavigationType::GridRow);
                Button* add = context.create_element<Button>(top, "Add…", ButtonStyle::Primary, ButtonSize::Card);
                add->set_min_width(200.0f);
                add->add_pressed_callback([]() {
                    // The picker answers later, maybe on another thread (a Mac's sheet, an iPhone's, Android's since
                    // 1.0.6); no answer (Back, Cancel) leaves the tab as it was.
                    auto picked = [](bool ok, const std::vector<fs::path>& paths) {
                        if (!ok || paths.empty()) {
                            fprintf(stderr, "[addons] the picker closed without a file\n");
                            return;
                        }
                        Report r = install(paths);
                        std::string msg;
                        if (r.done.empty() && r.skipped.empty()) msg = "Nothing in that the game uses.";
                        else {
                            if (!r.done.empty()) msg = "Added " + std::to_string(r.done.size()) + (r.done.size() == 1 ? ": " + r.done[0] : " items") + ".";
                            if (!r.skipped.empty()) msg += (msg.empty() ? "" : " ") + std::string("Skipped: ") + r.skipped[0] + (r.skipped.size() > 1 ? " (+" + std::to_string(r.skipped.size() - 1) + " more)" : "") + ".";
                            if (!r.done.empty()) msg += ultramodern::is_game_started() ? " Restart to use it." : " It loads when the game starts.";
                        }
                        for (const auto& s : r.done) fprintf(stderr, "[addons] added %s\n", s.c_str());
                        for (const auto& s : r.skipped) fprintf(stderr, "[addons] skipped %s\n", s.c_str());
                        tell(msg);
                    };
                    // Tests (the live harness can't work a system picker): BLITZ_ADDONS_PICK=<file> is what the picker
                    // returns.
                    if (const char* f = getenv("BLITZ_ADDONS_PICK"); f != nullptr && *f != '\0') {
                        picked(true, {fs::path{f}});
                        return;
                    }
#ifdef __APPLE__
                    recompui::file::apple::open_panel(true, picked, true);
#else
                    recompui::file::open_file_dialog_multiple([picked](bool ok, const std::list<fs::path>& files) {
                        picked(ok, std::vector<fs::path>(files.begin(), files.end()));  // files only (folders: on a Mac)
                    });
#endif
                });
                // Open Folder: the computer's file browser; a phone's Files app (or, when it can't open there, where the
                // folder is). Not on a console: the PS5's folder is reached over FTP (docs/PS5_HOMEBREW.md).
                if (blitz::platform::file_browser || blitz::platform::phone) {
                    Button* folder = context.create_element<Button>(top, "Open Folder", ButtonStyle::Tertiary, ButtonSize::Card);
                    folder->add_pressed_callback([]() {
#if defined(BLITZ_PLATFORM_PHONE)
                        blitz::open_app_folder_in_files([](bool opened) {
#if defined(BLITZ_PLATFORM_IOS)
                            // BRANDING: the Files app's name for the app's folder
                            tell(opened ? "Files shows New Blitz's folder (On My iPhone → New Blitz). A ROM or a .nbpak pak put there goes in at the next start; for anything else, use Add…."
                                        : "Open the Files app, then On My iPhone → New Blitz. A ROM or a .nbpak pak put there goes in at the next start; for anything else, use Add….");
#else
                            tell(opened ? "Files shows New Blitz's folder. What you put in packs/, uniforms/ or art/ loads when the game starts."
                                        : "No app here can open it. In the Files app, New Blitz is in the menu (☰). The folder: " + folder_words() + ".");
#endif
                        });
#else
                        blitz::open_in_file_browser(app());
#endif
                    });
                }
                if (blitz::export_available) {
                    // Export ROM is Blitz Builder's N64 or emulator path now (team, 2026-11-15): this opens it there.
                    Button* exp = context.create_element<Button>(top, "N64 or Emulator", ButtonStyle::Tertiary, ButtonSize::Card);
                    exp->add_pressed_callback([]() { blitz::firstrun_settings_row(6); });
                }
                restart = context.create_element<Button>(top, "Restart", ButtonStyle::Secondary, ButtonSize::Card);
                restart->add_pressed_callback([]() { blitz::request_restart(); });
                restart->set_display(Display::None);
                // Check, top right (team, 2026-11-27): the catalog now, then every new or changed pak, with progress below.
                check = context.create_element<Button>(top, "Check", ButtonStyle::Secondary, ButtonSize::Card);
                check->set_margin_left_auto();
                check->add_pressed_callback([]() {
                    static uint32_t last = 0;  // one press a second (a held button repeats)
                    if (SDL_GetTicks() - last < 1000) return;
                    last = SDL_GetTicks();
                    if (blitz::pak_server().empty()) {
                        tell("This build has no New Blitz server to check. Add… installs a pak (.nbpak) from a file.");
                        return;
                    }
                    sync_now({});
                });
#if !BLITZ_MODERN
                check->set_display(Display::None);  // the paks are New Blitz's
#endif

                status = context.create_element<Label>(this, "", theme::Typography::Body);
                status->set_font_size(28.0f);
                status->set_color(Color{246, 212, 0, 255});
                status->set_display(Display::None);

                progress = context.create_element<Element>(this, 0, "div", false);
                progress->set_display(Display::None);
                progress->set_flex_direction(FlexDirection::Column);
                progress->set_gap(8.0f);
                progress_text = context.create_element<Label>(progress, "", theme::Typography::Body);
                progress_text->set_font_size(26.0f);
                progress_text->set_color(Color{184, 196, 218, 255});
                Element* track = context.create_element<Element>(progress, 0, "div", false);
                track->set_height(10.0f);
                track->set_width(100.0f, Unit::Percent);
                track->set_border_radius(5.0f);
                track->set_background_color(Color{255, 255, 255, 40});
                progress_fill = context.create_element<Element>(track, 0, "div", false);
                progress_fill->set_height(100.0f, Unit::Percent);
                progress_fill->set_width(0.0f, Unit::Percent);
                progress_fill->set_border_radius(5.0f);
                progress_fill->set_background_color(Color{246, 212, 0, 255});

                list = context.create_element<Element>(this, 0, "div", false);
                list->set_display(Display::Flex);
                list->set_flex_direction(FlexDirection::Column);
                list->set_gap(10.0f);
                list->set_as_navigation_container(NavigationType::GridCol);
                refresh("");
                queue_update();
            }

            ~AddOnsPanel() {
                if (live == this) live = nullptr;
            }
        };
    }
}

namespace blitz {
    void create_addons_tab() {
        recompui::config::create_tab("Add-Ons", "addons", [](recompui::ContextId context, recompui::Element* parent) {
            context.create_element<recompui::AddOnsPanel>(parent);
        });
    }
}
