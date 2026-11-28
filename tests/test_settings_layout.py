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


class TeamsRows(unittest.TestCase):
    """Settings → Teams' top rows in 1.0.6 (branch league-select, docs/SETTINGS_LAYOUT.md "League"): Game, League (a cycling
    row over NFL 2026 and every league pack), then the NFL's Rosters and Lineups. Saved ids stay: season.json's game,
    rosters and lineups keep their keys; league is new."""

    def setUp(self):
        self.season = (GAME / "season_settings.cpp").read_text()
        self.lineups = (GAME / "rosters_live.cpp").read_text()
        self.adv = (GAME / "adv_mode.cpp").read_text()
        self.select = (GAME / "league_select.cpp").read_text()

    def test_row_order(self):
        tab = self.season[self.season.index("void create_season_tab()"):]
        at = [tab.index(m) for m in ("add_game_option(c)", "opt_league,", "opt_rosters,", "add_lineups_option(c)")]
        self.assertEqual(at, sorted(at), "Game, League, Rosters, Lineups")

    def test_league_is_a_cycling_row(self):
        self.assertIn('constexpr const char* opt_league = "league"', self.season)
        self.assertRegex(self.season, r"add_string_option\(\s*opt_league,")
        self.assertIn('set_option_cycle("season", opt_league', self.season)
        self.assertIn('nfl_label = "NFL 2026"', self.season)

    def test_rosters_nfl_choices_only(self):
        keys = re.findall(r'\{Rosters::\w+, "([a-z0-9]+)", "[^"]+"\}', self.season)
        self.assertEqual(keys, ["auto", "blitz2026", "rom"])
        self.assertNotIn("pack_choices", self.season, "packs are League's now")

    def test_lineups_current_and_all_starters(self):
        block = self.lineups[self.lineups.index("void add_lineups_option"):]
        block = block[:block.index("\n    }\n")]
        self.assertEqual(re.findall(r'\{(\w+), "([a-z]+)", ', block), [("Current", "week"), ("Full", "full")])
        self.assertIn('"All Starters (Healthy)"', block)
        self.assertIn("current_label()", block)
        self.assertIn('"Current ("', self.lineups)
        for gone in ("Built-In", "packed", "Week N"):
            self.assertNotIn(gone, block, gone)
        for gone in ("Packed", "FirstWeek", "week_keys"):  # Built-In and the pinned week, gone from the code too
            self.assertNotIn(gone, self.lineups, gone)

    def test_advanced_keeps_its_weeks(self):
        # An Advanced season plays each week's own lineup from the kept week files (2026-11-28: Advanced kept as it is).
        now = self.lineups[self.lineups.index("std::shared_ptr<const Lineup> lineup_now(int adv_week)"):]
        self.assertIn("saved_weeks()", now)
        self.assertIn("adv_week + 1", now)

    def test_nfl_rows_hide_for_a_league(self):
        self.assertIn('season.update_option_hidden("rosters", o || league)', self.adv)
        self.assertRegex(self.adv, r'\{"lineups", "roster_room"\}\) season\.update_option_hidden\(id, o \|\| none \|\| league\)')

    def test_restart_watch(self):
        # What the game reads only as it starts asks to restart; Lineups is read by every game, so it doesn't.
        watched = re.findall(r'\{"([a-z_]+)", "[A-Za-z ]+"\}', self.select[self.select.index("watched[] ="):])
        self.assertEqual(watched[:4], ["game", "league", "rosters", "season_mode"])
        self.assertNotIn("lineups", watched[:4])


if __name__ == "__main__":
    unittest.main()
