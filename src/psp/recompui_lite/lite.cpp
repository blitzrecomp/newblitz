// recompui-lite (docs/PSP_PORT.md §4.7): what the game code needs from RecompFrontend's recompui, with no UI.
//   - Paths: the app folder and the program folder (psp_main sets both before anything reads a file).
//   - Config registry: named librecomp Configs (the desktop's tabs, without the tabs); finalize() loads <app>/<id>.json.
//   - Prompts and message boxes: logged; a choice prompt resolves at once to its default (the focused button).
// The declarations are platform/psp/include's shadow headers, which keep the frontend's signatures.
#include <atomic>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "recompui/config.h"
#include "recompui/program_config.h"
#include "recompui/recompui.h"
#include "util/file.h"

#include "psp_log.h"
#include "psp_sys.h"
#include "recompui_lite/lite.h"

namespace {
    std::filesystem::path g_app_folder, g_program_folder;
    std::string g_program_name;
    std::u8string g_program_id;
    std::string g_game_mod_id;

    // Recursive by kernel thread id (pspsys::thread_self), not std::recursive_mutex: on the PSP that one asks pthread-embedded
    // for the owner, and every thread not made by pthread_create looks like the same one there (psp_tls.cpp), so a second
    // thread would walk straight in.
    class RecursiveMutex {
    public:
        void lock() {
            int me = pspsys::thread_self();
            if (owner_.load() == me) {
                depth_++;
                return;
            }
            m_.lock();
            owner_ = me;
            depth_ = 1;
        }
        void unlock() {
            if (--depth_ == 0) {
                owner_ = 0;
                m_.unlock();
            }
        }
    private:
        std::mutex m_;
        std::atomic<int> owner_{0};
        int depth_ = 0;
    };
    RecursiveMutex g_configs_mutex;
    std::map<std::string, std::unique_ptr<recomp::config::Config>>& configs() {
        static std::map<std::string, std::unique_ptr<recomp::config::Config>> m;
        return m;
    }

    void once(const char* what) {
        static std::mutex m;
        static std::map<std::string, bool> seen;
        std::lock_guard l(m);
        if (seen[what]) return;
        seen[what] = true;
        PLOG("ui", "%s: no UI on the PSP (logged once)", what);
    }
}

namespace recompui_lite {
    void set_paths(const std::filesystem::path& app_folder, const std::filesystem::path& program_folder) {
        g_app_folder = app_folder;
        g_program_folder = program_folder;
    }
}

// ---- util/file.h ----
std::filesystem::path recompui::file::get_app_folder_path() { return g_app_folder; }
std::filesystem::path recompui::file::get_program_path() { return g_program_folder; }
std::filesystem::path recompui::file::get_asset_path(const char* asset) { return g_program_folder / "assets" / asset; }
void recompui::file::open_file_dialog(std::function<void(bool, const std::filesystem::path&)> callback) {
    once("open_file_dialog");
    callback(false, {});
}
void recompui::file::open_file_dialog_multiple(std::function<void(bool, const std::list<std::filesystem::path>&)> callback) {
    once("open_file_dialog_multiple");
    callback(false, {});
}
void recompui::file::show_error_message_box(const char* title, const char* message) {
    PERR("ui", "%s: %s", title, message);
}

// ---- program_config.h ----
void recompui::programconfig::set_program_name(const std::string& name) { g_program_name = name; }
void recompui::programconfig::set_program_id(const std::u8string& id) { g_program_id = id; }
const std::string& recompui::programconfig::get_program_name() { return g_program_name; }
const std::u8string& recompui::programconfig::get_program_id() { return g_program_id; }

// ---- config.h ----
recomp::config::Config& recompui::config::create_config_tab(const std::string& name, const std::string& id, bool requires_confirmation) {
    std::lock_guard l(g_configs_mutex);
    auto& slot = configs()[id];
    if (!slot) slot = std::make_unique<recomp::config::Config>(name, id, requires_confirmation);
    return *slot;
}

bool recompui::config::has_config(const std::string& id) {
    std::lock_guard l(g_configs_mutex);
    return configs().count(id) != 0;
}

recomp::config::Config& recompui::config::get_config(const std::string& id) {
    std::lock_guard l(g_configs_mutex);
    auto it = configs().find(id);
    if (it != configs().end()) return *it->second;
    PWARN("config", "config '%s' was never registered: an empty one", id.c_str());
    return create_config_tab(id, id, false);
}

recomp::config::Config& recompui::config::get_general_config() { return get_config(general::id); }
recomp::config::Config& recompui::config::get_graphics_config() { return get_config(graphics::id); }
recomp::config::Config& recompui::config::get_sound_config() { return get_config(sound::id); }

