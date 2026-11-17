// "Feature off" bodies for the hooks whose real sources don't build on the PSP (docs/PSP_PORT.md §2.2, §4.9). Each returns
// what its real source returns with the feature switched off, and none calls abort(). A hook the game's code calls that
// has no body anywhere is a link error: that is the tripwire. psp_stubs_report() lists what is off, once, at boot.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "recomp.h"
#include "recompui/config.h"
#include "psp_log.h"

namespace blitz {
    void psp_stubs_report() {
        PLOG("stubs", "off on the PSP: RT64's extended GBI (gfx_ext: player and camera tags, sprite anchors, widescreen), "
                      "Receiver Buttons, save states and Redo Play, the BLITZ DS views, Crew"
#if BLITZ_MODERN
                      ", Pro Bowl, the Roster Room, Tall View, Instant Replay"
#else
                      ", added game files (art_dropin: button icon files)"
#endif
                      ", physical controller glyphs (the N64's own), live dev input, hints");
    }
}

// ---- gfx_ext.cpp (RT64's extended GBI): emits nothing ----
extern "C" void blitz_camera_tag(uint8_t*, int32_t, int32_t) {}
extern "C" void blitz_player_tag(uint8_t*, int32_t) {}
extern "C" void blitz_player_tags_end(uint8_t*) {}
extern "C" void blitz_gfx_frame_begin(void) {}
extern "C" void blitz_gfx_scissor_reset(void) {}
extern "C" void blitz_sprite_node(uint8_t*, int32_t) {}
extern "C" void blitz_sprite_before_texrect(uint8_t*, int32_t) {}
extern "C" void blitz_sprite_after_texrect(uint8_t*) {}
extern "C" void blitz_gfx_prologue(uint8_t*, int32_t) {}
// widescreen.cpp: Aspect Ratio Expand is never on.
extern "C" int blitz_force_color_clear(void) { return 0; }

// ---- crew.cpp: Classic has no player file ----
extern "C" int blitz_crew_wait(uint8_t*) { return 0; }
// ---- season_pregame.cpp: no pre-game screen (no settings UI); PLAY plays at once ----
extern "C" int blitz_season_pregame(uint8_t*, recomp_context*) { return 0; }

// ---- ds_view.cpp: no drive summary ----
extern "C" void blitz_ds_play(const char*, const char*, int, const char*, int, int, int, int, int, int, int) {}

// ---- receiver_targets.cpp: Receiver Buttons off; the game's own pick ----
extern "C" int32_t blitz_pass_target(uint8_t*, int32_t, int32_t picked) { return picked; }
namespace blitz {
    // The same option (id and default) as the desktop, so rules.json is the same file.
    void add_receiver_option(recomp::config::Config& c) {
        c.add_bool_option("receiver_buttons", "Receiver Buttons", "Off on the PSP.", false);
    }
}

// ---- savestate.cpp with BLITZ_SAVESTATES off (its own stub section, which can't compile here for its SDL include) ----
extern "C" void blitz_ss_note_stop(uint8_t*, int32_t) {}
extern "C" void blitz_ss_note_start(int32_t) {}
extern "C" void blitz_ss_note_destroy(int32_t) {}
extern "C" void blitz_redo_play_top(uint8_t*) {}
extern "C" void blitz_redo_play_ended(uint8_t*) {}
extern "C" void blitz_redo_clear() {}
extern "C" void blitz_team_select_frame() {}
extern "C" void blitz_savestate_tick(uint8_t*, recomp_context*) {}
namespace blitz {
    void savestate_register(const std::string&, std::function<std::vector<uint8_t>()>, std::function<void(const std::vector<uint8_t>&)>,
                            std::function<std::string(const std::vector<uint8_t>&)>) {}
}

// ---- config.cpp: the two General options the game asks about ----
extern "C" int blitz_skip_rumble_screen(void) {
    return std::get<bool>(recompui::config::get_general_config().get_option_value("skip_rumble_screen")) ? 1 : 0;
}
extern "C" int blitz_skip_intros(void) {
    return std::get<bool>(recompui::config::get_general_config().get_option_value("skip_intros")) ? 1 : 0;
}

// ---- art_dropin.cpp: no files are added to the game's file system (that rewrites the whole ROM in memory). Modern: the
// kit's ROM has them merged on the Mac, and psp_artfs.cpp reads its file table (psp-host's --make-kit runs art_dropin) ----
#if !BLITZ_MODERN
namespace blitz {
    void fs_mark(int) {}
    void fs_commit_files() {}
    bool fs_register_file(const std::string&, std::vector<uint8_t>) { return false; }
    int32_t fs_file_index(const std::string&) { return -1; }
    int32_t fs_rom_index(const std::string&) { return -1; }
    uint32_t fs_ram_count(uint8_t*) { return 0; }
    bool fs_restore_entry(uint8_t*, int32_t) { return false; }
}
#endif

