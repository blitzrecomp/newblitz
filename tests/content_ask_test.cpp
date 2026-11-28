// Unit tests for "Keep rosters, uniforms and new paks up to date?" (src/game/content_ask.h): asked once, only of a
// player updating with the setting off, never on a fresh install, never beside Cloud Sync's question.
// Run with `make test-content-ask`.
#include <cstdio>

#include "../src/game/content_ask.h"

using namespace blitz::content_ask;

static int failures = 0, checks = 0;
#define CHECK(cond)                                                         \
    do {                                                                    \
        checks++;                                                           \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                     \
        }                                                                   \
    } while (0)

// One start, as firstrun.cpp does it: settled when the setting is on, else asked when due. True when it was asked.
static bool start(Record& r, bool welcomed, bool& on, Answer a, bool server = true) {
    if (passes(r, welcomed, on)) passed(r);
    if (!due(r, welcomed, on, server)) return false;
    on = answered(r, a);
    return true;
}

int main() {
    // A 1.0.5 player with the setting off updates: asked once. Yes turns it on; never asked again.
    {
        Record r;
        bool on = false;
        CHECK(cloud_waits(r, true, on, true));  // Cloud Sync's question waits its turn
        CHECK(start(r, true, on, Answer::Yes));
        CHECK(on && r.done);
        CHECK(!cloud_waits(r, true, on, true));
        on = false;  // turned off later in Settings
        CHECK(!start(r, true, on, Answer::Yes));
        CHECK(!start(r, true, on, Answer::Yes));
    }
    // No leaves it off, for good (not per version).
    {
        Record r;
        bool on = false;
        CHECK(start(r, true, on, Answer::No));
        CHECK(!on && r.done);
        CHECK(!start(r, true, on, Answer::No));  // the next start
        CHECK(!start(r, true, on, Answer::No));  // a later update
    }
    // A player with the setting on at the first 1.0.6 start: never asked, even after turning it off.
    {
        Record r;
        bool on = true;
        CHECK(!start(r, true, on, Answer::Yes));
        CHECK(r.done);
        on = false;
        CHECK(!start(r, true, on, Answer::Yes));
    }
    // A fresh install: not before setup (setup asks), and setup finishing settles it whatever was chosen.
    {
        Record r;
        bool on = true;  // the default
        CHECK(!due(r, false, false, true));  // setup isn't done: never this question
        CHECK(!passes(r, false, on));
        setup_finished(r);
        on = false;  // the player unticked it in setup
        CHECK(!start(r, true, on, Answer::Yes));
        CHECK(!cloud_waits(r, true, on, true));
    }
    // No server in this build: nothing to bring, so nothing asked (and it stays open for a build that has one).
    {
        Record r;
        bool on = false;
        CHECK(!start(r, true, on, Answer::Yes, false));
        CHECK(!r.done);
        CHECK(!cloud_waits(r, true, on, false));
        CHECK(start(r, true, on, Answer::No, true));
    }
    printf("content_ask: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
