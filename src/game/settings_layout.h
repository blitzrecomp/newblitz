// The Settings tabs' layout for 1.0.5 (docs/SETTINGS_LAYOUT.md): which rows are drawn on another tab than their own
// config's. Only where a row is drawn moves: every option keeps its id, its config and that config's file (general.json
// and the rest), so nothing saved is read from anywhere new and no one's setting changes. recompui::config::place_option
// (platform/recompfrontend-patches/0052) draws them; settings_tabs.cpp applies this table. tests/test_settings_layout.py
// checks it against config.cpp: every General row is either kept on General or placed once, and no id was renamed.
#pragma once

namespace blitz::settings_layout {
    struct Placement {
        const char* config;   // the option's own config (its file: <config>.json)
        const char* option;   // its id, as saved
        const char* tab;      // the tab it's drawn on
        bool first;           // before the tab's own rows (else after them)
        const char* section;  // a heading drawn above it, or null
    };

    // Options a build may not have are skipped where they're missing (touch_controls is a phone's, the potato rows
    // Android's, save_sync the Save Sync branch's).
    inline constexpr Placement placements[] = {
        // Controls: the controller's own settings, above the button mappings.
        {"general", "p1_rumble", "controls", true, "CONTROLLER"},
        {"general", "rumble_strength", "controls", true, nullptr},
        {"general", "joystick_deadzone", "controls", true, nullptr},
        {"general", "button_icon_mode", "controls", true, nullptr},
        {"general", "fullscreen_button", "controls", true, nullptr},
        {"general", "touch_controls", "controls", true, nullptr},
        {"general", "background_input_mode", "controls", true, nullptr},
        // Graphics: after its own rows.
        {"general", "tv_mode", "graphics", false, "DISPLAY"},
        {"general", "fps_counter", "graphics", false, nullptr},
        {"general", "potato_mode", "graphics", false, "POTATO MODE"},
        {"general", "potato_two_screens_on", "graphics", false, nullptr},
        {"general", "potato_two_screen_sharpness", "graphics", false, nullptr},
        {"general", "potato_waits", "graphics", false, nullptr},
        {"general", "potato_frame_copy", "graphics", false, nullptr},
        // Two Screens (only on a device with two): the modes and the presets, before its Custom Layout.
        {"general", "two_screens", "screens", true, "SCREENS"},
        {"general", "ds_screens", "screens", true, nullptr},
        {"general", "swap_screens", "screens", true, nullptr},
        {"general", "ds_tilt", "screens", true, "CAMERA"},
        {"general", "ds_zoom", "screens", true, nullptr},
        // Rules: after its own rows.
        {"general", "redo_play", "rules", false, "DO-OVERS"},
        {"general", "instant_replay", "rules", false, nullptr},
        {"general", "skip_intros", "rules", false, "GAME FLOW"},
        {"general", "skip_rumble_screen", "rules", false, nullptr},
        // Teams: the Bracket tab folded in, after the rosters and Season.
        {"party", "names", "season", false, "BRACKET"},
        {"party", "series", "season", false, nullptr},
        {"party", "seeding", "season", false, nullptr},
        {"party", "create", "season", false, nullptr},
        {"party", "standings", "season", false, nullptr},
        // Saves: save states and the Controller Pak first, then whatever Save Sync adds (saves_tab_entries), then the
        // whole-app ones last.
        {"general", "savestate_buttons", "saves", true, "SAVE STATES"},
        {"general", "experimental_resume", "saves", true, nullptr},
        {"general", "sync_now", "saves", true, "CONTROLLER PAK"},
        {"general", "save_sync", "saves", true, nullptr},
        {"general", "pak_backups", "saves", true, nullptr},
        {"general", "reset_data", "saves", false, "ALL YOUR DATA"},
        {"general", "restore_backup", "saves", false, nullptr},
    };

    // General's own rows that stay on General (app-wide things). With the placements above, every General option.
    inline constexpr const char* general_kept[] = {
        "debug_mode", "main_menu", "blitz_builder", "change_rom", "switch_engine", "volume", "about", "hints", "hints_reset",
        "settings_freeze", "copy_debug_info", "save_logs", "send_logs", "crash_reports", "crash_report_view",
        // Hidden: kept for what they store.
        "welcomed", "setup_season_asked", "setup_updates_asked", "setup_ask_updates", "setup_started", "hints_seen",
        "install_notice_day", "phone_controllers", "launcher_at_start", "gyro_sensitivity", "mouse_sensitivity",
        "cloud_ask_count", "cloud_ask_never", "cloud_ask_version", "content_ask_done",
    };
}
