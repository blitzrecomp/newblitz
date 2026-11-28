// recompui-lite (docs/PSP_PORT.md §4.7): RecompFrontend's recompui/config.h with the same signatures for what the game code
// calls, and nothing that needs RmlUi (no tabs, modals or tab callbacks). The config registry is librecomp's own
// recomp::config::Config; the tabs are just named configs (src/psp/recompui_lite/lite.cpp). A source that needs more than
// this does not compile on the PSP, which is the signal to give its hooks a "feature off" body in src/psp/stubs_hooks.cpp.
#ifndef _RECOMP_UI_CONFIG_H_
#define _RECOMP_UI_CONFIG_H_

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "librecomp/config.hpp"

namespace recompui {
    namespace config {
        namespace general {
            inline const std::string id = "general";
            inline const std::string tab_name = "General";
            namespace options {
                inline const std::string rumble_strength = "rumble_strength";
                inline const std::string gyro_sensitivity = "gyro_sensitivity";
                inline const std::string mouse_sensitivity = "mouse_sensitivity";
                inline const std::string joystick_deadzone = "joystick_deadzone";
                inline const std::string background_input_mode = "background_input_mode";
                inline const std::string debug_mode = "debug_mode";
            }
            bool has_rumble_strength_option();
            double get_rumble_strength();
            double get_joystick_deadzone();
            bool get_background_input_mode_enabled();
            bool get_debug_mode_enabled();
        }
        namespace graphics {
            inline const std::string id = "graphics";
            inline const std::string tab_name = "Graphics";
        }
        namespace sound {
            inline const std::string id = "sound";
            inline const std::string tab_name = "Sound";
            namespace options {
                inline const std::string main_volume = "main_volume";
            }
            double get_main_volume();
        }

        // A named config (saved as <app folder>/<id>.json once finalize() has loaded it).
        recomp::config::Config &create_config_tab(const std::string &name, const std::string &id, bool requires_confirmation);
        void set_restart_callback(std::function<void()> callback);
        void finalize();
        // UI calls: nothing to show on the PSP. They log once and do nothing.
        void set_tab_visible(const std::string &id, bool is_visible);
        void set_tab(const std::string &id);
        void open();
        bool close();
        void set_option_section(const std::string &config_id, const std::string &option_id, const std::string &label,
                                std::function<std::string()> count = {});
        void set_option_action(const std::string &config_id, const std::string &option_id, const std::string &button_text);
        struct CycleChoice {  // RecompFrontend patch 0058: the League row
            std::string value;
            std::string label;
        };
        void set_option_cycle(const std::string &config_id, const std::string &option_id,
                              std::function<std::vector<CycleChoice>()> choices, bool confirm = false);
        std::optional<std::string> get_option_browse(const std::string &config_id, const std::string &option_id);
        void set_option_browse(const std::string &config_id, const std::string &option_id, std::optional<std::string> value);
        void set_option_browse_left(std::function<void(const std::string &config_id, const std::string &option_id)> callback);
        void set_close_guard(std::function<bool(bool modal_close)> guard);
        void set_accept_hint(const std::string &label);
        void set_restart_pending(std::function<std::string()> pending);
        void describe_header_button(const std::string &title, const std::string &text);

        recomp::config::Config &get_config(const std::string &id);
        bool has_config(const std::string &id);  // lite only
        recomp::config::Config &get_general_config();
        recomp::config::Config &get_graphics_config();
        recomp::config::Config &get_sound_config();
    }
}

#endif
