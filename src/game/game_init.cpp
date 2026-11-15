// Per-boot setup run by the runtime after the boot segment is loaded and before the
// game's entrypoint (GameEntry::on_init_callback).
#include "recomp.h"
#include "ultramodern/ultra64.h"

extern "C" void osViSetMode(uint8_t* rdram, PTR(OSViMode) mode);

namespace blitz {
    void dev_input_game_started();
    void monitor_set_rdram(uint8_t* rdram);
    void glyphs_init();    // button_glyphs.cpp
    void fs_commit_files();  // art_dropin.cpp
    void e32_register_files();  // engine32.cpp
#if BLITZ_MODERN
    void league_register_files();  // content_pack_tp.cpp
    void probowl_register_files();  // crew.cpp
#endif
    void uniform_sets_register(int pass);  // uniform_picker.cpp: 0 the 2026 home and away, 1 the rest
    void fs_mark(int mark);                 // art_dropin.cpp: 0 league, 1 Lite: Season, 2 every uniform set
    void e32_register_app_files();          // engine32.cpp: the credits page
#if BLITZ_MODERN
    void art_init();       // art_dropin.cpp
    void art_install_extras(uint8_t* rdram);
    bool advanced_mode();  // adv_mode.cpp
    bool original_game();  // adv_mode.cpp: Original 1999
#endif

    // libultra's osViModeTable as linked into Blitz's boot data (0x50 bytes per mode).
    constexpr int32_t vi_mode_table = (int32_t)0x8002BC50;
    // The mode Blitz itself selects on NTSC (func_8000FBAC returns 0 unless osTvType is MPAL).
    constexpr int vi_mode_ntsc_lpn1 = 0;

    void on_init(uint8_t* rdram, recomp_context* ctx) {
        (void)ctx;
        // On hardware osInitialize leaves a default VI mode in place. The runtime only installs a
        // placeholder until the game starts, and Blitz calls osViBlack before its first osViSetMode,
        // so the VI thread would read a null mode (crash in update_vi). Start with the game's own mode.
        osViSetMode(rdram, vi_mode_table + vi_mode_ntsc_lpn1 * 0x50);
#if BLITZ_MODERN
        bool original = original_game();  // fixes New Blitz / Original 1999 for this run: Original adds no art or teams
        advanced_mode();  // fixes Modern/Advanced for this run
        if (!original) {
            art_init();       // before the game's fs_init reads the asset file system
            art_install_extras(rdram);
        }
#else
        constexpr bool original = false;
#endif
        // New game files, in the order Export ROM keeps them (each kind of cart keeps the files up to its mark):
#if BLITZ_MODERN
        if (!original) league_register_files();  // a league pak's images under new names (files/added)
#endif
        fs_mark(0);
        if (!original) {
            e32_register_files();     // 32 teams on the original game (engine32.cpp, BLITZ_V32): Houston's files, VS helmets
            uniform_sets_register(0);  // each team's 2026 home and away from <app folder>/uniforms
        }
        fs_mark(1);               // Lite: Season
        if (!original) uniform_sets_register(1);  // every other uniform set
        fs_mark(2);               // Lite: Arcade, Full
        // The app's own, never on a cart:
        e32_register_app_files();  // the BlitzRecomp credits page
        glyphs_init();      // the button icons
#if BLITZ_MODERN
        if (!original) probowl_register_files();  // Pro Bowl art (tools/make_probowl_art.py)
#endif
        fs_commit_files();  // then every registered file goes into the file system, before fs_init
        dev_input_game_started();
        monitor_set_rdram(rdram);
    }
}
