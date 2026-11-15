// src/main/replay_gif.cpp, on its own: frames with a moving gradient, and with the sizes that have tripped LZW writers
// (a code table that fills up and clears). tests/test_replay_gif.py builds this, runs it and reads the files back.
//   c++ -std=c++20 -Isrc/main tests/replay_gif_test.cpp src/main/replay_gif.cpp -o /tmp/replay_gif_test && /tmp/replay_gif_test out_dir
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "replay_gif.h"

static std::vector<uint8_t> gradient(int w, int h, int t) {
    std::vector<uint8_t> px((size_t)w * h * 4);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t* p = &px[((size_t)y * w + x) * 4];
            p[0] = (uint8_t)((x + t * 7) * 255 / w);
            p[1] = (uint8_t)(y * 255 / h);
            p[2] = (uint8_t)(t * 40);
            p[3] = 255;
        }
    }
    return px;
}

static std::vector<uint8_t> noise(int w, int h, unsigned seed) {
    std::vector<uint8_t> px((size_t)w * h * 4);
    std::mt19937 rng(seed);
    for (auto& b : px) b = (uint8_t)rng();
    return px;
}

static bool write(const std::filesystem::path& path, int w, int h, int frames, bool random) {
    blitz::GifWriter gif;
    if (!gif.open(path, w, h)) return false;
    for (int t = 0; t < frames; t++) {
        auto px = random ? noise(w, h, (unsigned)t + 1) : gradient(w, h, t);
        if (!gif.add_frame(px.data(), 7)) return false;
    }
    return gif.close();
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    std::filesystem::path dir = argv[1];
    std::filesystem::create_directories(dir);
    bool ok = write(dir / "gradient.gif", 480, 270, 6, false) && write(dir / "noise.gif", 320, 180, 3, true) && write(dir / "tiny.gif", 1, 1, 2, false) &&
              write(dir / "tall.gif", 180, 270, 2, false);
    fprintf(stderr, ok ? "written\n" : "FAILED\n");
    return ok ? 0 : 1;
}
