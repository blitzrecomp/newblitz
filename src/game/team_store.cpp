// The team store (team_store.h) and its team+ encoding (block layout: docs/BLITZ2026.md). The original game's encoding
// is in team_store_original.cpp.
#include "team_store.h"

#include <algorithm>
#include <cstring>
#include <functional>

#include "recomp.h"

namespace {
    constexpr uint32_t block_base = 0x80330C9C, block_size = 0x220;
    constexpr uint32_t clash_addr = 0x8032D5E0, clash_row = 16;  // row a, bit for b >= a at byte 15 - b / 8, bit b % 8
    constexpr uint32_t player_size = 0x18;
    constexpr uint32_t p_scale = 0x0, p_heavy = 0x4, p_skin = 0x5, p_number = 0x6, p_call = 0x7, p_last = 0x8;
    constexpr uint32_t t_ratings = 0x1D0, t_vs_pause = 0x1D6, t_name = 0x1E0, t_select = 0x1F0, t_city = 0x200, t_abbr = 0x210;

    uint32_t player(int slot) { return (uint32_t)slot * player_size; }
}

namespace {
    using blitz::teamdata::Team;
    template <size_t N> void put(std::array<char, N>& f, const std::string& s) {
        for (size_t i = 0; i < N; i++) f[i] = i < s.size() ? s[i] : 0;
    }
    template <size_t N> std::string get(const std::array<char, N>& f) { return std::string(f.data(), strnlen(f.data(), N)); }

    // team+'s block layout (docs/BLITZ2026.md): the one place it's known, besides the live calls below.
    uint16_t be16(const uint8_t* b, uint32_t o) { return (uint16_t)(b[o] << 8 | b[o + 1]); }
    void put16(uint8_t* b, uint32_t o, uint16_t v) { b[o] = (uint8_t)(v >> 8); b[o + 1] = (uint8_t)v; }
    // n bytes of a field (team+ keeps 16 of the 32 the original game has for names and cities).
    template <size_t N> void get_bytes(const uint8_t* b, uint32_t o, std::array<char, N>& f, size_t n = N) { f = {}; std::memcpy(f.data(), b + o, n); }
    template <size_t N> void put_bytes(uint8_t* b, uint32_t o, const std::array<char, N>& f, size_t n = N) { std::memcpy(b + o, f.data(), n); }

    [[maybe_unused]] void decode(const uint8_t* b, Team& t) {
        for (int k = 0; k < blitz::teamdata::players_per_team; k++) {
            uint32_t p = player(k);
            Team::Player& pl = t.players[(size_t)k];
            pl.scale = (uint32_t)b[p] << 24 | (uint32_t)b[p + 1] << 16 | (uint32_t)b[p + 2] << 8 | b[p + 3];
            pl.heavy = b[p + p_heavy];
            pl.skin = b[p + p_skin];
            pl.number = b[p + p_number];
            pl.call = b[p + p_call];
            get_bytes(b, p + p_last, pl.last);
        }
        for (uint32_t k = 0; k < 10; k++) t.uniform[k] = {be16(b, 0x180 + 4 * k), be16(b, 0x180 + 4 * k + 2)};
        std::memcpy(t.cpu_plays.data(), b + 0x1A8, t.cpu_plays.size());
        std::memcpy(t.ratings.data(), b + t_ratings, 5);
        t.cpu = b[0x1D5];
        t.pause = b[t_vs_pause];
        t.selector = b[0x1D7];
        t.small_logo = be16(b, 0x1D8);
        t.logo = be16(b, 0x1DA);
        t.title_city = be16(b, 0x1DC);
        t.title_nick = be16(b, 0x1DE);
        get_bytes(b, t_name, t.nick, 16);
        get_bytes(b, t_select, t.select);
        get_bytes(b, t_city, t.city_name, 16);
        get_bytes(b, t_abbr, t.code);
        for (uint32_t k = 0; k < 3; k++) t.end_zones[k] = be16(b, 0x214 + 2 * k);
        t.win = be16(b, 0x21A);
        t.vs_left = be16(b, 0x21C);
        t.vs_right = be16(b, 0x21E);
    }

