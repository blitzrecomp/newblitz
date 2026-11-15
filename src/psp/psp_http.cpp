// See psp_http.h.
#include "psp_http.h"
#include "psp_sys.h"
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#ifndef __PSP__
#include <fcntl.h>
#endif

namespace psphttp {

constexpr size_t MAX_HEADER = 16 * 1024;   // a reply whose headers run past this is refused

bool parse_url(const std::string& url, Url& out) {
    const std::string scheme = "http://";
    if (url.compare(0, scheme.size(), scheme) != 0) return false;
    size_t host = scheme.size();
    size_t slash = url.find('/', host);
    std::string hp = url.substr(host, slash == std::string::npos ? std::string::npos : slash - host);
    out.path = slash == std::string::npos ? "/" : url.substr(slash);
    size_t colon = hp.find(':');
    out.port = 80;
    if (colon != std::string::npos) {
        out.port = atoi(hp.c_str() + colon + 1);
        hp.resize(colon);
    }
    out.host = hp;
    return !out.host.empty() && out.port > 0 && out.port < 65536;
}

static int ms_since(uint64_t t0) { return (int)((pspsys::now_us() - t0) / 1000); }

bool resolve(const std::string& host, uint32_t& addr, int& ms, std::string& err) {
    uint64_t t0 = pspsys::now_us();
    struct in_addr ia;
    if (inet_aton(host.c_str(), &ia) != 0) { addr = (uint32_t)ia.s_addr; ms = 0; return true; }
    struct hostent* he = gethostbyname(host.c_str());
    ms = ms_since(t0);
    if (!he || !he->h_addr_list || !he->h_addr_list[0] || he->h_length != 4) {
        err = "dns failed";
        return false;
    }
    memcpy(&addr, he->h_addr_list[0], 4);   // already network order
    return true;
}

static bool set_nonblocking(int fd) {
#ifdef __PSP__
    int one = 1;
    return setsockopt(fd, SOL_SOCKET, SO_NONBLOCK, &one, sizeof one) >= 0;
#else
    int fl = fcntl(fd, F_GETFL, 0);
    return fl >= 0 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) >= 0;
#endif
}

static bool in_progress(int e) {
    // newlib's EINPROGRESS (119) and the BSD numbers sceNetInet uses (36 EINPROGRESS, 35 EAGAIN), plus EAGAIN either way
    return e == EINPROGRESS || e == EWOULDBLOCK || e == EAGAIN || e == 36 || e == 35 || e == 11 || e == 119;
}

// wait until fd is readable (write=false) or writable; false on timeout
static bool wait_fd(int fd, bool write, int ms) {
    if (ms < 0) ms = 0;
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    int r = select(fd + 1, write ? nullptr : &set, write ? &set : nullptr, nullptr, &tv);
    return r > 0;
}

long long parse_date(const std::string& s) {
    // "Tue, 03 Nov 2026 14:05:06 GMT"
    static const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    int d, y, H, M, S;
    char mon[4] = {0};
    const char* p = strchr(s.c_str(), ',');
    p = p ? p + 1 : s.c_str();
    if (sscanf(p, " %d %3s %d %d:%d:%d", &d, mon, &y, &H, &M, &S) != 6) return 0;
    const char* m = strstr(months, mon);
    if (!m || mon[0] == 0) return 0;
    int mo = (int)(m - months) / 3 + 1;
    return pspsys::utc_from_fields(y, mo, d, H, M, S);
}

static std::string header_value(const std::string& headers, const char* name) {
    size_t n = strlen(name);
    size_t pos = 0;
    while (pos < headers.size()) {
        size_t end = headers.find("\r\n", pos);
        if (end == std::string::npos) end = headers.size();
        if (end - pos > n && headers[pos + n] == ':' && strncasecmp(headers.c_str() + pos, name, n) == 0) {
            size_t v = pos + n + 1;
            while (v < end && (headers[v] == ' ' || headers[v] == '\t')) v++;
            return headers.substr(v, end - v);
        }
        pos = end + 2;
    }
    return "";
}