// ---- controllers.cpp, dev_input.cpp, hints.cpp, os_shell.cpp, main.cpp ----
namespace blitz {
    int port_glyph_family(int) { return 3; }  // the N64's own icons
    int menu_owner_port() { return 0; }
    void dev_input_game_started() {}
    void hints_mark_team_select() {}
    void open_in_file_browser(const std::filesystem::path&) {}
}

#if BLITZ_MODERN
// ---- Modern (M8): the hooks of sources the PSP leaves out, feature off ----
#include <atomic>
#include <map>
#include "rosters_live.h"

// tall_view.cpp (Tall View, the BLITZ DS views): one screen, the game's own camera.
extern "C" void blitz_tall_frame_begin(uint8_t*) {}
extern "C" void blitz_tall_sprite_begin(uint8_t*, int32_t) {}
extern "C" void blitz_tall_sprite_end(uint8_t*) {}
extern "C" void blitz_tall_camera(uint8_t*, int32_t) {}
extern "C" uint32_t blitz_tall_fov(uint32_t fov_bits) { return fov_bits; }
extern "C" int32_t blitz_tall_far(int32_t far_units) { return far_units; }

// replay.cpp (Instant Replay: RT64 keeps the frames). The game's pause menu was given a fourth row in the toml (QUIT GAME
// moved to row 3), so its rows and labels must still be made exactly as replay.cpp makes them; REPLAY does nothing here.
extern "C" {
    void func_8027A808(uint8_t* rdram, recomp_context* ctx);  // a text's colour
    void blitz_replay_gate(uint8_t*) {}
    void blitz_replay_pause_setup(uint8_t* rdram) {
        constexpr int32_t row3 = (int32_t)0x8028BFB4;
        MEM_W(0, (gpr)row3) = 0x42E60000;      // 115.0
        MEM_W(4, (gpr)row3) = 0x429E0000;      // 79.0
        MEM_W(8, (gpr)row3) = 0x42A00000;      // 80.0
    }
}
namespace {
    constexpr int32_t text_resume = (int32_t)0x802AC4E0, text_options = (int32_t)0x802AC4D8, text_quit = (int32_t)0x802AC4CC;
    constexpr int text_quit_length = 10;
    char quit_saved[text_quit_length];
    bool quit_swapped = false;
}
extern "C" {
    void blitz_replay_pause_label(uint8_t* rdram, recomp_context* ctx) {
        int row = (int)ctx->r22 - 1;
        ctx->r7 = (gpr)(row == 0 ? text_resume : row == 1 ? text_options : text_quit);
        if (row != 2) return;
        static const char replay[text_quit_length] = "REPLAY";
        for (int i = 0; i < text_quit_length; i++) {
            quit_saved[i] = (char)MEM_B(i, (gpr)text_quit);
            MEM_B(i, (gpr)text_quit) = (int8_t)replay[i];
        }
        quit_swapped = true;
    }
    void blitz_replay_pause_label_done(uint8_t* rdram) {
        if (!quit_swapped) return;
        quit_swapped = false;
        for (int i = 0; i < text_quit_length; i++) MEM_B(i, (gpr)text_quit) = (int8_t)quit_saved[i];
    }
    void blitz_replay_pause_grey(uint8_t* rdram, recomp_context* ctx) {
        ctx->r4 = (gpr)MEM_W(0x2C, ctx->r29);
        ctx->r5 = (gpr)(int32_t)0xFFA3A300;
        func_8027A808(rdram, ctx);
    }
    void blitz_replay_pause_item(void) {}
}

// crew.cpp (Pick Your Crew, Pro Bowl: RmlUi panels): no crews, no Pro Bowl.
extern "C" void blitz_crew_kickoff(uint8_t*, int32_t, int32_t, int32_t, int32_t) {}
namespace blitz {
    std::map<int, LineupPlayer> crew_players(const std::string&) { return {}; }
    void probowl_register_files() {}
    bool probowl_code_saved(int) { return false; }
    void probowl_ask_code(int) {}
    int probowl_ask_state(int) { return 3; }  // "cancelled": no keyboard here
    std::string probowl_code(int, bool) { return "PRO"; }
    void probowl_names(uint8_t*) {}
    // The same option (id and default) as the desktop, so rules.json is the same file; off here.
    void add_crew_option(recomp::config::Config& c) {
        c.add_bool_option("pick_crew", "Pick Your Crew", "Off on the PSP.", true);
    }
    // roster_room.cpp (the Roster Room: RmlUi)
    std::atomic<bool> crews_changed{false};
    void roster_room_open() {}
    // dev_input.cpp: a press the picker sends (the Pro Bowl code prompt, which is off here)
    void inject_press(int, uint16_t, int) {}
}
#endif
