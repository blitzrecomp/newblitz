// game_moment.h: a player's moment in the game and what the buttons mean then.
#include "game_moment.h"

#include <array>

extern "C" int blitz_play_mode(void);         // savestate.cpp: 0 menus, 1 play select, 2 a play
extern "C" int blitz_offense_side(void);      //   the side with the ball (0 left team, 1 right)
extern "C" int blitz_port_side(int port);     //   a controller's side, -1 not in the game

namespace {
    using blitz::GameMoment;
    using blitz::MomentText;

    // In GameMoment order.
    constexpr std::array<MomentText, 5> table{{
        {"menu", "Menus", {"OK", ""}, {"BACK", ""}, {"Z", ""}},
        {"select", "Play select", {"PICK", "play"}, {"FLIP", "play"}, {"PAGE", "next"}},
        {"play", "", {"PASS", "or switch"}, {"JUMP", "or tackle"}, {"TURBO", ""}},
        {"offense", "Offense", {"PASS", ""}, {"JUMP", "spin w/ turbo"}, {"TURBO", ""}},
        {"defense", "Defense", {"SWITCH", "player"}, {"TACKLE", "dive"}, {"TURBO", ""}},
    }};

    std::string js_string(const char* s) {
        std::string out = "\"";
        for (const char* p = s; *p; p++) {
            if (*p == '"' || *p == '\\') out += '\\';
            out += *p;
        }
        return out + "\"";
    }
}

namespace blitz {
    GameMoment moment_for(int port) {
        int mode = blitz_play_mode();
        if (mode == 1) return GameMoment::PlaySelect;
        if (mode != 2) return GameMoment::Menus;
        int side = blitz_port_side(port);
        if (side < 0) return GameMoment::Play;
        return side == blitz_offense_side() ? GameMoment::Offense : GameMoment::Defense;
    }

    const MomentText& moment_text(GameMoment m) {
        return table[(size_t)m];
    }

    std::string moment_text_js() {
        std::string out = "{\n";
        for (const MomentText& t : table) {
            auto pair = [](const ButtonText& b) { return "[" + js_string(b.label) + ", " + js_string(b.small) + "]"; };
            out += std::string("  ") + t.key + ": { mode: " + js_string(t.name) + ", pass: " + pair(t.pass) +
                   ", jump: " + pair(t.jump) + ", turbo: " + pair(t.turbo) + " },\n";
        }
        return out + "}";
    }
}