    [[maybe_unused]] void encode(const Team& t, uint8_t* b) {
        for (int k = 0; k < blitz::teamdata::players_per_team; k++) {
            uint32_t p = player(k);
            const Team::Player& pl = t.players[(size_t)k];
            for (int i = 0; i < 4; i++) b[p + (uint32_t)i] = (uint8_t)(pl.scale >> (24 - 8 * i));
            b[p + p_heavy] = pl.heavy;
            b[p + p_skin] = pl.skin;
            b[p + p_number] = pl.number;
            b[p + p_call] = pl.call;
            put_bytes(b, p + p_last, pl.last);
        }
        for (uint32_t k = 0; k < 10; k++) {
            put16(b, 0x180 + 4 * k, t.uniform[k][0]);
            put16(b, 0x180 + 4 * k + 2, t.uniform[k][1]);
        }
        std::memcpy(b + 0x1A8, t.cpu_plays.data(), t.cpu_plays.size());
        std::memcpy(b + t_ratings, t.ratings.data(), 5);
        b[0x1D5] = t.cpu;
        b[t_vs_pause] = (uint8_t)t.pause;
        b[0x1D7] = t.selector;
        put16(b, 0x1D8, t.small_logo);
        put16(b, 0x1DA, t.logo);
        put16(b, 0x1DC, t.title_city);
        put16(b, 0x1DE, t.title_nick);
        put_bytes(b, t_name, t.nick, 16);
        put_bytes(b, t_select, t.select);
        put_bytes(b, t_city, t.city_name, 16);
        put_bytes(b, t_abbr, t.code);
        for (uint32_t k = 0; k < 3; k++) put16(b, 0x214 + 2 * k, t.end_zones[k]);
        put16(b, 0x21A, t.win);
        put16(b, 0x21C, t.vs_left);
        put16(b, 0x21E, t.vs_right);
    }
}

namespace blitz::teamdata {
    std::string Team::nickname() const { return get(nick); }
    void Team::set_nickname(const std::string& s) { put(nick, s); }
    void Team::set_city(const std::string& s) { put(city_name, s); }
    void Team::set_select(const std::string& s) {
        std::string up = s;
        std::transform(up.begin(), up.end(), up.begin(), [](char c) { return (char)std::toupper((unsigned char)c); });
        put(select, up);
    }
    void Team::set_abbr(const std::string& s) { put(code, s); }
    void Team::set_rating(int k, int v) { ratings[(size_t)k] = (uint8_t)v; }
    void Team::set_vs_pause(int ticks) { pause = (float)ticks; }
    void Team::set_art(const std::vector<uint8_t>& ids) {
        auto id = [&](size_t k) { return k * 2 + 1 < ids.size() ? (uint16_t)(ids[k * 2] << 8 | ids[k * 2 + 1]) : (uint16_t)0; };
        if (ids.size() < 60) return;
        for (size_t k = 0; k < 10; k++) uniform[k] = {id(2 * k), id(2 * k + 1)};
        small_logo = id(20); logo = id(21); title_city = id(22); title_nick = id(23);
        end_zones = {id(24), id(25), id(26)};
        win = id(27); vs_left = id(28); vs_right = id(29);
    }
    uint16_t Team::uniform_part(int part, int side) const { return uniform[(size_t)part][(size_t)side]; }
    void Team::set_uniform_part(int part, int side, uint16_t id) { uniform[(size_t)part][(size_t)side] = id; }
    void Team::set_cpu_skill(int skill) { cpu = (uint8_t)skill; }
    void Team::set_vs_helmet_right(uint16_t id) { vs_right = id; }

