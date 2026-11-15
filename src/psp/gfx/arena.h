// A first-fit allocator over one texture arena (the VRAM share or the RAM overflow; docs/PSP_PORT.md 4.5 As built (M7)).
// Live blocks are kept sorted by offset, so a free is a binary search and an allocation walks the gaps. The texture cache
// frees the least recently used entries into it when an allocation fails (tex.cpp), instead of dropping everything.
#pragma once
#include <algorithm>
#include <stddef.h>
#include <vector>

namespace blitzgfx {

class Arena {
public:
    void init(size_t size) { size_ = size; blocks_.clear(); used_ = 0; }
    void reset() { blocks_.clear(); used_ = 0; }
    size_t used() const { return used_; }
    size_t size() const { return size_; }
    size_t blocks() const { return blocks_.size(); }

    // false when no gap fits
    bool alloc(size_t bytes, size_t align, size_t& out) {
        if (bytes == 0) bytes = 1;
        size_t prev_end = 0;
        for (size_t i = 0; i <= blocks_.size(); i++) {
            size_t at = (prev_end + align - 1) & ~(align - 1);
            size_t limit = i < blocks_.size() ? blocks_[i].off : size_;
            if (at + bytes <= limit) {
                blocks_.insert(blocks_.begin() + (ptrdiff_t)i, B{ at, bytes });
                used_ += bytes;
                out = at;
                return true;
            }
            if (i < blocks_.size()) prev_end = blocks_[i].off + blocks_[i].len;
        }
        return false;
    }

    // false if off isn't the start of a live block
    bool free(size_t off) {
        auto it = std::lower_bound(blocks_.begin(), blocks_.end(), off, [](const B& b, size_t o) { return b.off < o; });
        if (it == blocks_.end() || it->off != off) return false;
        used_ -= it->len;
        blocks_.erase(it);
        return true;
    }

private:
    struct B { size_t off, len; };
    std::vector<B> blocks_;
    size_t size_ = 0, used_ = 0;
};

}  // namespace blitzgfx
