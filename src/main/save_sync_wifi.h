// Sync over Wi-Fi (docs/SAVE_SYNC.md §7.6): one device serves its saves bundle on the local network for ten minutes and
// shows a 12-character code; the other types the code and fetches the bundle, which goes in through phase 1's import
// (save_sync_core.h read_bundle, the same checks and questions as a file). No server of ours, nothing stored anywhere.
//
// The code carries where to look and the secret: the sender's address on the network (its last two numbers: the other
// device fills in the first two from its own, as on any home network), which of 16 ports, and 40 random bits. From the
// whole code, Argon2i (Monocypher, 4 MiB, 3 passes) makes the key the bundle is sealed with (save_sync_cloud.h seal) and
// the path it's served under; a wrong path gets nothing, and 20 wrong tries stop the sender. When the address the code
// names doesn't answer (another subnet, a 10.x network), the getter asks the network by UDP broadcast who has these saves
// (only the sender with that id answers), and failing that the player types the address the sender shows. Plain sockets
// (net_compat.h), so the same code on every platform and in tests/save_sync_cloud_test.cpp.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../game/save_sync_cloud.h"

namespace blitz::savesync::wifi {
    constexpr int first_port = 8780, port_count = 16;
    constexpr size_t code_chars = 12;
    constexpr int minutes_open = 10, max_wrong_tries = 20;

    struct Code {
        std::string text;                // 12 characters, normalized
        uint8_t ip3 = 0, ip4 = 0;        // the sender's address: x.y.ip3.ip4
        int port = 0;                    // first_port + 0..15
    };
    std::string make_code(uint8_t ip3, uint8_t ip4, int port);   // a fresh random secret
    bool parse_code(const std::string& typed, Code& out);        // false: not 12 good characters
    std::string group_code(const std::string& code);             // "ABCD-EFGH-JKMN"
    cloud::Keys derive(const std::string& code);                 // id (32 hex) and key; no token

    // ---- sending -----------------------------------------------------------------------------------------------------
    enum class SendState { Off, Serving, Fetched, TooManyTries, TimedOut, Failed };
    // Seals `zip` and serves it. local_ip: this device's address ("192.168.1.20"; "127.0.0.1" in tests). False with
    // why when there's no network or no free port.
    bool start_sending(const std::vector<uint8_t>& zip, const std::string& local_ip, std::string& code, std::string& why);
    void stop_sending();
    SendState send_state();
    int times_fetched();
    int seconds_left();

    // ---- getting -----------------------------------------------------------------------------------------------------
    struct Got {
        bool ok = false;
        std::vector<uint8_t> zip;        // the bundle, opened
        std::string why;                 // for the player
        bool no_answer = false;          // nothing answered at the address or to a broadcast: offer to type the address
    };
    // From the device a code names, on this device's network (my_ip: this device's address): the address the code names
    // first, then a broadcast asking who has these saves (UDP, the code's port). no_answer when neither found it.
    Got fetch(const std::string& typed, const std::string& my_ip);
    // From an address the player typed (the one the sending device shows under its code).
    Got fetch_at(const std::string& typed, const std::string& address);
    Got fetch_from(const std::string& host, const Code& code);
    // Who has the saves this code names: asks `target` (the broadcast address, or one device) on the code's port and
    // waits up to wait_ms for the answer. The answering device's address, or "".
    std::string discover(const Code& code, const std::string& target, int wait_ms);
    // This device's address while it sends ("" when not sending), shown under its code.
    std::string sending_address();
}
