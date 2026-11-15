// thread_local on the PSP (docs/PSP_PORT.md §14.12). psp-gcc has no native TLS, so every thread_local goes through libgcc's
// emulated TLS (__emutls_get_address), which keeps each thread's block in pthread-embedded's pthread_getspecific. That
// one finds a thread's slot by parsing the kernel thread's *name* ("pthread%04d__%x"); a thread made with
// sceKernelCreateThread (every thread of ours: the runtime's, the game's, the log's) doesn't have such a name and gets
// the one shared global slot. So thread_local was one variable for all of them: ultramodern's thread_self and
// is_game_thread, moodycamel's thread ids. The game's first osStartThread then took the wrong path (thread_self set by
// another thread) and the boot stopped with the idle thread never resumed (M5's first run).
//
// This file defines the two emutls entry points, so libgcc's emutls.o is never linked: each thread's values are found by
// its kernel thread id in a small open-addressing table. It is our own code against the emutls ABI (the object layout
// below, which gcc emits for every thread_local). It also gives libstdc++'s exception globals a per-thread home (below):
// those use a pthread key directly, not thread_local, so emutls alone didn't fix them.
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <malloc.h>

#include <pspkernel.h>
#include <pspthreadman.h>

namespace {
    struct emutls_object {           // gcc's __emutls_object: word size, word align, offset-or-pointer, template
        unsigned int size;
        unsigned int align;
        union {
            uintptr_t offset;        // 1 + this variable's index, 0 until its first use
            void* ptr;
        } loc;
        void* templ;
    };

    constexpr int table_bits = 9, table_size = 1 << table_bits;   // 512 threads at once (the PSP's limit is lower)
    constexpr SceUID empty = 0, deleted = -1;

    struct ThreadValues {
        volatile SceUID uid;
        void** values;
        uint32_t count;
    };
    ThreadValues g_table[table_size];
    SceUID g_lock = -1;
    uint32_t g_objects = 0;
    int g_threads = 0;

    void lock() {
        if (g_lock < 0) g_lock = sceKernelCreateSema("emutls", 0, 1, 1, nullptr);   // first use: the main thread, at start
        sceKernelWaitSema(g_lock, 1, nullptr);
    }
    void unlock() { sceKernelSignalSema(g_lock, 1); }

    uint32_t slot_of(SceUID uid) { return ((uint32_t)uid * 2654435761u) >> (32 - table_bits); }

    // Only the thread itself ever inserts or removes its own entry, so its lookup needs no lock.
    ThreadValues* find(SceUID uid) {
        for (uint32_t i = slot_of(uid), n = 0; n < (uint32_t)table_size; i = (i + 1) & (table_size - 1), n++) {
            SceUID u = g_table[i].uid;
            if (u == uid) return &g_table[i];
            if (u == empty) return nullptr;
        }
        return nullptr;
    }

    ThreadValues* insert(SceUID uid) {
        lock();
        ThreadValues* t = nullptr;
        for (uint32_t i = slot_of(uid), n = 0; n < (uint32_t)table_size; i = (i + 1) & (table_size - 1), n++) {
            if (g_table[i].uid == empty || g_table[i].uid == deleted) {
                t = &g_table[i];
                t->values = nullptr;
                t->count = 0;
                __asm__ __volatile__("" ::: "memory");
                t->uid = uid;
                g_threads++;
                break;
            }
        }
        unlock();
        if (t == nullptr) abort();   // more threads at once than the table holds
        return t;
    }
}

extern "C" void* __emutls_get_address(emutls_object* obj) {
    uintptr_t index = obj->loc.offset;
    if (index == 0) {
        lock();
        if (obj->loc.offset == 0) obj->loc.offset = ++g_objects;
        index = obj->loc.offset;
        unlock();
    }
    SceUID self = sceKernelGetThreadId();
    ThreadValues* t = find(self);
    if (t == nullptr) t = insert(self);
    if (index > t->count) {
        uint32_t n = (uint32_t)index + 8;
        void** v = static_cast<void**>(realloc(t->values, n * sizeof(void*)));
        if (v == nullptr) abort();
        memset(v + t->count, 0, (n - t->count) * sizeof(void*));
        t->values = v;
        t->count = n;
    }
    void*& p = t->values[index - 1];
    if (p == nullptr) {
        size_t align = obj->align > sizeof(void*) ? obj->align : sizeof(void*);
        p = memalign(align, obj->size ? obj->size : 1);
        if (p == nullptr) abort();
        if (obj->templ != nullptr) memcpy(p, obj->templ, obj->size);
        else memset(p, 0, obj->size);
    }
    return p;
}

// A common (uninitialised) thread_local seen in several objects: the biggest size and alignment win, as libgcc does.
extern "C" void __emutls_register_common(emutls_object* obj, unsigned int size, unsigned int align, void* templ) {
    if (obj->size < size) {
        obj->size = size;
        obj->templ = nullptr;
    }
    if (obj->align < align) obj->align = align;
    if (templ != nullptr && size == obj->size) obj->templ = templ;
}

// A thread that ends (psp_threads.cpp's dedicated threads) frees its values, so a later thread that gets the same kernel id
// starts from the templates again.
extern "C" void blitz_tls_thread_exit(void) {
    ThreadValues* t = find(sceKernelGetThreadId());
    if (t == nullptr) return;
    // the destructors of thread_local objects with them are not run (none of the PSP build's need it)
    lock();
    for (uint32_t i = 0; i < t->count; i++) free(t->values[i]);
    free(t->values);
    t->values = nullptr;
    t->count = 0;
    t->uid = deleted;
    g_threads--;
    unlock();
}

// libstdc++'s exception globals (the caught-exception stack and the uncaught count behind throw, catch, `throw;` and
// std::uncaught_exceptions). psp-gcc's libstdc++ is built without TLS, so its eh_globals.o keeps them in a pthread key,
// which pthread-embedded shares between every thread of ours (above): one stack for all of them, and a thread that
// catches while another is inside a catch block pops the other's exception. Defining the two entry points here keeps
// eh_globals.o out of the link and gives each kernel thread its own (a thread_local, so through the table above). The
// layout is libsupc++'s __cxa_eh_globals (unwind-cxx.h: caughtExceptions, uncaughtExceptions; MIPS has no ARM EABI
// field); the spare words only make it bigger than that.
namespace {
    struct EhGlobals {
        void* caught_exceptions;
        unsigned int uncaught_exceptions;
        void* spare[2];
    };
    thread_local EhGlobals t_eh_globals;   // zero-initialised: no template, no destructor
}
extern "C" void* __cxa_get_globals_fast() noexcept { return &t_eh_globals; }
extern "C" void* __cxa_get_globals() noexcept { return &t_eh_globals; }

extern "C" int blitz_tls_stats(int* threads) {
    if (threads) *threads = g_threads;
    return (int)g_objects;
}
