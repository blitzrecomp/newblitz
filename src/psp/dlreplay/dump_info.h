// The sidecar of an RDRAM dump (build/dumps/rdram_<n>.txt): "t=54.31 dlist=800536E8 size=12552 ucode=80027C10".
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <string>

namespace dlreplay {

struct DumpInfo { uint32_t dlist = 0, ucode = 0x80027C10u, size = 0; double t = 0; };

inline uint32_t hex_after(const std::string& s, const char* key) {
    size_t at = s.find(key);
    return at == std::string::npos ? 0 : (uint32_t)strtoul(s.c_str() + at + strlen(key), nullptr, 16);
}

inline bool parse_info(const std::string& text, DumpInfo& out) {
    out.dlist = hex_after(text, "dlist=");
    uint32_t u = hex_after(text, "ucode=");
    if (u) out.ucode = u;
    size_t at = text.find("size=");
    if (at != std::string::npos) out.size = (uint32_t)strtoul(text.c_str() + at + 5, nullptr, 10);
    at = text.find("t=");
    if (at != std::string::npos) out.t = atof(text.c_str() + at + 2);
    return out.dlist != 0;
}

// "<dir>/rdram_543.bin" -> "rdram_543"
inline std::string stem(const std::string& path) {
    size_t s = path.find_last_of("/:");
    std::string n = s == std::string::npos ? path : path.substr(s + 1);
    size_t d = n.rfind('.');
    return d == std::string::npos ? n : n.substr(0, d);
}

}  // namespace dlreplay
