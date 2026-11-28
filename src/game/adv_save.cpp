// Advanced mode's season saves (docs/ADVANCED_SEASON.md §3).
//
// The game saves a season as a 0x774-byte copy of its season record in one of 4 slots of its
// Controller Pak note. In Advanced the slot holds only a stub; the season itself goes to a sidecar
// file next to the pak (saves/advanced/<pack>/season/season-<id>-<gen>.adv):
//   - stub: +0/+1 = 0xFF (every other reader - Classic, Modern, team+ on an emulator - sees an empty
//     slot), "BZ26ADV" at +4, then season id, generation, display team/week and the sidecar's CRC;
//   - sidecar: header, the franchise of each team index, the season seed, the 0x774-byte record, CRC.
// The sidecar is written (and synced) before the pak, and the two newest generations are kept, so a
// crash or a failed pak write leaves a matching pair on disk.
//
// Hooks (tools/teamplus.py ADVANCED), all in main func_802679B8, the save/load screen:
//   0x80267B84 slots read     stubs with a sidecar show their team and week; other seasons in the
//                             slots (saved by Modern or an emulator) are hidden and kept as they are
//   0x80267F48 before LOAD    the sidecar's record replaces the stub, so the game loads the season
//   0x8026807C before write   sidecar written; the slot gets a stub; hidden slots are restored
//   0x8026809C after write    success: prune old generations; failure: drop the new one
// Between the two write hooks controller_pak.cpp lets that slot's stub change (pak_season_save_begin/end); any other
// write keeps the stubs on disk as they are. Pruning keeps every generation a kept pak image points at (the pak's
// .bak, .bak2 and pak_history/), so a restored backup still loads.
#if BLITZ_MODERN
#include <charconv>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif
#include "../main/platform_compat.h"

#include "recomp.h"
#include "blitz_hooks.h"
#include "blitz_savestate.h"
#include "librecomp/game.hpp"

namespace blitz {
    void post_notice(const std::string& msg);  // notices.cpp
    std::string active_pack_id();              // content_pack_tp.cpp
    std::vector<std::string> team_franchises();  // content_pack_tp.cpp
    uint64_t adv_season_seed();                // adv_season.cpp
    uint64_t adv_schedule_fingerprint();       // adv_engine.cpp
    void adv_set_season_seed(uint64_t seed);
    void pak_season_save_begin(int slot);  // controller_pak.cpp: the stub guard lets this slot change
    void pak_season_save_end();
    std::set<std::pair<uint64_t, uint32_t>> pak_kept_season_refs();
}

namespace {
    namespace fs = std::filesystem;

    constexpr uint32_t record = 0x802EF6F0, record_size = 0x774, slot_count = 4;
    constexpr const char stub_magic[8] = "BZ26ADV";
    constexpr const char file_magic[8] = "BLZADV1";
    constexpr uint32_t file_version = 2, engine_stage = 'B';  // 2: schedule fingerprint
    constexpr int team_count = 32;

    struct Stub {
        uint64_t id = 0;
        uint32_t gen = 0;
        uint32_t crc = 0;
    };

    struct Sidecar {
        uint64_t id = 0;
        uint32_t gen = 0;
        uint32_t stage = 0;
        uint64_t seed = 0;
        uint64_t schedule = 0;  // fingerprint of the league and schedule it was played on
        std::array<std::string, team_count> franchises;
        std::vector<uint8_t> core;  // record_size bytes
        uint32_t crc = 0;           // of everything before it
    };

    // What the last slot read found, per slot.
    std::array<std::optional<Stub>, slot_count> stub_in;               // Advanced slot with a sidecar
    std::array<std::optional<Sidecar>, slot_count> sidecar_in;
    std::array<std::optional<std::vector<uint8_t>>, slot_count> hidden;  // another format's season
    std::set<std::pair<uint64_t, uint32_t>> referenced;                 // (id, gen) any stub pointed at
    uint64_t season_id = 0;                                              // the season in play
    [[maybe_unused]] const bool registered = blitz::savestate_register_pod("adv_season_id", &season_id);  // save states
    std::optional<fs::path> pending;                                     // sidecar written, pak not yet

