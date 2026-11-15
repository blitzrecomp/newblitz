// PS5 host services: the functions src/main/macos_support.mm provides on a Mac (docs/PS5_HOMEBREW.md).
#include <filesystem>
#include "blitz_host.h"
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

// The console's HTTP/2 library (libSceHttp2, with libSceSsl and libSceNet), called as the payload SDK's
// samples/http2_get does: HTTPS with the system's own certificates. Linked like the title's other system modules
// (the SDK's stubs; the console loads them with the title): loading them at the first request with
// sceKernelLoadStartModule + sceKernelDlsym found libSceNet but none of its functions (PS5 log, 2026-10-23:
// "[http] no sceNetInit").
extern "C" {
    int sceNetInit();
    int sceNetPoolCreate(const char*, int, int);
    int sceSslInit(size_t);
    int sceHttp2Init(int, int, size_t, int);
    int sceHttp2CreateTemplate(int, const char*, int, int);
    int sceHttp2CreateRequestWithURL(int, const char*, const char*, uint64_t);
    int sceHttp2DeleteRequest(int);
    int sceHttp2AddRequestHeader(int, const char*, const char*, uint32_t);
    int sceHttp2SendRequest(int, const void*, size_t);
    int sceHttp2GetStatusCode(int, int*);
    int sceHttp2ReadData(int, void*, size_t);
    int sceNetCtlInit();
    int sceNetCtlGetInfo(int, void*);
}

namespace {
    // One HTTP template for the app's life, made at the first request; -2 once it couldn't be.
    int http_template() {
        static int tmpl = -1;
        if (tmpl != -1) return tmpl;
        sceNetInit();  // fails harmlessly when the network library is already up
        // BRANDING: HTTP User-Agent / pool name
        int pool = sceNetPoolCreate("BlitzRecomp", 64 * 1024, 0);
        int ssl = pool >= 0 ? sceSslInit(256 * 1024) : -1;
        int ctx = ssl >= 0 ? sceHttp2Init(pool, ssl, 256 * 1024, 1) : -1;
        tmpl = ctx >= 0 ? sceHttp2CreateTemplate(ctx, "BlitzRecomp", 3, 1) : -1;
        if (tmpl < 0) {
            fprintf(stderr, "[http] the console's HTTP library didn't start (pool %d, ssl %d, ctx %d, template %d)\n", pool, ssl, ctx, tmpl);
            tmpl = -2;
        }
        return tmpl;
    }
}

namespace blitz {
    // This console's address on the local network ("192.168.1.20"), or "" (net_compat.h's local_address): the
    // network control library's answer; a title may not be allowed the interface list.
    std::string ps5_local_address() {
        static std::mutex lock;
        std::lock_guard guard{lock};
        static bool started = false;
        if (!started) {
            started = true;
            sceNetCtlInit();  // an error when it's already up is fine: the question below tells
        }
        alignas(8) char info[512] = {};  // SceNetCtlInfo, a union; the IP address is a string in it
        constexpr int ip_address = 14;   // SCE_NET_CTL_INFO_IP_ADDRESS
        if (sceNetCtlGetInfo(ip_address, info) != 0) return {};
        info[sizeof(info) - 1] = '\0';
        std::string ip = info;
        return ip == "0.0.0.0" ? std::string() : ip;
    }

    // macOS's press-and-hold accent picker; the PS5 has none.
    void disable_press_and_hold() {}

    // The Cocoa window's number (capture.cpp's screenshots); no capture backend on the PS5.
    int window_number(void*) { return 0; }

    // The macOS app menu; the PS5 has none.
    void install_app_menu(const std::string&) {}

    // A blocking HTTP GET (call it off the main thread): true with the body when the server answers 200.
    // Rosters at launch (rosters_live.cpp).
    bool http_get(const std::string& url, const std::string& bearer, const std::string& accept, std::string& body, int& status) {
        static std::mutex lock;  // one request at a time through the one template
        std::lock_guard guard{lock};
        body.clear();
        status = 0;
        int tmpl = http_template();
        if (tmpl < 0) return false;
        int req = sceHttp2CreateRequestWithURL(tmpl, "GET", url.c_str(), 0);
        if (req < 0) return false;
        if (!accept.empty()) sceHttp2AddRequestHeader(req, "Accept", accept.c_str(), 0);
        if (!bearer.empty()) sceHttp2AddRequestHeader(req, "Authorization", ("Bearer " + bearer).c_str(), 0);
        std::string got;
        int sent = sceHttp2SendRequest(req, nullptr, 0);
        static bool told = false;
        if (sent != 0 && !told) {
            told = true;  // once: a console offline fails every request the same way
            fprintf(stderr, "[http] request failed: 0x%08x\n", (unsigned)sent);
        }
        if (sent == 0 && sceHttp2GetStatusCode(req, &status) == 0 && status == 200) {
            char buf[16 * 1024];
            int n;
            while ((n = sceHttp2ReadData(req, buf, sizeof(buf))) > 0) got.append(buf, (size_t)n);
        }
        sceHttp2DeleteRequest(req);
        if (status != 200) return false;
        body = std::move(got);
        return true;
    }
}

