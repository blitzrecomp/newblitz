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
// image as of each launch is kept as <name>.mpk.bak.
// Blank paks are formatted like mupen64plus's format_mempak (GPL-3.0).
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <random>
#include <string>
#include "../main/platform_compat.h"
#include <utility>
#include <ctime>
#include <vector>

#include "recomp.h"
#include "librecomp/game.hpp"
#include "ultramodern/ultra64.h"

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

    void notify(const std::string& msg) {
        blitz::post_notice("Controller Pak: " + msg);
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

    // Load the port's pak image. Only a missing file gets a fresh blank pak; an existing file that
    // isn't a readable 32 KB image is moved aside (never overwritten) and the player is told.
    Pak& load(int channel) {
        Pak& pak = paks[channel];
        if (pak.loaded) {
            return pak;
        }
        pak.loaded = true;
        std::error_code ec;
        // Each content pack keeps its own paks, so a season saved under one roster is never read back
        // under another; Classic uses saves/ itself. Advanced (2026) saves are a different format
        // and live apart from all of them.
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
        std::filesystem::create_directories(dir, ec);
        pak.path = dir / ("blitz2000_pak" + std::to_string(channel + 1) + ".mpk");

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
                fprintf(stderr, "[pak] loaded %s\n", pak.path.string().c_str());
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
    uint8_t previous[32];
    std::copy(block, block + 32, previous);
    for (int i = 0; i < 32; i++) {
        block[i] = MEM_B(i, buffer);
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
