// The team store's encoding for the original game (team_store.h, namespace original). A team's data is spread over
// three segments there, by table (docs/CONTENT_MAP.md §5, NOTES.md "Data loaders"); each segment's part is written
// when the segment is loaded. Pointers in a team record (+0x58 its roster, +0x60 its title box entry) belong to the
// slot, not the team. File ids are kept as team+ keeps them (TOC index + 1); the original game's tables hold asset
// ids, 0x0A000000 | id.
#include "team_store.h"
#include "engine32_layout.h"

#include <cstring>

#include "recomp.h"

namespace {
    using blitz::teamdata::Team;

    // Where the tables are: the game's own addresses, or Modern's 32-team copies (engine32_layout.h).
    bool moved_tables = false;
    thread_local int game_layout = 0;  // GameLayout scopes on this thread
    thread_local int cart_layout = 0;  // CartLayout scopes on this thread (Export ROM's 32-team cart)
    bool relocated() { return moved_tables && game_layout == 0 && cart_layout == 0; }
    bool cart() { return cart_layout > 0; }
    uint32_t at(uint32_t original) {
        return cart() ? blitz::engine32::cart::now(original) : relocated() ? blitz::engine32::now(original) : original;
    }
    int slot_count() { return relocated() || cart() ? blitz::engine32::teams : blitz::teamdata::original::slots; }

    // Main is resident; the overlays share 0x802EFE70.
    size_t rom_main(uint32_t v) { return 0x2FC20 + (v - 0x80248F00); }
    size_t rom_ovl_a(uint32_t v) { return 0x93EC0 + (v - 0x802EFE70); }
    size_t rom_ovl_b(uint32_t v) { return 0xDE350 + (v - 0x802EFE70); }

    // ovl_b
    constexpr uint32_t team_table = 0x8033800C, team_size = 0x64;
    constexpr uint32_t roster_table = 0x80330BCC, roster_size = 0x3C0, player_size = 0x3C;
    constexpr uint32_t select_names = 0x80338E68, title_boxes = 0x8032D5E0, clash_table = 0x8032D6D8;
    // main
    constexpr uint32_t uniform_home = 0x8028AAB0, uniform_away = 0x8028AF88, uniform_size = 0x28;
    constexpr uint32_t vs_left = 0x802836E4, vs_right_teams = 0x802836AC, vs_right_ids = 0x802836C0, vs_pause = 0x802AAFD8;
    constexpr int vs_right_entries = 10;
    // ovl_a
    constexpr uint32_t win_text = 0x8033460C, small_logos = 0x80337280, selectors = 0x80333D48;
    constexpr uint32_t end_zones[3] = {0x80335428, 0x803354A4, 0x80335520};  // three arrays, one image per team each
    constexpr uint32_t cpu_offense = 0x80336E20, cpu_defense = 0x80337168;
    constexpr uint32_t offense_plays = 27, defense_plays = 9;

    constexpr uint32_t asset(uint16_t id) { return 0x0A000000u | id; }

    struct Rom {
        std::span<const uint8_t> b;
        uint8_t u8(size_t o) const { return o < b.size() ? b[o] : 0; }
        uint16_t u16(size_t o) const { return (uint16_t)(u8(o) << 8 | u8(o + 1)); }
        uint32_t u32(size_t o) const { return (uint32_t)u16(o) << 16 | u16(o + 2); }
        template <size_t N> void text(size_t o, std::array<char, N>& f) const {
            for (size_t i = 0; i < N; i++) f[i] = (char)u8(o + i);
        }
    };

    gpr g(uint32_t a) { return (gpr)(int32_t)a; }
    void w8(uint8_t* rdram, uint32_t a, uint32_t v) { MEM_B(0, g(a)) = (int8_t)v; }
    void w16(uint8_t* rdram, uint32_t a, uint32_t v) { MEM_HU(0, g(a)) = (uint16_t)v; }
    void w32(uint8_t* rdram, uint32_t a, uint32_t v) { MEM_W(0, g(a)) = (int32_t)v; }
    template <size_t N> void wtext(uint8_t* rdram, uint32_t a, const std::array<char, N>& f) {
        for (size_t i = 0; i < N; i++) w8(rdram, a + (uint32_t)i, (uint8_t)f[i]);
    }