    std::string Team::last(int slot) const { return get(players[(size_t)slot].last); }
    void Team::set_last(int slot, const std::string& s) { put(players[(size_t)slot].last, s); }
    void Team::set_number(int slot, int n) { players[(size_t)slot].number = (uint8_t)((n / 10 % 10) * 16 + n % 10); }
    void Team::set_scale(int slot, float size) { std::memcpy(&players[(size_t)slot].scale, &size, 4); }
    void Team::set_skin(int slot, int shade) { players[(size_t)slot].skin = (uint8_t)shade; }
    void Team::set_heavy(int slot, int heavy) { players[(size_t)slot].heavy = (uint8_t)heavy; }
    void Team::clear_call(int slot) { players[(size_t)slot].call = 0; }

#if BLITZ_TEAMPLUS
    void Store::load(uint8_t* rdram, const std::vector<std::string>& franchise_of_slot) {
        teams.assign(franchise_of_slot.size(), Team{});
        for (size_t i = 0; i < teams.size(); i++) {
            teams[i].franchise = franchise_of_slot[i];
            uint32_t b = block_base + (uint32_t)i * block_size;
            std::array<uint8_t, block_size> raw;
            for (uint32_t k = 0; k < block_size; k++) raw[k] = (uint8_t)MEM_B(0, (gpr)(int32_t)(b + k));
            decode(raw.data(), teams[i]);
        }
        clash.assign(teams.size(), std::vector<bool>(clash_row * 8));
        for (size_t i = 0; i < teams.size(); i++)
            for (uint32_t j = 0; j < clash_row * 8; j++)
                clash[i][j] = ((uint8_t)MEM_B(0, (gpr)(int32_t)(clash_addr + (uint32_t)i * clash_row + 15 - (j >> 3))) >> (j & 7)) & 1;
        packed.clear();
    }
#endif

    bool Store::clashes(size_t a, size_t b) const {
        size_t lo = std::min(a, b), hi = std::max(a, b);
        return lo < clash.size() && hi < clash[lo].size() && clash[lo][hi];
    }

    namespace {
        // team+'s clash rows for n teams from a clash test (a <= b only, as team+ keeps it; 128 columns).
        std::vector<std::vector<bool>> rows(size_t n, const std::function<bool(size_t, size_t)>& clash) {
            std::vector<std::vector<bool>> out(n, std::vector<bool>(clash_row * 8));
            for (size_t i = 0; i < n; i++)
                for (size_t j = i; j < n; j++) out[i][j] = clash(i, j);
            return out;
        }
    }

    void Store::replace(std::vector<Team> t, const std::vector<std::vector<bool>>& c) {
        teams = std::move(t);
        clash = rows(teams.size(), [&](size_t i, size_t j) { return c[i][j]; });
        packed.clear();
    }

    void Store::reorder(const std::vector<std::string>& order) {
        std::vector<Team> out;
        std::vector<size_t> from;  // new slot -> old slot
        for (const std::string& f : order)
            for (size_t i = 0; i < teams.size(); i++)
                if (teams[i].franchise == f) {
                    out.push_back(teams[i]);
                    from.push_back(i);
                }
        if (out.size() != teams.size()) return;
        clash = rows(out.size(), [&](size_t i, size_t j) { return clashes(from[i], from[j]); });
        teams = std::move(out);
    }

    Team* Store::find(const std::string& franchise) {
        for (Team& t : teams) if (t.franchise == franchise) return &t;
        return nullptr;
    }

    std::vector<std::string> Store::franchise_of_slot() const {
        std::vector<std::string> out;
        for (const Team& t : teams) out.push_back(t.franchise);
        return out;
    }

#if BLITZ_TEAMPLUS
    void Store::write(uint8_t* rdram) const {
        for (size_t i = 0; i < teams.size(); i++) {
            uint32_t b = block_base + (uint32_t)i * block_size;
            std::array<uint8_t, block_size> raw{};
            encode(teams[i], raw.data());
            for (uint32_t k = 0; k < block_size; k++) MEM_B(0, (gpr)(int32_t)(b + k)) = (int8_t)raw[k];
        }
        for (size_t i = 0; i < clash.size(); i++)
            for (uint32_t k = 0; k < clash_row; k++) {
                uint8_t byte = 0;
                for (uint32_t bit = 0; bit < 8; bit++) {
                    size_t j = (15 - k) * 8 + bit;
                    if (j < clash[i].size() && clash[i][j]) byte |= (uint8_t)(1 << bit);
                }
                MEM_B(0, (gpr)(int32_t)(clash_addr + (uint32_t)i * clash_row + k)) = (int8_t)byte;
            }
    }

