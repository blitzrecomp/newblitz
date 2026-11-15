// SHA-256 and HMAC-SHA256, shared by every build (2026-11-13: moved here from src/psp, where it signs the PSP's Worker
// commands, docs/PSP_PORT.md §5.3; the season pak's download and file checks use it too, src/game/season_pak.cpp).
// Written for this repo from FIPS 180-4 and RFC 2104; no third-party code. Small and plain, not fast: a 2 MB pak hashes
// in a few milliseconds on a computer or a phone.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string>

namespace blitzsha {

struct Sha256 {
    uint32_t h[8];
    uint8_t buf[64];
    uint64_t len;
    size_t fill;
    Sha256();
    void update(const void* data, size_t n);
    void final(uint8_t out[32]);
};

void sha256(const void* data, size_t n, uint8_t out[32]);
void hmac_sha256(const void* key, size_t key_len, const void* msg, size_t msg_len, uint8_t out[32]);
std::string hex(const uint8_t* bytes, size_t n);
// The SHA-256 of a buffer as 64 lower-case hex digits (the season pak's catalog and pak.json write it so).
std::string sha256_hex(const void* data, size_t n);
// SHA-1 (FIPS 180-4), for the rosters manifest's file hashes (docs/PSP_PORT.md §6): hex digest, lower case.
std::string sha1_hex(const void* data, size_t n);
// The command tag: the first 16 hex digits of HMAC-SHA256(key, "<seq>|<verb args>"). The key is the cmd_key string as written
// in net.cfg / build/psp_cmd_key (its ASCII hex characters, not decoded).
std::string command_tag(const std::string& key, const std::string& seq, const std::string& line);

}  // namespace blitzsha