    uint32_t roster_at(int s) {
        if (cart() && s >= blitz::teamdata::original::slots) return blitz::engine32::cart::houston_roster;
        return (relocated() ? blitz::engine32::rosters : roster_table) + (uint32_t)s * roster_size;
    }
    uint32_t titles_at(int s) {
        if (cart() && s >= blitz::teamdata::original::slots) return blitz::engine32::cart::houston_titles;
        return (relocated() ? blitz::engine32::title_boxes : title_boxes) + (uint32_t)s * 8;
    }

    void write_ovl_b(uint8_t* rdram, int s, const Team& t) {
        uint32_t rec = at(team_table) + (uint32_t)s * team_size;
        for (uint32_t k = 0; k < 5; k++) w16(rdram, rec + 2 * k, t.ratings[k]);
        w16(rdram, rec + 0x0A, 0);
        w32(rdram, rec + 0x0C, t.cpu);
        wtext(rdram, rec + 0x10, t.nick);
        wtext(rdram, rec + 0x30, t.city_name);
        wtext(rdram, rec + 0x50, t.code);
        wtext(rdram, rec + 0x54, t.prefix);
        w32(rdram, rec + 0x58, roster_at(s));  // the slot's own
        w32(rdram, rec + 0x5C, asset(t.logo));
        w32(rdram, rec + 0x60, titles_at(s));  // the slot's own
        for (int k = 0; k < blitz::teamdata::players_per_team; k++) {
            uint32_t p = roster_at(s) + (uint32_t)k * player_size;
            const Team::Player& pl = t.players[(size_t)k];
            w32(rdram, p + 0x00, pl.scale);
            w32(rdram, p + 0x04, pl.heavy);
            w32(rdram, p + 0x08, pl.skin);
            w32(rdram, p + 0x0C, pl.number);
            w32(rdram, p + 0x10, pl.position);
            w32(rdram, p + 0x14, 0);
            w32(rdram, p + 0x18, pl.call);
            wtext(rdram, p + 0x1C, pl.last);
            wtext(rdram, p + 0x2C, pl.first);
        }
        wtext(rdram, at(select_names) + (uint32_t)s * 16, t.select);
        w32(rdram, titles_at(s), asset(t.title_city));
        w32(rdram, titles_at(s) + 4, asset(t.title_nick));
    }

    void write_main(uint8_t* rdram, int s, const Team& t) {
        for (uint32_t p = 0; p < 10; p++) {
            w32(rdram, at(uniform_home) + (uint32_t)s * uniform_size + 4 * p, asset(t.uniform[p][0]));
            w32(rdram, at(uniform_away) + (uint32_t)s * uniform_size + 4 * p, asset(t.uniform[p][1]));
        }
        w32(rdram, at(vs_left) + 4 * (uint32_t)s, asset(t.vs_left));
        uint32_t bits;
        std::memcpy(&bits, &t.pause, 4);
        w32(rdram, at(vs_pause) + 4 * (uint32_t)s, bits);
    }

    void write_ovl_a(uint8_t* rdram, int s, const Team& t) {
        w32(rdram, at(win_text) + 8 * (uint32_t)s, asset(t.win));
        w32(rdram, at(win_text) + 8 * (uint32_t)s + 4, 0);
        for (uint32_t k = 0; k < 3; k++) w32(rdram, at(end_zones[k]) + 4 * (uint32_t)s, asset(t.end_zones[k]));
        w32(rdram, at(small_logos) + 4 * (uint32_t)s, asset(t.small_logo));
        w32(rdram, at(selectors) + 4 * (uint32_t)s, t.selector);
        for (uint32_t k = 0; k < offense_plays; k++) w8(rdram, at(cpu_offense) + offense_plays * (uint32_t)s + k, t.cpu_plays[k]);
        for (uint32_t k = 0; k < defense_plays; k++) w8(rdram, at(cpu_defense) + defense_plays * (uint32_t)s + k, t.cpu_plays[offense_plays + k]);
    }
}

