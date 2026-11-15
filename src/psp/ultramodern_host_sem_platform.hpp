// ultramodern::host_sem for the PSP builds (ULTRAMODERN_HOST_SEM_PLATFORM, lib/N64ModernRuntime's host_thread.hpp; docs
// /PSP_PORT.md §4.1). The interface is the part of moodycamel::LightweightSemaphore the runtime uses: wait(), wait(usecs),
// tryWait(), signal(n).
//   PSP       a kernel semaphore (sceKernelCreateSema). Never a condition variable: the probe found libstdc++'s
//             std::condition_variable loses notifies there (§14.5), and std::atomic::wait falls back to one.
//   psp-host  moodycamel's LightweightSemaphore, as before, so the Mac runs the same runtime code paths (recomp.cpp's game
//             start waits on a host_sem under this define: patch 0021).
// Kernel semaphores made before main() (static host_sem objects in the runtime) are fine: static constructors run in the
// module's start thread, which may call the kernel.
#pragma once
#include <cstdint>
#include <cstddef>

#if defined(__PSP__)
#include <pspkerneltypes.h>
#include <pspthreadman.h>

namespace ultramodern {
    class host_sem {
    public:
        explicit host_sem(int initial = 0) {
            id_ = sceKernelCreateSema("host_sem", 0, initial, 0x7FFFFFFF, nullptr);
        }
        ~host_sem() {
            if (id_ >= 0) sceKernelDeleteSema(id_);
        }
        host_sem(const host_sem&) = delete;
        host_sem& operator=(const host_sem&) = delete;

        bool wait() { return sceKernelWaitSema(id_, 1, nullptr) >= 0; }
        // false on a timeout. moodycamel's semantics: usecs < 0 waits for ever, 0 only polls.
        bool wait(std::int64_t usecs) {
            if (usecs < 0) return wait();
            if (usecs == 0) return tryWait();
            SceUInt t = usecs > 0x7FFFFFFF ? 0x7FFFFFFFu : (SceUInt)usecs;
            return sceKernelWaitSema(id_, 1, &t) >= 0;
        }
        bool tryWait() { return sceKernelPollSema(id_, 1) >= 0; }
        void signal(std::ptrdiff_t count = 1) {
            if (count > 0) sceKernelSignalSema(id_, (int)count);
        }

    private:
        SceUID id_ = -1;
    };
}
#else
#undef MOODYCAMEL_DELETE_FUNCTION
#define MOODYCAMEL_DELETE_FUNCTION = delete
#include "lightweightsemaphore.h"

namespace ultramodern {
    using host_sem = moodycamel::LightweightSemaphore;
}
#endif
