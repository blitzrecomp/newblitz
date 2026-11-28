// Controller Pak (the only save medium Blitz uses). The game's own libultra Controller Pak file system
// (osPfsInitPak, osPfsFindFile, osPfsReadWriteFile, ...) is recompiled from the ROM, so the pak
// layout is exactly what real hardware writes. Only the hardware layer is replaced here:
//   __osContRamRead / __osContRamWrite  32-byte block I/O to pak memory over the SI (address = block)
//   __osPfsGetStatus                    "is a pak inserted?"
//   osMotorInit                         Rumble Pak probe; must fail with PFS_ERR_DEVICE when a
//                                       Controller Pak is inserted, as on hardware, or the game takes
//                                       every controller to have a Rumble Pak and never saves
// Each pak is a standard 32 KB .mpk image in <app folder>/saves/, interchangeable with emulators.
// Writes are checked and synced per block; a failed write is undone in memory and reported to the
// game. A file that exists but isn't a 32 KB image is moved aside, never overwritten, and the
// image as of each launch is kept as <name>.mpk.bak (and the one before as .bak2).
// Blank paks are formatted like mupen64plus's format_mempak (GPL-3.0).
//
// Since 1.0.4 (an Advanced season's stub vanished from the team's pak on 2026-11-26, docs/ADVANCED_SEASON.md §3.6):
//   - the stub guard (pak_guard.h): in Advanced, a block write that would clear or change a season stub keeps the
//     stub as it is on disk, unless it's the player's own Save to that slot (adv_save.cpp brackets the game's write
//     with pak_season_save_begin/end) or the note itself was deleted. Each kept stub prints a [pak] line;
//   - the history: pak_history/<name>-YYYYMMDD-HHMMSS.mpk beside the pak, a copy at each launch and before each
//     write that changes the image (one per burst of writes), skipping one the same as the newest. The newest 20
//     stay, and the newest of each of the last 14 days;
//   - Settings → General → Controller Pak Backups (src/main/pak_backups.cpp) lists them and puts one back. With the
//     game running that goes through pak_restore (a mark in the pak's folder) and a restart: the pak is replaced
//     here, at the next start's first pak access, before the game reads it.
//
// The root cause of that wipe (2026-11-26, docs/ADVANCED_SEASON.md §3.6): libultra keeps a copy of the pak's page table
// (the inode cache, __osPfsRWInode) in RDRAM, so a save state brings back the copy from when it was taken. The real
// __osPfsGetStatus empties it at every osPfsInitPak; ours didn't. A Nov 14 state held the page table of a pak with no
// note yet, so osPfsChecker, run by the next osPfsInitPak, took the Blitz note's pages for free ones and deleted the
// note; the game said BLITZ NOTE NOT FOUND!, and CREATE made a new empty note in its place. Now blitz_PfsGetStatus
// empties the cache as libultra does, and pak_state_restored (savestate.cpp, after each restore) empties it too and
// clears the game's "pak ready" flags, so the game opens the pak afresh, as it is on disk now, before it uses it again.
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <random>
#include <string>
#include "../main/platform_compat.h"
#include <utility>
#include <ctime>
#include <vector>

#include "recomp.h"
#include "librecomp/game.hpp"
#include "ultramodern/ultra64.h"
#include "pak_guard.h"
#include "save_sync_core.h"
#include "blitz_pak_backups.h"

namespace blitz {
    void post_notice(const std::string& msg);  // notices.cpp
    std::string active_pack_id();              // content_pack.cpp: "" for Classic
    bool controller_connected(int controller_num);  // main.cpp (controllers.cpp)
#if BLITZ_MODERN
    bool advanced_mode();                      // adv_mode.cpp
#endif
}

namespace {
    constexpr size_t pak_size = 0x8000;
    constexpr uint32_t block_count = pak_size / 32;
    constexpr int32_t PFS_ERR_NOPACK = 1;
    constexpr int32_t PFS_ERR_CONTRFAIL = 4;  // __osContRamWrite: the transfer failed
    constexpr int32_t PFS_ERR_DEVICE = 11;

    enum class Accessory { None, ControllerPak, RumblePak };

    // Player 1 has a Controller Pak (saves); players 2-4 have a Rumble Pak when their controller is
    // connected (controllers.cpp routes the motor to that controller).
    Accessory accessory(int channel) {
        if (channel == 0) return Accessory::ControllerPak;
        return blitz::controller_connected(channel) ? Accessory::RumblePak : Accessory::None;
    }

    struct Pak {
        std::vector<uint8_t> data;
        std::filesystem::path path;
        bool loaded = false;
        bool usable = false;  // false: the image couldn't be read or created; the game sees no pak
    };
    std::array<Pak, 4> paks;
    std::mutex paks_mutex;

