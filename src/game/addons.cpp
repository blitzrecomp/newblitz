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
//   - a 2026 Season pak (.nbpak, or a .zip with pak.json at its root) -> paks/season2026 (season_pak.cpp)
// PNG uniform art isn't game format yet: it's reported, with how to convert it (tools/import_uniform_sets.py).
//
// The 2026 Season (2026-11-13): listed first when it's installed (Remove takes it out), with Update when the server has
// a newer one; without it (the public app), Get the 2026 Season downloads it (the same as Blitz Builder's Add It).
#include "../main/os_shell.h"
#include "blitz_config.h"
#include <algorithm>
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
        std::string kind;    // ROSTERS, UNIFORMS, ART, ANNOUNCER, RULES
        std::string name;
        std::string detail;
        fs::path path;       // what Remove deletes
    };

    // The season download's end, from its thread, for the open tab to show (process_event).
    std::mutex season_mutex;
    bool season_done = false;
    std::string season_message;

    std::vector<Item> scan() {
        std::vector<Item> out;
        std::error_code ec;
#if BLITZ_MODERN
        if (blitz::season_pak_installed()) {
            std::string v = blitz::season_pak_version(), update = blitz::pak_update_available();
            out.push_back({"SEASON", "2026 Season", (v.empty() ? std::string("") : "version " + v + ": ") +
                           "rosters, lineups, uniforms, team art, Houston and Pro Bowl" +
                           (update.empty() ? "" : " (version " + update + " is out)"), blitz::season_pak_dir()});
        }
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
            else if (blitz::is_pak_file(p)) {  // the 2026 Season: into paks/, checked (season_pak.cpp)
                std::string error;
                if (blitz::pak_install_file(p, "", error)) r.add("the 2026 Season (" + blitz::season_pak_version() + ")");
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

        class AddOnsPanel : public Element {
            ContextId context;
            Element* list = nullptr;
            Label* status = nullptr;
            Button* restart = nullptr;
            Button* get_season = nullptr;

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
                Button* remove = context.create_element<Button>(card, "Remove", ButtonStyle::Tertiary, ButtonSize::Card);
                remove->set_flex_shrink(0.0f);
                fs::path target = it.path;
                bool season = it.kind == "SEASON";
                // Confirmed on the button itself (a prompt can't open over Settings from the launcher): the first
                // press asks "Sure?", a second within a few seconds removes.
                remove->add_pressed_callback([remove, target, season]() {
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
                    if (season) {
                        if (!blitz::pak_remove(error)) ec = std::make_error_code(std::errc::io_error);
                    }
                    else fs::remove_all(target, ec);
                    if (!error.empty()) error = "Couldn't remove it: " + error;
                    else if (ec) error = "Couldn't remove it: " + ec.message();
                    fprintf(stderr, "[addons] removed %s\n", target.string().c_str());
                    if (live) live->refresh_later(!error.empty() ? error : std::string("Removed.") + (ultramodern::is_game_started() ? " Restart to see the change." : ""));
                });
            }

            bool refresh_due = false;
            std::string refresh_message;

        protected:
            std::string_view get_type_name() override { return "BlitzAddOns"; }
            void process_event(const Event& e) override {
                if (e.type != EventType::Update) return;
                {
                    std::lock_guard lock{season_mutex};
                    if (season_done) {
                        season_done = false;
                        refresh_message = season_message;
                        refresh_due = true;
                    }
                }
                if (refresh_due) {
                    refresh_due = false;
                    refresh(refresh_message);
                }
                queue_update();
            }

        public:
            // From a button's own callback: rebuilding the list then would delete the button mid-event (it crashed),
            // so the list is rebuilt on the next update.
            void refresh_later(const std::string& message) {
                refresh_message = message;
                refresh_due = true;
            }

            void refresh(const std::string& message) {
                bool opened = context.open_if_not_already();
#if BLITZ_MODERN
                static bool had_season = blitz::season_content();
                if (!had_season && blitz::season_content()) blitz::season_arrived();  // New Blitz, from the next start
                had_season = blitz::season_content();
                blitz::original_settings_visibility();  // an install or a removal shows or hides what needs the season
                bool offer = !blitz::season_content() || !blitz::pak_update_available().empty();
                get_season->set_text(blitz::season_pak_installed() ? "Update 2026 Season" : "Get the 2026 Season");
                get_season->set_display(offer && !blitz::pak_download_busy() ? Display::Flex : Display::None);
#endif
                list->clear_children();
                auto items = scan();
                if (items.empty()) {
                    Label* none = context.create_element<Label>(list, "Nothing added yet. Add… takes a roster pack, uniform sets, art, announcer calls, a rules file or the 2026 Season (.nbpak): a file, a folder or a .zip.", theme::Typography::Body);
                    none->set_font_size(28.0f);
                    none->set_color(Color{184, 196, 218, 255});
                }
                for (const Item& it : items) row(it);
                status->set_text(message);
                status->set_display(message.empty() ? Display::None : Display::Block);
                restart->set_display(message.empty() || !ultramodern::is_game_started() || !blitz::restart_available ? Display::None : Display::Flex);
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
#ifdef __APPLE__
                    recompui::file::apple::open_panel(true, [](bool ok, const std::vector<fs::path>& paths) {
#else
                    recompui::file::open_file_dialog_multiple([](bool ok, const std::list<fs::path>& picked) {
                        std::vector<fs::path> paths(picked.begin(), picked.end());  // files only (folders: on a Mac)
#endif
                        if (!ok) return;
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
                        if (live) live->refresh_later(msg);
                    }
#ifdef __APPLE__
                    , true
#endif
                    );
                });
                // The 2026 Season from the server (season_pak.cpp), when it isn't here or a newer one is out.
                get_season = context.create_element<Button>(top, "Get the 2026 Season", ButtonStyle::Primary, ButtonSize::Card);
                get_season->set_display(Display::None);
                get_season->add_pressed_callback([this]() {
                    bool started = blitz::pak_download_start([](bool ok, const std::string& message) {
                        std::lock_guard lock{season_mutex};
                        season_done = true;
                        season_message = ok ? std::string("2026 Season added.") + (ultramodern::is_game_started() ? " Restart to use it." : " It loads when the game starts.")
                                            : message;
                    });
                    if (started) refresh_later("Getting the 2026 Season…");
                });
                Button* folder = context.create_element<Button>(top, "Open Folder", ButtonStyle::Tertiary, ButtonSize::Card);
                folder->add_pressed_callback([]() {
                    blitz::open_in_file_browser(app());
                });
                if (blitz::export_available) {
                    // Export ROM is Blitz Builder's N64 or emulator path now (team, 2026-11-15): this opens it there.
                    Button* exp = context.create_element<Button>(top, "N64 or Emulator", ButtonStyle::Tertiary, ButtonSize::Card);
                    exp->add_pressed_callback([]() { blitz::firstrun_settings_row(6); });
                }
                restart = context.create_element<Button>(top, "Restart", ButtonStyle::Secondary, ButtonSize::Card);
                restart->add_pressed_callback([]() { blitz::request_restart(); });
                restart->set_display(Display::None);

                status = context.create_element<Label>(this, "", theme::Typography::Body);
                status->set_font_size(28.0f);
                status->set_color(Color{246, 212, 0, 255});
                status->set_display(Display::None);

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
