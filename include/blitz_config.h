#ifndef __BLITZ_CONFIG_H__
#define __BLITZ_CONFIG_H__

#include <cstdint>
#include <string>

#include "blitz_platform.h"

namespace blitz {
#if BLITZ_TEAMPLUS
    // The 2026 flavour (docs/BLITZ2026.md): its own app folder, saves and stored ROM.
    // BRANDING: program_id names the user's app folder (saves); renaming moves them
    inline const std::u8string program_id = u8"Blitz2026Recompiled";
    // BRANDING: window title and macOS app menu (main.cpp); see docs/RELEASE_SWITCHES.md
    inline const std::string program_name = "BlitzRecomp: Modern Blitz";
    inline const std::u8string game_id = u8"blitz2000.teamplus.n64.us";
    inline const std::string mod_game_id = "blitz2026";
    inline const std::string display_name = "NFL Blitz 2000 + Team Extension";
    // XXH3-64 of Zoinkity's Team Extension v2.0 applied to v1.1 (config/teamplus.json). ROMs with the
    // same code but other data (e.g. community roster updates) are accepted by a code-region check.
    constexpr uint64_t rom_hash = 0xAF7E4EA8EE47BBB3ULL;
    constexpr bool teamplus = true;
#elif BLITZ_ENGINE_ORIGINAL
    // Modern Blitz on the original v1.1 game (no Team Extension; engine32.cpp): Modern's app folder and settings,
    // the plain ROM stored under its own name.
    // BRANDING: program_id names the user's app folder (saves); renaming moves them
    inline const std::u8string program_id = u8"Blitz2026Recompiled";
    // BRANDING: window title and macOS app menu (main.cpp); see docs/RELEASE_SWITCHES.md
    inline const std::string program_name = "New Blitz 2027";
    inline const std::u8string game_id = u8"blitz2000.n64.us.1.1";
    inline const std::string mod_game_id = "blitz2026";
    inline const std::string display_name = "NFL Blitz 2000";
    constexpr uint64_t rom_hash = 0x45E521F9499A7BA5ULL;
    constexpr bool teamplus = false;
#else
    // BRANDING: program_id names the user's app folder (saves); renaming moves them
    inline const std::u8string program_id = u8"BlitzRecompiled";
    // BRANDING: window title and macOS app menu (main.cpp); see docs/RELEASE_SWITCHES.md
    inline const std::string program_name = "BlitzRecomp: Classic Blitz";

    // Game identity. game_id names the stored ROM (<app folder>/<game_id>.z64);
    // mod_game_id is what `--game` takes on the command line.
    inline const std::u8string game_id = u8"blitz2000.n64.us.1.1";
    inline const std::string mod_game_id = "blitz2000";
    inline const std::string display_name = "NFL Blitz 2000";

    // XXH3-64 of the big-endian v1.1 ROM (SHA-1 3754c8fdf5cdce5de62685ce9be73e06ba5e3577),
    // which is how the runtime validates the ROM the user picks.
    constexpr uint64_t rom_hash = 0x45E521F9499A7BA5ULL;
    constexpr bool teamplus = false;
#endif

    // Per-platform features: include/blitz_platform.h (the one table of what each device has).
    constexpr bool export_available = platform::export_rom;
    constexpr bool restart_available = platform::restart;

    void init_config();
}

#endif
