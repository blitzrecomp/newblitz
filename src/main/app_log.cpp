// <app folder>/blitz.log: everything the app prints (stdout and stderr), capped at 8 MB, plus the
// crash location if it crashes. The previous run's log is kept as blitz.prev.log. Packaged apps have
// no terminal, so this is the only record of what went wrong. The terminal still gets everything.
extern "C" void blitz_crash_details(int fd);  // play_log.cpp: the game's state, in a crash report
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <thread>

#ifdef _WIN32
// Windows: a packaged app has no terminal, so stdout and stderr go straight to blitz.log (no 8 MB cap yet). A crash
// writes its code, each stack frame as module+offset (symbolized on the Mac with the build's .pdb:
// llvm-symbolizer --obj="Classic Blitz.exe" 0x140000000+offset) and a minidump, dumps/crash-<time>.dmp, in the app
// folder. DbgHelp is the system's, loaded only when a crash happens.
#include <exception>
#include <io.h>
#include <windows.h>
#include <dbghelp.h>

namespace {
    std::filesystem::path crash_dump_folder;

    // "Classic Blitz.exe+0x1a2b3c" for an address, or the bare address outside any module.
    void describe(DWORD64 addr, char* out, size_t size) {
        HMODULE mod = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)addr, &mod) && mod != nullptr) {
            char name[MAX_PATH] = {};
            GetModuleFileNameA(mod, name, MAX_PATH);
            const char* base = strrchr(name, '\\');
            snprintf(out, size, "%s+0x%llx", base ? base + 1 : name, (unsigned long long)(addr - (DWORD64)mod));
        } else {
            snprintf(out, size, "0x%llx", (unsigned long long)addr);
        }
    }

    LONG WINAPI on_crash(EXCEPTION_POINTERS* info) {
        static LONG once = 0;
        if (InterlockedExchange(&once, 1) != 0) return EXCEPTION_CONTINUE_SEARCH;
        char where[MAX_PATH + 32];
        describe((DWORD64)info->ExceptionRecord->ExceptionAddress, where, sizeof(where));
        fprintf(stderr, "\n[crash] exception 0x%08lX at %s\n", info->ExceptionRecord->ExceptionCode, where);
        if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2)
            fprintf(stderr, "[crash] %s address 0x%llx\n", info->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                    (unsigned long long)info->ExceptionRecord->ExceptionInformation[1]);

        HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
        if (dbghelp != nullptr) {
            // The faulting thread's stack, frame by frame.
            auto stack_walk = (decltype(&StackWalk64))GetProcAddress(dbghelp, "StackWalk64");
            auto table_access = (decltype(&SymFunctionTableAccess64))GetProcAddress(dbghelp, "SymFunctionTableAccess64");
            auto module_base = (decltype(&SymGetModuleBase64))GetProcAddress(dbghelp, "SymGetModuleBase64");
            auto sym_init = (decltype(&SymInitialize))GetProcAddress(dbghelp, "SymInitialize");
            if (stack_walk && table_access && module_base && sym_init) {
                HANDLE process = GetCurrentProcess();
                sym_init(process, nullptr, TRUE);
                CONTEXT context = *info->ContextRecord;
                STACKFRAME64 frame{};
                frame.AddrPC.Offset = context.Rip;
                frame.AddrPC.Mode = AddrModeFlat;
                frame.AddrFrame.Offset = context.Rbp;
                frame.AddrFrame.Mode = AddrModeFlat;
                frame.AddrStack.Offset = context.Rsp;
                frame.AddrStack.Mode = AddrModeFlat;
                fprintf(stderr, "[crash] stack:\n");
                for (int i = 0; i < 40; i++) {
                    if (!stack_walk(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &context, nullptr,
                                    table_access, module_base, nullptr) || frame.AddrPC.Offset == 0)
                        break;
                    describe(frame.AddrPC.Offset, where, sizeof(where));
                    fprintf(stderr, "  %2d %s\n", i, where);
                }
            }
            // A minidump beside the log, for a closer look.
            auto write_dump = (decltype(&MiniDumpWriteDump))GetProcAddress(dbghelp, "MiniDumpWriteDump");
            if (write_dump && !crash_dump_folder.empty()) {
                std::error_code ec;
                std::filesystem::create_directories(crash_dump_folder, ec);
                char stamp[32];
                time_t now = time(nullptr);
                strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime(&now));
                std::filesystem::path file = crash_dump_folder / (std::string("crash-") + stamp + ".dmp");
                HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h != INVALID_HANDLE_VALUE) {
                    MINIDUMP_EXCEPTION_INFORMATION exception{GetCurrentThreadId(), info, FALSE};
                    BOOL ok = write_dump(GetCurrentProcess(), GetCurrentProcessId(), h, MiniDumpWithThreadInfo, &exception, nullptr, nullptr);
                    CloseHandle(h);
                    fprintf(stderr, "[crash] %s %s\n", ok ? "dump written:" : "dump failed:", file.string().c_str());
                }
            }
        }
        fflush(stderr);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // An uncaught C++ exception or abort() isn't a structured exception: note those too.
    void on_terminate() {
        fprintf(stderr, "\n[crash] std::terminate (an uncaught exception)\n");
        fflush(stderr);
        std::abort();
    }

    void on_abort(int) {
        fprintf(stderr, "\n[crash] abort\n");
        fflush(stderr);
    }
}

