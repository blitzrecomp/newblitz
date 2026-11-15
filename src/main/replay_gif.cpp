// GIF89a by hand (GifWriter, replay_gif.h): median cut over a 15-bit histogram for each frame's palette, then LZW.
#include "replay_gif.h"

#include <algorithm>
#include <array>
#include <memory>
#include <vector>

namespace {
    constexpr int cells = 1 << 15;
    inline int cell_of(uint8_t r, uint8_t g, uint8_t b) { return ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3); }

    struct Box {
        std::vector<int> members;  // cells with any pixels
        uint64_t weight = 0;
    };

    // The 256 colours of this frame, and which of them each cell of the histogram became.
    void make_palette(const uint8_t* rgba, int count, std::array<uint8_t, 768>& palette, std::vector<uint8_t>& lut) {
        std::vector<uint32_t> hist(cells, 0);
        for (int i = 0; i < count; i++) hist[cell_of(rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2])]++;

        std::vector<Box> boxes(1);
        for (int c = 0; c < cells; c++) {
            if (hist[c] == 0) continue;
            boxes[0].members.push_back(c);
            boxes[0].weight += hist[c];
        }
        auto channel = [](int c, int ch) { return ch == 0 ? (c >> 10) & 31 : ch == 1 ? (c >> 5) & 31 : c & 31; };
        while (boxes.size() < 256) {
            // The heaviest box that can still be split, along its longest side, at its pixels' middle.
            int best = -1;
            uint64_t best_score = 0;
            int best_channel = 0;
            for (size_t b = 0; b < boxes.size(); b++) {
                if (boxes[b].members.size() < 2) continue;
                int lo[3] = {31, 31, 31}, hi[3] = {0, 0, 0};
                for (int c : boxes[b].members) {
                    for (int ch = 0; ch < 3; ch++) {
                        lo[ch] = std::min(lo[ch], channel(c, ch));
                        hi[ch] = std::max(hi[ch], channel(c, ch));
                    }
                }
                int ch = 0;
                for (int k = 1; k < 3; k++) {
                    if (hi[k] - lo[k] > hi[ch] - lo[ch]) ch = k;
                }
                uint64_t score = boxes[b].weight * (uint64_t)(hi[ch] - lo[ch] + 1);
                if (score > best_score) {
                    best_score = score;
                    best = (int)b;
                    best_channel = ch;
                }
            }
            if (best < 0) break;
            Box& box = boxes[(size_t)best];
            std::sort(box.members.begin(), box.members.end(), [&](int a, int b) { return channel(a, best_channel) < channel(b, best_channel); });
            uint64_t half = box.weight / 2, sum = 0;
            size_t cut = 0;
            while (cut < box.members.size() - 1) {
                sum += hist[box.members[cut]];
                cut++;
                if (sum >= half) break;
            }
            Box other;
            other.members.assign(box.members.begin() + (long)cut, box.members.end());
            other.weight = box.weight - sum;
            box.members.resize(cut);
            box.weight = sum;
            boxes.push_back(std::move(other));
        }

        palette.fill(0);
        lut.assign(cells, 0);
        for (size_t b = 0; b < boxes.size(); b++) {
            uint64_t r = 0, g = 0, bl = 0, w = 0;
            for (int c : boxes[b].members) {
                uint64_t n = hist[c];
                r += n * (uint64_t)(((c >> 10) & 31) * 255 / 31);
                g += n * (uint64_t)(((c >> 5) & 31) * 255 / 31);
                bl += n * (uint64_t)((c & 31) * 255 / 31);
                w += n;
                lut[c] = (uint8_t)b;
            }
            if (w == 0) continue;
            palette[b * 3 + 0] = (uint8_t)(r / w);
            palette[b * 3 + 1] = (uint8_t)(g / w);
            palette[b * 3 + 2] = (uint8_t)(bl / w);
        }
    }

    // Bytes out in sub-blocks of up to 255, bits packed low first.
    struct BitWriter {
        FILE* f;
        uint32_t bits = 0;
        int count = 0;
        uint8_t block[255];
        int used = 0;
        void byte(uint8_t b) {
            block[used++] = b;
            if (used == 255) flush_block();
        }
        void flush_block() {
            if (used == 0) return;
            fputc(used, f);
            fwrite(block, 1, (size_t)used, f);
            used = 0;
        }
        void code(uint32_t c, int size) {
            bits |= c << count;
            count += size;
            while (count >= 8) {
                byte((uint8_t)(bits & 0xFF));
                bits >>= 8;
                count -= 8;
            }
        }
        void finish() {
            if (count > 0) byte((uint8_t)(bits & 0xFF));
            bits = 0;
            count = 0;
            flush_block();
            fputc(0, f);  // the block terminator
        }
    };

    void lzw(FILE* f, const std::vector<uint8_t>& indices) {
        constexpr int min_code = 8, clear = 1 << min_code, end = clear + 1, table_size = 1 << 14;
        fputc(min_code, f);
        BitWriter out{f};
        // (prefix code << 8 | next byte) + 1 -> code, open addressing; 0 is empty.
        auto keys = std::make_unique<uint32_t[]>(table_size);
        auto codes = std::make_unique<uint16_t[]>(table_size);
        int next = end + 1, size = min_code + 1;
        auto reset = [&] {
            std::fill_n(keys.get(), table_size, 0u);
            next = end + 1;
            size = min_code + 1;
        };
        reset();
        out.code(clear, size);
        if (indices.empty()) {
            out.code(end, size);
            out.finish();
            return;
        }
        uint32_t prefix = indices[0];
        for (size_t i = 1; i < indices.size(); i++) {
            uint32_t key = ((prefix << 8) | indices[i]) + 1;
            uint32_t slot = (key * 2654435761u) >> 18;
            while (keys[slot] != 0 && keys[slot] != key) slot = (slot + 1) & (table_size - 1);
            if (keys[slot] == key) {
                prefix = codes[slot];
                continue;
            }
            out.code(prefix, size);
            if (next < 4096) {
                keys[slot] = key;
                codes[slot] = (uint16_t)next;
                // The decoder's table is one behind: it needs the longer code once next passes the current size.
                if (next == (1 << size) && size < 12) size++;
                next++;
            }
            else {
                out.code(clear, size);
                reset();
            }
            prefix = indices[i];
        }
        out.code(prefix, size);
        out.code(end, size);
        out.finish();
    }

    void put16(FILE* f, int v) {
        fputc(v & 0xFF, f);
        fputc((v >> 8) & 0xFF, f);
    }
}