namespace blitz::teamdata::original {
    void use_engine32_layout() { moved_tables = true; }
    GameLayout::GameLayout() { game_layout++; }
    GameLayout::~GameLayout() { game_layout--; }
    CartLayout::CartLayout() { cart_layout++; }
    CartLayout::~CartLayout() { cart_layout--; }

    void load(Store& st, std::span<const uint8_t> image, const std::vector<std::string>& franchise_of_slot) {
        Rom rom{image};
        std::vector<Team> teams(original::slots);
        for (int s = 0; s < original::slots; s++) {
            Team& t = teams[(size_t)s];
            t.franchise = (size_t)s < franchise_of_slot.size() ? franchise_of_slot[(size_t)s] : std::string{};
            size_t rec = rom_ovl_b(team_table + (uint32_t)s * team_size);
            for (size_t k = 0; k < 5; k++) t.ratings[k] = (uint8_t)rom.u16(rec + 2 * k);
            t.cpu = (uint8_t)rom.u32(rec + 0x0C);
            rom.text(rec + 0x10, t.nick);
            rom.text(rec + 0x30, t.city_name);
            rom.text(rec + 0x50, t.code);
            rom.text(rec + 0x54, t.prefix);
            t.logo = (uint16_t)rom.u32(rec + 0x5C);
            for (int k = 0; k < players_per_team; k++) {
                size_t p = rom_ovl_b(roster_table + (uint32_t)s * roster_size + (uint32_t)k * player_size);
                Team::Player& pl = t.players[(size_t)k];
                pl.scale = rom.u32(p + 0x00);
                pl.heavy = (uint8_t)rom.u32(p + 0x04);
                pl.skin = (uint8_t)rom.u32(p + 0x08);
                pl.number = (uint8_t)rom.u32(p + 0x0C);
                pl.position = (uint8_t)rom.u32(p + 0x10);
                pl.call = (uint8_t)rom.u32(p + 0x18);
                rom.text(p + 0x1C, pl.last);
                rom.text(p + 0x2C, pl.first);
            }
            rom.text(rom_ovl_b(select_names + (uint32_t)s * 16), t.select);
            t.title_city = (uint16_t)rom.u32(rom_ovl_b(title_boxes + (uint32_t)s * 8));
            t.title_nick = (uint16_t)rom.u32(rom_ovl_b(title_boxes + (uint32_t)s * 8 + 4));
            for (size_t p = 0; p < 10; p++) {
                t.uniform[p][0] = (uint16_t)rom.u32(rom_main(uniform_home + (uint32_t)s * uniform_size + 4 * (uint32_t)p));
                t.uniform[p][1] = (uint16_t)rom.u32(rom_main(uniform_away + (uint32_t)s * uniform_size + 4 * (uint32_t)p));
            }
            t.vs_left = (uint16_t)rom.u32(rom_main(vs_left + 4 * (uint32_t)s));
            for (int k = 0; k < vs_right_entries; k++)
                if (rom.u16(rom_main(vs_right_teams + 2 * (uint32_t)k)) == s) t.vs_right = (uint16_t)rom.u32(rom_main(vs_right_ids + 4 * (uint32_t)k));
            uint32_t bits = rom.u32(rom_main(vs_pause + 4 * (uint32_t)s));
            std::memcpy(&t.pause, &bits, 4);
            t.win = (uint16_t)rom.u32(rom_ovl_a(win_text + 8 * (uint32_t)s));
            for (size_t k = 0; k < 3; k++) t.end_zones[k] = (uint16_t)rom.u32(rom_ovl_a(end_zones[k] + 4 * (uint32_t)s));
            t.small_logo = (uint16_t)rom.u32(rom_ovl_a(small_logos + 4 * (uint32_t)s));
            t.selector = (uint8_t)rom.u32(rom_ovl_a(selectors + 4 * (uint32_t)s));
            for (uint32_t k = 0; k < offense_plays; k++) t.cpu_plays[k] = rom.u8(rom_ovl_a(cpu_offense + offense_plays * (uint32_t)s + k));
            for (uint32_t k = 0; k < defense_plays; k++) t.cpu_plays[offense_plays + k] = rom.u8(rom_ovl_a(cpu_defense + defense_plays * (uint32_t)s + k));
        }
        constexpr int n = original::slots;
        std::vector<std::vector<bool>> clash(n, std::vector<bool>(n));
        for (int a = 0; a < n; a++)
            for (int b = 0; b < n; b++) clash[(size_t)a][(size_t)b] = rom.u8(rom_ovl_b(clash_table + (uint32_t)(a * n + b))) != 0;
        st.set_all(std::move(teams), std::move(clash));
    }