// C library functions the title's link needs that neither the console's modules nor the payload SDK's platform layer
// provide (tools/ps5/link_title.sh binds ffs and timegm to ps5_ names, as UnleashedRecomp-Prospero's link does).
// Hidden: a title may not export libc's names.
#include <cstddef>
#include <ctime>

extern "C" {
    __attribute__((visibility("hidden"))) int ps5_ffs(int v) { return __builtin_ffs(v); }

    __attribute__((visibility("hidden"))) time_t ps5_timegm(struct tm* tm) {
        // Days from 1970-01-01 to the civil date (Howard Hinnant's days_from_civil), then the time of day.
        long long y = tm->tm_year + 1900LL + tm->tm_mon / 12;
        int m = tm->tm_mon % 12;
        if (m < 0) { m += 12; y--; }
        m += 1;
        y -= m <= 2;
        const long long era = (y >= 0 ? y : y - 399) / 400;
        const long long yoe = y - era * 400;
        const long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + tm->tm_mday - 1;
        const long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        const long long days = era * 146097 + doe - 719468;
        return (time_t)(days * 86400 + tm->tm_hour * 3600LL + tm->tm_min * 60LL + tm->tm_sec);
    }

    // sljit (the mod runtime's JIT, unused here: no mods on the PS5) hints at unused stack pages.
    __attribute__((visibility("hidden"))) int posix_madvise(void*, size_t, int) { return 0; }
}

// Ending a title: the kernel's exit() raises SIGSYS in a title (UnleashedRecomp-Prospero), so returning from main
// (the start-up code calls catchReturnFromMain) and every exit/_Exit/quick_exit (wrapped: tools/ps5/link_title.sh)
// ask the system shell to close it instead. The log pipe gets a moment to reach blitz.log first.
#include <unistd.h>
extern "C" {
    int sceSystemServiceLoadExec(const char* path, char* const argv[]);

    [[noreturn]] void catchReturnFromMain(int status) {
        fprintf(stderr, "[ps5] closing (status %d)\n", status);
        fflush(nullptr);
        usleep(200000);
        sceSystemServiceLoadExec("exit", nullptr);
        for (;;) usleep(100000);
    }
    [[noreturn]] void __wrap_exit(int status) { catchReturnFromMain(status); }
    [[noreturn]] void __wrap__Exit(int status) { catchReturnFromMain(status); }
    [[noreturn]] void __wrap_quick_exit(int status) { catchReturnFromMain(status); }
}

// Start-up breadcrumbs (bring-up on the console): /data/blitzrecomp/boot.log gets a line at each early step and the
// signal of an early crash, written straight to the file, before blitz.log exists. The first runs from
// .preinit_array, ahead of every static constructor.
#include <csignal>
#include <cstring>
#include <exception>
#include <string>
#include <fcntl.h>
#include <sys/stat.h>
extern "C" void blitz_ps5_crumb(const char* step) {
    int fd = open("/data/blitzrecomp/boot.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    write(fd, step, strlen(step));
    write(fd, "\n", 1);
    close(fd);
}
namespace {
    void early_crash(int sig) {
        char line[48];
        snprintf(line, sizeof(line), "crash: signal %d", sig);
        blitz_ps5_crumb(line);
        signal(sig, SIG_DFL);
        raise(sig);
    }
    // An uncaught exception names itself in boot.log before the abort.
    void early_terminate() {
        if (std::exception_ptr e = std::current_exception()) {
            try {
                std::rethrow_exception(e);
            } catch (const std::exception& x) {
                std::string line = std::string("uncaught exception: ") + x.what();
                blitz_ps5_crumb(line.c_str());
            } catch (...) {
                blitz_ps5_crumb("uncaught exception (not a std::exception)");
            }
        } else {
            blitz_ps5_crumb("std::terminate without an exception");
        }
        std::abort();
    }
    void boot_first() {
        std::set_terminate(early_terminate);
        mkdir("/data/blitzrecomp", 0777);
        int fd = open("/data/blitzrecomp/boot.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) close(fd);
        blitz_ps5_crumb("preinit (before static constructors)");
        for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGSYS}) signal(sig, early_crash);
    }
    __attribute__((section(".preinit_array"), used)) void (*const boot_first_entry)() = boot_first;
}

namespace blitz {
    // Copied over FTP (docs/PS5_HOMEBREW.md): a ROM put here is imported at startup.
    // BRANDING: PS5 app folder /data/blitzrecomp (user-visible over FTP)
    std::filesystem::path rom_drop_folder() { return "/data/blitzrecomp"; }
}

namespace blitz {
    void window_created(SDL_Window*) {}
}

namespace blitz {
    void early_init() {}
}
