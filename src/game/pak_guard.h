// Controller Pak images, read as the libultra file system lays them out (no game or runtime needed, so
// tests/pak_guard_test.cpp runs it on its own). Used by controller_pak.cpp for three things:
//   - the stub guard: a block write that would clear or change an Advanced season stub ("BZ26ADV" in a
//     season slot of a note) keeps the stub's bytes as they are on disk, unless the write is the player's own
//     Save to that slot (adv_save.cpp says so around the game's write). A deleted note has no stubs left to keep.
//   - the history: dated copies of the image in pak_history/, which ones to keep;
//   - what an image holds, in a few words, for Settings → General → Controller Pak Backups.
#pragma once
#include <cstdint>
#include <ctime>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace blitz::pakimg {
    constexpr size_t pak_size = 0x8000;

    // One note (a game's save file on the pak): its directory entry and the pak offset of each 256-byte page.
    struct Note {
        int entry = -1;                // 0-15 in the directory (pages 3-4)
        uint32_t game_code = 0;
        uint16_t company_code = 0;
        std::string name;              // the note's name, decoded from the pak's character set
        std::vector<uint32_t> pages;   // pak offset of file page 0, 1, ...
        uint32_t pak_offset(uint32_t file_offset) const { return pages[file_offset >> 8] + (file_offset & 0xFF); }
        uint32_t size() const { return (uint32_t)pages.size() * 256; }
    };

    // An Advanced season stub in a note's season slot (adv_save.cpp).
    struct Stub {
        int note = -1;                 // index into notes()
        int slot = -1;                 // 0-3
        uint64_t id = 0;
        uint32_t gen = 0;
        uint8_t team = 0, week = 0;    // display team index, week (0-based; the game shows week + 1)
        uint32_t crc = 0;              // the season file's CRC, which the file must match (adv_save.cpp load_sidecar)
        uint32_t pak[0x20] = {};       // pak offset of each of the stub's 32 bytes (a slot can cross a page)
    };

    constexpr uint32_t slot_file_offset = 0xF00, slot_size = 0x774, slot_count = 4;
    constexpr uint32_t stub_bytes = 0x20;

    std::vector<Note> notes(const uint8_t* image);
    std::vector<Stub> stubs(const uint8_t* image);
    std::set<std::pair<uint64_t, uint32_t>> stub_refs(const uint8_t* image);  // (season id, generation)

    // The guard. `block` is the 32-byte block being written (address = block number), `data` its new bytes, which
    // are changed in place so that every stub on `image` (the pak as it is now) keeps its bytes. A stub in
    // `allowed_slot` (the slot a season Save is writing; -1 none) may change, and so may a stub in `released` (its
    // note was deleted this session: see released_by). Returns the stubs that were kept.
    std::vector<Stub> guard_block(const uint8_t* image, uint32_t block, uint8_t* data, int allowed_slot,
                                  const std::set<std::pair<uint64_t, uint32_t>>& released = {});
    // After a block of the directory or inode table (pages 0-4) is written: the stubs `before` had that `after`
    // doesn't (their note was deleted, as the Controller Pak manager does). A new note made over it is then free
    // to start empty.
    std::set<std::pair<uint64_t, uint32_t>> released_by(const uint8_t* before, const uint8_t* after);
    constexpr uint32_t system_blocks = 0x500 / 32;

    // History file names: "<stem>-YYYYMMDD-HHMMSS.mpk" (local time). parse_time gives 0 for any other name.
    std::string history_name(const std::string& stem, std::time_t when);
    std::time_t parse_time(const std::string& name, const std::string& stem);
    // Which history files to delete: the newest `keep_last` stay, and the newest of each of the last `days`
    // calendar days (local time, counting today). `names` in any order; returns the names to delete.
    std::vector<std::string> history_prune(const std::vector<std::string>& names, const std::string& stem, std::time_t now,
                                           size_t keep_last = 20, int days = 14);

    // What an image holds, short: "LAR, week 6" for a season stub (franchise from `team_name`, which may return ""),
    // "1 season", "No saves". `team_name(id, gen, team)` looks the team up in the season's own file.
    template <typename F>
    std::string describe(const uint8_t* image, F team_name);
    std::string describe_plain(const uint8_t* image);  // no team names
    std::string note_name(const uint8_t* name16);
}

// ---- describe (a template: the caller says how a stub's team is named) --------------------------------------------
namespace blitz::pakimg {
    std::string describe_rest(const uint8_t* image, const std::vector<Stub>& stubs, std::vector<std::string> parts);

    template <typename F>
    std::string describe(const uint8_t* image, F team_name) {
        std::vector<Stub> st = stubs(image);
        std::vector<std::string> parts;
        for (const Stub& s : st) {
            std::string team = team_name(s.id, s.gen, s.team);
            // Advanced: weeks 0-17 regular, 18-21 the playoffs, 22 when the Super Bowl is played.
            std::string week = s.week >= 22 ? "season over" : s.week >= 18 ? "playoffs" : "week " + std::to_string(s.week + 1);
            parts.push_back(team.empty() ? "Season, " + week : team + ", " + week);
        }
        return describe_rest(image, st, std::move(parts));
    }
}