namespace blitz {
    void log_init(const std::filesystem::path& app_folder, const std::string& version) {
        if (getenv("BLITZ_NO_LOG") != nullptr) return;
        std::error_code ec;
        std::filesystem::create_directories(app_folder, ec);
        crash_dump_folder = app_folder / "dumps";
        std::filesystem::path path = app_folder / "blitz.log", prev = app_folder / "blitz.prev.log";
        std::filesystem::remove(prev, ec);
        std::filesystem::rename(path, prev, ec);
        // A windowed app starts with no stdout or stderr: both are opened on the log, appending (so neither
        // overwrites the other), after starting it empty. Unbuffered, so a crash loses nothing (Windows has no line
        // buffering: _IOLBF is full buffering).
        if (FILE* fresh = _wfopen(path.c_str(), L"w")) fclose(fresh);
        if (_wfreopen(path.c_str(), L"a", stderr) == nullptr) return;
        _wfreopen(path.c_str(), L"a", stdout);
        setvbuf(stderr, nullptr, _IONBF, 0);
        setvbuf(stdout, nullptr, _IONBF, 0);
        std::set_terminate(on_terminate);
        signal(SIGABRT, on_abort);
        time_t now = time(nullptr);
        char when[64];
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", localtime(&now));
        fprintf(stderr, "[log] %s %s started %s\n", app_folder.filename().string().c_str(), version.c_str(), when);
        SetUnhandledExceptionFilter(on_crash);
    }

    void log_shutdown() {
        fflush(stdout);
        fflush(stderr);
    }
}
#else
#include <fcntl.h>
#include <unistd.h>
#include "backtrace_compat.h"
#ifdef __ANDROID__
#include <android/log.h>
#include <chrono>
#include <cstdlib>
#endif
#ifdef __PROSPERO__
#include <ps5platform/klog.h>
extern "C" void blitz_ps5_crumb(const char* step);  // platform_ps5.cpp: /data/blitzrecomp/boot.log
#define LOG_CRUMB(step) blitz_ps5_crumb(step)
#else
#define LOG_CRUMB(step) ((void)0)
#endif

namespace {
    constexpr size_t log_cap = 8 * 1024 * 1024;
    int log_fd = -1;
    int terminal_fd = -1;
    std::atomic<size_t> written{0};
    std::atomic<bool> log_up{false};  // stderr reaches blitz.log

    void write_all(int fd, const char* p, size_t n) {
        while (n > 0) {
            ssize_t w = ::write(fd, p, n);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) return;
            p += w;
            n -= (size_t)w;
        }
    }

#ifdef __ANDROID__
    // Potato Mode: a runaway line (880 a second once, a fight over limits) costs the card, logcat and the 8 MB this log may hold,
    // so past 32 KB in a second the rest goes, and is counted. The first 20 s are left alone: a start is a burst, and matters most.
    bool log_over_budget(size_t n) {
        static const bool potato = getenv("BLITZ_LEAN") != nullptr;
        if (!potato) return false;
        using Clock = std::chrono::steady_clock;
        static const Clock::time_point start = Clock::now();
        static Clock::time_point window = start;
        static size_t window_bytes = 0, dropped = 0;
        const Clock::time_point now = Clock::now();
        if (now - start < std::chrono::seconds(20)) return false;
        if (now - window >= std::chrono::seconds(1)) {
            window = now;
            window_bytes = 0;
            if (dropped > 0) {
                char note[64];
                const int len = snprintf(note, sizeof(note), "[log] %zu bytes dropped: too much at once\n", dropped);
                write_all(log_fd, note, (size_t)len);
                dropped = 0;
            }
        }
        window_bytes += n;
        if (window_bytes <= 32 * 1024) return false;
        dropped += n;
        return true;
    }
