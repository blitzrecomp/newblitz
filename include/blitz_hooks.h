#ifndef __BLITZ_HOOKS_H__
#define __BLITZ_HOOKS_H__

// Functions called from hooks that N64Recomp inserts into the recompiled game
// (see [[patches.hook]] in blitz2000.toml).

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Called at the start of dma_read (rom, dest, size) and load_main_segment. Registers
// a code segment with the runtime when the game DMAs one in, since the runtime only
// detects overlays for the initial boot copy.
void blitz_on_code_dma(uint8_t* rdram, uint32_t rom, int32_t ram, uint32_t size);

// libultra OSThread.state (1 stopped, 2 runnable, 4 running, 8 waiting) for a thread, derived from
// the runtime's own bookkeeping. Used where the game reads the field directly (src/game/threads.cpp).
uint32_t blitz_libultra_thread_state(uint8_t* rdram, int32_t thread);

// Development counters (src/main/dev_monitor.cpp), printed by BLITZ_MONITOR.
void blitz_count(int id);
// Development log line for audio investigation (dev_monitor.cpp).
void blitz_log_add_player(uint8_t* rdram, int32_t player);
// Records the display list of each graphics task the game submits (for BLITZ_DUMP_RDRAM_AT).
void blitz_on_sp_task(uint8_t* rdram, int32_t task);

// Widescreen (src/game/widescreen.cpp): non-zero while gfx_build_frame should clear the colour buffer.
int blitz_force_color_clear(void);
// Players at the widescreen sides (widescreen.cpp): the cull process's wider answer, given only to the draws.
void blitz_ws_cull_begin(void);
uint32_t blitz_ws_draw_flags(uint8_t* rdram, int32_t player, uint32_t flags);
#ifdef __RECOMP_H__
void blitz_ws_cull_test(uint8_t* rdram, recomp_context* ctx);
void blitz_ws_cull_hidden(recomp_context* ctx);
void blitz_ws_cull_done(uint8_t* rdram, recomp_context* ctx);
#endif
// COP0 Count for the 2026 flavour's patched mfc0 reads (src/game/ultra_extras.cpp).
uint64_t blitz_cop0_count(void);
// Content packs (src/game/content_pack.cpp): called when dma_read finishes.
void blitz_on_dma_done(uint8_t* rdram, uint32_t rom_end, int32_t ram_end);
// RT64 extended commands (src/game/gfx_ext.cpp).
void blitz_gfx_frame_begin(void);
void blitz_gfx_scissor_reset(void);
void blitz_sprite_node(uint8_t* rdram, int32_t node);
void blitz_sprite_before_texrect(uint8_t* rdram, int32_t gp);
// Each player's controller in the game's button icons (button_glyphs.cpp).
void blitz_glyph_owner(int32_t controller);
int32_t blitz_glyph_id(uint8_t* rdram, int32_t id, int per_player);
void blitz_gfx_prologue(uint8_t* rdram, int32_t task);
void blitz_sprite_after_texrect(uint8_t* rdram);  // gfx_ext.cpp: widescreen edge mirroring
// Interpolation tags (gfx_ext.cpp): camera cuts and per-player matrix groups.
void blitz_camera_tag(uint8_t* rdram, int32_t gp, int32_t cam);
void blitz_tall_camera(uint8_t* rdram, int32_t cam);
void blitz_tall_frame_begin(uint8_t* rdram);
void blitz_tall_sprite_begin(uint8_t* rdram, int32_t node);
void blitz_tall_sprite_end(uint8_t* rdram);
int blitz_tall_2d(void);
float blitz_tall_window_y(float picture_y, int w, int h);
uint32_t blitz_tall_fov(uint32_t fov_bits);
int32_t blitz_tall_far(int32_t far_units);
void blitz_player_tag(uint8_t* rdram, int32_t record);
void blitz_player_tags_end(uint8_t* rdram);

// Settings → Rules (src/game/house_rules.cpp).
void blitz_house_rules(uint8_t* rdram);
int32_t blitz_play_clock_seconds(uint8_t* rdram, int32_t game_seconds);  // Settings → Rules → Play Clock
void blitz_rules_play_ended(uint8_t* rdram);                // Rules → Realism: the play-outcome join
int blitz_pat_missed(uint8_t* rdram);                       // Rules → Realism: Extra Points Can Miss
int blitz_rules_end_tied_game(uint8_t* rdram, int32_t quarter);  // Rules → Realism: Overtime

