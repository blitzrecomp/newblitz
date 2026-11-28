"""Settings' 1.0.5 layout (src/game/settings_layout.h, docs/SETTINGS_LAYOUT.md) against the options config.cpp defines.

The layout only moves where a row is drawn: every option keeps its id and its config (and so its file, general.json and
the rest). These checks keep it that way: no saved id from 1.0.4 is renamed or dropped, every General row is either
kept on General or drawn on exactly one other tab, and every placed option exists in the config it names."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GAME = ROOT / "src" / "game"

# General's saved ids in 1.0.4 (config.cpp, and RecompFrontend's stock General rows): a player's general.json holds these.
GENERAL_1_0_4 = {
    "about", "potato_two_screen_sharpness", "blitz_builder", "button_icon_mode", "change_rom", "copy_debug_info",
    "crash_report_view", "crash_reports", "ds_screens", "ds_tilt", "ds_zoom", "experimental_resume", "fps_counter",
    "fullscreen_button", "hints", "hints_reset", "hints_seen", "install_notice_day", "instant_replay", "launcher_at_start",
    "main_menu", "p1_rumble", "pak_backups", "phone_controllers", "potato_frame_copy", "potato_mode", "potato_two_screens_on",
    "potato_waits", "redo_play", "reset_data", "restore_backup", "save_logs", "savestate_buttons", "send_logs",
    "setup_ask_updates", "setup_season_asked", "setup_started", "setup_updates_asked", "skip_intros", "skip_rumble_screen",
    "swap_screens", "switch_engine", "touch_controls", "tv_mode", "two_screens", "volume", "welcomed",
}
# RecompFrontend's General tab (ui_config_tab_general.cpp): gyro and mouse aren't added in Blitz (GeneralTabOptions).
STOCK_GENERAL = {"debug_mode", "rumble_strength", "joystick_deadzone", "background_input_mode", "gyro_sensitivity", "mouse_sensitivity"}
# Rows other branches add to General that the layout already places (Save Sync's, branch cloud-sync).
OTHER_BRANCHES = {"save_sync"}


def option_ids(source, var):
    """The ids a file adds to a config through `var` (general, c): add_*_option("id", ...)."""
    text = (GAME / source).read_text()
    ids = re.findall(var + r'\.add_\w+?_option\(\s*("[a-z0-9_]+"|blitz::potato_sharpness_id)', text)
    return {"potato_two_screen_sharpness" if i == "blitz::potato_sharpness_id" else i.strip('"') for i in ids}


def layout():
    """settings_layout.h's placements (config, option, tab, first, section) and general_kept."""
    text = (GAME / "settings_layout.h").read_text()
    rows = re.findall(r'\{"([a-z_]+)", "([a-z0-9_]+)", "([a-z_]+)", (true|false), ("[^"]*"|nullptr)\}', text)
    kept_block = re.search(r"general_kept\[\] = \{(.*?)\};", text, re.S).group(1)
    kept = set(re.findall(r'"([a-z0-9_]+)"', kept_block))
    return rows, kept


class SettingsLayout(unittest.TestCase):
    def setUp(self):
        self.rows, self.kept = layout()
        self.general = option_ids("config.cpp", "general") | STOCK_GENERAL
        self.placed_general = [r[1] for r in self.rows if r[0] == "general"]

    def test_layout_read(self):
        self.assertGreater(len(self.rows), 30)
        self.assertIn("main_menu", self.kept)

    def test_no_saved_id_renamed_or_dropped(self):
        missing = GENERAL_1_0_4 - self.general
        self.assertEqual(missing, set(), "General ids from 1.0.4 no longer defined (a player's saved value would be lost)")

    def test_every_general_row_has_one_place(self):
        placed = set(self.placed_general)
        self.assertEqual(len(placed), len(self.placed_general), "an option placed twice")
        both = placed & self.kept
        self.assertEqual(both, set(), "kept on General and placed elsewhere")
        unaccounted = self.general - placed - self.kept
        self.assertEqual(unaccounted, set(), "General rows the layout doesn't mention (add them to settings_layout.h)")

    def test_placed_options_exist(self):
        configs = {
            "general": self.general | OTHER_BRANCHES,
            "party": option_ids("tournament.cpp", "c"),
        }
        for config, option, tab, _first, _section in self.rows:
            self.assertIn(config, configs, f"{config}/{option}: a config this test doesn't know")
            self.assertIn(option, configs[config], f"{config}/{option} is placed on {tab} but not defined")
        self.assertEqual({o for c, o, *_ in self.rows if c == "party"}, configs["party"], "Bracket's rows, all on Teams")

    def test_tabs_exist(self):
        tabs = {r[2] for r in self.rows}
        known = {"controls", "graphics", "screens", "rules", "season", "saves"}
        self.assertEqual(tabs - known, set())
        tabs_src = (GAME / "settings_tabs.cpp").read_text()
        self.assertIn('create_config_tab("Saves", "saves"', tabs_src)
        self.assertIn('constexpr const char* screens_id = "screens"', tabs_src)

    def test_two_screens_ids_apart_from_general(self):
        # The Two Screens tab's own file (screens.json): its dials never shadow a General id.
        header = (GAME / "two_screens.h").read_text()
        dial_ids = set(re.findall(r'\{\s*(?:tilt_id|zoom_id|"([a-z_]+)"), "[a-z_]+", "', header)) - {""}
        self.assertGreater(len(dial_ids), 10)
        self.assertEqual(dial_ids & self.general, set())


if __name__ == "__main__":
    unittest.main()