bool request(const char* method, const std::string& url, const std::string& body, Response& out, const char* content_type,
             int timeout_ms, size_t max_body) {
    out = Response();
    uint64_t t0 = pspsys::now_us();
    Url u;
    if (!parse_url(url, u)) { out.error = "bad url"; return false; }
    uint32_t addr = 0;
    if (!resolve(u.host, addr, out.dns_ms, out.error)) return false;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { out.error = "socket failed errno " + std::to_string(errno); return false; }
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    bool nb = set_nonblocking(fd);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)u.port);
    sa.sin_addr.s_addr = addr;
    uint64_t tc = pspsys::now_us();
    int r = connect(fd, (struct sockaddr*)&sa, sizeof sa);
    if (r < 0) {
        int e = errno;
        if (!nb || !in_progress(e)) { out.error = "connect failed errno " + std::to_string(e); close(fd); return false; }
        if (!wait_fd(fd, true, timeout_ms)) { out.error = "connect timeout"; close(fd); return false; }
        int soerr = 0;
        socklen_t len = sizeof soerr;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len) < 0) soerr = 0;   // PPSSPP may not answer SO_ERROR
        if (soerr) { out.error = "connect failed so_error " + std::to_string(soerr); close(fd); return false; }
    }
    out.connect_ms = ms_since(tc);

    std::string req = std::string(method) + " " + u.path + " HTTP/1.0\r\nHost: " + u.host;
    if (u.port != 80) req += ":" + std::to_string(u.port);
    // BRANDING: HTTP User-Agent
    req += "\r\nUser-Agent: NewBlitz-PSP/1\r\nConnection: close\r\n";
    if (strcmp(method, "GET") != 0 || !body.empty()) {
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        if (content_type) req += std::string("Content-Type: ") + content_type + "\r\n";
    }
    req += "\r\n";
    uint64_t deadline = pspsys::now_us() + (uint64_t)timeout_ms * 1000;
    auto left_ms = [&]() { uint64_t n = pspsys::now_us(); return n >= deadline ? 0 : (int)((deadline - n) / 1000); };
    const std::string* parts[2] = {&req, &body};
    for (const std::string* part : parts) {
        size_t sent = 0;
        while (sent < part->size()) {
            int n = (int)send(fd, part->data() + sent, part->size() - sent, 0);
            if (n > 0) { sent += (size_t)n; deadline = pspsys::now_us() + (uint64_t)timeout_ms * 1000; continue; }
            if (n < 0 && in_progress(errno) && left_ms() > 0) { wait_fd(fd, true, left_ms() < 500 ? left_ms() : 500); continue; }
            out.error = n == 0 ? "send closed" : (left_ms() ? "send failed errno " + std::to_string(errno) : "send timeout");
            close(fd);
            return false;
        }
    }

    std::string data;
    size_t header_end = std::string::npos;
    char buf[4096];
    for (;;) {
        int n = (int)recv(fd, buf, sizeof buf, 0);
        if (n > 0) {
            data.append(buf, (size_t)n);
            deadline = pspsys::now_us() + (uint64_t)timeout_ms * 1000;
            if (header_end == std::string::npos) {
                // look only where the end of the headers can be now (the new bytes and the 3 before them)
                size_t from = data.size() > (size_t)n + 3 ? data.size() - (size_t)n - 3 : 0;
                header_end = data.find("\r\n\r\n", from);
                if (header_end == std::string::npos && data.size() > MAX_HEADER) {
                    out.error = "header too large";
                    close(fd);
                    return false;
                }
                if (header_end != std::string::npos) {
                    std::string headers = data.substr(0, header_end + 2);
                    std::string te = header_value(headers, "Transfer-Encoding");
                    for (char& ch : te) ch = (char)tolower((unsigned char)ch);
                    if (te.find("chunked") != std::string::npos) {   // "chunked", or "gzip, chunked"
                        out.error = "chunked reply refused";
                        close(fd);
                        return false;
                    }
                    std::string cl = header_value(headers, "Content-Length");
                    if (!cl.empty()) out.content_length = atoll(cl.c_str());
                    if (out.content_length > (long long)max_body) { out.error = "reply too large"; close(fd); return false; }
                }
            }
            if (header_end != std::string::npos) {
                size_t have = data.size() - header_end - 4;
                if (out.content_length >= 0 && have >= (size_t)out.content_length) break;
                if (have > max_body) { out.error = "reply too large"; close(fd); return false; }
            }
            continue;
        }
        if (n == 0) break;   // closed: HTTP/1.0 end of body
        if (in_progress(errno) && left_ms() > 0) { wait_fd(fd, false, left_ms() < 500 ? left_ms() : 500); continue; }
        if (!left_ms()) { out.error = "recv timeout"; close(fd); return false; }
        out.error = "recv failed errno " + std::to_string(errno);
        close(fd);
        return false;
    }
    close(fd);
    out.ms = ms_since(t0);
    if (header_end == std::string::npos) { out.error = "no header"; return false; }
    std::string headers = data.substr(0, header_end + 2);
    int major = 0, minor = 0;
    if (sscanf(headers.c_str(), "HTTP/%d.%d %d", &major, &minor, &out.status) != 3) { out.error = "bad status line"; return false; }
    out.date = header_value(headers, "Date");
    out.date_utc = parse_date(out.date);
    out.body = data.substr(header_end + 4);
    if (out.content_length >= 0 && out.body.size() > (size_t)out.content_length) out.body.resize((size_t)out.content_length);
    if (out.content_length >= 0 && out.body.size() < (size_t)out.content_length) {
        // closed early: a cut-off manifest or pack must not pass for a whole one
        out.error = "short body: " + std::to_string(out.body.size()) + " of " + std::to_string(out.content_length) + " bytes";
        return false;
    }
    return true;
}

}  // namespace psphttp
