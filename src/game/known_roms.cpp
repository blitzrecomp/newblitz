// The ROMs we know by name (XXH3-64 of the whole 16 MB image, big-endian .z64 order), so the launcher and the
// roster question can say "NFL Blitz 2026 Gold Edition (Resurrection)" instead of a file name. Unknown ROMs
// still play; they just have no name here.
#include <atomic>
#include <cstdint>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#define XXH_INLINE_ALL
#include "xxHash/xxhash.h"

#include "librecomp/game.hpp"
#include "blitz_config.h"

namespace blitz {
    std::atomic<int> stored_rom_changes{0};  // bumped by Load ROM / Change ROM, so the launcher re-reads the name

    std::string rom_display_name(std::span<const uint8_t> rom) {
        struct Known { uint64_t hash; const char* name; };
        static const Known known[] = {
            {0x45e521f9499a7ba5ULL, "NFL Blitz 2000 (USA v1.1)"},
            {0xaf7e4ea8ee47bbb3ULL, "Team Extension (Zoinkity)"},
            {0x40a5f3c08af323e9ULL, "NFL Blitz 2025 (Resurrection)"},
            {0xd22256ac074d92dcULL, "NFL Blitz 2025 v2 (Resurrection)"},
            {0x335e4ab3ab5b5e81ULL, "NFL Blitz 2026 (Resurrection)"},
            {0x3579fa5adf0b0bbcULL, "NFL Blitz 2026 v2 (Resurrection)"},
            {0xbd3992f6c0181fd2ULL, "NFL Blitz 2026 Gold Edition (Resurrection)"},
            {0x05882816608eb203ULL, "NFL Blitz Silver Special Edition (Resurrection)"},
        };
        if (rom.empty()) return {};
        uint64_t h = XXH3_64bits(rom.data(), rom.size());
        for (const Known& k : known) {
            if (k.hash == h) return k.name;
        }
        return {};
    }

    // The stored game ROM (<app folder>/<game id>.z64) by name; "" when there's none or it isn't one we know.
    std::string stored_rom_display_name() {
        std::ifstream f(recomp::get_config_path() / (game_id + u8".z64"), std::ios::binary);
        if (!f) return {};
        std::vector<uint8_t> rom{std::istreambuf_iterator<char>(f), {}};
        return rom_display_name(rom);
    }
}