    void Store::write_players(uint8_t* rdram) const {
        for (size_t i = 0; i < teams.size(); i++) {
            uint32_t b = block_base + (uint32_t)i * block_size;
            std::array<uint8_t, block_size> raw{};
            encode(teams[i], raw.data());
            for (uint32_t k = 0; k < players_per_team * player_size; k++) MEM_B(0, (gpr)(int32_t)(b + k)) = (int8_t)raw[k];
        }
    }

    namespace live {
        namespace {
            uint32_t at(int slot) { return block_base + (uint32_t)slot * block_size; }
            gpr g(uint32_t a) { return (gpr)(int32_t)a; }
            constexpr uint32_t t_uniform = 0x180, t_small_logo = 0x1D8, t_logo = 0x1DA, t_win = 0x21A;
            constexpr uint32_t game_copy = 0x802E9460;  // [side * 0x64]: the pointer to this game's copy of the block
            constexpr uint32_t short_record = 0x802E9408, r_nickname = 0x10, r_city = 0x20, r_city2 = 0x30;
            constexpr uint32_t r_small_logo = 0x08, r_logo = 0x0A, r_win = 0x4A;
            uint32_t art_off(Art a) { return a == Art::SmallLogo ? t_small_logo : a == Art::Logo ? t_logo : t_win; }
            uint32_t rec_art_off(Art a) { return a == Art::SmallLogo ? r_small_logo : a == Art::Logo ? r_logo : r_win; }
            std::string rd_text(uint8_t* rdram, uint32_t a, uint32_t len) {
                std::string out;
                for (uint32_t i = 0; i < len; i++) {
                    char c = (char)MEM_B(0, g(a + i));
                    if (c == 0) break;
                    out += c;
                }
                return out;
            }
            void wr_text(uint8_t* rdram, uint32_t a, const std::string& s, uint32_t len) {
                for (uint32_t i = 0; i < len; i++) MEM_B(0, g(a + i)) = (int8_t)(i < s.size() ? s[i] : 0);
            }
            uint32_t copy_of(uint8_t* rdram, int side) {
                uint32_t c = (uint32_t)MEM_W(0, g(game_copy + 0x64 * (uint32_t)side));
                return c >= 0x80000000u && c < 0x80800000u ? c : 0;
            }
        }
        uint16_t uniform_part(uint8_t* rdram, int slot, int part, int side) {
            return (uint16_t)MEM_HU(0, g(at(slot) + t_uniform + 4 * (uint32_t)part + 2 * (uint32_t)side));
        }
        void set_uniform_part(uint8_t* rdram, int slot, int part, int side, uint16_t id) {
            MEM_HU(0, g(at(slot) + t_uniform + 4 * (uint32_t)part + 2 * (uint32_t)side)) = id;
        }
        uint16_t art(uint8_t* rdram, int slot, Art a) { return (uint16_t)MEM_HU(0, g(at(slot) + art_off(a))); }
        void set_art(uint8_t* rdram, int slot, Art a, uint16_t id) { MEM_HU(0, g(at(slot) + art_off(a))) = id; }
        std::string list_name(uint8_t* rdram, int slot) { return rd_text(rdram, at(slot) + t_select, 16); }
        void set_list_name(uint8_t* rdram, int slot, const std::string& s) { wr_text(rdram, at(slot) + t_select, s, 16); }
        std::string city(uint8_t* rdram, int slot) { return rd_text(rdram, at(slot) + t_city, 16); }
        std::string abbr(uint8_t* rdram, int slot) { return rd_text(rdram, at(slot) + t_abbr, 4); }
        void set_abbr(uint8_t* rdram, int slot, const std::string& s) { wr_text(rdram, at(slot) + t_abbr, s, 4); }
        int cpu_skill(uint8_t* rdram, int slot) { return (int8_t)MEM_B(0, g(at(slot) + 0x1D5)); }
        int skin(uint8_t* rdram, int slot, int player) { return (uint8_t)MEM_B(0, g(at(slot) + (uint32_t)player * player_size + p_skin)); }
        void set_skin(uint8_t* rdram, int slot, int player, int shade) { MEM_B(0, g(at(slot) + (uint32_t)player * player_size + p_skin)) = (int8_t)shade; }

