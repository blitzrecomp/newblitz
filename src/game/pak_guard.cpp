// Controller Pak images (pak_guard.h). The layout is libultra's: page 0 the ID block, pages 1-2 the inode
// table (and its copy), pages 3-4 the directory (16 entries of 32 bytes), data from page 5.
#include "pak_guard.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>

namespace blitz::pakimg {
    namespace {
        uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
        uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
        uint64_t le(const uint8_t* p, int n) {
            uint64_t v = 0;
            for (int i = 0; i < n; i++) v |= (uint64_t)p[i] << (8 * i);
            return v;
        }
        constexpr char stub_magic[8] = "BZ26ADV";
        constexpr uint32_t first_data_page = 5, page_count = 128;
    }

    std::string note_name(const uint8_t* name16) {
        static const char punct[] = "!\"#'*+,-./:=?@";
        std::string out;
        for (int i = 0; i < 16; i++) {
            uint8_t c = name16[i];
            if (c == 0) break;
            if (c == 0x0F) out += ' ';
            else if (c >= 0x10 && c <= 0x19) out += (char)('0' + c - 0x10);
            else if (c >= 0x1A && c <= 0x33) out += (char)('A' + c - 0x1A);
            else if (c >= 0x34 && c < 0x34 + sizeof(punct) - 1) out += punct[c - 0x34];
            else out += '?';
        }
        while (!out.empty() && out.back() == ' ') out.pop_back();
        return out;
    }

    std::vector<Note> notes(const uint8_t* image) {
        std::vector<Note> out;
        const uint8_t* inode = image + 0x100;
        for (int e = 0; e < 16; e++) {
            const uint8_t* d = image + 0x300 + 32 * e;
            Note n;
            n.entry = e;
            n.game_code = be32(d);
            n.company_code = be16(d + 4);
            if (n.game_code == 0 || n.company_code == 0) continue;
            n.name = note_name(d + 0x10);
            // Follow the inode chain: each page's entry is the next page (bank, page), 1 for the last.
            uint32_t page = d[7];
            bool ok = d[6] == 0;  // one bank: a 32 KB pak
            std::vector<bool> seen(page_count, false);
            while (ok) {
                if (page < first_data_page || page >= page_count || seen[page]) {
                    ok = false;
                    break;
                }
                seen[page] = true;
                n.pages.push_back(page * 256);
                uint16_t next = be16(inode + 2 * page);
                if (next == 1) break;
                if ((next >> 8) != 0) ok = false;
                page = next & 0xFF;
            }
            if (ok && !n.pages.empty()) out.push_back(std::move(n));
        }
        return out;
    }

    std::vector<Stub> stubs(const uint8_t* image) {
        std::vector<Stub> out;
        std::vector<Note> ns = notes(image);
        for (int ni = 0; ni < (int)ns.size(); ni++) {
            const Note& n = ns[ni];
            for (uint32_t k = 0; k < slot_count; k++) {
                uint32_t f = slot_file_offset + k * slot_size;
                if (f + stub_bytes > n.size()) break;
                Stub s;
                uint8_t b[stub_bytes];
                for (uint32_t i = 0; i < stub_bytes; i++) {
                    s.pak[i] = n.pak_offset(f + i);
                    b[i] = image[s.pak[i]];
                }
                if (b[0] != 0xFF || b[1] != 0xFF || std::memcmp(b + 4, stub_magic, 8) != 0) continue;
                s.note = ni;
                s.slot = (int)k;
                s.id = le(b + 0xC, 8);
                s.gen = (uint32_t)le(b + 0x14, 4);
                s.team = b[0x18];
                s.week = b[0x19];
                out.push_back(s);
            }
        }
        return out;
    }

    std::set<std::pair<uint64_t, uint32_t>> stub_refs(const uint8_t* image) {
        std::set<std::pair<uint64_t, uint32_t>> out;
        for (const Stub& s : stubs(image)) out.insert({s.id, s.gen});
        return out;
    }

    std::set<std::pair<uint64_t, uint32_t>> released_by(const uint8_t* before, const uint8_t* after) {
        std::set<std::pair<uint64_t, uint32_t>> was = stub_refs(before), now = stub_refs(after), out;
        for (const auto& r : was) {
            if (!now.count(r)) out.insert(r);
        }
        return out;
    }

