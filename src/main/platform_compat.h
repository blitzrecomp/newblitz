// The few POSIX names the app uses that Windows spells differently (docs/WINDOWS_VULKAN.md). Plain renames and one
// helper only; anything bigger lives behind a platform function (macos_support.mm / platform_windows.cpp).
#pragma once
#include <filesystem>

#ifdef _WIN32
#include <io.h>
#include <string.h>
#include <windows.h>
#define strncasecmp _strnicmp
#define strcasecmp _stricmp
inline int fsync(int fd) { return _commit(fd); }
#else
#include <fcntl.h>
#include <strings.h>
#include <unistd.h>
#endif

#include <chrono>
#include <thread>

namespace blitz {
    // Non-throwing checks: a refused look (a sandboxed console app) answers false instead of ending the app, as the
    // throwing std::filesystem forms did on the PS5 (docs/PS5_HOMEBREW.md).
    inline bool file_exists(const std::filesystem::path& p) {
        std::error_code ec;
        return std::filesystem::exists(p, ec);
    }
    inline bool is_folder(const std::filesystem::path& p) {
        std::error_code ec;
        return std::filesystem::is_directory(p, ec);
    }

    // Moves a finished temporary file over the real one (a save's last step). Windows: an antivirus or the search
    // indexer can hold a just-written file for a moment, so a refused rename is retried briefly.
    inline void replace_file(const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& ec) {
        std::filesystem::rename(from, to, ec);
#ifdef _WIN32
        for (int i = 0; ec && i < 10; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::filesystem::rename(from, to, ec);
        }
#endif
    }

    // Flushes a written file to the disk before it replaces an older one (save states, Advanced saves).
    inline bool sync_file(const std::filesystem::path& p) {
#ifdef _WIN32
        HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        bool ok = FlushFileBuffers(h) != 0;
        CloseHandle(h);
        return ok;
#else
        int fd = ::open(p.c_str(), O_RDONLY);
        if (fd < 0) return false;
        bool ok = ::fsync(fd) == 0;
        ::close(fd);
        return ok;
#endif
    }
}
