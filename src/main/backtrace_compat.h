// backtrace() and backtrace_symbols_fd() (execinfo.h) everywhere but Windows: Android's C library has them only from
// API 33, so there they're a walk of the unwinder (frames as library+offset: llvm-symbolizer with the unstripped
// build/android/app_<flavour>/libBlitzRecompiled.so). For app_log.cpp's crash report and debug_print.cpp's call chains.
#pragma once
#if defined(__ANDROID__)
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <unistd.h>
#include <unwind.h>

namespace blitz_bt {
    struct Walk {
        void** frames;
        int count, max;
    };
    inline _Unwind_Reason_Code step(_Unwind_Context* ctx, void* arg) {
        Walk* w = static_cast<Walk*>(arg);
        uintptr_t pc = _Unwind_GetIP(ctx);
        if (pc != 0) {
            if (w->count >= w->max) return _URC_END_OF_STACK;
            w->frames[w->count++] = reinterpret_cast<void*>(pc);
        }
        return _URC_NO_REASON;
    }
}

static inline int backtrace(void** frames, int max) {
    blitz_bt::Walk w{frames, 0, max};
    _Unwind_Backtrace(blitz_bt::step, &w);
    return w.count;
}

static inline void backtrace_symbols_fd(void* const* frames, int count, int fd) {
    for (int i = 0; i < count; i++) {
        Dl_info info{};
        char line[512];
        int n;
        if (dladdr(frames[i], &info) && info.dli_fname != nullptr) {
            const char* base = strrchr(info.dli_fname, '/');
            n = snprintf(line, sizeof(line), "%2d %s+0x%zx %s\n", i, base ? base + 1 : info.dli_fname,
                         (size_t)((uintptr_t)frames[i] - (uintptr_t)info.dli_fbase), info.dli_sname ? info.dli_sname : "");
        } else {
            n = snprintf(line, sizeof(line), "%2d %p\n", i, frames[i]);
        }
        if (n > 0) (void)!write(fd, line, (size_t)n);
    }
}
#elif !defined(_WIN32)
#include <execinfo.h>
#endif
