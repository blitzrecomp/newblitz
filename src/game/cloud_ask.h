// "Playing on multiple devices? Turn on Cloud Sync?" (docs/SAVE_SYNC.md §7.9): when it's asked, and what each answer
// leaves behind. This header is the rule alone, with nothing of the app in it, so tests/cloud_ask_test.cpp can check it;
// firstrun.cpp shows the question and keeps the record in General's hidden cloud_ask_count, cloud_ask_never and
// cloud_ask_version (general.json: one record per app, whichever game mode is chosen).
//
// The rule (the owner's, 2026-11-27):
//   - asked in the first run's setup, and at the first start after an update (a new BLITZ_VERSION: rebuilds of one
//     version are no update), up to max_asks times after updates: setup's ask doesn't count ("during onboarding and
//     3x after every update", the team 2026-11-27);
//   - only while Cloud Sync is off, the build has it (BLITZ_SAVE_SYNC_NET and a server address), and the player hasn't
//     chosen Don't Ask Again;
//   - at most once per version: an answer (or Exit, which counts as Not Now) records the version, so a setup resumed or
//     a second start of the same version doesn't ask again.
#pragma once

#include <string>

namespace blitz::cloud_ask {
    constexpr int max_asks = 3;  // after updates; setup's own ask is extra

    struct Record {
        int asked = 0;          // times shown after an update and answered (Yes, Not Now, Don't Ask Again or Exit)
        bool never = false;     // Don't Ask Again
        std::string version;    // the app version it was last asked at, or setup finished without it; "" never
    };

    enum class Moment { None, FirstRun, Update };
    enum class Answer { Yes, NotNow, Never };

    // Whether to ask now. available: the build has Cloud Sync (and a server); on: Cloud Sync is on on this device;
    // first_run: setup isn't finished (welcomed false); version: this app's BLITZ_VERSION.
    inline Moment due(const Record& r, bool available, bool on, bool first_run, const std::string& version) {
        if (!available || on || r.never || r.asked >= max_asks) return Moment::None;
        if (r.version == version) return Moment::None;  // asked (or passed) at this version already
        return first_run ? Moment::FirstRun : Moment::Update;
    }

    // The player answered (Exit counts as NotNow). Setup's ask (first_run) isn't counted.
    inline void answered(Record& r, Answer a, const std::string& version, bool first_run = false) {
        if (!first_run && r.asked < max_asks) r.asked++;
        if (a == Answer::Never) r.never = true;
        r.version = version;
    }

    // Setup finished without the question (a path that doesn't play here, or Cloud Sync already on): this version
    // counts as seen, so the start straight after setup isn't taken for an update. Not an ask.
    inline void passed(Record& r, const std::string& version) { r.version = version; }
}
