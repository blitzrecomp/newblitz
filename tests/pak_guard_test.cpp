// Unit tests for the Controller Pak stub guard and history (src/game/pak_guard.cpp). Run with `make test-pak`.
// Optional: `build/tests/pak_guard_test <good.mpk> <wiped.mpk>` replays the 2026-11-26 wipe (the good image, then
// every block of the wiped one written over it) and checks the season stub survives.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

#include "../src/game/pak_guard.h"

using namespace blitz::pakimg;

static int failures = 0;
#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                     \
        }                                                                   \
    } while (0)

// A pak with one Blitz note on pages 5..127 (as the game makes it), its slots empty.
static std::vector<uint8_t> blitz_pak() {
    std::vector<uint8_t> p(pak_size, 0);
    for (int page = 5; page < 128; page++) {
        uint16_t next = page == 127 ? 1 : page + 1;
        p[0x100 + 2 * page] = next >> 8;
        p[0x100 + 2 * page + 1] = next & 0xFF;
    }
    const uint8_t dir[32] = {0x4E, 0x42, 0x49, 0x45, 0x35, 0x44, 0x00, 0x05, 0x02, 0, 0, 0, 0, 0, 0, 0,
                             0x1B, 0x25, 0x22, 0x2D, 0x33, 0x0F, 0x12, 0x10, 0x10, 0x10, 0, 0, 0, 0, 0, 0};
    std::memcpy(&p[0x300], dir, 32);
    for (uint32_t k = 0; k < 4; k++) p[0x500 + 0xF00 + k * 0x774] = p[0x500 + 0xF00 + k * 0x774 + 1] = 0xFF;
    return p;
}

static void put_stub(std::vector<uint8_t>& p, int slot, uint64_t id, uint32_t gen, uint8_t team, uint8_t week) {
    uint32_t s = 0x500 + 0xF00 + slot * 0x774;
    std::memset(&p[s], 0, 0x20);
    p[s] = p[s + 1] = 0xFF;
    std::memcpy(&p[s + 4], "BZ26ADV", 8);
    for (int i = 0; i < 8; i++) p[s + 0xC + i] = (uint8_t)(id >> (8 * i));
    for (int i = 0; i < 4; i++) p[s + 0x14 + i] = (uint8_t)(gen >> (8 * i));
    p[s + 0x18] = team;
    p[s + 0x19] = week;
    p[s + 0x1C] = 0xA5;
}

// Writes `next` over `disk` block by block through the guard, as controller_pak.cpp does.
static std::set<std::pair<uint64_t, uint32_t>> released;
static int write_through(std::vector<uint8_t>& disk, const std::vector<uint8_t>& next, int allowed) {
    int kept = 0;
    for (uint32_t b = 0; b < pak_size / 32; b++) {
        uint8_t data[32];
        std::memcpy(data, &next[b * 32], 32);
        kept += (int)guard_block(disk.data(), b, data, allowed, released).size();
        std::vector<uint8_t> before;
        if (b < system_blocks) before = disk;
        std::memcpy(&disk[b * 32], data, 32);
        if (b < system_blocks) {
            for (const auto& r : released_by(before.data(), disk.data())) released.insert(r);
        }
    }
    return kept;
}

static std::vector<uint8_t> read(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>{std::istreambuf_iterator<char>(f), {}};
}

