// Plain HTTP/1.0 client for the PSP (docs/PSP_PORT.md §5.3). BSD sockets: libcglue's on the PSP (sceNetInet underneath), POSIX on
// the Mac, the same code for both. Non-blocking connect plus select with a timeout; Host, Content-Length and Connection: close
// are sent, never Accept-Encoding or Expect; a chunked reply is refused. The status line, Content-Length and Date are parsed;
// a reply cut off before its Content-Length, or with more than 16 KB of headers, fails.
// No TLS. Call it from the net or remote thread only: it blocks the caller for up to timeout_ms per phase.
#pragma once
#include <stdint.h>
#include <string>

namespace psphttp {

struct Url {
    std::string host;
    int port = 80;
    std::string path = "/";
};
bool parse_url(const std::string& url, Url& out);   // http://host[:port]/path only

struct Response {
    int status = 0;                 // 0: no reply (see error)
    std::string body;
    std::string date;               // the Date header as sent
    long long date_utc = 0;         // Date as seconds since 1970, 0 if absent or unparsable
    long long content_length = -1;
    int dns_ms = 0, connect_ms = 0, ms = 0;
    std::string error;              // "" when a reply arrived
};

bool request(const char* method, const std::string& url, const std::string& body, Response& out,
             const char* content_type = "application/octet-stream", int timeout_ms = 10000,
             size_t max_body = 2 * 1024 * 1024);
inline bool get(const std::string& url, Response& out, int timeout_ms = 10000) {
    return request("GET", url, std::string(), out, nullptr, timeout_ms);
}
inline bool post(const std::string& url, const std::string& body, Response& out, int timeout_ms = 10000) {
    return request("POST", url, body, out, "application/octet-stream", timeout_ms);
}

// host name or dotted quad -> IPv4 in network order (h_addr used as-is, no swap). ms: how long the lookup took.
bool resolve(const std::string& host, uint32_t& addr, int& ms, std::string& err);

long long parse_date(const std::string& rfc1123);   // "Tue, 03 Nov 2026 14:05:06 GMT" -> UTC seconds, 0 if bad

}  // namespace psphttp
