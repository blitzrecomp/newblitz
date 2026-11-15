#pragma once
// An animated GIF, written as it goes: each frame gets its own 256 colours, so nothing but the frame in hand is kept.
// No dependencies, so it builds the same everywhere, and the file opens anywhere: Photos, a browser, a message.
#include <cstdint>
#include <cstdio>
#include <filesystem>

namespace blitz {
    class GifWriter {
    public:
        ~GifWriter();
        bool open(const std::filesystem::path& path, int width, int height);
        // rgba: width x height x 4, top row first (alpha ignored). delay_cs: hundredths of a second on screen.
        bool add_frame(const uint8_t* rgba, int delay_cs);
        bool close();

    private:
        FILE* file = nullptr;
        int width = 0, height = 0;
        bool failed = false;
    };
}
