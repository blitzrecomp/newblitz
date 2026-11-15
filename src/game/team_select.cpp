// 2026 app: the team select screen lists teams alphabetically by their (2026) city, then select name.
//
// team+ lists teams in block order: the 1999 cities plus Houston at the end, so Las Vegas, the two
// Los Angeles teams and Houston are out of place. The blocks can't move in Modern (team+'s Season
// assigns divisions by slot), so only the screen changes (hooks from tools/teamplus.py TEAMSEL):
// UP/DOWN steps to the next team in name order, and the rows above and below the cursor show the
// neighbours in that order. The cursor, the choice the game stores and the CPU's picks stay real
// slots. In Advanced the blocks are already in this order, so nothing changes there.
#if BLITZ_TEAMPLUS
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>

#include "recomp.h"
#include "team_store.h"

namespace {
    // Case-insensitive order of two names (the game's 16-byte fields).
    int compare(const std::string& a, const std::string& b) {
        for (size_t i = 0; i < 16; i++) {
            int x = i < a.size() ? tolower((uint8_t)a[i]) : 0;
            int y = i < b.size() ? tolower((uint8_t)b[i]) : 0;
            if (x != y) return x - y;
            if (x == 0) return 0;
        }
        return 0;
    }
}

extern "C" {
    // The slot after (dir 1) or before (dir -1) `slot` in name order, wrapping.
    int32_t blitz_teamsel_step(uint8_t* rdram, int32_t slot, int32_t dir, int32_t count) {
        if (count < 1 || count > 64) return slot + dir;
        if (slot < 0 || slot >= count) return ((slot + dir) % count + count) % count;
        int32_t order[64];
        for (int i = 0; i < count; i++) order[i] = i;
        std::stable_sort(order, order + count, [&](int32_t x, int32_t y) {
            namespace live = blitz::teamdata::live;
            int c = compare(live::city(rdram, x), live::city(rdram, y));
            return (c != 0 ? c : compare(live::list_name(rdram, x), live::list_name(rdram, y))) < 0;
        });
        int p = int(std::find(order, order + count, slot) - order);
        return order[((p + dir) % count + count) % count];
    }

    // ovl_b 0x802F9B60: the game has just moved s6 by +-1 for direction code `code` (its jump table
    // at 0x803391A0); redo the move in name order. The game's wrap that follows then does nothing.
    int32_t blitz_teamsel_moved(uint8_t* rdram, int32_t s6, int32_t code, int32_t count) {
        static const int8_t step_of[11] = {0, -1, 1, 0, -1, -1, 1, 0, 1, -1, 1};
        int d = (code >= 1 && code <= 10) ? step_of[code] : 0;
        return d != 0 ? blitz_teamsel_step(rdram, s6 - d, d, count) : s6;
    }
}
#endif