    gpr guest(uint32_t vaddr) { return (gpr)(int32_t)vaddr; }
    uint8_t rd8(uint8_t* rdram, uint32_t a) { return (uint8_t)MEM_B(0, guest(a)); }
    void wr8(uint8_t* rdram, uint32_t a, uint8_t v) { MEM_B(0, guest(a)) = (int8_t)v; }

    uint32_t crc32(const uint8_t* p, size_t n) {
        uint32_t c = 0xFFFFFFFFu;
        for (size_t i = 0; i < n; i++) {
            c ^= p[i];
            for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
        }
        return ~c;
    }

    void put(std::vector<uint8_t>& out, uint64_t v, int n) {
        for (int i = 0; i < n; i++) out.push_back((uint8_t)(v >> (8 * i)));
    }
    uint64_t get(const uint8_t* p, int n) {
        uint64_t v = 0;
        for (int i = 0; i < n; i++) v |= (uint64_t)p[i] << (8 * i);
        return v;
    }

    fs::path season_dir() {
        std::string pack = blitz::active_pack_id();
        return recomp::get_config_path() / "saves" / "advanced" / (pack.empty() ? "default" : pack) / "season";
    }

    std::string hex(uint64_t v) {
        char b[17];
        snprintf(b, sizeof(b), "%016llX", (unsigned long long)v);
        return b;
    }

    fs::path sidecar_path(uint64_t id, uint32_t gen) {
        return season_dir() / ("season-" + hex(id) + "-" + std::to_string(gen) + ".adv");
    }

    std::vector<uint8_t> encode(const Sidecar& s) {
        std::vector<uint8_t> out(file_magic, file_magic + 8);
        put(out, file_version, 4);
        put(out, s.stage, 4);
        put(out, s.id, 8);
        put(out, s.gen, 4);
        put(out, s.seed, 8);
        put(out, s.schedule, 8);
        for (const std::string& f : s.franchises) {
            char name[4] = {};
            std::memcpy(name, f.data(), std::min<size_t>(f.size(), 3));
            out.insert(out.end(), name, name + 4);
        }
        put(out, record_size, 4);
        out.insert(out.end(), s.core.begin(), s.core.end());
        put(out, crc32(out.data(), out.size()), 4);
        return out;
    }

    std::optional<Sidecar> decode(const std::vector<uint8_t>& d) {
        constexpr size_t head = 8 + 4 + 4 + 8 + 4 + 8 + 8 + team_count * 4 + 4;
        if (d.size() != head + record_size + 4 || std::memcmp(d.data(), file_magic, 8) != 0) return std::nullopt;
        if (crc32(d.data(), d.size() - 4) != get(d.data() + d.size() - 4, 4)) return std::nullopt;
        const uint8_t* p = d.data() + 8;
        if (get(p, 4) != file_version) return std::nullopt;
        Sidecar s;
        s.stage = (uint32_t)get(p + 4, 4);
        s.id = get(p + 8, 8);
        s.gen = (uint32_t)get(p + 16, 4);
        s.seed = get(p + 20, 8);
        s.schedule = get(p + 28, 8);
        p += 36;
        for (int t = 0; t < team_count; t++, p += 4) s.franchises[t] = std::string((const char*)p, strnlen((const char*)p, 4));
        if (get(p, 4) != record_size) return std::nullopt;
        s.core.assign(p + 4, p + 4 + record_size);
        s.crc = crc32(d.data(), d.size() - 4);
        return s;
    }

