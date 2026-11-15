// The PSP's settings (docs/PSP_PORT.md §4.7): the desktop's option ids and defaults, registered in recompui-lite without
// tabs, so <app folder>/<id>.json are the same files as on the desktop. The Tier 2 sources that compile here register their
// own (Rules, Bracket, Season); General has only the options the PSP's code reads, with the desktop's defaults
// (src/game/config.cpp).
#include "recompui/config.h"
#include "psp_log.h"

namespace blitz {
    void create_rules_tab();   // house_rules.cpp
    void create_party_tab();   // tournament.cpp
    void create_season_tab();  // season_settings.cpp
    void psp_stubs_report();   // stubs_hooks.cpp

    void psp_register_settings() {
        recomp::config::Config& general = recompui::config::create_config_tab("General", recompui::config::general::id, false);
        general.add_bool_option("skip_intros", "Skip Intro Screens", "", false);
        general.add_bool_option("skip_rumble_screen", "Skip Rumble Pak Screen", "", true);
        general.add_bool_option("experimental_resume", "Resume", "", false, true);
        recompui::config::create_config_tab("Sound", recompui::config::sound::id, false)
            .add_number_option(recompui::config::sound::options::main_volume, "Main Volume", "", 0, 100, 1, 0, false, 100);
        create_season_tab();
        create_rules_tab();
        create_party_tab();
        recompui::config::finalize();
        psp_stubs_report();
    }
}
