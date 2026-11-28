// USA v1.0 -> v1.1 at import (src/common/rom_upgrade.cpp, assets/rom/v10_to_v11.nbdiff): `make test-rom-upgrade`.
//
//   rom_upgrade_test <patch> [<v1.0 .z64 or .zip> [<v1.1 .z64 or .zip>]]
//
// The ROMs are the player's own dumps, read where they are at test time (the Makefile passes ~/Desktop/Gaming/roms, or
// ROM_V10= / ROM_V11=); each check that needs one is skipped when it isn't there. Nothing here writes a ROM anywhere.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "miniz.h"
#include "../src/common/rom_upgrade.h"
#include "../src/common/sha256.h"

static int failures = 0;
#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            failures++;                                                   \
        }                                                                 \
    } while (0)

static std::vector<uint8_t> read_all(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

// A loose ROM, or the biggest .z64/.n64/.v64 in a zip (as main.cpp import_rom picks).
static std::vector<uint8_t> read_rom(const char* path) {
    std::vector<uint8_t> data = read_all(path);
    if (data.size() < 4 || std::memcmp(data.data(), "PK\x03\x04", 4) != 0) return data;
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, data.data(), data.size(), 0)) return {};
    int pick = -1;
    mz_uint64 best = 0;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st) || st.m_is_directory || st.m_uncomp_size <= best) continue;
        std::string n = st.m_filename;
        std::string ext = n.size() >= 4 ? n.substr(n.size() - 4) : "";
        for (char& c : ext) c = (char)tolower((unsigned char)c);
        if (ext != ".z64" && ext != ".n64" && ext != ".v64") continue;
        pick = (int)i;
        best = st.m_uncomp_size;
    }
    std::vector<uint8_t> rom;
    if (pick >= 0) {
        size_t n = 0;
        void* p = mz_zip_reader_extract_to_heap(&zip, (mz_uint)pick, &n, 0);
        if (p) rom.assign((uint8_t*)p, (uint8_t*)p + n);
        mz_free(p);
    }
    mz_zip_reader_end(&zip);
    return rom;
}

static bool upgrades(std::vector<uint8_t> rom, const std::vector<uint8_t>& patch) {
    std::vector<uint8_t> out;
    std::string why;
    if (!blitzrom::to_z64(rom) || !blitzrom::header_is_v10(rom)) return false;
    return blitzrom::apply_v10_patch(rom, patch.data(), patch.size(), out, why) == blitzrom::Upgrade::done &&
           blitzsha::sha1_hex(out.data(), out.size()) == blitzrom::v11_sha1;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: rom_upgrade_test <patch> [<v1.0 ROM> [<v1.1 ROM>]]\n");
        return 2;
    }
    const std::vector<uint8_t> patch = read_all(argv[1]);
    CHECK(patch.size() > 68 && patch.size() < 64 * 1024);  // small: no assets in it
    std::vector<uint8_t> out;
    std::string why;

    // No ROM needed: a patch that isn't one, and a ROM that isn't v1.0.
    {
        std::vector<uint8_t> junk(16 * 1024 * 1024, 0);
        CHECK(blitzrom::apply_v10_patch(junk, nullptr, 0, out, why) == blitzrom::Upgrade::bad_patch);
        CHECK(blitzrom::apply_v10_patch(junk, patch.data(), patch.size() - 1, out, why) == blitzrom::Upgrade::bad_patch);
        CHECK(blitzrom::apply_v10_patch(junk, patch.data(), patch.size(), out, why) == blitzrom::Upgrade::not_v10);
        CHECK(out.empty());
        CHECK(!blitzrom::to_z64(junk));
        CHECK(!blitzrom::header_is_v10(junk));
    }

    std::vector<uint8_t> v10 = argc > 2 ? read_rom(argv[2]) : std::vector<uint8_t>{};
    if (v10.empty()) {
        printf("rom_upgrade: no v1.0 dump (%s): its checks skipped\n", argc > 2 ? argv[2] : "none given");
    }
    else {
        CHECK(blitzrom::to_z64(v10));
        CHECK(blitzrom::header_is_v10(v10));
        CHECK(upgrades(v10, patch));
        // The other byte orders, as .v64 and .n64 dumps hold them.
        std::vector<uint8_t> v64 = v10, n64 = v10;
        for (size_t i = 0; i < v64.size(); i += 2) std::swap(v64[i], v64[i + 1]);
        for (size_t i = 0; i < n64.size(); i += 4) {
            std::swap(n64[i], n64[i + 3]);
            std::swap(n64[i + 1], n64[i + 2]);
        }
        CHECK(upgrades(v64, patch));
        CHECK(upgrades(n64, patch));
        // A damaged dump (one byte off) is refused before anything is made; so is a damaged patch.
        {
            std::vector<uint8_t> bad = v10;
            bad[0x200000] ^= 1;
            CHECK(blitzrom::header_is_v10(bad));
            CHECK(blitzrom::apply_v10_patch(bad, patch.data(), patch.size(), out, why) == blitzrom::Upgrade::not_v10);
            std::vector<uint8_t> bad_patch = patch;
            bad_patch[bad_patch.size() - 100] ^= 0x55;
            blitzrom::Upgrade r = blitzrom::apply_v10_patch(v10, bad_patch.data(), bad_patch.size(), out, why);
            CHECK(r == blitzrom::Upgrade::bad_patch || r == blitzrom::Upgrade::bad_result);
            CHECK(out.empty());
        }
        printf("rom_upgrade: v1.0 (.z64, .v64, .n64) -> v1.1 %s\n", blitzrom::v11_sha1);
    }

    std::vector<uint8_t> v11 = argc > 3 ? read_rom(argv[3]) : std::vector<uint8_t>{};
    if (v11.empty()) {
        printf("rom_upgrade: no v1.1 dump: its checks skipped\n");
    }
    else {
        CHECK(blitzrom::to_z64(v11));
        CHECK(blitzsha::sha1_hex(v11.data(), v11.size()) == blitzrom::v11_sha1);
        CHECK(!blitzrom::header_is_v10(v11));  // v1.1 is stored as it is, never patched
        CHECK(blitzrom::apply_v10_patch(v11, patch.data(), patch.size(), out, why) == blitzrom::Upgrade::not_v10);
    }

    if (failures) {
        fprintf(stderr, "rom_upgrade: %d failure(s)\n", failures);
        return 1;
    }
    printf("rom_upgrade: all passed\n");
    return 0;
}
