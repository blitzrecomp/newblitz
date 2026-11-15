// Blitz portraits (Pick Your Crew, the Pro Bowl VS screen; 2026-10-28): a player's face as the game draws it, in his
// skin shade, on his club's colour, in place of photos (no ESPN headshots in the app: the team, 2026-10-25).
//
// Everything comes from the player's own ROM at run time, nothing ships: the game's two face textures (facea light,
// faceb dark: 16 x 64, 4-bit with a 16-colour palette, half a face unwrapped around the head, upside down). The face is
// mirrored into a whole one, turned upright, tinted to a portrait skin colour for his shade and scaled to fill the card,
// forehead to chin.
#if BLITZ_MODERN
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace blitz {
    std::optional<std::vector<uint8_t>> rom_file(const std::string& name);  // art_dropin.cpp
}

namespace {
    // A 4-bit .wms (type 16): pixels, then the 16-entry RGBA5551 palette.
    struct Ci4 {
        int w = 0, h = 0;
        std::vector<uint8_t> idx;
        std::array<uint16_t, 16> pal{};
    };

    std::optional<Ci4> read_ci4(const std::string& name) {
        auto d = blitz::rom_file(name);
        if (!d || d->size() < 32) return std::nullopt;
        auto be32 = [&](size_t o) { return (uint32_t)(*d)[o] << 24 | (uint32_t)(*d)[o + 1] << 16 | (uint32_t)(*d)[o + 2] << 8 | (*d)[o + 3]; };
        Ci4 c;
        c.w = (int)be32(8);
        c.h = (int)be32(12);
        size_t px = (size_t)c.w * c.h / 2;
        if (be32(4) != 16 || c.w <= 0 || c.h <= 0 || d->size() < 32 + px + 32) return std::nullopt;
        for (size_t i = 0; i < px; i++) {
            c.idx.push_back((*d)[32 + i] >> 4);
            c.idx.push_back((*d)[32 + i] & 15);
        }
        for (int i = 0; i < 16; i++) c.pal[(size_t)i] = (uint16_t)((*d)[32 + px + 2 * i] << 8 | (*d)[32 + px + 2 * i + 1]);
        return c;
    }

    struct Rgb { float c[3]; };
    Rgb rgb5551(uint16_t v) { return {{((v >> 11) & 31) / 31.0f, ((v >> 6) & 31) / 31.0f, ((v >> 1) & 31) / 31.0f}}; }
    bool skin_like(const Rgb& c) { return c.c[2] <= c.c[0]; }  // as skin_tones.cpp: the helmet trim is blue

    float lum(const Rgb& c) { return 0.3f * c.c[0] + 0.59f * c.c[1] + 0.11f * c.c[2]; }

    // The face's skin colour in each shade (0 Darker, 4, 3, 2, 1 Light, 5 Lighter: skin_tones.cpp's order), for a
    // flat-lit menu: the field's shades are aimed at the game's lighting, and its palest (lavender there) read grey
    // here (team, 2026-10-29: "why do the white guys look so funny?"). Which of the game's two faces each starts from.
    constexpr std::array<uint32_t, 6> portrait_skin = {0x5E3628, 0xC08A66, 0x93603F, 0x7E5037, 0x6E412F, 0xE4BA9E};
    bool dark_face(int shade) { return shade == 0 || shade == 4; }

    // The face's palette in a shade, as 8-bit RGBA: every skin colour keeps its brightness against the face's average
    // (so eyes, nose and mouth keep their shadows) around the shade's colour, highlights squeezed into the headroom
    // that colour has left (a pale face would blow out to white). The helmet's blue trim over the eyes becomes brows.
    std::array<std::array<uint8_t, 4>, 16> shade_palette(int shade, const Ci4& face) {
        std::array<Rgb, 16> cols;
        for (int i = 0; i < 16; i++) cols[(size_t)i] = rgb5551(face.pal[(size_t)i]);
        auto skin = [&](int i) { return (face.pal[(size_t)i] & 1) && skin_like(cols[(size_t)i]); };
        double sum = 0;
        long n = 0;
        float darkest = 1, brightest = 0;
        for (int y = 32; y < 64; y++) {  // the face half of the strip (it runs chin-up)
            for (int x = 0; x < 16; x++) {
                int i = face.idx[(size_t)(y * 16 + x)];
                if (!skin(i)) continue;
                float l = lum(cols[(size_t)i]);
                sum += l;
                n++;
                brightest = std::max(brightest, l);
            }
        }
        Rgb brow{{0, 0, 0}};
        for (int i = 0; i < 16; i++) {
            if (skin(i) && lum(cols[(size_t)i]) < darkest) {
                darkest = lum(cols[(size_t)i]);
                brow = cols[(size_t)i];
            }
        }
        for (float& v : brow.c) v *= 0.5f;
        float mean = n ? (float)(sum / n) : 0.5f;
        uint32_t aim = portrait_skin[(size_t)shade];
        float a[3];
        for (int k = 0; k < 3; k++) a[k] = (float)((aim >> (16 - 8 * k)) & 0xFF) / 255.0f;
        float top = brightest / std::fmax(0.01f, mean);
        float squeeze = top > 1.0f ? std::fmin(1.0f, (0.95f / std::max({a[0], a[1], a[2]}) - 1.0f) / (top - 1.0f)) : 1.0f;

        std::array<std::array<uint8_t, 4>, 16> out{};
        for (int i = 0; i < 16; i++) {
            Rgb c = skin_like(cols[(size_t)i]) ? cols[(size_t)i] : brow;
            float r = lum(c) / std::fmax(0.01f, mean);
            if (r > 1.0f) r = 1.0f + (r - 1.0f) * squeeze;
            for (int k = 0; k < 3; k++) {
                float v = a[k] * r;
                if (v > 0.9f) v = 0.9f + 0.1f * (1.0f - std::exp(-(v - 0.9f) / 0.1f));
                out[(size_t)i][(size_t)k] = (uint8_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f);
            }
            out[(size_t)i][3] = (face.pal[(size_t)i] & 1) ? 255 : 0;
        }
        return out;
    }

