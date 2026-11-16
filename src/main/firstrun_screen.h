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
    };

    // Shows this screen (in place of the one up). Call from the main thread's UI tick, never from a button's own
    // callback: that rebuilds the button it's running in.
    void show(const Screen& screen);
    void hide();
    bool shown();
    // Every UI tick (firstrun.cpp): Esc as Back, which the framework keeps for the game's menu.
    void update();
}