namespace blitz {
    GifWriter::~GifWriter() {
        if (file != nullptr) fclose(file);
    }

    bool GifWriter::open(const std::filesystem::path& path, int w, int h) {
        width = w;
        height = h;
        failed = false;
#if defined(_WIN32)
        file = _wfopen(path.c_str(), L"wb");
#else
        file = fopen(path.c_str(), "wb");
#endif
        if (file == nullptr) return false;
        fwrite("GIF89a", 1, 6, file);
        put16(file, w);
        put16(file, h);
        fputc(0x70, file);  // no global palette
        fputc(0, file);
        fputc(0, file);
        // Loop forever (NETSCAPE2.0).
        static const uint8_t loop[] = {0x21, 0xFF, 0x0B, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0', 0x03, 0x01, 0x00, 0x00, 0x00};
        fwrite(loop, 1, sizeof loop, file);
        return !ferror(file);
    }

    bool GifWriter::add_frame(const uint8_t* rgba, int delay_cs) {
        if (file == nullptr) return false;
        const int count = width * height;
        std::array<uint8_t, 768> palette;
        std::vector<uint8_t> lut;
        make_palette(rgba, count, palette, lut);
        std::vector<uint8_t> indices((size_t)count);
        for (int i = 0; i < count; i++) indices[(size_t)i] = lut[(size_t)cell_of(rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2])];

        // Graphic control: the delay. Image descriptor: this frame's own 256 colours.
        const uint8_t control[] = {0x21, 0xF9, 0x04, 0x04, (uint8_t)(delay_cs & 0xFF), (uint8_t)((delay_cs >> 8) & 0xFF), 0x00, 0x00};
        fwrite(control, 1, sizeof control, file);
        fputc(0x2C, file);
        put16(file, 0);
        put16(file, 0);
        put16(file, width);
        put16(file, height);
        fputc(0x87, file);
        fwrite(palette.data(), 1, palette.size(), file);
        lzw(file, indices);
        if (ferror(file)) failed = true;
        return !failed;
    }

    bool GifWriter::close() {
        if (file == nullptr) return false;
        fputc(0x3B, file);
        bool ok = !failed && fflush(file) == 0 && !ferror(file);
        fclose(file);
        file = nullptr;
        return ok;
    }
}