    // The stub guard (pak_guard.h): on for Advanced's paks only (Classic and Modern have no stubs, and must be free
    // to save over a slot that holds one, as an emulator would).
    bool guard_on = false;
    std::atomic<int> season_save_slot{-1};                  // adv_save.cpp: a season Save is writing this slot
    std::set<std::pair<uint64_t, uint32_t>> released;       // stubs whose note was deleted this session
    // The history: the newest copy's bytes (to skip a copy that's the same), and when the last changing write was
    // (a write of the whole note is about a thousand blocks: one copy before them, not one per block).
    std::map<std::string, std::vector<uint8_t>> history_newest;  // by pak path
    std::chrono::steady_clock::time_point last_change{};
    bool any_change = false;
    std::atomic<uint64_t> change_bursts{0};                // Cloud Sync (save_sync.cpp): one per burst of changing writes
    constexpr auto burst_gap = std::chrono::seconds(3);
    constexpr const char* restore_mark = "pak_restore";     // in the pak's folder: the history file to put back

    void notify(const std::string& msg) {
        blitz::post_notice("Controller Pak: " + msg);
    }

    // The pak as the game and libultra remember it in RDRAM (boot segment: the same addresses in every flavour).
    constexpr uint32_t inode_cache_channel = 0x8002CDD0;  // __osPfsInodeCacheChannel (s32)
    constexpr uint32_t inode_cache_bank = 0x8002CDD4;     // __osPfsInodeCacheBank (u8)
    constexpr uint32_t inode_cache = 0x8005E850;          // __osPfsInodeCache: the cached bank's page table, 256 bytes
    constexpr uint8_t no_cached_bank = 250;               // what __osPfsGetStatus sets: no bank is cached
    constexpr uint32_t pak_ready = 0x8002B080;            // the game's s8 per port: 1 once osPfsInitPak succeeded there
    constexpr uint32_t game_pfs = 0x8005E390;             // the game's OSPfs for port 1 (0x68 per port): status +0, id +0xC
    constexpr uint32_t pfs_id = 0xC;
    constexpr uint32_t inode_page = 0x100;                // page 1: bank 0's page table on the pak
    constexpr uint32_t first_data_page = 5, pages = 128;
    constexpr uint16_t page_free = 0x0003;                // PFS_PAGE_NOT_USED

    gpr ram(uint32_t vram) { return (gpr)(int32_t)vram; }

    void drop_inode_cache(uint8_t* rdram) {
        MEM_B(0, ram(inode_cache_bank)) = (int8_t)no_cached_bank;
        MEM_W(0, ram(inode_cache_channel)) = -1;
    }

    // BLITZ_PAK_STALE=1 (tests only): keep the 1.0.3 behaviour, to show what a stale pak view does.
    bool keep_stale_view() {
        static const bool on = [] {
            const char* v = getenv("BLITZ_PAK_STALE");
            return v != nullptr && v[0] == '1';
        }();
        return on;
    }