    std::vector<Stub> guard_block(const uint8_t* image, uint32_t block, uint8_t* data, int allowed_slot,
                                  const std::set<std::pair<uint64_t, uint32_t>>& released) {
        std::vector<Stub> kept;
        const uint32_t lo = block * 32, hi = lo + 32;
        if (hi > pak_size) return kept;
        for (const Stub& s : stubs(image)) {
            if (s.slot == allowed_slot || released.count({s.id, s.gen})) continue;
            bool changed = false;
            for (uint32_t i = 0; i < stub_bytes; i++) {
                uint32_t at = s.pak[i];
                if (at < lo || at >= hi) continue;
                if (data[at - lo] != image[at]) {
                    data[at - lo] = image[at];
                    changed = true;
                }
            }
            if (changed) kept.push_back(s);
        }
        return kept;
    }

    std::string history_name(const std::string& stem, std::time_t when) {
        char buf[32];
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &when);
#else
        localtime_r(&when, &tm);
#endif
        std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
        return stem + "-" + buf + ".mpk";
    }

    std::time_t parse_time(const std::string& name, const std::string& stem) {
        // stem-YYYYMMDD-HHMMSS.mpk, or with a -N before .mpk when two fell in one second
        if (name.size() < stem.size() + 20 || name.compare(0, stem.size() + 1, stem + "-") != 0) return 0;
        if (name.size() < 4 || name.compare(name.size() - 4, 4, ".mpk") != 0) return 0;
        int y, mo, d, h, mi, s;
        if (sscanf(name.c_str() + stem.size() + 1, "%4d%2d%2d-%2d%2d%2d", &y, &mo, &d, &h, &mi, &s) != 6) return 0;
        std::tm tm{};
        tm.tm_year = y - 1900;
        tm.tm_mon = mo - 1;
        tm.tm_mday = d;
        tm.tm_hour = h;
        tm.tm_min = mi;
        tm.tm_sec = s;
        tm.tm_isdst = -1;
        return std::mktime(&tm);
    }

    std::vector<std::string> history_prune(const std::vector<std::string>& names, const std::string& stem, std::time_t now, size_t keep_last,
                                           int days) {
        std::vector<std::pair<std::time_t, std::string>> dated;
        for (const std::string& n : names) {
            std::time_t t = parse_time(n, stem);
            if (t != 0) dated.push_back({t, n});
        }
        // Newest first (the name breaks ties, so "-2" sorts after its twin).
        std::sort(dated.begin(), dated.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second > b.second; });
        auto day_of = [](std::time_t t) {
            std::tm tm{};
#ifdef _WIN32
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif
            return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
        };
        std::set<int> recent_days;
        for (int i = 0; i < days; i++) recent_days.insert(day_of(now - (std::time_t)i * 86400));
        std::set<int> day_kept;
        std::vector<std::string> remove;
        for (size_t i = 0; i < dated.size(); i++) {
            int day = day_of(dated[i].first);
            bool keep = i < keep_last;
            if (recent_days.count(day) && !day_kept.count(day)) {
                day_kept.insert(day);
                keep = true;
            }
            if (!keep) remove.push_back(dated[i].second);
        }
        return remove;
    }

    std::string describe_rest(const uint8_t* image, const std::vector<Stub>& st, std::vector<std::string> parts) {
        std::vector<Note> ns = notes(image);
        if (ns.empty()) return "Empty pak";
        for (int ni = 0; ni < (int)ns.size(); ni++) {
            const Note& n = ns[ni];
            if (n.name.find("BLITZ") == std::string::npos || n.size() < slot_file_offset + slot_count * slot_size) {
                parts.push_back(n.name.empty() ? "Another game's save" : n.name);
                continue;
            }
            // The game's own seasons (Classic, Modern, an emulator): team at +0, 0xFF for an empty slot.
            int seasons = 0;
            for (uint32_t k = 0; k < slot_count; k++) {
                bool is_stub = false;
                for (const Stub& s : st) is_stub = is_stub || (s.note == ni && s.slot == (int)k);
                if (!is_stub && image[n.pak_offset(slot_file_offset + k * slot_size)] != 0xFF) seasons++;
            }
            if (seasons > 0) parts.push_back(std::to_string(seasons) + (seasons == 1 ? " season" : " seasons"));
        }
        if (parts.empty()) return "No seasons";
        std::string out;
        for (size_t i = 0; i < parts.size(); i++) out += (i ? "; " : "") + parts[i];
        return out;
    }

    std::string describe_plain(const uint8_t* image) {
        return describe(image, [](uint64_t, uint32_t, uint8_t) { return std::string(); });
    }
}