// Settings → General → Skip Intro Screens (src/game/config.cpp).
int blitz_skip_intros(void);
int blitz_skip_rumble_screen(void);  // config.cpp: Settings → General → Skip Rumble Pak Screen
void blitz_play_ended(uint8_t* rdram);  // play_log.cpp: one plays.log line per play
int32_t blitz_pass_target(uint8_t* rdram, int32_t qb, int32_t picked);  // receiver_targets.cpp: Receiver Buttons
void blitz_checkpoint_save(uint8_t* rdram);     // checkpoint.cpp: resume an interrupted game
void blitz_checkpoint_clear(uint8_t* rdram);
void blitz_checkpoint_restore(uint8_t* rdram);
void blitz_tournament_menu(uint8_t* rdram);       // tournament.cpp: Blitz Party tournaments
void blitz_tournament_recorded(uint8_t* rdram);
void blitz_e32_team_select(uint8_t* rdram);  // content_pack_tp.cpp
void blitz_board_image(uint32_t pixels);  // play_log.cpp: an image the loader just finished
void blitz_board_frame(uint8_t* rdram);  // board_digits.cpp
void blitz_loader_enter(uint32_t id, uint32_t caller);  // play_log.cpp: the image loaders, which crash on a file that won't open
void blitz_loader_opened(uint8_t* rdram, uint32_t file);
void blitz_opp_reset(uint8_t* rdram);                    // opponent_select.cpp: choose your CPU opponent
int blitz_opp_wake(uint8_t* rdram, int32_t side);
int blitz_opp_panel_off(int32_t slot);
int blitz_opp_hold(uint8_t* rdram);
void blitz_opp_restore(uint8_t* rdram);
// 32 teams on the original game (src/game/engine32.cpp; hooks only in tools/engine32_toml.py's build).
int32_t blitz_v32_moved(uint8_t* rdram, int32_t side, int32_t s6);
int32_t blitz_v32_row(uint8_t* rdram, int32_t slot, int32_t dir);
int32_t blitz_e32_clash(uint8_t* rdram);
int32_t blitz_e32_list_name(int32_t team);  // the season schedule's line for a team
int32_t blitz_e32_screen_image(int32_t id);  // the Players Inc screen's image, or the credits page's
// Pick Your Crew (src/game/crew.cpp): team select waits while the panel is up (Modern; 0 in Classic).
int blitz_crew_wait(uint8_t* rdram);
int blitz_opp_preview(uint8_t* rdram);
// Mixed uniforms (src/game/mixed_uniforms.cpp): a player's own body and helmet textures (Modern; no-ops in Classic).
void blitz_mixed_body(uint8_t* rdram, int32_t entry);
void blitz_mixed_helmet(uint8_t* rdram, int32_t entry);
void blitz_uniform_reset(int32_t side);  // uniform_picker.cpp

// Drop-in files with new names (src/game/art_dropin.cpp, 2026 flavour).
int blitz_fs_is_extra(int32_t index);
int32_t blitz_fs_extra_entry(int32_t index);
// Skin tones (src/game/skin_tones.cpp): the render parameters' skin read, the skin pass and the bare head.
int32_t blitz_skin_param(uint8_t* rdram, int32_t entry, int32_t raw);
#ifdef __RECOMP_H__
void blitz_skin_pass(uint8_t* rdram, recomp_context* ctx);
void blitz_skin_bare_head(uint8_t* rdram, recomp_context* ctx);
void blitz_savestate_tick(uint8_t* rdram, recomp_context* ctx);  // savestate.cpp: dispatcher loop head
// Instant Replay (src/game/replay.cpp): stopping the game at the dispatcher's loop head, and the pause menu's REPLAY row.
void blitz_replay_gate(uint8_t* rdram);
void blitz_replay_pause_setup(uint8_t* rdram);
void blitz_replay_pause_label(uint8_t* rdram, recomp_context* ctx);
void blitz_replay_pause_label_done(uint8_t* rdram);
void blitz_replay_pause_grey(uint8_t* rdram, recomp_context* ctx);
void blitz_replay_pause_item(void);
void blitz_fs_lookup_extra(uint8_t* rdram, recomp_context* ctx);
void blitz_fs_scan(uint8_t* rdram, recomp_context* ctx);  // art_dropin.cpp: the original game's lookup
void blitz_e32_credits(uint8_t* rdram, recomp_context* ctx);  // engine32.cpp: the credits page, once
void blitz_league_menu(uint8_t* rdram, recomp_context* ctx, int32_t where);  // league paks (content_pack_tp.cpp)
void blitz_uniform_picker(uint8_t* rdram, recomp_context* ctx);  // uniform_picker.cpp
void blitz_uniform_kickoff(recomp_context* ctx);
// Season's pre-game screen (src/game/season_pregame.cpp; engine32 only): 1 = PLAY waits for it (the handler returns).
int blitz_season_pregame(uint8_t* rdram, recomp_context* ctx);
#endif

