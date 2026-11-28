// NFL Blitz 2000 USA v1.0 into the v1.1 New Blitz plays (1.0.6). v1.0 (NBIE, md5 ba49514441023722f02d41c62612f6c3) is
// the same game with its code moved and its assets byte for byte the same, so a 38 KB patch rebuilds v1.1 from it
// (assets/rom/v10_to_v11.nbdiff, made by tools/make_rom_patch.py from bsdiff's edit list and packed with zstd, the
// decoder the app already links through RT64). The app's import (src/main/main.cpp import_rom) runs this on a ROM that
// is NFL Blitz 2000 but not v1.1, then stores the result as any v1.1 ROM. Shared by every build and the unit test
// (tests/rom_upgrade_test.cpp); no app dependencies.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace blitzrom {

// v1.1's SHA-1 (config/target_rom.json): what an upgrade must make.
inline constexpr const char* v11_sha1 = "3754c8fdf5cdce5de62685ce9be73e06ba5e3577";

// A ROM in any of the N64's byte orders (.z64, .v64, .n64) to .z64's big-endian order, in place (padded to a multiple of
// 4). False if it doesn't start as an N64 ROM does.
bool to_z64(std::vector<uint8_t>& rom);

// Its header says USA v1.0: NBIE, revision 0, v1.0's header CRCs (15A00969 34E5A285), 16 MB. Cheap; the patch checks the
// whole file's SHA-1 before using it.
bool header_is_v10(const std::vector<uint8_t>& z64);

enum class Upgrade {
    done,        // out is v1.1 (its SHA-1 checked)
    not_v10,     // the ROM isn't the v1.0 this patch is for (its SHA-1)
    bad_patch,   // the patch file is missing, damaged or another one
    bad_result,  // applied, but the result isn't v1.1
};

// v1.0 (.z64 order) and the patch's bytes -> v1.1 in out. why says what went wrong, for the log.
Upgrade apply_v10_patch(const std::vector<uint8_t>& old_z64, const uint8_t* patch, size_t patch_size, std::vector<uint8_t>& out,
                        std::string& why);

}  // namespace blitzrom