    void write_team(uint8_t* rdram, Segment seg, int slot, const Team& t) {
        if (slot < 0 || slot >= slot_count()) return;
        if (seg == Segment::OvlB) write_ovl_b(rdram, slot, t);
        if (seg == Segment::Main) write_main(rdram, slot, t);
        if (seg == Segment::OvlA) write_ovl_a(rdram, slot, t);
    }

    void write_clashes(uint8_t* rdram, const std::vector<std::vector<bool>>& m) {
        if (relocated() || cart()) return;  // the store (engine32.cpp blitz_e32_clash) / the cart's own table
        constexpr int n = original::slots;
        for (int a = 0; a < n; a++)
            for (int b = 0; b < n; b++)
                w8(rdram, clash_table + (uint32_t)(a * n + b), (size_t)a < m.size() && (size_t)b < m[(size_t)a].size() && m[(size_t)a][(size_t)b]);
    }

    // The teams with their own right-facing image, highest slot first (as the game lists them); a spare entry
    // names no team.
    void write_right_helmets(uint8_t* rdram, const std::vector<const Team*>& in_slot) {
        // The game scans 9 entries of its list; the moved list (Modern, the 32-team cart) holds 32. The game's own list
        // has 10 teams but only 9 images: a 10th image would land on the first VS helmet (vs_left, slot 0), so a cart
        // whose 10 or more teams have their own right helmet showed another team's on slot 0's side.
        const int entries = relocated() || cart() ? blitz::engine32::vs_right_entries : vs_right_entries - 1;
        const uint32_t teams_at = at(vs_right_teams), ids_at = at(vs_right_ids);
        int k = 0;
        for (int s = (int)std::min<size_t>(in_slot.size(), (size_t)slot_count()) - 1; s >= 0 && k < entries; s--) {
            if (!in_slot[(size_t)s] || in_slot[(size_t)s]->vs_right == 0) continue;
            w16(rdram, teams_at + 2 * (uint32_t)k, (uint32_t)s);
            w32(rdram, ids_at + 4 * (uint32_t)k, asset(in_slot[(size_t)s]->vs_right));
            k++;
        }
        for (; k < entries; k++) {
            w16(rdram, teams_at + 2 * (uint32_t)k, 0x7FFF);
            w32(rdram, ids_at + 4 * (uint32_t)k, 0);
        }
    }

    void write(uint8_t* rdram, Segment seg, const Store& st) {
        const auto& teams = st.slots();
        int n = (int)std::min<size_t>(teams.size(), (size_t)slot_count());
        for (int s = 0; s < n; s++) write_team(rdram, seg, s, teams[(size_t)s]);
        if (seg == Segment::OvlB) write_clashes(rdram, st.clash_matrix());
        if (seg == Segment::Main) {
            std::vector<const Team*> in_slot;
            for (int s = 0; s < n; s++) in_slot.push_back(&teams[(size_t)s]);
            write_right_helmets(rdram, in_slot);
        }
    }
}