    std::optional<Sidecar> load_sidecar(uint64_t id, uint32_t gen, uint32_t crc) {
        std::ifstream f{sidecar_path(id, gen), std::ios::binary};
        if (!f) return std::nullopt;
        std::vector<uint8_t> d{std::istreambuf_iterator<char>(f), {}};
        auto s = decode(d);
        if (!s || s->id != id || s->gen != gen || s->crc != crc) return std::nullopt;
        return s;
    }

    // Write and sync a file under a temporary name, then rename it into place.
    bool write_file(const fs::path& path, const std::vector<uint8_t>& data) {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        fs::path tmp = path;
        tmp += ".tmp";
#ifdef _WIN32
        bool ok;
        {
            std::ofstream f{tmp, std::ios::binary | std::ios::trunc};
            f.write(reinterpret_cast<const char*>(data.data()), (std::streamsize)data.size());
            f.close();
            ok = !f.fail() && blitz::sync_file(tmp);
        }
#else
        int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) return false;
        bool ok = ::write(fd, data.data(), data.size()) == (ssize_t)data.size() && ::fsync(fd) == 0;
        ok = (::close(fd) == 0) && ok;
#endif
        if (ok) {
            blitz::replace_file(tmp, path, ec);
            ok = !ec;
        }
        if (!ok) fs::remove(tmp, ec);
        return ok;
    }

    std::optional<Stub> read_stub(uint8_t* rdram, uint32_t slot) {
        for (int i = 0; i < 8; i++) {
            if (rd8(rdram, slot + 4 + i) != (uint8_t)stub_magic[i]) return std::nullopt;
        }
        auto field = [&](uint32_t off, int n) {
            uint64_t v = 0;
            for (int i = 0; i < n; i++) v |= (uint64_t)rd8(rdram, slot + off + i) << (8 * i);
            return v;
        };
        return Stub{field(0xC, 8), (uint32_t)field(0x14, 4), (uint32_t)field(0x1C, 4)};
    }

    void write_stub(uint8_t* rdram, uint32_t slot, const Sidecar& s) {
        for (uint32_t i = 0; i < record_size; i++) wr8(rdram, slot + i, 0);
        wr8(rdram, slot + 0, 0xFF);
        wr8(rdram, slot + 1, 0xFF);
        for (int i = 0; i < 8; i++) wr8(rdram, slot + 4 + i, (uint8_t)stub_magic[i]);
        auto field = [&](uint32_t off, uint64_t v, int n) {
            for (int i = 0; i < n; i++) wr8(rdram, slot + off + i, (uint8_t)(v >> (8 * i)));
        };
        field(0xC, s.id, 8);
        field(0x14, s.gen, 4);
        wr8(rdram, slot + 0x18, s.core[0]);
        wr8(rdram, slot + 0x19, s.core[1]);
        field(0x1C, s.crc, 4);
    }

    // The slot list shows +0 (team) and +1 (week); 0xFF in +0 is an empty slot.
    void show_slots(uint8_t* rdram, uint32_t slots) {
        for (uint32_t k = 0; k < slot_count; k++) {
            uint32_t slot = slots + k * record_size;
            if (sidecar_in[k]) {
                wr8(rdram, slot + 0, sidecar_in[k]->core[0]);
                wr8(rdram, slot + 1, std::min<uint8_t>(sidecar_in[k]->core[1], 21));  // shown +1; "week 22" = over, as in the game
            }
            else if (hidden[k]) {
                wr8(rdram, slot + 0, 0xFF);
            }
        }
    }

    // Before the pak write: every slot back to what belongs on the pak.
    void restore_slots(uint8_t* rdram, uint32_t slots) {
        for (uint32_t k = 0; k < slot_count; k++) {
            uint32_t slot = slots + k * record_size;
            if (sidecar_in[k]) {
                wr8(rdram, slot + 0, 0xFF);
                wr8(rdram, slot + 1, 0xFF);
            }
            else if (hidden[k]) {
                for (uint32_t i = 0; i < record_size; i++) wr8(rdram, slot + i, (*hidden[k])[i]);
            }
        }
    }

    // Team indices follow the content pack's order. If it changed since the save, move every
    // team-indexed part of the record: the user's team, the stats rows and the playoff seeds.
    bool remap(std::vector<uint8_t>& core, const std::array<std::string, team_count>& saved) {
        const std::vector<std::string>& now = blitz::team_franchises();
        if (now.size() != team_count) return false;
        std::array<int, team_count> to{};
        bool same = true;
        for (int t = 0; t < team_count; t++) {
            auto it = std::find(now.begin(), now.end(), saved[t]);
            if (it == now.end()) return false;
            to[t] = (int)(it - now.begin());
            same = same && to[t] == t;
        }
        if (same) return true;
        auto stats_at = [](int t) { return t == 31 ? 0x704u : 4u + 28u * t; };
        std::vector<uint8_t> out = core;
        for (int t = 0; t < team_count; t++) std::memcpy(&out[stats_at(to[t])], &core[stats_at(t)], 28);
        if (core[0] < team_count) out[0] = (uint8_t)to[core[0]];
        for (uint32_t o = 0x758; o < 0x772; o++) {
            if (core[o] < team_count) out[o] = (uint8_t)to[core[o]];
        }
        core = std::move(out);
        fprintf(stderr, "[adv] season remapped to the current team order\n");
        return true;
    }

    // "season-<id>-<gen>.adv" -> gen; false for any other name (a stray file must not crash a save).
    bool parse_gen(const std::string& name, const std::string& prefix, uint32_t& gen) {
        if (name.rfind(prefix, 0) != 0) return false;
        const char* first = name.data() + prefix.size();
        const char* last = name.data() + name.size() - 4;  // before ".adv"
        if (first >= last) return false;
        auto [ptr, ec] = std::from_chars(first, last, gen);
        return ec == std::errc{} && ptr == last;
    }
}