        bool has_copy(uint8_t* rdram, int side) { return copy_of(rdram, side) != 0; }
        uint16_t copy_uniform_part(uint8_t* rdram, int side, int part, int which) {
            uint32_t c = copy_of(rdram, side);
            return c ? (uint16_t)MEM_HU(0, g(c + t_uniform + 4 * (uint32_t)part + 2 * (uint32_t)which)) : 0;
        }
        void set_copy_uniform_part(uint8_t* rdram, int side, int part, int which, uint16_t id) {
            if (uint32_t c = copy_of(rdram, side)) MEM_HU(0, g(c + t_uniform + 4 * (uint32_t)part + 2 * (uint32_t)which)) = id;
        }
        void set_copy_abbr(uint8_t* rdram, int side, const std::string& s) {
            if (uint32_t c = copy_of(rdram, side)) wr_text(rdram, c + t_abbr, s, 4);
        }
        void set_copy_names(uint8_t* rdram, int side, const std::string& s) {
            uint32_t rec = short_record + 0x64 * (uint32_t)side;
            wr_text(rdram, rec + r_nickname, s, 16);  // "TEAM 1 (BHR) RECEIVES BALL"
            wr_text(rdram, rec + r_city, s, 16);      // the cities: the quarter banner, halftime and final
            wr_text(rdram, rec + r_city2, s, 16);     // line scores
        }
        void set_copy_art(uint8_t* rdram, int side, Art a, uint16_t id) {
            MEM_HU(0, g(short_record + 0x64 * (uint32_t)side + rec_art_off(a))) = id;
        }
        namespace {
            uint32_t copy_player(uint8_t* rdram, int side, int slot) {
                uint32_t c = copy_of(rdram, side);
                return c ? c + (uint32_t)slot * player_size : 0;
            }
        }
        std::string copy_last(uint8_t* rdram, int side, int slot) {
            uint32_t a = copy_player(rdram, side, slot);
            return a ? rd_text(rdram, a + p_last, 16) : std::string{};
        }
        void set_copy_last(uint8_t* rdram, int side, int slot, const std::string& s) {
            if (uint32_t a = copy_player(rdram, side, slot)) wr_text(rdram, a + p_last, s, 16);
        }
        void clear_copy_call(uint8_t* rdram, int side, int slot) {
            if (uint32_t a = copy_player(rdram, side, slot)) MEM_B(p_call, g(a)) = 0;
        }
        void set_copy_number(uint8_t* rdram, int side, int slot, int n) {
            if (uint32_t a = copy_player(rdram, side, slot)) MEM_B(p_number, g(a)) = (int8_t)((n / 10 % 10) * 16 + n % 10);
        }
        void set_copy_heavy(uint8_t* rdram, int side, int slot, int heavy) {
            if (uint32_t a = copy_player(rdram, side, slot)) MEM_B(p_heavy, g(a)) = (int8_t)heavy;
        }
        void set_copy_skin(uint8_t* rdram, int side, int slot, int shade) {
            if (uint32_t a = copy_player(rdram, side, slot)) MEM_B(p_skin, g(a)) = (int8_t)shade;
        }
    }

#endif

    Store& store() {
        static Store s;
        return s;
    }

    namespace teamplus {
        void decode(const uint8_t* block, Team& t) { ::decode(block, t); }
    }
}