int main(int argc, char** argv) {
    // Reading: the note, its name, the stubs (slot 2's crosses a page).
    {
        auto p = blitz_pak();
        put_stub(p, 0, 0x0EE109E6D63E3BF3ull, 5, 27, 5);
        put_stub(p, 2, 0x1234, 9, 3, 19);
        auto ns = notes(p.data());
        CHECK(ns.size() == 1);
        CHECK(ns[0].name == "BLITZ 2000");
        CHECK(ns[0].pages.size() == 123);
        auto st = stubs(p.data());
        CHECK(st.size() == 2);
        CHECK(st[0].slot == 0 && st[0].id == 0x0EE109E6D63E3BF3ull && st[0].gen == 5 && st[0].team == 27 && st[0].week == 5);
        CHECK(st[1].slot == 2 && st[1].pak[0] + 0x1F == st[1].pak[0x1F]);
        CHECK(stub_refs(p.data()).count({0x1234, 9}) == 1);
        std::string d = describe(p.data(), [](uint64_t id, uint32_t, uint8_t) { return id == 0x1234 ? std::string("NYG") : std::string(); });
        CHECK(d == "Season, week 6; NYG, playoffs");
        CHECK(describe_plain(blitz_pak().data()) == "No seasons");
        std::vector<uint8_t> blank(pak_size, 0);
        CHECK(describe_plain(blank.data()) == "Empty pak");
        auto c = blitz_pak();
        c[0x500 + 0xF00 + 0x774] = 4;  // a Classic season in slot 1
        CHECK(describe_plain(c.data()) == "1 season");
    }
    // The guard: a write that empties slot 0 keeps the stub; the rest of the write goes through.
    {
        auto disk = blitz_pak();
        put_stub(disk, 0, 77, 5, 27, 5);
        auto next = blitz_pak();       // the wiped image: slot 0 empty again
        next[0x520] = 0x42;            // and a change elsewhere (a record)
        next[0x7FC0] = 0x99;           // and in the tail
        CHECK(write_through(disk, next, -1) == 1);
        auto st = stubs(disk.data());
        CHECK(st.size() == 1 && st[0].id == 77 && st[0].gen == 5);
        CHECK(disk[0x520] == 0x42 && disk[0x7FC0] == 0x99);
    }
    // The player's own Save to that slot may replace it; a write to another slot may not touch it.
    {
        auto disk = blitz_pak();
        put_stub(disk, 0, 77, 5, 27, 5);
        put_stub(disk, 1, 88, 1, 2, 0);
        auto next = disk;
        put_stub(next, 0, 77, 6, 27, 6);   // the Save: gen 6 in slot 0
        next[0x500 + 0xF00 + 0x774 + 4] = 0;  // and slot 1's stub damaged
        CHECK(write_through(disk, next, 0) == 1);
        auto st = stubs(disk.data());
        CHECK(st.size() == 2 && st[0].slot == 0 && st[0].gen == 6 && st[1].slot == 1 && st[1].id == 88);
    }
    // A stub that crosses a block (slot 2) is kept whole.
    {
        auto disk = blitz_pak();
        put_stub(disk, 2, 5, 3, 1, 1);
        auto next = blitz_pak();
        CHECK(write_through(disk, next, -1) >= 1);
        CHECK(stubs(disk.data()).size() == 1);
    }
    // A deleted note (directory entry cleared, as the Controller Pak manager does) has no stubs left to keep: a
    // new note can be made over it.
    {
        released.clear();
        auto disk = blitz_pak();
        put_stub(disk, 0, 77, 5, 27, 5);
        auto deleted = disk;
        std::memset(&deleted[0x300], 0, 32);
        write_through(disk, deleted, -1);
        CHECK(stubs(disk.data()).empty());
        auto fresh = blitz_pak();
        CHECK(write_through(disk, fresh, -1) == 0);
        CHECK(disk == fresh);
        released.clear();
    }
    // No note, no stubs: other games' paks and Classic writes go through untouched.
    {
        std::vector<uint8_t> disk(pak_size, 0), next(pak_size, 0x5A);
        CHECK(write_through(disk, next, -1) == 0);
        CHECK(disk == next);
    }
    // History names and pruning.
    {
        std::time_t now = std::time(nullptr);
        std::string n = history_name("blitz2000_pak1", now);
        CHECK(parse_time(n, "blitz2000_pak1") != 0);
        CHECK(std::llabs((long long)(parse_time(n, "blitz2000_pak1") - now)) <= 1);
        CHECK(parse_time("other-20261126-135310.mpk", "blitz2000_pak1") == 0);
        CHECK(parse_time("blitz2000_pak1-20261126-135310-2.mpk", "blitz2000_pak1") != 0);
        // 40 changes today: the newest 20 stay.
        std::vector<std::string> names;
        for (int i = 0; i < 40; i++) names.push_back(history_name("p", now - i * 60));
        auto gone = history_prune(names, "p", now);
        CHECK(gone.size() == 20);
        for (const auto& g : gone) CHECK(parse_time(g, "p") <= now - 20 * 60);
        // 30 days, 3 a day: the newest 20, then one a day back to 14 days, nothing older.
        names.clear();
        for (int d = 0; d < 30; d++)
            for (int k = 0; k < 3; k++) names.push_back(history_name("p", now - d * 86400 - k * 600));
        gone = history_prune(names, "p", now);
        size_t kept = names.size() - gone.size();
        CHECK(kept >= 20 && kept <= 20 + 14);
        bool old_kept = false;
        for (const auto& nm : names) {
            bool removed = std::find(gone.begin(), gone.end(), nm) != gone.end();
            if (parse_time(nm, "p") < now - 15 * 86400 && !removed) old_kept = true;
        }
        CHECK(!old_kept);
        CHECK(history_prune({"p-junk.mpk"}, "p", now).empty());
    }
    // The real thing, when given: the good image, then the wiped one written over it.
    if (argc >= 3) {
        auto good = read(argv[1]), wiped = read(argv[2]);
        CHECK(good.size() == pak_size && wiped.size() == pak_size);
        if (good.size() == pak_size && wiped.size() == pak_size) {
            auto before = stubs(good.data());
            printf("good: %s\nwiped: %s\n", describe_plain(good.data()).c_str(), describe_plain(wiped.data()).c_str());
            CHECK(before.size() == 1);
            CHECK(stubs(wiped.data()).empty());
            auto disk = good;
            int kept = write_through(disk, wiped, -1);
            auto after = stubs(disk.data());
            CHECK(kept >= 1);
            CHECK(after.size() == 1 && after[0].id == before[0].id && after[0].gen == before[0].gen);
            // Everything but the stub is the wiped write's.
            int other = 0;
            for (size_t i = 0; i < pak_size; i++) other += disk[i] != wiped[i];
            CHECK(other > 0 && other <= 0x20);
            printf("replayed the wipe: stub kept (%d bytes differ from the write)\n", other);
        }
    }
    if (failures) {
        fprintf(stderr, "pak_guard: %d failure(s)\n", failures);
        return 1;
    }
    printf("pak_guard: all passed\n");
    return 0;
}