#endif

    void pump(int read_fd) {
        char buf[4096];
        bool capped = false;
        for (;;) {
            ssize_t n = ::read(read_fd, buf, sizeof(buf));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return;
#ifdef __ANDROID__
            if (log_over_budget((size_t)n)) continue;
#endif
#ifdef __PROSPERO__
            // No terminal on the console: the kernel log (klogsrv) gets it instead.
            std::string text(buf, (size_t)n);
            sceKernelDebugOutText(0, text.c_str());
#elif defined(__ANDROID__)
            // No terminal on a phone: logcat gets it (adb logcat -s BlitzRecomp).
            std::string text(buf, (size_t)n);
            // BRANDING: logcat tag
            __android_log_write(ANDROID_LOG_INFO, "BlitzRecomp", text.c_str());
#else
            write_all(terminal_fd, buf, (size_t)n);
#endif
            if (written + (size_t)n <= log_cap) {
                write_all(log_fd, buf, (size_t)n);
                written += (size_t)n;
            }
            else if (!capped) {
                capped = true;
                const char msg[] = "\n[log] blitz.log reached 8 MB; further output goes to the terminal only\n";
                write_all(log_fd, msg, sizeof(msg) - 1);
            }
        }
    }

    // Async-signal-safe as far as macOS allows: write(), backtrace(), backtrace_symbols_fd().
    // (blitz_crash_details reads the game's memory and formats it: all this does at a moment that's already lost.)
    void on_crash(int sig) {
        char head[96];
#ifdef __PROSPERO__
        const char* name = "";  // no strsignal in the console's C library
#else
        const char* name = strsignal(sig);
#endif
        int n = snprintf(head, sizeof(head), "\n[crash] signal %d (%s). Call stack:\n", sig, name);
        for (int fd : {log_fd, terminal_fd}) {
            if (fd < 0) continue;
            write_all(fd, head, (size_t)n);
            void* frames[64];
            int count = backtrace(frames, 64);
            backtrace_symbols_fd(frames, count, fd);
            blitz_crash_details(fd);
        }
        signal(sig, SIG_DFL);
        raise(sig);
    }
}

namespace blitz {
    void log_init(const std::filesystem::path& app_folder, const std::string& version) {
        if (getenv("BLITZ_NO_LOG") != nullptr) return;
        std::error_code ec;
        std::filesystem::create_directories(app_folder, ec);
        LOG_CRUMB(ec ? ("log_init: create_directories failed: " + ec.message()).c_str() : "log_init: folder ok");
        std::filesystem::path path = app_folder / "blitz.log", prev = app_folder / "blitz.prev.log";
        std::filesystem::rename(path, prev, ec);
        log_fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (log_fd < 0) { LOG_CRUMB("log_init: can't open blitz.log"); return; }
        int pipe_fds[2];
        if (::pipe(pipe_fds) != 0) { LOG_CRUMB("log_init: no pipe"); return; }
        LOG_CRUMB("log_init: file and pipe ok");
        fflush(stdout);
        fflush(stderr);
#ifdef __PROSPERO__
        // A title may not dup2 (the console refuses it): the streams themselves move to the pipe, as the platform's
        // ps5_klog_capture_stderr does. Everything printed through stdout/stderr arrives; a raw write(2) doesn't.
        if (FILE* stream = fdopen(pipe_fds[1], "w")) {
            setvbuf(stream, nullptr, _IOLBF, 0);
            stdout = stream;
            stderr = stream;
        }
#else
        terminal_fd = ::dup(STDERR_FILENO);
        ::dup2(pipe_fds[1], STDOUT_FILENO);
        ::dup2(pipe_fds[1], STDERR_FILENO);
        ::close(pipe_fds[1]);
#endif
        LOG_CRUMB("log_init: starting the pump thread");
        std::thread(pump, pipe_fds[0]).detach();
        LOG_CRUMB("log_init: pump thread started");

        time_t now = time(nullptr);
        char when[64];
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", localtime(&now));
        fprintf(stderr, "[log] %s %s started %s\n", app_folder.filename().c_str(), version.c_str(), when);
        for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) signal(sig, on_crash);
        log_up = true;
    }

    // Before log_init, stderr leads nowhere on a phone: BlitzActivity holds its reports until this says they'd arrive.
    bool log_is_up() { return log_up.load(); }

    // Before replacing the process (Settings → Restart): stdout and stderr back to the terminal, since
    // the pipe's reader thread doesn't survive execv. The relaunched copy opens a new log.
    void log_shutdown() {
        if (terminal_fd < 0) return;
        fflush(stdout);
        fflush(stderr);
        ::dup2(terminal_fd, STDOUT_FILENO);
        ::dup2(terminal_fd, STDERR_FILENO);
    }
}
#endif