#if !BLITZ_TEAMPLUS
// The store into the running game, and the live calls, on the original engine (engine32.cpp holds the slot map).
extern "C" void blitz_e32_refresh(uint8_t* rdram);  // engine32.cpp

namespace blitz::teamdata {
    void Store::write(uint8_t* rdram) const { blitz_e32_refresh(rdram); }
    void Store::write_players(uint8_t* rdram) const { blitz_e32_refresh(rdram); }

    namespace live {
        namespace {
            constexpr uint32_t chosen = 0x802EA434;      // [side] the slot picked
            constexpr uint32_t copy_record = 0x802E9408;  // [side * 0x64] this game's copy of the team record
            constexpr uint32_t copy_players = 0x802E9460; // [side * 0x64]: the pointer to its copy of the roster
            uint32_t rec(int slot) { return at(team_table) + (uint32_t)slot * team_size; }
            std::string rtext(uint8_t* rdram, uint32_t a, uint32_t len) {
                std::string out;
                for (uint32_t i = 0; i < len; i++) {
                    char c = (char)MEM_B(0, g(a + i));
                    if (c == 0) break;
                    out += c;
                }
                return out;
            }
            void wtext_s(uint8_t* rdram, uint32_t a, const std::string& s, uint32_t len) {
                for (uint32_t i = 0; i < len; i++) MEM_B(0, g(a + i)) = (int8_t)(i < s.size() ? s[i] : 0);
            }
            int chosen_slot(uint8_t* rdram, int side) { return MEM_W(0, g(chosen + 4 * (uint32_t)side)); }
            uint32_t uniform_at(int slot, int part, int which) {
                return at(which == 0 ? uniform_home : uniform_away) + (uint32_t)slot * uniform_size + 4 * (uint32_t)part;
            }
            uint32_t player_copy(uint8_t* rdram, int side, int slot) {
                uint32_t c = (uint32_t)MEM_W(0, g(copy_players + 0x64 * (uint32_t)side));
                return c >= 0x80000000u && c < 0x80800000u ? c + (uint32_t)slot * player_size : 0;
            }
        }
        uint16_t uniform_part(uint8_t* rdram, int slot, int part, int side) { return (uint16_t)MEM_W(0, g(uniform_at(slot, part, side))); }
        void set_uniform_part(uint8_t* rdram, int slot, int part, int side, uint16_t id) { w32(rdram, uniform_at(slot, part, side), asset(id)); }
        // The small logo and win text are in-game tables (ovl_a, which shares memory with the menus): only the team
        // select logo here; a game takes the others through set_copy_art at kickoff.
        uint16_t art(uint8_t* rdram, int slot, Art a) {
            if (a == Art::Logo) return (uint16_t)MEM_W(0, g(rec(slot) + 0x5C));
            if (!relocated()) return 0;
            return (uint16_t)MEM_W(0, g(a == Art::SmallLogo ? at(small_logos) + 4 * (uint32_t)slot : at(win_text) + 8 * (uint32_t)slot));
        }
        void set_art(uint8_t* rdram, int slot, Art a, uint16_t id) {
            if (a == Art::Logo) w32(rdram, rec(slot) + 0x5C, asset(id));
            else if (relocated()) w32(rdram, a == Art::SmallLogo ? at(small_logos) + 4 * (uint32_t)slot : at(win_text) + 8 * (uint32_t)slot, asset(id));
        }
        std::string list_name(uint8_t* rdram, int slot) { return rtext(rdram, at(select_names) + (uint32_t)slot * 16, 16); }
        void set_list_name(uint8_t* rdram, int slot, const std::string& s) { wtext_s(rdram, at(select_names) + (uint32_t)slot * 16, s, 16); }
        std::string city(uint8_t* rdram, int slot) { return rtext(rdram, rec(slot) + 0x30, 32); }
        std::string abbr(uint8_t* rdram, int slot) { return rtext(rdram, rec(slot) + 0x50, 4); }
        void set_abbr(uint8_t* rdram, int slot, const std::string& s) { wtext_s(rdram, rec(slot) + 0x50, s, 4); }
        int cpu_skill(uint8_t* rdram, int slot) { return MEM_W(0, g(rec(slot) + 0x0C)); }
        int skin(uint8_t* rdram, int slot, int player) {
            return MEM_W(0, g(roster_at(slot) + (uint32_t)player * player_size + 0x08));
        }
        void set_skin(uint8_t* rdram, int slot, int player, int shade) {
            w32(rdram, roster_at(slot) + (uint32_t)player * player_size + 0x08, (uint32_t)shade);
        }

