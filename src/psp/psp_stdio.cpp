// newlib's stdout and stderr into the log ring on the PSP (§14.10 open problem 5; psp-host does it with a pipe). Every
// stdio write in newlib ends in _write_r (libc.a's writer.o); this file defines it, so the linker never takes the archive's.
// Writes to fds 1 and 2 are cut into lines and logged: a line "[tag] message" (the game's own fprintf(stderr, ...) style)
// keeps its tag, anything else is tagged "out". Before psp_log is up, and for every other fd, the write goes to libcglue's
// _write as before. psp_log's own stdout sink writes with sceIoWrite, so nothing loops.
#include <cerrno>
#include <cstring>
#include <reent.h>
#include <string>

#include "psp_log.h"
#include "psp_sys.h"

extern "C" int _write(int fd, const void* buf, size_t count);   // libcglue

namespace {
    pspsys::Mutex* g_mutex = nullptr;
    std::string g_pending[2];

    void forward_line(const std::string& line) {
        if (line.empty()) return;
        if (line[0] == '[') {
            size_t close = line.find(']');
            if (close != std::string::npos && close < 24 && close + 1 < line.size()) {
                std::string tag = line.substr(1, close - 1);
                size_t body = close + 1;
                while (body < line.size() && line[body] == ' ') body++;
                psplog::log(psplog::INFO, tag.c_str(), "%s", line.c_str() + body);
                return;
            }
        }
        psplog::log(psplog::INFO, "out", "%s", line.c_str());
    }
}

namespace host {
    // From psp_main once psp_log is up (the mutex is made here, on the main thread, before any other thread prints).
    int capture_output() {
        if (g_mutex == nullptr) g_mutex = new pspsys::Mutex();
        setvbuf(stdout, nullptr, _IOLBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
        return 0;
    }
}

extern "C" _ssize_t _write_r(struct _reent* r, int fd, const void* buf, size_t count) {
    if ((fd == 1 || fd == 2) && g_mutex != nullptr && psplog::ready()) {
        const char* p = static_cast<const char*>(buf);
        std::string lines[8];
        int n = 0;
        {
            pspsys::Lock l(*g_mutex);
            std::string& pend = g_pending[fd - 1];
            pend.append(p, count);
            size_t at;
            while ((at = pend.find('\n')) != std::string::npos) {
                std::string line = pend.substr(0, at);
                pend.erase(0, at + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (n < 8) lines[n++] = line;
                else forward_line(line);
            }
            if (pend.size() > 2048) {   // no newline coming: log what there is
                lines[n < 8 ? n++ : 7] = pend;
                pend.clear();
            }
        }
        for (int i = 0; i < n; i++) forward_line(lines[i]);
        return (_ssize_t)count;
    }
    errno = 0;
    int ret = _write(fd, buf, count);
    if (ret < 0 && errno != 0) r->_errno = errno;
    return ret;
}