// Season standings / seeding by host code (src/game/season_tiebreak.cpp): 1 = done, skip the game's.
int blitz_season_standings(uint8_t* rdram, int32_t out, int32_t div);
int blitz_season_seeding(uint8_t* rdram);

// Advanced mode (2026 flavour; src/game/adv_*.cpp). blitz_adv is fixed when the game starts; every
// Advanced hook does nothing while it is 0 (Modern).
extern int blitz_adv;
int blitz_adv_autosim(void);
void blitz_adv_new_season(uint8_t* rdram);
int32_t blitz_adv_build_week(uint8_t* rdram, int32_t week, int32_t out);
void blitz_adv_standings(uint8_t* rdram, int32_t out, int32_t div);
int32_t blitz_adv_week_name(int32_t week);
int32_t blitz_adv_div_names(void);
int32_t blitz_adv_nfc_header_y(void);
void blitz_adv_slots_read(uint8_t* rdram, int32_t status, int32_t mode, int32_t slots);
void blitz_adv_before_load(uint8_t* rdram, int32_t index, int32_t slot);
void blitz_adv_before_write(uint8_t* rdram, int32_t index, int32_t slots);
void blitz_adv_after_write(uint8_t* rdram, int32_t status, int32_t slots);
// Standard Season with 32 teams (2026 flavour; src/game/season32.cpp). blitz_s32 is set while the
// season in the record is a 32-team one; every SEASON32 hook does nothing while it is 0.
extern int blitz_s32;
int blitz_s32_capable(void);
int blitz_s32_div_size(int32_t div);
void blitz_s32_new_season(uint8_t* rdram);
void blitz_s32_after_load(uint8_t* rdram);
// Team select in name order (team_select.cpp, 2026 app).
int32_t blitz_teamsel_step(uint8_t* rdram, int32_t slot, int32_t dir, int32_t count);
int32_t blitz_teamsel_moved(uint8_t* rdram, int32_t s6, int32_t code, int32_t count);
void blitz_league_bank(uint8_t* rdram);
#ifdef __RECOMP_H__
void blitz_opp_setup(uint8_t* rdram, int32_t side, gpr* controller, gpr* team);
void blitz_opp_uniform(uint8_t* rdram, recomp_context* ctx);
void blitz_opp_input(recomp_context* ctx);
void blitz_opp_label_off(uint8_t* rdram, recomp_context* ctx);
void blitz_checkpoint_clock(uint8_t* rdram, recomp_context* ctx);
void blitz_checkpoint_pause_quit(uint8_t* rdram, recomp_context* ctx);
void blitz_checkpoint_vs(uint8_t* rdram, recomp_context* ctx);
void blitz_adv_cpu_skill(uint8_t* rdram, recomp_context* ctx);
void blitz_s32_cpu_skill(uint8_t* rdram, recomp_context* ctx);
void blitz_adv_week_driver(uint8_t* rdram, recomp_context* ctx);
void blitz_adv_div_menu_pos(recomp_context* ctx);
void blitz_adv_schedule_column(recomp_context* ctx);
// Settings → Rules → Smarter Play Calling (src/game/cpu_smart.cpp).
void blitz_cpu_offense_call(uint8_t* rdram, recomp_context* ctx);
void blitz_cpu_fourth_down_fake(uint8_t* rdram, recomp_context* ctx);
void blitz_cpu_punt_fake(uint8_t* rdram, recomp_context* ctx, int second_roll);
void blitz_cpu_fg_fake(uint8_t* rdram, recomp_context* ctx);
int blitz_cpu_go_for_two(uint8_t* rdram, recomp_context* ctx);
#endif

#ifdef __cplusplus
}
#endif

#endif