        bool has_copy(uint8_t* rdram, int side) { return player_copy(rdram, side, 0) != 0; }
        // A game dresses each side from the main tables of its slot (func_80256580): its copy is those tables.
        uint16_t copy_uniform_part(uint8_t* rdram, int side, int part, int which) {
            int s = chosen_slot(rdram, side);
            return s >= 0 && s < slot_count() ? uniform_part(rdram, s, part, which) : 0;
        }
        void set_copy_uniform_part(uint8_t* rdram, int side, int part, int which, uint16_t id) {
            int s = chosen_slot(rdram, side);
            if (s >= 0 && s < slot_count()) set_uniform_part(rdram, s, part, which, id);
        }
        void set_copy_abbr(uint8_t* rdram, int side, const std::string& s) { wtext_s(rdram, copy_record + 0x64 * (uint32_t)side + 0x50, s, 4); }
        void set_copy_names(uint8_t* rdram, int side, const std::string& s) {
            wtext_s(rdram, copy_record + 0x64 * (uint32_t)side + 0x10, s, 32);  // the nickname
            wtext_s(rdram, copy_record + 0x64 * (uint32_t)side + 0x30, s, 32);  // the city: banners and line scores
        }
        void set_copy_art(uint8_t* rdram, int side, Art a, uint16_t id) {
            int s = chosen_slot(rdram, side);
            if (a == Art::Logo) w32(rdram, copy_record + 0x64 * (uint32_t)side + 0x5C, asset(id));
            else if (s >= 0 && s < slot_count()) w32(rdram, (a == Art::SmallLogo ? at(small_logos) + 4 * (uint32_t)s : at(win_text) + 8 * (uint32_t)s), asset(id));
        }
        std::string copy_last(uint8_t* rdram, int side, int slot) {
            uint32_t a = player_copy(rdram, side, slot);
            return a ? rtext(rdram, a + 0x1C, 16) : std::string{};
        }
        void set_copy_last(uint8_t* rdram, int side, int slot, const std::string& s) {
            if (uint32_t a = player_copy(rdram, side, slot)) wtext_s(rdram, a + 0x1C, s, 16);
        }
        void clear_copy_call(uint8_t* rdram, int side, int slot) {
            if (uint32_t a = player_copy(rdram, side, slot)) w32(rdram, a + 0x18, 0);
        }
        void set_copy_number(uint8_t* rdram, int side, int slot, int n) {
            if (uint32_t a = player_copy(rdram, side, slot)) w32(rdram, a + 0x0C, (uint32_t)((n / 10 % 10) * 16 + n % 10));
        }
        void set_copy_heavy(uint8_t* rdram, int side, int slot, int heavy) {
            if (uint32_t a = player_copy(rdram, side, slot)) w32(rdram, a + 0x04, (uint32_t)heavy);
        }
        void set_copy_skin(uint8_t* rdram, int side, int slot, int shade) {
            if (uint32_t a = player_copy(rdram, side, slot)) w32(rdram, a + 0x08, (uint32_t)shade);
        }
    }
}
#endif
