// What's happening for a player, and what the face buttons mean then: one table for the on-screen touch controls
// (touch_controls.cpp), the phone controllers' page (phone_pads.cpp, phone_page.h) and the hints (hints.cpp), so a
// change to a label or a moment reaches all three.
#pragma once
#include <string>

namespace blitz {
    enum class GameMoment { Menus, PlaySelect, Play, Offense, Defense };

    // A controller's moment (port 0-3): menus (team select too), play select, or a play, on offense or defense by
    // its team (Play when the port isn't in the game).
    GameMoment moment_for(int port);

    struct ButtonText {
        const char* label;
        const char* small;  // the phone page's small print under the label ("" none)
    };
    struct MomentText {
        const char* key;   // the phone page's mode name ("menu", "select", ...)
        const char* name;  // shown on the phone page ("Play select"; "" none)
        ButtonText pass, jump, turbo;  // N64 A, B, Z
    };
    const MomentText& moment_text(GameMoment m);

    // The table as a JavaScript object literal, {menu: {mode: ..., pass: [label, small], ...}, ...}: the phone page's
    // LABELS, put in where the page says __LABELS__.
    std::string moment_text_js();
}
