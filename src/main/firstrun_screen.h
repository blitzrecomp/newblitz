// One screen of first-run setup and Blitz Builder (docs/design/FIRST_RUN_V2.md §3): the eyebrow with the step, a
// title, the body, then any of a QR code, check boxes and a long scrolling text, then the buttons, and Back and Exit
// at the bottom with their button hints. The framework's two-button prompt has no Back, no Exit and no check boxes,
// so setup draws its own (firstrun.cpp decides what goes on it).
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace blitz::firstrun_ui {
    struct Action {
        std::string label;
        std::function<void()> pressed;
    };

    struct Check {
        std::string label;
        std::string about;                 // the smaller line under it
        std::function<bool()> get;
        std::function<void(bool)> set;     // called on each flip
    };

    struct Screen {
        std::string eyebrow;               // "SETUP · STEP 2 OF 5"
        std::string title;
        std::string body;                  // \n starts a new line
        std::string qr_url;                // a QR code for this address, with caption under it
        std::string qr_caption;
        std::vector<Check> checks;
        std::vector<std::string> long_text;  // blocks of text in a scrolling box (each block focusable, for the D-pad)
        std::string note;                  // a small line in the accent colour (offline, a count of wrong tries)
        std::vector<Action> actions;       // the first is the default: big; focused when the screen opens
        int focus = 0;                     // unless an earlier answer is (Back keeps what was chosen)
        std::function<void()> back;        // Back: the on-screen button, the cancel key; none on a first screen
        bool back_button = true;           // false: the cancel key only (a Cancel among the actions)
        std::function<void()> exit;        // Exit: leaves setup; always there for setup's screens
        std::string exit_label = "Exit";
        // The Saves screens (save_sync.cpp): text and buttons sized for a phone held at arm's length (large_scale()), a
        // wider card, the buttons two to a row where there's room, and the whole card scrolls if it's still too tall.
        bool large = false;
        std::string big;                   // one huge line under the body (a pairing number, "482 915")
        std::vector<Action> pad;           // a number pad, three keys to a row, beside the body (below it when narrow)
        std::function<void(char)> typed;   // a keyboard's digit keys ('0'..'9') while this screen is up
    };

    // How much bigger a large screen's text and buttons are: about 1.9 on a phone (iPhone landscape: body text ~16 pt),
    // 1.3 on a computer. BLITZ_PHONE_UI=1 shows a phone's sizes on a computer.
    float large_scale();

    // Shows this screen (in place of the one up). Call from the main thread's UI tick, never from a button's own
    // callback: that rebuilds the button it's running in.
    void show(const Screen& screen);
    bool update_note(const std::string& text);  // the note's words only, no redraw (a countdown)
    void hide();
    bool shown();
    // Every UI tick (firstrun.cpp): Esc as Back, which the framework keeps for the game's menu.
    void update();
}
