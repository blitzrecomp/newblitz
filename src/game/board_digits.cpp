// The stadium scoreboards' digits (New Blitz): the game paints them as hollow zeros, so they're painted here from the live
// score, quarter, down, distance and ball spot, in the board's image in RDRAM. RT64 reads that image each time it's loaded
// for the board. Only the common boards: Day, Night and Snow (128x32), and Old (64x64). Original 1999 stays as it shipped.
//
// Images are 4-bit colour index, stored upside down like the signs (tools/make_logo2027.py): y here counts in the file, so
// a digit's top row is its largest y. A cell is where the art has its zero, found with the art's own colours, x as the
// art reads. Day, Night and Snow's boards also come out mirrored on the stadium's model ("QTR" backwards, as shipped): they're
// turned over once when they load, so they read. The Old and Roman stadiums' dot-matrix strips also have the clock.
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

#include "recomp.h"

#if BLITZ_MODERN && !BLITZ_TEAMPLUS
namespace blitz {
    bool original_game();
}

namespace {
    struct Cell { int x, y; };
    struct Board {
        const char* file;
        int width, cell_w, cell_h;
        uint8_t lit, dark;
        Cell quarter, down, togo[2], ball[2];
        bool has_score;
        Cell score[2][2];
        bool mirrored;
    };
    constexpr Board boards[] = {
        {"score.wms", 128, 5, 7, 5, 8, {29, 18}, {11, 7}, {{55, 7}, {62, 7}}, {{105, 7}, {112, 7}}, true, {{{72, 18}, {79, 18}}, {{89, 18}, {96, 18}}}, true},
        {"nscore.wms", 128, 5, 7, 5, 3, {29, 18}, {11, 7}, {{55, 7}, {62, 7}}, {{105, 7}, {112, 7}}, true, {{{72, 18}, {79, 18}}, {{89, 18}, {96, 18}}}, true},
        {"sscore.wms", 128, 5, 7, 4, 1, {29, 18}, {11, 7}, {{55, 7}, {62, 7}}, {{105, 7}, {112, 7}}, true, {{{72, 18}, {79, 18}}, {{89, 18}, {96, 18}}}, true},
        {"scoreb1.wms", 64, 5, 8, 10, 1, {28, 36}, {9, 26}, {{44, 26}, {50, 26}}, {{26, 15}, {31, 15}}, false, {}, false},
    };
    constexpr int board_count = (int)std::size(boards);

    // The strips: 3 by 5 dots, each dot 2 pixels square on a 3 pixel grid, from y 9. Lit 1, dim 2 (128 wide).
    constexpr int strip_ball = 4, strip_down = 5;
    constexpr uint8_t dots[11][5] = {{7, 5, 5, 5, 7}, {2, 2, 2, 2, 2}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1}, {7, 4, 7, 1, 7},
                                     {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7}, {0, 0, 0, 0, 0}};
    bool turned[board_count] = {};

    // 5x7, top row first, bit 4 the left column. 10: blank.
    constexpr uint8_t font[11][7] = {
        {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}, {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
        {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}, {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
        {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
        {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}, {0, 0, 0, 0, 0, 0, 0}};

    std::array<uint32_t, board_count + 2> loaded_at{};

    int32_t rd(uint8_t* rdram, uint32_t a) { return MEM_W(0, (gpr)(int32_t)a); }

    void put(uint8_t* rdram, uint32_t pixels, int width, int x, int y, uint8_t index) {
        uint32_t a = pixels + (uint32_t)(y * width / 2 + x / 2);
        uint8_t v = (uint8_t)MEM_B(0, (gpr)(int32_t)a);
        v = x % 2 == 0 ? (uint8_t)((v & 0x0F) | (index << 4)) : (uint8_t)((v & 0xF0) | index);
        MEM_B(0, (gpr)(int32_t)a) = (int8_t)v;
    }

    uint8_t get(uint8_t* rdram, uint32_t pixels, int width, int x, int y) {
        uint8_t v = (uint8_t)MEM_B(0, (gpr)(int32_t)(pixels + (uint32_t)(y * width / 2 + x / 2)));
        return x % 2 == 0 ? v >> 4 : v & 15;
    }

    void turn_over(uint8_t* rdram, uint32_t pixels, int width, int height) {
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width / 2; x++) {
                uint8_t a = get(rdram, pixels, width, x, y), b = get(rdram, pixels, width, width - 1 - x, y);
                put(rdram, pixels, width, x, y, b);
                put(rdram, pixels, width, width - 1 - x, y, a);
            }
        }
    }

    void digit(uint8_t* rdram, uint32_t pixels, const Board& b, Cell c, int value) {
        for (int y = 0; y < b.cell_h; y++) {
            int row = b.cell_h - 1 - y;  // upside down in the file
            uint8_t bits = row < 7 ? font[value][row] : 0;
            for (int x = 0; x < b.cell_w; x++) {
                int at = b.mirrored ? b.width - 1 - (c.x + x) : c.x + x;
                put(rdram, pixels, b.width, at, c.y + y, (bits >> (4 - x)) & 1 ? b.lit : b.dark);
            }
        }
    }

    void dot_digit(uint8_t* rdram, uint32_t pixels, int left, int value) {
        for (int r = 0; r < 5; r++) {
            uint8_t bits = dots[value][4 - r];  // upside down in the file
            for (int col = 0; col < 3; col++) {
                uint8_t index = (bits >> (2 - col)) & 1 ? 1 : 2;
                for (int py = 0; py < 2; py++) {
                    for (int px = 0; px < 2; px++) put(rdram, pixels, 128, left + col * 3 + px, 9 + r * 3 + py, index);
                }
            }
        }
    }

    void dot_two(uint8_t* rdram, uint32_t pixels, int tens, int ones, int value, bool blank_tens) {
        value = value < 0 ? 0 : value > 99 ? 99 : value;
        dot_digit(rdram, pixels, tens, blank_tens && value < 10 ? 10 : value / 10);
        dot_digit(rdram, pixels, ones, value % 10);
    }

    void two(uint8_t* rdram, uint32_t pixels, const Board& b, const Cell* cells, int value) {
        value = value < 0 ? 0 : value > 99 ? 99 : value;
        digit(rdram, pixels, b, cells[0], value >= 10 ? value / 10 : 10);
        digit(rdram, pixels, b, cells[1], value % 10);
    }
}

