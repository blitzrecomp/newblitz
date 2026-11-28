#pragma once
// Per-set texture-coordinate overrides (src/game/uv_overrides.cpp; tools/uv_override.py makes them): a league pak's
// pack.json "uv_overrides" tables and the sets that use them. A set with one wears its own texture for a uniform part
// AND that part's own texture coordinates on the player models the table lists; every other player, model and set
// keeps the stock texture paired with the stock coordinates.
//
// Per-cap textures (2026-11-28, the CFL's right sleeves): a part drawn as several caps (the shoulder: one per upper
// arm, each placed by its own bone matrix) can take another texture on some caps. The table names its caps by the
// G_MTX each cap's draw is placed with ("caps": {"right": "0B000300"}), and a set's "caps" gives a texture per cap name.
// A table may keep the stock coordinates ("uv" equal to "st"): its sets change only which texture each cap samples.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace blitz {
    struct UvVerts {
        uint32_t at = 0;                              // the model's vertex buffer (main, static)
        std::vector<std::array<int16_t, 2>> st, uv;  // each vertex's stock S/T (checked before use) and new S/T
    };
    struct UvMesh {
        int model = -1;             // main's player model (0-5), for the log
        uint32_t block = 0, end = 0;  // the part's block in the body display list: its G_TEXTURE .. the next command
        std::vector<UvVerts> verts;
    };
    struct UvCap {
        std::string name;           // "right", "left"
        uint32_t mtx = 0;           // the G_MTX (bone matrix, a segment address) the cap's draw is placed with
    };
    struct UvTable {
        std::string name, part;     // part: "shoulder"
        std::vector<UvCap> caps;    // optional: the part's caps, for sets with per-cap textures
        std::vector<UvMesh> meshes;
    };
    struct UvCapTexture {
        std::string cap, file;      // one of the table's caps and the texture drawn on it (files/added)
        uint32_t mtx = 0;           // that cap's G_MTX (the table's)
        std::vector<uint8_t> texture;
    };
    struct UvEntry {
        std::string team, set;      // for the log: "CSU", "home" / "away" / an alternate's id
        std::string stock_file;     // the set's own part file (files/added): the key, as the side is dressed in it
        std::string texture_file;   // the texture drawn with the table's coordinates (files/added); "": the set's own
        size_t table = 0;
        std::vector<uint8_t> texture;  // that .wms, read from the pak
        std::vector<UvCapTexture> caps;  // caps with a texture of their own (the others: texture_file, or the set's own)
    };
    struct UvOverrides {
        std::vector<UvTable> tables;
        std::vector<UvEntry> entries;
    };
    const UvOverrides* league_uv_overrides();  // content_pack_tp.cpp: the active league pak's, or nullptr

    // mixed_uniforms.cpp: a Pro Bowl kickoff just dressed `slot` of `side` in his own set (the last dress decides).
    void uvo_mixed_dressed(int side, int slot);
    void uvo_mixed_reset();
    // mixed_uniforms.cpp: the side and roster slot of a render entry wearing his own set, or false.
    bool mixed_player_of(uint8_t* rdram, int32_t entry, int& side, int& slot);
}