    // Each club's main colour (the helmet shell a fan pictures), behind the face.
    const std::map<std::string, uint32_t> helmet = {
        {"ARI", 0xF4F4F4}, {"ATL", 0x1A1A1A}, {"BAL", 0x241773}, {"BUF", 0xF4F4F4}, {"CAR", 0x1A1A1A}, {"CHI", 0x0B162A},
        {"CIN", 0xFB4F14}, {"CLE", 0xFF3C00}, {"DAL", 0x8C9AA5}, {"DEN", 0x0A2343}, {"DET", 0xB0B7BC}, {"GB", 0x203731},
        {"HOU", 0x03202F}, {"IND", 0xF4F4F4}, {"JAX", 0x101820}, {"KC", 0xE31837}, {"LAC", 0xF4F4F4}, {"LAR", 0x003594},
        {"LV", 0xA5ACAF}, {"MIA", 0xF4F4F4}, {"MIN", 0x4F2683}, {"NE", 0x0C2340}, {"NO", 0xD3BC8D}, {"NYG", 0x0B2265},
        {"NYJ", 0x125740}, {"PHI", 0x004C54}, {"PIT", 0xFFB612}, {"SEA", 0x002244}, {"SF", 0xB3995D}, {"TB", 0x34302B},
        {"TEN", 0x0C2340}, {"WAS", 0x5A1414},
    };
}

namespace blitz {
    // A player's portrait, 96 x 70 RGBA (the size the crew cards' photos were): skin 0-5 (-1, not picked yet:
    // Medium), his club for the colour behind. False without the game's face textures (no ROM yet).
    bool blitz_portrait(int skin, const std::string& franchise, std::vector<char>& rgba, uint32_t& w, uint32_t& h) {
        static std::mutex lock;  // the UI thread (crews) and the Roster Room's server
        std::lock_guard guard{lock};
        static std::optional<Ci4> faces[2];
        static bool read = false;
        if (!read) {
            read = true;
            faces[0] = read_ci4("facea.wms");
            faces[1] = read_ci4("faceb.wms");
        }
        if (skin < 0 || skin > 5) skin = 3;
        const auto& face = faces[dark_face(skin) ? 1 : 0];
        if (!face || face->w != 16 || face->h != 64) return false;
        auto pal = shade_palette(skin, *face);

        // The whole face: the strip and its mirror, upright (the texture runs chin-up).
        constexpr int FW = 32, FH = 64;
        std::array<std::array<uint8_t, 4>, FW * FH> fullface{};
        for (int y = 0; y < FH; y++) {
            for (int x = 0; x < 16; x++) {
                const auto& c = pal[face->idx[(size_t)(y * 16 + x)]];
                int uy = FH - 1 - y;
                fullface[(size_t)(uy * FW + x)] = c;
                fullface[(size_t)(uy * FW + (FW - 1 - x))] = c;
            }
        }

        // The face fills the card (team, 2026-10-29), zoomed out a little (later the same day): forehead to chin, a
        // sliver of the club's colour at the sides. 2.6 card pixels a texel, centred between the eyes.
        constexpr float scale = 2.6f, cx = 16.0f, cy = 15.0f;
        constexpr int FACE_ROWS = 31;  // the face half of the strip, upright: below it the back of the head
        w = 96;
        h = 70;
        uint32_t col = helmet.count(franchise) ? helmet.at(franchise) : 0xB8C4DA;
        rgba.assign((size_t)w * h * 4, 0);
        for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < w; x++) {
                int fx = (int)std::floor(cx + (x + 0.5f - w / 2.0f) / scale);
                int fy = (int)std::floor(cy + (y + 0.5f - h / 2.0f) / scale);
                bool in = fx >= 0 && fx < FW && fy >= 0 && fy < FACE_ROWS;
                const auto& c = fullface[(size_t)((in ? fy : 0) * FW + (in ? fx : 0))];
                char* o = &rgba[(size_t)(y * w + x) * 4];
                for (int k = 0; k < 3; k++) o[k] = (char)(in && c[3] ? c[(size_t)k] : (uint8_t)(((col >> (16 - 8 * k)) & 0xFF) * 0.7f));
                o[3] = (char)255;
            }
        }
        return true;
    }
}
#endif
