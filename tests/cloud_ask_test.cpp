// Unit tests for "Playing on multiple devices? Turn on Cloud Sync?" (src/game/cloud_ask.h): when it's asked, setup's ask
// and 3 after updates, Don't Ask Again, and once per version. Run with `make test-cloud-ask`.
#include <cstdio>
#include <string>

#include "../src/game/cloud_ask.h"

using namespace blitz::cloud_ask;

static int failures = 0, checks = 0;
#define CHECK(cond)                                                         \
    do {                                                                    \
        checks++;                                                           \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                     \
        }                                                                   \
    } while (0)

// One start of the app at `version`: the question if it's due, answered `a`; true when it was asked.
static bool start(Record& r, const std::string& version, Answer a, bool first_run = false, bool on = false, bool available = true) {
    Moment m = due(r, available, on, first_run, version);
    if (m == Moment::None) return false;
    answered(r, a, version, m == Moment::FirstRun);
    return true;
}

int main() {
    // A new player: asked in setup (first run), No; the start straight after setup isn't an update.
    {
        Record r;
        CHECK(due(r, true, false, true, "1.0.5") == Moment::FirstRun);
        answered(r, Answer::NotNow, "1.0.5", true);
        CHECK(r.asked == 0 && !r.never && r.version == "1.0.5");  // setup's ask isn't one of the 3
        CHECK(due(r, true, false, false, "1.0.5") == Moment::None);  // welcomed now, same version
        CHECK(due(r, true, false, true, "1.0.5") == Moment::None);   // setup resumed after Exit: not again
        // Updates: asked at 1.0.6, 1.0.7 and 1.0.8 (3 after setup's), then never.
        CHECK(start(r, "1.0.6", Answer::NotNow));
        CHECK(!start(r, "1.0.6", Answer::NotNow));  // a second start of 1.0.6
        CHECK(start(r, "1.0.7", Answer::NotNow));
        CHECK(start(r, "1.0.8", Answer::NotNow));
        CHECK(r.asked == 3);
        CHECK(due(r, true, false, false, "1.0.9") == Moment::None);
        CHECK(due(r, true, false, false, "2.0.0") == Moment::None);
    }
    // A player from 1.0.4 (no record yet, set up already): the first 1.0.5 start is an update; three updates in all.
    {
        Record r;
        CHECK(due(r, true, false, false, "1.0.5") == Moment::Update);
        int asked = 0;
        for (const char* v : {"1.0.5", "1.0.6", "1.0.7", "1.0.8", "1.0.9"}) asked += start(r, v, Answer::NotNow) ? 1 : 0;
        CHECK(asked == max_asks);
        CHECK(r.asked == 3);
    }
    // Don't Ask Again: never again, whatever the version.
    {
        Record r;
        CHECK(start(r, "1.0.5", Answer::Never));
        CHECK(r.never && r.asked == 1);
        CHECK(due(r, true, false, false, "1.0.6") == Moment::None);
        CHECK(due(r, true, false, true, "1.0.6") == Moment::None);
    }
    // Yes counts as an ask (the player saw Cloud Sync's screen); turned on, it isn't asked; turned off later, it is again.
    {
        Record r;
        CHECK(start(r, "1.0.5", Answer::Yes, true));
        CHECK(r.asked == 0 && !r.never);                           // setup's ask
        CHECK(due(r, true, true, false, "1.0.6") == Moment::None);  // on
        CHECK(r.asked == 0);                                       // and not counted
        CHECK(due(r, true, false, false, "1.0.6") == Moment::Update);  // off again by 1.0.6
    }
    // Cloud Sync on at a start: not asked, the version passed (firstrun.cpp); turned off later in that version, it waits
    // for the next update.
    {
        Record r;
        CHECK(due(r, true, true, false, "1.0.5") == Moment::None);
        passed(r, "1.0.5");
        CHECK(due(r, true, false, false, "1.0.5") == Moment::None);
        CHECK(due(r, true, false, false, "1.0.6") == Moment::Update);
        CHECK(r.asked == 0);
    }
    // Not in a build without Cloud Sync (or its server), and nothing counted.
    {
        Record r;
        CHECK(due(r, false, false, true, "1.0.5") == Moment::None);
        CHECK(due(r, false, false, false, "1.0.6") == Moment::None);
        CHECK(r.asked == 0 && r.version.empty());
    }
    // Setup finished on a path without the question (made a phone's app here, PS5, N64): not an ask, and the next
    // start of the same version isn't an update; the next version's is.
    {
        Record r;
        passed(r, "1.0.5");
        CHECK(r.asked == 0);
        CHECK(due(r, true, false, false, "1.0.5") == Moment::None);
        CHECK(due(r, true, false, false, "1.0.6") == Moment::Update);
    }
    // The count never passes max_asks, even from a hand-edited record.
    {
        Record r;
        r.asked = 7;
        CHECK(due(r, true, false, false, "1.0.6") == Moment::None);
        answered(r, Answer::NotNow, "1.0.6");
        CHECK(r.asked == 7);
        Record s;
        for (int i = 0; i < 10; i++) answered(s, Answer::NotNow, "v" + std::to_string(i));
        CHECK(s.asked == max_asks);
    }
    if (failures) {
        fprintf(stderr, "cloud_ask: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("cloud_ask: ok %d\n", checks);
    return 0;
}
