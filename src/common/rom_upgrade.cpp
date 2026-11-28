// v1.0 -> v1.1 (rom_upgrade.h). The patch's format is tools/make_rom_patch.py's docstring.
#include "rom_upgrade.h"

#include <cstring>

#include "sha256.h"
#include "zstd/lib/zstd.h"

namespace blitzrom {

namespace {
    uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
    uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3]; }

    constexpr size_t header_size = 68;  // "NBDIFF01", 3 sizes, 2 SHA-1s, 2 frame sizes
    constexpr size_t rom_size = 16 * 1024 * 1024;

    // A zstd frame of exactly n bytes into out.
    bool unzstd(const uint8_t* in, size_t in_size, uint8_t* out, size_t n) {
        unsigned long long declared = ZSTD_getFrameContentSize(in, in_size);
        if (declared != ZSTD_CONTENTSIZE_UNKNOWN && declared != (unsigned long long)n) return false;
        size_t got = ZSTD_decompress(out, n, in, in_size);
        return !ZSTD_isError(got) && got == n;
    }
}

bool to_z64(std::vector<uint8_t>& rom) {
    if (rom.size() < 0x40) return false;
    rom.resize((rom.size() + 3) & ~(size_t)3);
    const uint32_t w = be32(rom.data());
    if (w == 0x80371240u) return true;  // .z64
    if (w == 0x37804012u) {             // .v64: 16-bit words swapped
        for (size_t i = 0; i < rom.size(); i += 2) std::swap(rom[i], rom[i + 1]);
        return true;
    }
    if (w == 0x40123780u) {  // .n64: 32-bit little-endian words
        for (size_t i = 0; i < rom.size(); i += 4) {
            std::swap(rom[i], rom[i + 3]);
            std::swap(rom[i + 1], rom[i + 2]);
        }
        return true;
    }
    return false;
}

bool header_is_v10(const std::vector<uint8_t>& z64) {
    return z64.size() == rom_size && be32(&z64[0x10]) == 0x15A00969u && be32(&z64[0x14]) == 0x34E5A285u &&
           std::memcmp(&z64[0x3B], "NBIE", 4) == 0 && z64[0x3F] == 0;
}

Upgrade apply_v10_patch(const std::vector<uint8_t>& old_z64, const uint8_t* patch, size_t patch_size, std::vector<uint8_t>& out,
                        std::string& why) {
    out.clear();
    if (patch == nullptr || patch_size < header_size || std::memcmp(patch, "NBDIFF01", 8) != 0) {
        why = "the patch file is missing or isn't one";
        return Upgrade::bad_patch;
    }
    const uint32_t old_size = le32(patch + 8), new_size = le32(patch + 12), count = le32(patch + 16);
    const uint8_t* old_sha = patch + 20;
    const uint8_t* new_sha = patch + 40;
    const uint32_t ctrl_z = le32(patch + 60), data_z = le32(patch + 64);
    if (new_size != rom_size || old_size != rom_size || count > 1u << 20 || (uint64_t)header_size + ctrl_z + data_z != patch_size) {
        why = "the patch file is damaged (its sizes)";
        return Upgrade::bad_patch;
    }
    if (blitzsha::hex(new_sha, 20) != v11_sha1) {
        why = "the patch file makes another ROM";
        return Upgrade::bad_patch;
    }
    if (old_z64.size() != old_size || blitzsha::sha1_hex(old_z64.data(), old_z64.size()) != blitzsha::hex(old_sha, 20)) {
        why = "not the v1.0 dump the patch is for (its SHA-1)";
        return Upgrade::not_v10;
    }
    std::vector<uint8_t> ctrl((size_t)count * 12);
    if (!unzstd(patch + header_size, ctrl_z, ctrl.data(), ctrl.size())) {
        why = "the patch file is damaged (its runs)";
        return Upgrade::bad_patch;
    }
    out.resize(new_size);
    if (!unzstd(patch + header_size + ctrl_z, data_z, out.data(), out.size())) {
        out.clear();
        why = "the patch file is damaged (its data)";
        return Upgrade::bad_patch;
    }
    // Each run: add the old bytes under it to the differences already in out, then step past the new bytes after it.
    int64_t npos = 0, opos = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t add = le32(&ctrl[12 * i]), copy = le32(&ctrl[12 * i + 4]);
        const int32_t seek = (int32_t)le32(&ctrl[12 * i + 8]);
        if (npos + (int64_t)add + copy > (int64_t)new_size) {
            out.clear();
            why = "the patch file is damaged (a run past the end)";
            return Upgrade::bad_patch;
        }
        for (uint32_t k = 0; k < add; k++) {
            const int64_t o = opos + k;
            if (o >= 0 && o < (int64_t)old_size) out[(size_t)(npos + k)] = (uint8_t)(out[(size_t)(npos + k)] + old_z64[(size_t)o]);
        }
        npos += (int64_t)add + copy;
        opos += (int64_t)add + seek;
    }
    if (npos != (int64_t)new_size || blitzsha::sha1_hex(out.data(), out.size()) != v11_sha1) {
        out.clear();
        why = "the patched ROM isn't v1.1 (its SHA-1)";
        return Upgrade::bad_result;
    }
    why.clear();
    return Upgrade::done;
}

}  // namespace blitzrom