namespace blitz {
    // New season (adv_season.cpp's reset hook).
    void adv_save_new_season() {
        std::random_device rd;
        season_id = ((uint64_t)rd() << 32) | rd();
    }
}

extern "C" {
    // 0x80267B84: the note has been read into the slot buffers (mode 3 = seasons).
    void blitz_adv_slots_read(uint8_t* rdram, int32_t status, int32_t mode, int32_t slots) {
        if (mode != 3 || status != 0) return;
        for (uint32_t k = 0; k < slot_count; k++) {
            uint32_t slot = (uint32_t)slots + k * record_size;
            stub_in[k].reset();
            sidecar_in[k].reset();
            hidden[k].reset();
            if (auto stub = read_stub(rdram, slot)) {
                auto sc = load_sidecar(stub->id, stub->gen, stub->crc);
                if (sc && sc->stage != engine_stage) {
                    fprintf(stderr, "[adv] slot %u: season %s is from an earlier Advanced version; not loadable\n", k, hex(stub->id).c_str());
                    continue;
                }
                if (sc && sc->schedule != blitz::adv_schedule_fingerprint()) {
                    // Results are stored by week and game number, so another schedule would misread them.
                    static bool told = false;
                    if (!told) {
                        told = true;
                        blitz::post_notice("A saved Advanced season was played on a different schedule or league than the current "
                                           "content pack has, so it can't be loaded. Its file is kept.");
                    }
                    fprintf(stderr, "[adv] slot %u: season %s was played on another schedule\n", k, hex(stub->id).c_str());
                    continue;
                }
                if (sc) {
                    stub_in[k] = stub;
                    sidecar_in[k] = std::move(sc);
                    referenced.insert({stub->id, stub->gen});
                }
                else {
                    fprintf(stderr, "[adv] slot %u: season %s gen %u has no matching file in %s\n", k, hex(stub->id).c_str(),
                            stub->gen, season_dir().string().c_str());
                }
            }
            else if (rd8(rdram, slot) != 0xFF) {
                std::vector<uint8_t> bytes(record_size);
                for (uint32_t i = 0; i < record_size; i++) bytes[i] = rd8(rdram, slot + i);
                hidden[k] = std::move(bytes);
            }
        }
        show_slots(rdram, (uint32_t)slots);
    }

    // 0x80267F48: LOAD of slot `index` (buffer `slot`), just before the game copies it into the record.
    void blitz_adv_before_load(uint8_t* rdram, int32_t index, int32_t slot) {
        if (index < 0 || index >= (int32_t)slot_count || !sidecar_in[index]) return;
        Sidecar& s = *sidecar_in[index];
        std::vector<uint8_t> core = s.core;
        if (!remap(core, s.franchises)) {
            blitz::post_notice("This season was saved with teams this game data doesn't have; it may not load correctly.");
        }
        for (uint32_t i = 0; i < record_size; i++) wr8(rdram, (uint32_t)slot + i, core[i]);
        season_id = s.id;
        blitz::adv_set_season_seed(s.seed);
        fprintf(stderr, "[adv] loading season %s gen %u from slot %d\n", hex(s.id).c_str(), s.gen, index);
    }

    // 0x8026807C: SAVE to slot `index`; the record has just been copied into its buffer.
    void blitz_adv_before_write(uint8_t* rdram, int32_t index, int32_t slots) {
        if (index < 0 || index >= (int32_t)slot_count) return;
        uint32_t slot = (uint32_t)slots + (uint32_t)index * record_size;
        Sidecar s;
        s.id = season_id;
        s.stage = engine_stage;
        s.seed = blitz::adv_season_seed();
        s.schedule = blitz::adv_schedule_fingerprint();
        const std::vector<std::string>& teams = blitz::team_franchises();
        for (int t = 0; t < team_count && t < (int)teams.size(); t++) s.franchises[t] = teams[t];
        s.core.resize(record_size);
        for (uint32_t i = 0; i < record_size; i++) s.core[i] = rd8(rdram, slot + i);
        // Next generation of this season: one past the newest on disk.
        std::error_code ec;
        std::string prefix = "season-" + hex(s.id) + "-";
        for (const auto& e : fs::directory_iterator(season_dir(), ec)) {
            std::string n = e.path().filename().string();
            uint32_t gen = 0;
            if (e.path().extension() == ".adv" && parse_gen(n, prefix, gen)) {
                s.gen = std::max<uint32_t>(s.gen, gen + 1);
            }
        }
        std::vector<uint8_t> data = encode(s);
        s.crc = crc32(data.data(), data.size() - 4);
        fs::path path = sidecar_path(s.id, s.gen);
        restore_slots(rdram, (uint32_t)slots);
        if (!write_file(path, data)) {
            // Leave the slot as it was on the pak (the game's own write then rewrites the same data).
            blitz::post_notice("Couldn't save the season file in " + season_dir().string() + ". The season wasn't saved.");
            if (stub_in[index] && sidecar_in[index]) {
                write_stub(rdram, slot, *sidecar_in[index]);
            }
            else if (hidden[index]) {
                for (uint32_t i = 0; i < record_size; i++) wr8(rdram, slot + i, (*hidden[index])[i]);
            }
            else {
                for (uint32_t i = 0; i < record_size; i++) wr8(rdram, slot + i, i < 2 ? 0xFF : 0);
            }
            pending.reset();
            return;
        }
        write_stub(rdram, slot, s);
        blitz::pak_season_save_begin(index);  // until blitz_adv_after_write: the game's write of this Save
        pending = path;
        stub_in[index] = Stub{s.id, s.gen, s.crc};
        hidden[index].reset();
        sidecar_in[index] = std::move(s);
    }

    // 0x8026809C: the pak write returned `status` (0 = written).
    void blitz_adv_after_write(uint8_t* rdram, int32_t status, int32_t slots) {
        blitz::pak_season_save_end();
        std::error_code ec;
        if (pending) {
            if (status != 0) {
                // Keep the file: the pak is written block by block, so the slot may already point at it. The
                // next good save prunes it if nothing does.
                fprintf(stderr, "[adv] pak write failed (%d); kept %s\n", status, pending->filename().string().c_str());
            }
            else {
                // Keep the generations any slot points at and the newest two of this season.
                const Sidecar* saved = nullptr;
                for (auto& sc : sidecar_in) {
                    if (sc && pending->filename() == sidecar_path(sc->id, sc->gen).filename()) saved = &*sc;
                }
                if (saved != nullptr) {
                    referenced.insert({saved->id, saved->gen});
                    for (const auto& r : blitz::pak_kept_season_refs()) referenced.insert(r);
                    std::string prefix = "season-" + hex(saved->id) + "-";
                    for (const auto& e : fs::directory_iterator(season_dir(), ec)) {
                        std::string n = e.path().filename().string();
                        uint32_t gen = 0;
                        if (e.path().extension() != ".adv" || !parse_gen(n, prefix, gen)) continue;
                        if (gen + 1 < saved->gen && !referenced.count({saved->id, gen})) fs::remove(e.path(), ec);
                    }
                    fprintf(stderr, "[adv] saved season %s gen %u\n", hex(saved->id).c_str(), saved->gen);
                }
            }
            pending.reset();
        }
        show_slots(rdram, (uint32_t)slots);
    }
}
namespace {
    // The newest generation of a season in this pack's season folder, or -1 for none.
    int64_t newest_gen(uint64_t id) {
        std::error_code ec;
        int64_t newest = -1;
        std::string prefix = "season-" + hex(id) + "-";
        for (const auto& e : fs::directory_iterator(season_dir(), ec)) {
            uint32_t gen = 0;
            if (e.path().extension() == ".adv" && parse_gen(e.path().filename().string(), prefix, gen)) {
                newest = std::max<int64_t>(newest, gen);
            }
        }
        return newest;
    }

