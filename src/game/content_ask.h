// "Keep rosters, uniforms and new paks up to date?" (1.0.6, the team 2026-11-27): asked once of a player who updates with
// Settings → Teams → Keep Rosters and Uniforms Up to Date saved as off (a public 1.0.5 that never ticked it), since that
// setting now also brings the posted paks (season_pak.cpp). The rule alone, with nothing of the app in it, so
// tests/content_ask_test.cpp can check it; firstrun.cpp shows the question (on the launcher, before the game, never over
// one) and keeps the record in General's hidden content_ask_done.
//
// The rule:
//   - only for a player who's set up already (welcomed): a fresh install's setup asks the same in Updates and reports,
//     so finishing setup settles it (passed);
//   - only while the setting is off, and only where there's a server to bring paks;
//   - once per app, not per version: Yes (the setting on, a sync now) or No (left off) settles it for good, and so does
//     any start that finds the setting on (passed), so a player who turns it off later isn't asked;
//   - not stacked with Cloud Sync's "Playing on multiple devices?" (cloud_ask.h): when both are due at one start, this
//     one goes first and Cloud Sync's comes after it, its own screen (one after the other, never two at once).
#pragma once

namespace blitz::content_ask {
    struct Record {
        bool done = false;  // answered, or passed (setup asked it, or the setting was on at a start)
    };

    enum class Answer { Yes, No };

    // Whether to ask now. welcomed: set up already; on: the setting is on; server: this build has a pak server.
    inline bool due(const Record& r, bool welcomed, bool on, bool server) {
        return !r.done && welcomed && !on && server;
    }

    // A start that won't ask, and why it never needs to: set up with the setting on. (Not before setup: setup's own
    // question decides, then setup_finished.)
    inline bool passes(const Record& r, bool welcomed, bool on) { return !r.done && welcomed && on; }
    inline void passed(Record& r) { r.done = true; }

    // Setup finished (a fresh install): its Updates and reports asked it.
    inline void setup_finished(Record& r) { r.done = true; }

    // Yes or No: settled for good. The setting the answer leaves.
    inline bool answered(Record& r, Answer a) {
        r.done = true;
        return a == Answer::Yes;
    }

    // Cloud Sync's question at the same start: after this one, never beside it.
    inline bool cloud_waits(const Record& r, bool welcomed, bool on, bool server) { return due(r, welcomed, on, server); }
}
