// League structure and schedule for Advanced seasons, from the content pack's "season" section
// (packs/README.md; parsed in content_pack_tp.cpp, used by adv_season.cpp). 2026 flavour only.
#pragma once
#include <array>
#include <string>
#include <utility>
#include <vector>

namespace blitz {
    struct SeasonData {
        static constexpr int division_count = 8, division_size = 4, max_weeks = 18, max_games = 16;
        int year = 0;
        // AFC East, North, South, West, then the NFC's: franchise ids.
        std::array<std::string, division_count> division_names;
        std::array<std::array<std::string, division_size>, division_count> divisions;
        // weeks[w] = {away, home} franchise pairs.
        std::vector<std::vector<std::pair<std::string, std::string>>> weeks;
    };

    // The active pack's season, or nullptr (no pack, or a pack without one).
    const SeasonData* pack_season();
}