    // A save state keeps the season it was taken in and that season's newest generation on disk at the time. Loading
    // one from before a later Season save is a rollback, as switching the N64 off without saving is: the season files
    // stay as they are, and saving the season from here replaces the later save. The player is asked before it loads,
    // Load It or Cancel (team, 2026-11-27: it was said after, with only OK) (docs/SAVE_STATES.md, "Seasons").
    [[maybe_unused]] const bool gen_registered = (blitz::savestate_register(
        "adv_season_gen",
        [] {
            uint64_t id = season_id;
            int64_t gen = id != 0 ? newest_gen(id) : -1;
            blitz::StateBlob b(sizeof id + sizeof gen);
            std::memcpy(b.data(), &id, sizeof id);
            std::memcpy(b.data() + sizeof id, &gen, sizeof gen);
            return b;
        },
        [](const blitz::StateBlob& b) {
            if (b.size() != sizeof(uint64_t) + sizeof(int64_t)) return;
            uint64_t id = 0;
            int64_t then = -1;
            std::memcpy(&id, b.data(), sizeof id);
            std::memcpy(&then, b.data() + sizeof id, sizeof then);
            if (id == 0) return;
            int64_t now = newest_gen(id);
            if (now > then) {
                fprintf(stderr, "[adv] state from season %s gen %lld; the newest on disk is gen %lld\n", hex(id).c_str(),
                        (long long)then, (long long)now);
            }
        },
        [](const blitz::StateBlob& b) -> std::string {
            if (b.size() != sizeof(uint64_t) + sizeof(int64_t)) return "";
            uint64_t id = 0;
            int64_t then = -1;
            std::memcpy(&id, b.data(), sizeof id);
            std::memcpy(&then, b.data() + sizeof id, sizeof then);
            if (id == 0 || newest_gen(id) <= then) return "";
            return "?This save state is from before your last season save. If you load it and then save the season, "
                   "your later season save is replaced.";
        }), true);
}
#endif