void recompui::config::finalize() {
    std::lock_guard l(g_configs_mutex);
    for (auto& [id, config] : configs()) {
        std::error_code ec;
        bool existed = std::filesystem::exists(g_app_folder / (id + ".json"), ec);
        config->load_config();
        if (!existed) config->save_config();
        PLOG("config", "%s.json %s", id.c_str(), existed ? "loaded" : "not there: the defaults, written");
    }
}

void recompui::config::set_restart_callback(std::function<void()>) {}
void recompui::config::set_tab_visible(const std::string&, bool) {}
void recompui::config::set_tab(const std::string&) { once("config::set_tab"); }
void recompui::config::open() { once("config::open"); }
bool recompui::config::close() { return true; }
void recompui::config::set_option_section(const std::string&, const std::string&, const std::string&, std::function<std::string()>) {}
void recompui::config::set_option_action(const std::string&, const std::string&, const std::string&) {}
void recompui::config::set_option_cycle(const std::string&, const std::string&, std::function<std::vector<CycleChoice>()>, bool) {}
std::optional<std::string> recompui::config::get_option_browse(const std::string&, const std::string&) { return std::nullopt; }
void recompui::config::set_option_browse(const std::string&, const std::string&, std::optional<std::string>) {}
void recompui::config::set_option_browse_left(std::function<void(const std::string&, const std::string&)>) {}
void recompui::config::set_close_guard(std::function<bool(bool)>) {}
void recompui::config::set_accept_hint(const std::string&) {}
void recompui::config::set_restart_pending(std::function<std::string()>) {}
void recompui::config::describe_header_button(const std::string&, const std::string&) {}
void recompui::config::place_option(const std::string&, const std::string&, const std::string&, bool) {}

namespace {
    template <typename T>
    T option_or(const std::string& config_id, const std::string& option_id, T fallback) {
        if (!recompui::config::has_config(config_id)) return fallback;
        recomp::config::Config& c = recompui::config::get_config(config_id);
        if (!c.has_option(option_id)) return fallback;
        auto v = c.get_option_value(option_id);
        if (const T* p = std::get_if<T>(&v)) return *p;
        return fallback;
    }
}

bool recompui::config::general::has_rumble_strength_option() { return false; }
double recompui::config::general::get_rumble_strength() { return 0.0; }
double recompui::config::general::get_joystick_deadzone() { return option_or<double>(id, options::joystick_deadzone, 15.0); }
bool recompui::config::general::get_background_input_mode_enabled() { return true; }
bool recompui::config::general::get_debug_mode_enabled() { return false; }
double recompui::config::sound::get_main_volume() { return option_or<double>(id, options::main_volume, 100.0); }

// ---- recompui.h ----
void recompui::message_box(const char* msg) {
    PWARN("ui", "message box: %s", msg);
}

void recompui::open_choice_prompt(const std::string& header_text, const std::string& content_text, const std::string& confirm_label,
                                  const std::string& cancel_label, std::function<void()> confirm_action,
                                  std::function<void()> cancel_action, ButtonStyle, ButtonStyle, bool focus_on_cancel) {
    PLOG("ui", "prompt '%s' (%s): answered '%s', its default", header_text.c_str(), content_text.c_str(),
         focus_on_cancel ? cancel_label.c_str() : confirm_label.c_str());
    if (focus_on_cancel) {
        if (cancel_action) cancel_action();
    } else if (confirm_action) {
        confirm_action();
    }
}

void recompui::open_info_prompt(const std::string& header_text, const std::string& content_text, const std::string& okay_label,
                                std::function<void()> okay_action, ButtonStyle) {
    PLOG("ui", "info '%s' (%s): answered '%s'", header_text.c_str(), content_text.c_str(), okay_label.c_str());
    if (okay_action) okay_action();
}

void recompui::open_notification(const std::string& header_text, const std::string& content_text) {
    PLOG("ui", "notification '%s': %s", header_text.c_str(), content_text.c_str());
}

void recompui::set_prompt_scoreline(const std::string&) {}
void recompui::close_prompt() {}
bool recompui::is_prompt_open() { return false; }
void recompui::hide_all_contexts() {}
bool recompui::is_any_context_shown() { return false; }
bool recompui::is_context_capturing_input() { return false; }
void recompui::update_game_mod_id(const std::string& game_mod_id) { g_game_mod_id = game_mod_id; }
const std::string& recompui::get_game_mod_id() { return g_game_mod_id; }
