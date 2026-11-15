// The game's debug printf (two identical empty stubs in the retail ROM, 0x8001DC20 and 0x8001DC34;
// renamed in syms/names.toml and left out of recompilation). Blitz still calls them with its own
// diagnostics: heap errors ("The memory heap has become corrupted!"), hung processes, file system
// errors. Formats them and writes "[game] ..." lines to stderr.
//
// Arguments follow the o32 convention: the format in a0, then a1-a3, then the caller's outgoing
// argument area at sp+0x10. A double takes an 8-byte-aligned pair of slots (high word first).
//
// Each line carries the time since launch and the game's call chain.
#include <algorithm>
#include <chrono>
#if !defined(_WIN32) && !defined(__PSP__)
#include <dlfcn.h>
#include "../main/backtrace_compat.h"
#endif
#include <cstdio>
#include <cstring>
#include <string>

#include "recomp.h"

namespace {
    struct Args {
        uint8_t* rdram;
        recomp_context* ctx;
        int slot = 1;  // slot 0 is the format string

        uint32_t word(int i) {
            switch (i) {
                case 1: return (uint32_t)ctx->r5;
                case 2: return (uint32_t)ctx->r6;
                case 3: return (uint32_t)ctx->r7;
                default: return (uint32_t)MEM_W(0x10 + 4 * (i - 4), ctx->r29);
            }
        }
        uint32_t next32() { return word(slot++); }
        double next_double() {
            if (slot & 1) {
                slot++;
            }
            uint64_t bits = ((uint64_t)word(slot) << 32) | word(slot + 1);
            slot += 2;
            double d;
            memcpy(&d, &bits, sizeof(d));
            return d;
        }
    };

    std::string read_string(uint8_t* rdram, uint32_t addr, size_t max = 256) {
        std::string out;
        if (addr < 0x80000000u || addr >= 0x80800000u) {
            return "(bad string)";
        }
        for (size_t i = 0; i < max; i++) {
            char c = (char)MEM_B(i, (int32_t)addr);
            if (c == 0) {
                break;
            }
            out += c;
        }
        return out;
    }

    const auto process_start = std::chrono::steady_clock::now();

    // The game's call chain. Recompiled calls don't maintain ra, but every recompiled function is a
    // native function named after the original (heap_error, func_8000xxxx, ovl_b_func_...), so
    // the host stack gives the chain directly.
    std::string backtrace_names() {
#if defined(_WIN32)
        return "";  // Windows: no symbol names yet (needs DbgHelp and a .pdb)
#elif defined(__PSP__)
        return "";  // PSP: no dladdr (psp-addr2line on newblitz.elf names the PCs instead)
#else
        void* frames[16];
        int n = ::backtrace(frames, 16);
        std::string out;
        int shown = 0;
        for (int i = 0; i < n && shown < 6; i++) {
            Dl_info info;
            if (!dladdr(frames[i], &info) || info.dli_sname == nullptr) {
                continue;
            }
            std::string name = info.dli_sname;
            if (name.find("debug_printf") != std::string::npos || name.find("game_printf") != std::string::npos ||
                name.find("backtrace_names") != std::string::npos) {
                continue;
            }
            out += (shown++ ? " < " : " ") + name;
        }
        return out;
#endif
    }

    void game_printf(uint8_t* rdram, recomp_context* ctx) {
        std::string fmt = read_string(rdram, (uint32_t)ctx->r4);
        Args args{rdram, ctx};
        std::string out;
        for (size_t i = 0; i < fmt.size(); i++) {
            if (fmt[i] != '%') {
                out += fmt[i];
                continue;
            }
            // Copy the conversion spec (flags, width, precision, length) and format one argument.
            size_t j = i + 1;
            while (j < fmt.size() && strchr("-+ #0123456789.lh", fmt[j])) {
                j++;
            }
            if (j >= fmt.size()) {
                break;
            }
            std::string spec = fmt.substr(i, j - i);
            char conv = fmt[j];
            spec.erase(std::remove_if(spec.begin(), spec.end(), [](char c) { return c == 'l' || c == 'h'; }), spec.end());
            char buf[512];
            switch (conv) {
                case 'd': case 'i': snprintf(buf, sizeof(buf), (spec + "d").c_str(), (int32_t)args.next32()); break;
                case 'u': case 'x': case 'X': case 'o': case 'c':
                    snprintf(buf, sizeof(buf), (spec + conv).c_str(), args.next32()); break;
                case 'p': snprintf(buf, sizeof(buf), "%08X", args.next32()); break;
                case 's': snprintf(buf, sizeof(buf), (spec + "s").c_str(), read_string(rdram, args.next32()).c_str()); break;
                case 'f': case 'e': case 'g': snprintf(buf, sizeof(buf), (spec + conv).c_str(), args.next_double()); break;
                case '%': snprintf(buf, sizeof(buf), "%%"); break;
                default: snprintf(buf, sizeof(buf), "%s%c", spec.c_str(), conv); break;
            }
            out += buf;
            i = j;
        }
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
            out.pop_back();
        }
        double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - process_start).count();
        fprintf(stderr, "[game %.1fs] %s   (calls:%s)\n", t, out.c_str(),
                backtrace_names().c_str());
    }
}

extern "C" void blitz_debug_printf(uint8_t* rdram, recomp_context* ctx) {
    game_printf(rdram, ctx);
}

extern "C" void blitz_debug_printf_2(uint8_t* rdram, recomp_context* ctx) {
    game_printf(rdram, ctx);
}
