// The team store (Modern Blitz): every franchise's team data on the app side, with one writer that puts it into the
// game. Content packs, the weekly lineups and saved crews edit the store; only the writer knows the game's tables.
//
// A team is a typed record (players, uniform and art file ids, ratings, names, CPU plays), not any engine's layout.
// team_store.cpp encodes it into team+'s blocks (one 0x220-byte block per team, docs/BLITZ2026.md) and is the only
// place that layout is known, with the live calls below. A writer for the original game's tables
// (docs/NO_TEAMPLUS_PATH.md) can encode the same record, so features never touch an engine's layout directly.
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace blitz::teamdata {
    constexpr int players_per_team = 16;

    class Team {
    public:
        std::string franchise;  // "ARI" ... "WAS"

        std::string nickname() const;
        void set_nickname(const std::string& s);
        void set_city(const std::string& s);
        void set_select(const std::string& s);  // the team select list's name (upper case)
        void set_abbr(const std::string& s);
        void set_rating(int k, int v);           // passing, rushing, linemen, defense, special teams: 0-5
        void set_vs_pause(int ticks);            // the VS screen's wait after this team's full name
        // A community ROM's art for this team (art_dropin.cpp): its 20 uniform part ids, 4 menu images and 6 in-game
        // images, as that ROM's block holds them (u16, big-endian, in that order).
        void set_art(const std::vector<uint8_t>& ids);
        uint16_t uniform_part(int part, int side) const;  // part 0-9, side 0 home / 1 away: a file id
        void set_uniform_part(int part, int side, uint16_t id);
        void set_cpu_skill(int skill);
        void set_vs_helmet_right(uint16_t id);  // its own right-side VS helmet (else the left one mirrored)

        std::string last(int slot) const;
        void set_last(int slot, const std::string& s);
        void set_number(int slot, int n);        // 0-99
        void set_scale(int slot, float size);
        void set_skin(int slot, int shade);
        void set_heavy(int slot, int heavy);
        void clear_call(int slot);               // the announcer doesn't know this name

        // The data itself: what a team is, in no engine's layout. Text fields keep their exact bytes (16, or 4 for the
        // code) so an untouched team writes back as it was loaded.
        struct Player {
            uint32_t scale = 0;          // f32 bits: body size
            uint8_t heavy = 0, skin = 0, number = 0, call = 0;  // number in BCD; call: the announcer's name call
            std::array<char, 16> last{};
            std::array<char, 16> first{};  // the original game keeps them (never shown); team+ doesn't
            uint8_t position = 0;          // the original game's position code
        };
        std::array<Player, players_per_team> players{};
        std::array<std::array<uint16_t, 2>, 10> uniform{};  // [part][home, away]: file ids
        std::array<uint8_t, 40> cpu_plays{};                // the CPU's play lists
        std::array<uint8_t, 5> ratings{};
        uint8_t cpu = 0, selector = 0;                       // CPU skill, the in-game per-team selector
        float pause = 0;                                     // the VS screen's wait after the full name (team+: whole)
        uint16_t small_logo = 0, logo = 0, title_city = 0, title_nick = 0;  // menu images
        std::array<uint16_t, 3> end_zones{};
        uint16_t win = 0, vs_left = 0, vs_right = 0;         // in-game images
        std::array<char, 32> nick{}, city_name{};            // team+ keeps 16 of them
        std::array<char, 16> select{};
        std::array<char, 4> code{}, prefix{};                // prefix: the original game's (unused) nickname code
    };

    class Store {
    public:
        // The game's teams as loaded (team+ blocks 0..n-1), with their franchises.
        void load(uint8_t* rdram, const std::vector<std::string>& franchise_of_slot);
        // Slot order: slot i holds franchise order[i] (Advanced's menu order). Teams keep their data.
        void reorder(const std::vector<std::string>& order);
        Team* find(const std::string& franchise);
        // A whole new set of teams (a league pak's, built on NFL teams): slot i holds teams[i]; clash(i, j) says
        // whether teams i and j can't both wear home jerseys.
        void replace(std::vector<Team> t, const std::vector<std::vector<bool>>& clash);
        // The same, with the whole clash matrix as an engine holds it (row a, column b).
        void set_all(std::vector<Team> t, std::vector<std::vector<bool>> clash_matrix) {
            teams = std::move(t);
            clash = std::move(clash_matrix);
            packed.clear();
        }
        const std::vector<std::vector<bool>>& clash_matrix() const { return clash; }
        // Whether the teams in slots a and b clash (their home jerseys look alike).
        bool clashes(size_t a, size_t b) const;
        const std::vector<Team>& slots() const { return teams; }
        std::vector<std::string> franchise_of_slot() const;

        // Everything into the game (after a menu load), or only the player records (a lineup or crew change while
        // the menus are up: the rest of a block may hold settings other code made since).
        void write(uint8_t* rdram) const;
        void write_players(uint8_t* rdram) const;

        // The packed state (before lineups and crews), so they can be laid over again.
        void keep_packed() { packed = teams; }
        void restore_packed() { teams = packed; }

    private:
        std::vector<Team> teams, packed;
        std::vector<std::vector<bool>> clash;  // [a][b], as the engine had it (team+ keeps a <= b only)
    };

    Store& store();

    // The original game's tables (the Classic ROM; docs/NO_TEAMPLUS_PATH.md): its teams live across main (uniforms,
    // VS helmets and pauses), ovl_b (records, rosters, names, title boxes, clashes) and ovl_a (win text, end zones,
    // small logos, CPU plays), each written when its segment is loaded.
    namespace original {
        enum class Segment { Main, OvlA, OvlB };
        constexpr int slots = 31;
        // Every team as the ROM image ships it, read in place (pass recomp::get_rom() itself, not a copy).
        void load(Store& st, std::span<const uint8_t> rom, const std::vector<std::string>& franchise_of_slot);
        void write(uint8_t* rdram, Segment seg, const Store& st);
        // From now on, the 32-team tables Modern's build moves the game to (engine32_layout.h).
        void use_engine32_layout();
        // While one is in scope, this thread's writes go to the game's own 31-team tables again (Export ROM on
        // Modern's build: a cart runs the unpatched code).
        struct GameLayout { GameLayout(); ~GameLayout(); GameLayout(const GameLayout&) = delete; GameLayout& operator=(const GameLayout&) = delete; };
        // While one is in scope, this thread writes Export ROM's 32-team cart layout (engine32_layout.h cart).
        struct CartLayout { CartLayout(); ~CartLayout(); CartLayout(const CartLayout&) = delete; CartLayout& operator=(const CartLayout&) = delete; };
        // Pieces of write(): one slot's part of a segment; the whole clash table ([slot][slot]); the right VS helmet
        // list from the teams in the slots.
        void write_team(uint8_t* rdram, Segment seg, int slot, const Team& t);
        void write_clashes(uint8_t* rdram, const std::vector<std::vector<bool>>& m);
        void write_right_helmets(uint8_t* rdram, const std::vector<const Team*>& in_slot);
    }

    // team+'s block layout: a block (0x220 bytes) as a team record, file ids as team+ has them.
    namespace teamplus {
        void decode(const uint8_t* block, Team& t);
    }

    // The teams as the running game has them (team select's overrides and readers; and this game's copies from
    // kickoff on). Slots are the game's team indices, sides 0 (left) and 1 (right).
    enum class Art { SmallLogo, Logo, Win };
    namespace live {
        uint16_t uniform_part(uint8_t* rdram, int slot, int part, int side);  // part 0-9, side 0 home / 1 away: a file id
        void set_uniform_part(uint8_t* rdram, int slot, int part, int side, uint16_t id);
        uint16_t art(uint8_t* rdram, int slot, Art a);
        void set_art(uint8_t* rdram, int slot, Art a, uint16_t id);
        std::string list_name(uint8_t* rdram, int slot);   // team select's line
        void set_list_name(uint8_t* rdram, int slot, const std::string& s);
        std::string city(uint8_t* rdram, int slot);
        std::string abbr(uint8_t* rdram, int slot);       // the scoreboard code
        void set_abbr(uint8_t* rdram, int slot, const std::string& s);
        int cpu_skill(uint8_t* rdram, int slot);          // signed
        int skin(uint8_t* rdram, int slot, int player);
        void set_skin(uint8_t* rdram, int slot, int player, int shade);

        bool has_copy(uint8_t* rdram, int side);          // a game is set up (kickoff on)
        uint16_t copy_uniform_part(uint8_t* rdram, int side, int part, int which);
        void set_copy_uniform_part(uint8_t* rdram, int side, int part, int which, uint16_t id);
        void set_copy_abbr(uint8_t* rdram, int side, const std::string& s);
        void set_copy_names(uint8_t* rdram, int side, const std::string& s);  // coin toss, quarter banner, line scores
        void set_copy_art(uint8_t* rdram, int side, Art a, uint16_t id);
        std::string copy_last(uint8_t* rdram, int side, int slot);  // this game's copy's players
        void set_copy_last(uint8_t* rdram, int side, int slot, const std::string& s);
        void clear_copy_call(uint8_t* rdram, int side, int slot);
        void set_copy_number(uint8_t* rdram, int side, int slot, int n);
        void set_copy_heavy(uint8_t* rdram, int side, int slot, int heavy);
        void set_copy_skin(uint8_t* rdram, int side, int slot, int shade);
    }
}
