// Unit tests for Save Sync's bundle (src/game/save_sync_core.cpp, docs/SAVE_SYNC.md). Run with `make test-sync`.
// Each test builds app folders in a temporary directory: paks with a Blitz note and Advanced stubs, season files in
// adv_save.cpp's format, settings, save states; then exports, imports, stages and applies as the app does.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <unistd.h>

#include "../src/game/pak_guard.h"
#include "../src/game/save_sync_core.h"

namespace fs = std::filesystem;
using namespace blitz::savesync;
using Bytes = std::vector<uint8_t>;

static int failures = 0, checks = 0;
#define CHECK(cond)                                                         \
    do {                                                                    \
        checks++;                                                           \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                     \
        }                                                                   \
    } while (0)

static const uint64_t season_id = 0x0EE109E6D63E3BF3ull;
static const std::string adv_folder = "original/advanced/blitz2026";

static uint32_t crc32(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

static void put(Bytes& out, uint64_t v, int n) {
    for (int i = 0; i < n; i++) out.push_back((uint8_t)(v >> (8 * i)));
}

// A season file as adv_save.cpp encodes it; `salt` makes two files of one generation differ.
static Bytes adv(uint64_t id, uint32_t gen, uint8_t team, uint8_t week, int salt) {
    Bytes out{'B', 'L', 'Z', 'A', 'D', 'V', '1', 0};
    put(out, 2, 4);
    put(out, 'B', 4);
    put(out, id, 8);
    put(out, gen, 4);
    put(out, 0x5EED, 8);
    put(out, 0xF1F1, 8);
    for (int t = 0; t < 32; t++) {
        char name[4] = {'T', (char)('0' + t / 10), (char)('0' + t % 10), 0};
        if (t == 3) std::memcpy(name, "LAR", 4);
        out.insert(out.end(), name, name + 4);
    }
    put(out, 0x774, 4);
    for (uint32_t i = 0; i < 0x774; i++) out.push_back(i == 0 ? team : i == 1 ? week : (uint8_t)(i * 7 + salt));
    put(out, crc32(out.data(), out.size()), 4);
    return out;
}

static uint32_t adv_crc(const Bytes& b) { return crc32(b.data(), b.size() - 4); }

// A pak with one Blitz note on pages 5..127 (as the game makes it), its slots empty (pak_guard_test.cpp's).
static Bytes blitz_pak() {
    Bytes p(blitz::pakimg::pak_size, 0);
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

static void put_stub(Bytes& p, int slot, uint64_t id, uint32_t gen, uint8_t team, uint8_t week, uint32_t crc) {
    uint32_t s = 0x500 + 0xF00 + slot * 0x774;
    std::memset(&p[s], 0, 0x20);
    p[s] = p[s + 1] = 0xFF;
    std::memcpy(&p[s + 4], "BZ26ADV", 8);
    for (int i = 0; i < 8; i++) p[s + 0xC + i] = (uint8_t)(id >> (8 * i));
    for (int i = 0; i < 4; i++) p[s + 0x14 + i] = (uint8_t)(gen >> (8 * i));
    p[s + 0x18] = team;
    p[s + 0x19] = week;
    for (int i = 0; i < 4; i++) p[s + 0x1C + i] = (uint8_t)(crc >> (8 * i));
}

static fs::path root;

static Env env_for(const std::string& name, bool pack_here = true) {
    Env e;
    e.app = root / name;
    e.version = "1.0.5";
    e.build = "900";
    e.platform = "Test";
    e.pack_installed = [pack_here](const std::string&) { return pack_here; };
    fs::create_directories(e.app / "saves");
    return e;
}

static fs::path dir_of(const Env& e, const std::string& folder) { return e.app / "saves" / folder; }
// Where the game keeps the Advanced season files: one folder per pack, not next to Modern-on-original's pak
// (saves/original/advanced/<pack>/), so the tests put them where adv_save.cpp does (2026-11-27).
static fs::path season_dir(const Env& e) { return e.app / "saves" / "advanced" / "blitz2026" / "season"; }

// An Advanced season save as the game makes it: the season file, then the pak's stub (adv_save.cpp's order). The pak
// before is kept in pak_history/ as controller_pak.cpp does before a write.
static Bytes save_season(const Env& e, uint32_t gen, int salt, uint8_t week = 5) {
    fs::path d = dir_of(e, adv_folder);
    Bytes a = adv(season_id, gen, 3, week, salt);
    write_file(season_dir(e) / adv_name(season_id, gen), a);
    bool had = false;
    Bytes pak = read_file(d / pak_name, &had);
    if (!had) pak = blitz_pak();
    else write_file(d / "pak_history" / ("blitz2000_pak1-20261127-1000" + std::to_string(gen) + "-" + std::to_string(salt) + ".mpk"), pak);
    put_stub(pak, 0, season_id, gen, 3, week, adv_crc(a));
    write_file(d / pak_name, pak);
    return a;
}

static FolderPlan* folder_in(Plan& p, const std::string& folder) {
    for (FolderPlan& f : p.folders) {
        if (f.folder == folder) return &f;
    }
    return nullptr;
}

// Import `zip` into `e` taking whatever read_bundle says to take (plus `force` folders), stage and apply.
static Plan import_into(const Env& e, const Bytes& zip, bool force = false) {
    Plan p = read_bundle(e, zip);
    if (!p.ok) return p;
    for (FolderPlan& f : p.folders) {
        if (force && f.verdict != Verdict::Same) f.take = true;
    }
    std::string why;
    CHECK(stage(e, p, true, why));
    for (const fs::path& d : staged_folders(e, p)) CHECK(apply_staged(d, write_pak_with_history) == Applied::Done);
    record_import(e, p);
    return p;
}

int main() {
    root = fs::temp_directory_path() / ("save_sync_test_" + std::to_string((long)getpid()));
    fs::remove_all(root);

    // 1. Export: paks, season files and the two league settings; never save states, quit.*, history, .bak or per-device settings.
    {
        Env a = env_for("export");
        save_season(a, 5, 1);
        save_season(a, 6, 2);
        fs::path d = dir_of(a, adv_folder);
        write_file(d / "blitz2000_pak1.mpk.bak", blitz_pak());
        write_file(d / "quit.state", Bytes(100, 1));
        write_file(a.app / "savestates" / "slot1.state", Bytes(100, 2));
        write_file(a.app / "savestates" / "quit.state", Bytes(100, 3));
        write_file(a.app / "saves" / "quit.state", Bytes(10, 3));
        write_file(a.app / "saves" / "slot2.state", Bytes(10, 3));
        write_file(a.app / "rules.json", Bytes{'{', '}'});
        write_file(a.app / "season.json", Bytes{'{', '}'});
        write_file(a.app / "controls.json", Bytes{'{', '}'});
        write_file(a.app / "general.json", Bytes{'{', '}'});
        write_file(a.app / "saves" / "packs" / "blitz2026" / pak_name, Bytes(blitz::pakimg::pak_size, 0));  // no note on it: left out
        ExportResult r = make_bundle(a);
        CHECK(r.ok);
        CHECK(r.paks == 1 && r.seasons == 2);
        for (const std::string& n : r.entries) {
            CHECK(n.find(".state") == std::string::npos);
            CHECK(n.find("quit") == std::string::npos);
            CHECK(n.find("savestates") == std::string::npos);
            CHECK(n.find("pak_history") == std::string::npos);
            CHECK(n.find(".bak") == std::string::npos);
            CHECK(n.find("controls") == std::string::npos && n.find("general") == std::string::npos);
            CHECK(n.find("packs/blitz2026") == std::string::npos);
        }
        CHECK(std::find(r.entries.begin(), r.entries.end(), "saves/" + adv_folder + "/" + pak_name) != r.entries.end());
        CHECK(std::find(r.entries.begin(), r.entries.end(), "settings/rules.json") != r.entries.end());
        // The device label is generated, and kept.
        std::string label = device_label(a);
        CHECK(label.rfind("Test ", 0) == 0 && label.size() == 9);
        CHECK(device_label(a) == label);
        // Nothing to export: said so.
        Env empty = env_for("export-empty");
        ExportResult none = make_bundle(empty);
        CHECK(!none.ok && !none.why.empty());
    }

    // 2. A first import into a device with no saves: New, put in place as a matching pair; again: Same (a no-op).
    {
        Env a = env_for("first-a"), b = env_for("first-b");
        save_season(a, 5, 1);
        write_file(a.app / "rules.json", Bytes{'{', '"', 'x', '"', ':', '1', '}'});
        Bytes zip = make_bundle(a).zip;
        Plan p = import_into(b, zip);
        CHECK(p.ok && p.folders.size() == 1 && p.folders[0].verdict == Verdict::New);
        CHECK(pair_ok(dir_of(b, adv_folder)));
        CHECK(read_file(dir_of(b, adv_folder) / pak_name) == read_file(dir_of(a, adv_folder) / pak_name));
        CHECK(!has_staged(dir_of(b, adv_folder)));
        // Settings wait for the next start, then go in with the old one kept.
        write_file(b.app / "rules.json", Bytes{'{', '}'});
        Plan again = read_bundle(b, zip);
        CHECK(again.ok && again.folders[0].verdict == Verdict::Same && !again.folders[0].take);
        // The first import already staged their rules.json for the next start: that's what this device will have, so it
        // isn't offered again (save_sync_core.cpp settings_file; Cloud Sync relies on it to not bounce settings back).
        CHECK(again.settings.count("rules.json") == 0);
        std::string why;
        CHECK(stage(b, again, true, why));
        CHECK(apply_staged_settings(b.app) == 1);
        CHECK(read_file(b.app / "rules.json") == read_file(a.app / "rules.json"));
        CHECK(fs::exists(b.app / "rules.json.before-sync"));
        CHECK(!fs::exists(b.app / staging));
    }

    // 3. Same generation, same CRC: nothing to do, even with the pak's own bytes the same.
    {
        Env a = env_for("same-a"), b = env_for("same-b");
        save_season(a, 7, 1);
        Bytes zip = make_bundle(a).zip;
        import_into(b, zip);
        Plan p = read_bundle(b, zip);
        CHECK(p.ok && p.folders[0].verdict == Verdict::Same && !p.folders[0].take);
    }

    // 4. Newer: B took A's season at gen 5, A played on to gen 7; B takes it. And A's lineage is enough when gen 5's
    // file was pruned on A (the pak's history still names it).
    {
        Env a = env_for("newer-a"), b = env_for("newer-b");
        save_season(a, 5, 1);
        import_into(b, make_bundle(a).zip);
        save_season(a, 6, 2);
        save_season(a, 7, 3, 9);
        fs::remove(season_dir(a) / adv_name(season_id, 5));
        Plan p = import_into(b, make_bundle(a).zip);
        CHECK(p.ok && p.folders[0].verdict == Verdict::Newer && p.folders[0].take);
        CHECK(pair_ok(dir_of(b, adv_folder)));
        CHECK(p.folders[0].theirs == "LAR, week 10");
    }

    // 5. Older is refused by default: A exported at gen 7, saved gen 8, then opens the old file.
    {
        Env a = env_for("older-a");
        save_season(a, 7, 1);
        Bytes old = make_bundle(a).zip;
        save_season(a, 8, 2);
        Bytes before = read_file(dir_of(a, adv_folder) / pak_name);
        Plan p = read_bundle(a, old);
        CHECK(p.ok && p.folders[0].verdict == Verdict::Older && !p.folders[0].take);
        std::string why;
        CHECK(stage(a, p, false, why));
        CHECK(!has_staged(dir_of(a, adv_folder)));
        CHECK(read_file(dir_of(a, adv_folder) / pak_name) == before);
        // And on another device that had the newer one: the old file is older there too.
        Env b = env_for("older-b");
        import_into(b, make_bundle(a).zip);
        Plan q = read_bundle(b, old);
        CHECK(q.ok && q.folders[0].verdict == Verdict::Older);
    }

    // 6. Conflicts: the same generation with different CRCs; and both sides going on from one parent.
    {
        Env a = env_for("conflict-a"), b = env_for("conflict-b");
        save_season(a, 5, 1);
        import_into(b, make_bundle(a).zip);
        save_season(a, 6, 2);
        save_season(b, 6, 3);
        Plan p = read_bundle(b, make_bundle(a).zip);
        CHECK(p.ok && p.folders[0].verdict == Verdict::Conflict && !p.folders[0].take);
        save_season(a, 7, 4);
        Plan q = read_bundle(b, make_bundle(a).zip);
        CHECK(q.ok && q.folders[0].verdict == Verdict::Conflict);
        // Two devices that never synced, each with its own season pak: a conflict, not a silent overwrite.
        Env c = env_for("conflict-c"), d = env_for("conflict-d");
        save_season(c, 1, 5);
        Bytes pak = blitz_pak();
        Bytes other = adv(0x1234, 3, 1, 1, 6);
        write_file(season_dir(d) / adv_name(0x1234, 3), other);
        put_stub(pak, 1, 0x1234, 3, 1, 1, adv_crc(other));
        write_file(dir_of(d, adv_folder) / pak_name, pak);
        Plan r = read_bundle(d, make_bundle(c).zip);
        CHECK(r.ok && r.folders[0].verdict == Verdict::Conflict);
        // Classic and Modern paks (no generations): both changed since they last synced is a conflict too.
        Env e = env_for("conflict-e"), f = env_for("conflict-f");
        Bytes ce = blitz_pak(), cf = blitz_pak();
        ce[0x500 + 0x10] = 1;
        cf[0x500 + 0x10] = 2;
        write_file(dir_of(e, "packs/blitz2026") / pak_name, ce);
        write_file(dir_of(f, "packs/blitz2026") / pak_name, cf);
        Plan s = read_bundle(f, make_bundle(e).zip);
        CHECK(s.ok && s.folders[0].verdict == Verdict::Conflict);
        // ... and after one sync, the side that didn't change takes the other's next save as newer.
        import_into(f, make_bundle(e).zip, true);
        ce[0x500 + 0x11] = 3;
        write_file(dir_of(e, "packs/blitz2026") / pak_name, ce);
        Plan t = read_bundle(f, make_bundle(e).zip);
        CHECK(t.ok && t.folders[0].verdict == Verdict::Newer);
    }

    // 7. A stub and its season file are one save: a pak whose stub has no matching file is refused, and nothing changes.
    {
        Env a = env_for("mismatch-a"), b = env_for("mismatch-b");
        save_season(a, 5, 1);
        Bytes good = adv(season_id, 5, 3, 5, 99);  // gen 5 rewritten with other bytes: the stub's CRC no longer matches
        write_file(season_dir(a) / adv_name(season_id, 5), good);
        Plan p = read_bundle(b, make_bundle(a).zip);
        CHECK(!p.ok && !p.why.empty());
        fs::remove(season_dir(a) / adv_name(season_id, 5));  // the file gone altogether
        Plan q = read_bundle(b, make_bundle(a).zip);
        CHECK(!q.ok);
        CHECK(!fs::exists(dir_of(b, adv_folder)));
        std::string why;
        CHECK(!pair_ok(dir_of(a, adv_folder), &why) && !why.empty());
    }

    // 8. A bundle for a pack that isn't installed is refused, naming the pack.
    {
        Env a = env_for("pack-a"), b = env_for("pack-b", false);
        save_season(a, 5, 1);
        Plan p = read_bundle(b, make_bundle(a).zip);
        CHECK(!p.ok && p.why.find("blitz2026") != std::string::npos);
        CHECK(!fs::exists(dir_of(b, adv_folder)));
    }

    // 9. A crash between the season files and the pak: the old pair is untouched until the pak goes in, the next start
    // finishes it, and a replaced season file is kept (and comes back with Controller Pak Backups).
    {
        Env a = env_for("crash-a"), b = env_for("crash-b");
        save_season(a, 5, 1);
        import_into(b, make_bundle(a).zip);
        save_season(a, 6, 2);
        Bytes mine6 = save_season(b, 6, 3);  // the same generation on both: B's gen 6 file is replaced if B takes A's
        Bytes b_pak = read_file(dir_of(b, adv_folder) / pak_name);
        Plan p = read_bundle(b, make_bundle(a).zip);
        CHECK(p.ok && p.folders[0].verdict == Verdict::Conflict);
        p.folders[0].take = true;  // the player picked theirs
        std::string why;
        CHECK(stage(b, p, false, why));
        fs::path d = dir_of(b, adv_folder);
        // The pak write never happens (the app is killed there).
        CHECK(apply_staged(d, [](const fs::path&, const Bytes&) { return false; }) == Applied::Failed);
        CHECK(read_file(d / pak_name) == b_pak);
        CHECK(has_staged(d));
        // Next start, before the game reads the pak.
        CHECK(apply_staged(d, write_pak_with_history) == Applied::Done);
        CHECK(pair_ok(d));
        CHECK(read_file(d / pak_name) == read_file(dir_of(a, adv_folder) / pak_name));
        // Put back B's own pak from the history: its season file comes back too.
        write_pak_with_history(d / pak_name, b_pak);
        CHECK(!pair_ok(d));
        CHECK(restore_replaced_seasons(d, b_pak) == 1);
        CHECK(pair_ok(d));
        CHECK(read_file(season_dir(b) / adv_name(season_id, 6)) == mine6);

        // A new generation (no file replaced): cut off after the season files, the old pair still matches.
        Env c = env_for("crash-c");
        save_season(c, 4, 7);
        Plan q = read_bundle(c, make_bundle(a).zip);
        CHECK(q.ok);
        q.folders[0].take = true;
        CHECK(stage(c, q, false, why));
        fs::path dc = dir_of(c, adv_folder);
        CHECK(apply_staged(dc, [](const fs::path&, const Bytes&) { return false; }) == Applied::Failed);
        CHECK(pair_ok(dc));
        CHECK(apply_staged(dc, write_pak_with_history) == Applied::Done);
        CHECK(pair_ok(dc));

        // Cut off while staging (no ready mark): dropped at the next start, nothing changed.
        Env e = env_for("crash-e");
        save_season(e, 2, 8);
        Bytes e_pak = read_file(dir_of(e, adv_folder) / pak_name);
        write_file(dir_of(e, adv_folder) / staging / pak_name, read_file(dir_of(a, adv_folder) / pak_name));
        CHECK(apply_staged(dir_of(e, adv_folder), write_pak_with_history) == Applied::Nothing);
        CHECK(!has_staged(dir_of(e, adv_folder)));
        CHECK(read_file(dir_of(e, adv_folder) / pak_name) == e_pak);
    }

    // 10. A damaged or foreign file: refused before anything is written.
    {
        Env a = env_for("damaged-a"), b = env_for("damaged-b");
        save_season(a, 5, 1);
        Bytes zip = make_bundle(a).zip;
        CHECK(!read_bundle(b, Bytes{'P', 'K', 3, 4}).ok);
        CHECK(!read_bundle(b, Bytes(100, 7)).ok);
        Bytes bad = zip;
        bad[bad.size() / 2] ^= 0xFF;
        CHECK(!read_bundle(b, bad).ok);
        CHECK(!fs::exists(dir_of(b, adv_folder)));
    }

    fs::remove_all(root);
    printf("save_sync: %d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