// By the ROM's own file numbers (the stadium code asks for them by number), for the game's own image: an added file comes after.
// One stadium's images load together, so a gap means another stadium: what's remembered of the last is out of date.
extern "C" void blitz_board_loaded(uint32_t id, uint32_t pixels) {
    static constexpr uint32_t numbers[board_count + 2] = {1418, 1183, 1584, 1419, 128, 366};
    static auto last = std::chrono::steady_clock::time_point{};
    for (int i = 0; i < board_count + 2; i++) {
        if (id != 0x0A000000u + numbers[i]) continue;
        auto now = std::chrono::steady_clock::now();
        if (now - last > std::chrono::seconds(2)) loaded_at = {};
        last = now;
        loaded_at[(size_t)i] = pixels;
        if (i < board_count) turned[i] = false;
        fprintf(stderr, "[board] file %u at %08X\n", numbers[i], pixels);
    }
}

// Every frame (gfx_build_frame). A few times a second is plenty, and a save state's restored image is put right too.
extern "C" void blitz_board_frame(uint8_t* rdram) {
    static int tick = 0;
    if (++tick % 5 != 0 || blitz::original_game() || rd(rdram, 0x802EA420) != 2) return;  // 2: a game's on
    int spot_now = rd(rdram, 0x802EA48C), marker_now = rd(rdram, 0x802EA49C), quarter_now = rd(rdram, 0x802EA424), down_now = rd(rdram, 0x802EA498), clock_now = rd(rdram, 0x802EA428);
    if (uint32_t strip = loaded_at[strip_ball]) {
        dot_two(rdram, strip, 47, 58, spot_now > 50 ? 100 - spot_now : spot_now, true);
        dot_two(rdram, strip, 78, 89, (clock_now >> 16) & 0xFF, true);
        dot_two(rdram, strip, 103, 114, (clock_now >> 8) & 0xFF, false);
    }
    if (uint32_t strip = loaded_at[strip_down]) {
        dot_digit(rdram, strip, 4, down_now >= 1 && down_now <= 4 ? down_now : 10);
        dot_two(rdram, strip, 70, 81, (marker_now >= 100 ? 100 : marker_now) - spot_now, true);
        dot_digit(rdram, strip, 96, quarter_now >= 0 && quarter_now < 4 ? quarter_now + 1 : 10);
    }
    for (int i = 0; i < board_count; i++) {
        uint32_t pixels = loaded_at[(size_t)i];
        if (pixels == 0) continue;
        const Board& b = boards[i];
        if (b.mirrored && !turned[i]) {
            turned[i] = true;
            turn_over(rdram, pixels, b.width, 32);
        }
        int spot = rd(rdram, 0x802EA48C), marker = rd(rdram, 0x802EA49C), quarter = rd(rdram, 0x802EA424), down = rd(rdram, 0x802EA498);
        digit(rdram, pixels, b, b.quarter, quarter >= 0 && quarter < 4 ? quarter + 1 : 10);
        digit(rdram, pixels, b, b.down, down >= 1 && down <= 4 ? down : 10);
        two(rdram, pixels, b, b.togo, (marker >= 100 ? 100 : marker) - spot);
        two(rdram, pixels, b, b.ball, spot > 50 ? 100 - spot : spot);
        if (b.has_score) {
            for (int side = 0; side < 2; side++) {
                int total = 0;
                for (int q = 0; q < 7; q++) total += rd(rdram, 0x802EA43C + (uint32_t)side * 0x1C + (uint32_t)q * 4);
                two(rdram, pixels, b, b.score[side], total);
            }
        }
    }
}
#else
extern "C" void blitz_board_loaded(uint32_t, uint32_t) {}
extern "C" void blitz_board_frame(uint8_t*) {}
#endif