    void store_be16(uint8_t* p, uint16_t v) { p[0] = v >> 8; p[1] = v & 0xFF; }
    void store_be32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (v >> (24 - 8 * i)) & 0xFF; }

    // A blank, formatted 32 KB pak (mupen64plus format_mempak): page 0 holds the ID block at 0x20 plus
    // backups at 0x60, 0x80 and 0xC0; pages 1 and 2 hold the index table (and its backup) with pages
    // 5..127 free (0x0003) and a checksum byte; everything else is zero.
    std::vector<uint8_t> formatted_pak() {
        std::vector<uint8_t> mem(pak_size, 0);
        uint8_t* id = mem.data() + 0x20;
        std::random_device rd;
        for (int i = 0; i < 6; i++) {
            store_be32(id + 4 * i, rd());
        }
        store_be16(id + 0x18, 0x0001);  // device id
        id[0x1A] = 0x01;                // banks
        id[0x1B] = 0x00;                // version
        uint16_t sum = 0;
        for (int i = 0; i < 0x1C; i += 2) {
            sum += (id[i] << 8) | id[i + 1];
        }
        store_be16(id + 0x1C, sum);
        store_be16(id + 0x1E, 0xFFF2 - sum);
        for (int backup : {0x60, 0x80, 0xC0}) {
            std::copy(id, id + 32, mem.data() + backup);
        }
        uint8_t* index = mem.data() + 0x100;
        for (int page = 5; page < 128; page++) {
            store_be16(index + 2 * page, 0x0003);
        }
        uint8_t check = 0;
        for (int i = 2 * 5; i < 2 * 128; i++) {
            check += index[i];
        }
        index[1] = check;
        std::copy(index, index + 0x100, mem.data() + 0x200);
        return mem;
    }

    // Write a whole image via a temporary file and rename, so a failure never truncates `path`.
    bool write_image(const std::filesystem::path& path, const std::vector<uint8_t>& data) {
        std::filesystem::path tmp = path;
        tmp += ".tmp";
        FILE* f = fopen(tmp.string().c_str(), "wb");
        if (f == nullptr) {
            return false;
        }
        bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
        ok = (fflush(f) == 0) && ok;
        ok = (fsync(fileno(f)) == 0) && ok;
        ok = (fclose(f) == 0) && ok;
        std::error_code ec;
        if (ok) {
            blitz::replace_file(tmp, path, ec);
            ok = !ec;
        }
        if (!ok) {
            std::filesystem::remove(tmp, ec);
        }
        return ok;
    }

    bool read_image(const std::filesystem::path& path, std::vector<uint8_t>& out) {
        std::error_code ec;
        auto size = std::filesystem::file_size(path, ec);
        if (ec || size != pak_size) {
            return false;
        }
        FILE* f = fopen(path.string().c_str(), "rb");
        if (f == nullptr) {
            return false;
        }
        out.assign(pak_size, 0);
        bool ok = fread(out.data(), 1, pak_size, f) == pak_size;
        fclose(f);
        return ok;
    }

    std::string timestamp() {
        char buf[32];
        time_t now = time(nullptr);
        strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", localtime(&now));
        return buf;
    }

    // Where this run's paks live. Each content pack keeps its own paks, so a season saved under one roster is never
    // read back under another; Classic uses saves/ itself. Advanced (2026) saves are a different format and live
    // apart from all of them.
    std::filesystem::path pak_dir() {
        std::filesystem::path dir = recomp::get_config_path() / "saves";
#if BLITZ_ENGINE_ORIGINAL
        dir /= "original";  // Modern on the original game: its own saves (the game's code differs from team+'s)
#endif
        std::string pack = blitz::active_pack_id();
#if BLITZ_MODERN
        if (blitz::advanced_mode()) {
            dir /= "advanced";
            dir /= pack.empty() ? "default" : pack;
        }
        else
#endif
        if (!pack.empty()) {
            dir /= "packs";
            dir /= pack;
        }
        return dir;
    }

    std::filesystem::path history_dir(const std::filesystem::path& pak_path) { return pak_path.parent_path() / "pak_history"; }

    // The history's files for this pak, by name.
    std::vector<std::string> history_names(const std::filesystem::path& pak_path) {
        std::vector<std::string> out;
        std::error_code ec;
        std::string stem = pak_path.stem().string();
        for (std::filesystem::directory_iterator it{history_dir(pak_path), ec}, end; !ec && it != end; it.increment(ec)) {
            std::string n = it->path().filename().string();
            if (blitz::pakimg::parse_time(n, stem) != 0) out.push_back(n);
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    // A dated copy of `data` in the history, unless the newest copy is the same; then the old ones pruned.
    void save_history(const std::filesystem::path& pak_path, const std::vector<uint8_t>& data, const char* why) {
        std::error_code ec;
        std::filesystem::path dir = history_dir(pak_path);
        std::string stem = pak_path.stem().string();
        std::vector<std::string> names = history_names(pak_path);
        std::vector<uint8_t>& newest = history_newest[pak_path.string()];
        if (newest.empty() && !names.empty()) read_image(dir / names.back(), newest);
        if (!newest.empty() && newest == data) return;
        std::filesystem::create_directories(dir, ec);
        std::string name = blitz::pakimg::history_name(stem, time(nullptr));
        for (int n = 2; std::filesystem::exists(dir / name, ec) && n < 100; n++) {
            name = blitz::pakimg::history_name(stem, time(nullptr));
            name = name.substr(0, name.size() - 4) + "-" + std::to_string(n) + ".mpk";
        }
        if (!write_image(dir / name, data)) {
            fprintf(stderr, "[pak] history: couldn't write %s\n", (dir / name).string().c_str());
            return;
        }
        newest = data;
        names.push_back(name);
        int removed = 0;
        for (const std::string& old : blitz::pakimg::history_prune(names, stem, time(nullptr))) {
            if (old != name && std::filesystem::remove(dir / old, ec)) removed++;
        }
        fprintf(stderr, "[pak] history: %s (%s: %s)%s\n", name.c_str(), why, blitz::pakimg::describe_plain(data.data()).c_str(),
                removed ? (", " + std::to_string(removed) + " old one(s) pruned").c_str() : "");
    }

    // A restore asked for in Settings (blitz::pak_restore): the chosen history copy replaces the pak, the pak as it
    // is first going into the history. Before anything reads the pak, so the game never sees the swap.
    void apply_restore_mark(const std::filesystem::path& pak_path) {
        std::error_code ec;
        std::filesystem::path mark = pak_path.parent_path() / restore_mark;
        if (!std::filesystem::is_regular_file(mark, ec)) return;
        std::string name;
        {
            std::ifstream in(mark);
            std::getline(in, name);
        }
        std::filesystem::remove(mark, ec);
        std::filesystem::path from = history_dir(pak_path) / std::filesystem::path(name).filename();
        std::vector<uint8_t> chosen, current;
        if (name.empty() || !read_image(from, chosen)) {
            fprintf(stderr, "[pak] restore: couldn't read %s; the pak is as it was\n", from.string().c_str());
            blitz::post_notice("Controller Pak: the backup to restore couldn't be read, so your pak is as it was.");
            return;
        }
        if (read_image(pak_path, current)) save_history(pak_path, current, "before a restore");
        if (!write_image(pak_path, chosen)) {
            fprintf(stderr, "[pak] restore: couldn't write %s\n", pak_path.string().c_str());
            blitz::post_notice("Controller Pak: couldn't restore the backup (the pak file couldn't be written).");
            return;
        }
        fprintf(stderr, "[pak] restored %s from %s (%s)\n", pak_path.filename().string().c_str(), from.filename().string().c_str(),
                blitz::pakimg::describe_plain(chosen.data()).c_str());
        // A season file a Save Sync import replaced, that this pak points at: back too (save_sync_core.h).
        blitz::savesync::restore_replaced_seasons(pak_path.parent_path(), chosen);
    }

    // Save Sync's import (save_sync_core.h apply_staged) writes the pak through the restore mark above: the incoming image
    // goes into the history as a dated copy, the mark names it, and apply_restore_mark keeps the pak as it is in the
    // history and puts the copy in place. True when the pak is the incoming image after.
    bool restore_through_mark(const std::filesystem::path& pak_path, const std::vector<uint8_t>& image) {
        std::error_code ec;
        std::filesystem::path dir = history_dir(pak_path);
        std::filesystem::create_directories(dir, ec);
        std::string stem = pak_path.stem().string();
        std::string name = blitz::pakimg::history_name(stem, time(nullptr));
        for (int n = 2; std::filesystem::exists(dir / name, ec) && n < 100; n++) {
            name = blitz::pakimg::history_name(stem, time(nullptr));
            name = name.substr(0, name.size() - 4) + "-" + std::to_string(n) + ".mpk";
        }
        if (!write_image(dir / name, image)) {
            fprintf(stderr, "[pak] sync: couldn't write %s\n", (dir / name).string().c_str());
            return false;
        }
        {
            std::ofstream out(pak_path.parent_path() / restore_mark, std::ios::trunc);
            out << name << "\n";
            if (!out) return false;
        }
        fprintf(stderr, "[pak] sync: the imported pak goes in through %s\n", name.c_str());
        apply_restore_mark(pak_path);
        std::vector<uint8_t> now;
        return read_image(pak_path, now) && now == image;
    }

    // A Save Sync import staged in this pak's folder: its season files, then the pak. Before the game reads the pak.
    void apply_sync_import(const std::filesystem::path& pak_path) {
        if (pak_path.filename() != blitz::savesync::pak_name) return;
        if (blitz::savesync::apply_staged(pak_path.parent_path(), restore_through_mark) == blitz::savesync::Applied::Failed)
            blitz::post_notice("Controller Pak: the saves you imported couldn't all be put in place. New Blitz tries again at the next start.");
    }

    // Load the port's pak image. Only a missing file gets a fresh blank pak; an existing file that
    // isn't a readable 32 KB image is moved aside (never overwritten) and the player is told.
    Pak& load(int channel) {
        Pak& pak = paks[channel];
        if (pak.loaded) {
            return pak;
        }
        pak.loaded = true;
        std::error_code ec;
        std::filesystem::path dir = pak_dir();
#if BLITZ_MODERN
        guard_on = blitz::advanced_mode();
#endif
        std::filesystem::create_directories(dir, ec);
        pak.path = dir / ("blitz2000_pak" + std::to_string(channel + 1) + ".mpk");
        apply_sync_import(pak.path);  // Save Sync (save_sync.cpp): season files first, then the pak by the restore mark
        apply_restore_mark(pak.path);

        bool exists = std::filesystem::exists(pak.path, ec);
        if (ec) {
            notify("Can't check the Controller Pak file " + pak.path.string() + " (" + ec.message() + "). Saving is off this session.");
            return pak;
        }
        if (exists) {
            if (read_image(pak.path, pak.data)) {
                pak.usable = true;
                // The image as of this launch, for recovery by hand, and the one before it (.bak2): if the last
                // session was cut off mid-save, the torn image becomes .bak but the good one survives as .bak2.
                std::filesystem::path backup = pak.path, older = pak.path;
                backup += ".bak";
                older += ".bak2";
                std::vector<uint8_t> previous;
                bool same = read_image(backup, previous) && previous == pak.data;
                if (!same) {
                    if (std::filesystem::exists(backup, ec)) std::filesystem::rename(backup, older, ec);
                    if (!write_image(backup, pak.data)) {
                        fprintf(stderr, "[pak] couldn't write backup %s\n", backup.string().c_str());
                    }
                }
                fprintf(stderr, "[pak] loaded %s (%s)\n", pak.path.string().c_str(), blitz::pakimg::describe_plain(pak.data.data()).c_str());
                save_history(pak.path, pak.data, "launch");
                return pak;
            }
            std::filesystem::path aside = pak.path;
            aside += ".unreadable-" + timestamp();
            std::filesystem::rename(pak.path, aside, ec);
            if (ec) {
                notify("The Controller Pak file " + pak.path.string() + " isn't a readable 32 KB pak image and couldn't be moved aside (" +
                       ec.message() + "). It was left untouched; saving is off this session.");
                return pak;
            }
            notify("The Controller Pak file wasn't a readable 32 KB pak image. It was kept as " + aside.filename().string() +
                   " and a blank pak was started.");
        }
        pak.data = formatted_pak();
        if (!write_image(pak.path, pak.data)) {
            notify("Couldn't create the Controller Pak file " + pak.path.string() + ". Saving is off this session.");
            return pak;
        }
        pak.usable = true;
        fprintf(stderr, "[pak] created blank pak %s\n", pak.path.string().c_str());
        return pak;
    }

    // Write one 32-byte block back to the file in place. Every step is checked; on failure the
    // caller restores the block in memory and reports the error to the game.
    bool persist_block(Pak& pak, uint32_t block, std::string& error) {
        errno = 0;
        FILE* f = fopen(pak.path.string().c_str(), "r+b");
        if (f == nullptr) {
            error = strerror(errno);
            return false;
        }
        bool ok = fseek(f, (long)block * 32, SEEK_SET) == 0;
        ok = ok && fwrite(pak.data.data() + block * 32, 1, 32, f) == 32;
        ok = (fflush(f) == 0) && ok;
        ok = (fsync(fileno(f)) == 0) && ok;
        if (!ok) {
            error = errno != 0 ? strerror(errno) : "write failed";
        }
        if (fclose(f) != 0 && ok) {
            error = strerror(errno);
            ok = false;
        }
        return ok;
    }
}

// s32 __osContRamRead(OSMesgQueue* mq, int channel, u16 address, u8* buffer)
extern "C" void blitz_ContRamRead(uint8_t* rdram, recomp_context* ctx) {
    int channel = (int)ctx->r5;
    uint32_t address = (uint32_t)ctx->r6 & 0xFFFF;
    gpr buffer = ctx->r7;
    if (channel < 0 || channel > 3 || accessory(channel) != Accessory::ControllerPak) {
        ctx->r2 = PFS_ERR_NOPACK;
        return;
    }
    std::lock_guard lock{paks_mutex};
    Pak& pak = load(channel);
    if (!pak.usable) {
        ctx->r2 = PFS_ERR_NOPACK;
        return;
    }
    for (int i = 0; i < 32; i++) {
        // Above 32 KB (bank select at 0x8000 and up) a Controller Pak has nothing to read.
        MEM_B(i, buffer) = address < block_count ? pak.data[address * 32 + i] : 0;
    }
    ctx->r2 = 0;
}

// s32 __osContRamWrite(OSMesgQueue* mq, int channel, u16 address, u8* buffer, int force)
extern "C" void blitz_ContRamWrite(uint8_t* rdram, recomp_context* ctx) {
    int channel = (int)ctx->r5;
    uint32_t address = (uint32_t)ctx->r6 & 0xFFFF;
    gpr buffer = ctx->r7;
    if (channel < 0 || channel > 3 || accessory(channel) != Accessory::ControllerPak) {
        ctx->r2 = PFS_ERR_NOPACK;
        return;
    }
    if (address >= block_count) {
        ctx->r2 = 0;  // bank select (0x8000) on a single-bank pak: nothing to store
        return;
    }
    std::lock_guard lock{paks_mutex};
    Pak& pak = load(channel);
    if (!pak.usable) {
        ctx->r2 = PFS_ERR_NOPACK;
        return;
    }
    uint8_t* block = pak.data.data() + address * 32;
    uint8_t previous[32], next[32];
    std::copy(block, block + 32, previous);
    for (int i = 0; i < 32; i++) {
        next[i] = MEM_B(i, buffer);
    }
    // The stub guard: what's written never drops a season stub but the player's own Save (pak_guard.h).
    if (guard_on) {
        for (const blitz::pakimg::Stub& s : blitz::pakimg::guard_block(pak.data.data(), address, next, season_save_slot.load(), released)) {
            fprintf(stderr, "[pak] kept the season stub in slot %d (season %016llX gen %u, week %d): a write that wasn't a Save of "
                    "that slot would have changed it (block %u)\n", s.slot, (unsigned long long)s.id, s.gen, s.week + 1, address);
        }
    }
    const bool changes = !std::equal(next, next + 32, previous);
    if (changes) {
        // The history: the image before this burst of writes.
        auto now = std::chrono::steady_clock::now();
        if (!any_change || now - last_change > burst_gap) {
            save_history(pak.path, pak.data, "before a write");
            change_bursts++;
        }
        any_change = true;
        last_change = now;
    }
    std::vector<uint8_t> before;
    if (guard_on && changes && address < blitz::pakimg::system_blocks) before = pak.data;  // the directory or inodes
    std::copy(next, next + 32, block);
    if (!before.empty()) {
        for (const auto& r : blitz::pakimg::released_by(before.data(), pak.data.data())) {
            fprintf(stderr, "[pak] season %016llX gen %u: its note was deleted\n", (unsigned long long)r.first, r.second);
            released.insert(r);
        }
    }
    std::string error;
    if (!persist_block(pak, address, error)) {
        // Keep memory in step with the file, and let the game report the failed save.
        std::copy(previous, previous + 32, block);
        notify("Couldn't save to the Controller Pak (saves/" + pak.path.filename().string() + "): " + error +
               ".\n\nThe last save did not complete. The full path is " + pak.path.string());
        ctx->r2 = PFS_ERR_CONTRFAIL;
        return;
    }
    ctx->r2 = 0;
}

// s32 __osPfsGetStatus(OSMesgQueue* mq, int channel)
extern "C" void blitz_PfsGetStatus(uint8_t* rdram, recomp_context* ctx) {
    // As libultra's own does first (its __osPfsInodeCacheBank = 250): osPfsInitPak then reads the page table from the
    // pak, never from a copy left in RDRAM (a save state's: see the top of this file).
    if (!keep_stale_view()) MEM_B(0, ram(inode_cache_bank)) = (int8_t)no_cached_bank;
    int channel = (int)ctx->r5;
    if (channel < 0 || channel > 3 || accessory(channel) != Accessory::ControllerPak) {
        ctx->r2 = PFS_ERR_NOPACK;
        return;
    }
    std::lock_guard lock{paks_mutex};
    ctx->r2 = load(channel).usable ? 0 : PFS_ERR_NOPACK;
}

// s32 osMotorInit(OSMesgQueue* mq, OSPfs* pfs, int channel)
extern "C" void blitz_osMotorInit(uint8_t* rdram, recomp_context* ctx) {
    gpr pfs = ctx->r5;
    int channel = (int)ctx->r6;
    // OSPfs.channel (+8, after status and queue), read by osMotorStart/Stop. Set it whatever the
    // result, as libultra does: the game stops the motor on every port, pak or not, and a stale
    // channel sent those stops to port 1.
    MEM_W(0x8, pfs) = channel;
    switch (channel >= 0 && channel <= 3 ? accessory(channel) : Accessory::None) {
        case Accessory::RumblePak:
            ctx->r2 = 0;
            break;
        case Accessory::ControllerPak:
            ctx->r2 = PFS_ERR_DEVICE;
            break;
        default:
            ctx->r2 = PFS_ERR_NOPACK;
            break;
    }
}

namespace blitz {
    uint8_t* monitor_rdram();  // dev_monitor.cpp

    // Game thread (osContGetReadData: controllers.cpp's port_input), for a connected port 2-4. The game looks at its ports only
    // at boot and when it probes them (pak_probe 0x80004250, e.g. leaving the title screen), keeping what it found in a byte
    // per port (0x8005DEFC + port: 0 nothing there, 1 a controller, 5 one with a Rumble Pak), and it reads no input from a
    // port it found empty. So a controller that joined after that (a phone opening the page from the main menu, a pad
    // switched on there) did nothing as player 2-4: no icon at Controller Select and no presses, on every platform (the team's
    // phone as player 2 next to a DualSense on Windows, 2026-11-15; as player 1 a phone always worked: port 1 is always
    // there). Mark the port as the probe would have: a controller with a Rumble Pak, its motor's channel set (an OSPfs per
    // port at 0x8005E390, 0x68 apart). Only an empty port: what the game's own probe found stays. Controller Select makes its
    // icons when it opens, so a controller that joins while it's up shows after backing out and going in again.
    void game_port_plugged(int port) {
        uint8_t* rdram = monitor_rdram();
        if (rdram == nullptr || port < 1 || port > 3) return;
        const gpr state = (gpr)(int32_t)(0x8005DEFC + port);
        if (MEM_BU(0, state) != 0) return;
        MEM_W(0x8, (gpr)(int32_t)(0x8005E390 + 0x68 * port)) = port;
        MEM_B(0, state) = 5;
        fprintf(stderr, "[pads] player %d joined after the game looked at its ports: marked plugged in\n", port + 1);
    }
}

namespace blitz {
    // adv_save.cpp, around the game's write of a season Save (its sidecar is already on disk): that slot's stub may change.
    void pak_season_save_begin(int slot) { season_save_slot = slot; }
    void pak_season_save_end() { season_save_slot = -1; }

    // savestate.cpp, after a state's RDRAM is back (slots, Resume, Redo Play, menu steps). The play stays as it was
    // saved; only the pak's view is brought up to date: libultra's copy of the page table is dropped, and the game's
    // "pak ready" flags are cleared, so its next pak use runs osPfsInitPak (and osPfsChecker) on the pak as it is now
    // and rebuilds its OSPfs from it, as after plugging a pak in. No pak request can be in flight here: a state is
    // only taken and restored with the controller thread waiting for its next request.
    void pak_state_restored(uint8_t* rdram) {
        auto byte = [&](uint32_t vram) { return (uint8_t)MEM_BU(0, ram(vram)); };
        const uint8_t bank = byte(inode_cache_bank);
        const int32_t channel = MEM_W(0, ram(inode_cache_channel));
        int ready = 0;
        for (uint32_t p = 0; p < 4; p++) ready += byte(pak_ready + p) != 0;
        std::string cache_note, id_note;
        {
            std::lock_guard lock{paks_mutex};
            const Pak& pak = paks[0];
            const bool known = pak.loaded && pak.usable && pak.data.size() == pak_size;
            if (bank == no_cached_bank) {
                cache_note = "no page table cached";
            }
            else if (bank != 0 || channel != 0 || !known) {
                cache_note = "page table cached (bank " + std::to_string(bank) + ", port " + std::to_string(channel + 1) + ")";
            }
            else {
                int used_then = 0, used_now = 0;
                bool same = true;
                for (uint32_t i = 0; i < 0x100; i++) same = same && byte(inode_cache + i) == pak.data[inode_page + i];
                for (uint32_t pg = first_data_page; pg < pages; pg++) {
                    used_then += ((byte(inode_cache + 2 * pg) << 8) | byte(inode_cache + 2 * pg + 1)) != page_free;
                    used_now += ((pak.data[inode_page + 2 * pg] << 8) | pak.data[inode_page + 2 * pg + 1]) != page_free;
                }
                cache_note = same ? "its page table copy matched the pak"
                                  : "its page table copy was stale (" + std::to_string(used_then) + " pages in use then, " +
                                        std::to_string(used_now) + " now)";
            }
            if (known && (MEM_W(0, ram(game_pfs)) & 1) != 0) {  // PFS_INITIALIZED: the state had opened the pak
                bool same_id = true;
                for (uint32_t i = 0; i < 0x20; i++) same_id = same_id && byte(game_pfs + pfs_id + i) == pak.data[0x20 + i];
                id_note = same_id ? ", same pak" : ", made with another pak (its ID differs)";
            }
        }
        if (keep_stale_view()) {
            fprintf(stderr, "[savestate] pak view kept as the state had it (BLITZ_PAK_STALE=1): %s%s\n", cache_note.c_str(), id_note.c_str());
            return;
        }
        drop_inode_cache(rdram);
        for (uint32_t p = 0; p < 4; p++) MEM_B(0, ram(pak_ready + p)) = 0;
        fprintf(stderr, "[savestate] pak view refreshed: %s%s; %s; the game reopens the pak at its next use\n", cache_note.c_str(),
                id_note.c_str(), ready ? "its pak-ready flag cleared" : "its pak-ready flag was already clear");
    }

    // Every season generation a kept pak image points at: the pak, its .bak and .bak2 and the history. adv_save.cpp
    // keeps those season files when it prunes, so each backup can still be loaded.
    std::set<std::pair<uint64_t, uint32_t>> pak_kept_season_refs() {
        std::set<std::pair<uint64_t, uint32_t>> out;
        std::filesystem::path pak_path;
        {
            std::lock_guard lock{paks_mutex};
            if (!paks[0].loaded || paks[0].path.empty()) return out;
            pak_path = paks[0].path;
            for (const auto& r : blitz::pakimg::stub_refs(paks[0].data.data())) out.insert(r);
        }
        std::vector<std::filesystem::path> files = {pak_path.string() + ".bak", pak_path.string() + ".bak2"};
        for (const std::string& n : history_names(pak_path)) files.push_back(history_dir(pak_path) / n);
        std::vector<uint8_t> img;
        for (const auto& f : files) {
            if (!read_image(f, img)) continue;
            for (const auto& r : blitz::pakimg::stub_refs(img.data())) out.insert(r);
        }
        return out;
    }
}

namespace {
    // A stub's team, from its season file (the franchise of each team index is in its header): "LAR". "" without one.
    std::string stub_team(const std::filesystem::path& pak_folder, uint64_t id, uint32_t gen, uint8_t team) {
        if (pak_folder.parent_path().filename() != "advanced" || team >= 32) return "";
        char hexid[17];
        snprintf(hexid, sizeof(hexid), "%016llX", (unsigned long long)id);
        std::filesystem::path f = recomp::get_config_path() / "saves" / "advanced" / pak_folder.filename() / "season" /
                                  ("season-" + std::string(hexid) + "-" + std::to_string(gen) + ".adv");
        std::ifstream in(f, std::ios::binary);
        char head[44 + 32 * 4];
        if (!in.read(head, sizeof head) || std::memcmp(head, "BLZADV1", 8) != 0) return "";
        const char* name = head + 44 + 4 * team;
        return std::string(name, strnlen(name, 4));
    }

    // What the player calls the folder a pak is in.
    std::string folder_label(const std::filesystem::path& pak_folder) {
        std::string up = pak_folder.parent_path().filename().string();
        if (up == "advanced") return "Advanced";
        if (up == "packs") return "Standard";
        return "Original 1999";
    }
}

namespace blitz {
    std::vector<PakBackup> pak_backups() {
        std::vector<PakBackup> out;
        std::error_code ec;
        std::filesystem::path saves = recomp::get_config_path() / "saves";
        std::set<std::string> folders;
        for (std::filesystem::recursive_directory_iterator it{saves, std::filesystem::directory_options::skip_permission_denied, ec}, end;
             !ec && it != end; it.increment(ec)) {
            if (it.depth() > 4) {
                it.disable_recursion_pending();
                continue;
            }
            std::error_code fec;
            if (!it->is_directory(fec) || it->path().filename() != "pak_history") continue;
            it.disable_recursion_pending();
            std::filesystem::path folder = it->path().parent_path();
            for (std::filesystem::directory_iterator f{it->path(), fec}, fend; !fec && f != fend; f.increment(fec)) {
                std::string n = f->path().filename().string();
                size_t dash = n.find('-');
                if (dash == std::string::npos) continue;
                std::string stem = n.substr(0, dash);
                std::time_t when = blitz::pakimg::parse_time(n, stem);
                std::vector<uint8_t> img, current;
                if (when == 0 || !read_image(f->path(), img)) continue;
                std::filesystem::path pak_path = folder / (stem + ".mpk");
                // The same as the pak now: nothing to put back.
                if (read_image(pak_path, current) && current == img) continue;
                PakBackup b;
                b.file = f->path();
                b.pak = pak_path;
                b.when = when;
                b.folder = folder_label(folder);
                b.what = blitz::pakimg::describe(img.data(), [&](uint64_t id, uint32_t gen, uint8_t team) { return stub_team(folder, id, gen, team); });
                folders.insert(folder.string());
                out.push_back(std::move(b));
            }
        }
        if (folders.size() < 2) {
            for (PakBackup& b : out) b.folder.clear();
        }
        std::sort(out.begin(), out.end(), [](const PakBackup& a, const PakBackup& b) {
            return a.when != b.when ? a.when > b.when : a.file.filename() > b.file.filename();
        });
        return out;
    }

    bool pak_backups_exist() {
        std::error_code ec;
        std::filesystem::path saves = recomp::get_config_path() / "saves";
        for (std::filesystem::recursive_directory_iterator it{saves, std::filesystem::directory_options::skip_permission_denied, ec}, end;
             !ec && it != end; it.increment(ec)) {
            if (it.depth() > 4) {
                it.disable_recursion_pending();
                continue;
            }
            if (it->path().filename() == "pak_history" && !std::filesystem::is_empty(it->path(), ec)) return true;
        }
        return false;
    }

    // Save Sync (save_sync.cpp): true while the game is in the middle of writing the pak (a Save is a burst of about a
    // thousand block writes), when an export would catch it half written.
    bool pak_busy() {
        std::lock_guard lock{paks_mutex};
        return season_save_slot.load() >= 0 || (any_change && std::chrono::steady_clock::now() - last_change < burst_gap);
    }

    // Cloud Sync (save_sync.cpp): how many times the game has changed a pak this run (one per Save, not per block),
    // to send the saves up once the game has finished saving (pak_busy false) and things have been quiet a while.
    uint64_t pak_change_count() { return change_bursts.load(); }

    // Cloud Sync: whether the game has opened the pak in `folder` this run (an import into it waits for the next start).
    bool pak_folder_open(const std::filesystem::path& folder) {
        std::lock_guard lock{paks_mutex};
        const std::filesystem::path want = folder.lexically_normal();
        for (const Pak& p : paks) {
            if (p.loaded && p.path.parent_path().lexically_normal() == want) return true;
        }
        return false;
    }

    // Save Sync (save_sync.cpp): an import staged in `folder` goes in now when the game hasn't opened that folder's pak this
    // run, else at the next start's first pak access (load above), as pak_restore does. True when done or left for the
    // next start (needs_restart); else why not.
    bool pak_sync_apply(const std::filesystem::path& folder, bool& needs_restart, std::string& why) {
        needs_restart = false;
        std::lock_guard lock{paks_mutex};
        const std::filesystem::path want = folder.lexically_normal();
        bool open = false;
        for (const Pak& p : paks) open = open || (p.loaded && p.path.parent_path().lexically_normal() == want);
        if (open) {
            fprintf(stderr, "[pak] sync: %s is open; its import goes in at the next start\n", folder.string().c_str());
            needs_restart = true;
            return true;
        }
        if (blitz::savesync::apply_staged(folder, restore_through_mark) == blitz::savesync::Applied::Failed) {
            why = "Some of your saves couldn't be written.";
            return false;
        }
        return true;
    }

    // Puts a backup back. When that pak hasn't been opened this run (the game hasn't touched it yet), at once;
    // otherwise a mark for the next start, and the caller restarts the app (the game keeps the pak's file system in
    // memory, so the file can't change under it). Returns "" when done or marked, else why not.
    std::string pak_restore(const PakBackup& b, bool& needs_restart) {
        needs_restart = false;
        std::error_code ec;
        std::vector<uint8_t> img;
        if (!read_image(b.file, img)) return "That backup can't be read.";
        std::lock_guard lock{paks_mutex};
        bool open = false;
        for (const Pak& p : paks) open = open || (p.loaded && p.path == b.pak);
        std::filesystem::path mark = b.pak.parent_path() / restore_mark;
        {
            std::ofstream out(mark, std::ios::trunc);
            out << b.file.filename().string() << "\n";
            if (!out) return "Couldn't write in the saves folder.";
        }
        fprintf(stderr, "[pak] restore asked: %s from %s%s\n", b.pak.string().c_str(), b.file.filename().string().c_str(),
                open ? " (at the next start)" : "");
        if (open) {
            needs_restart = true;
            return "";
        }
        apply_restore_mark(b.pak);
        return "";
    }
}
