// Sync over Wi-Fi: the code, the sender's small server and the fetch (save_sync_wifi.h).
#include "save_sync_wifi.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>

#include "net_compat.h"
#include "monocypher.h"
#include "../common/sha256.h"

namespace blitz::savesync::wifi {
    namespace {
        constexpr const char* alphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
        constexpr size_t max_body = 2u << 20;

        std::mutex mutex;                 // guards the fields below
        std::vector<uint8_t> served;      // the sealed bundle
        std::string served_path;          // "/nbsave/<id>"
        std::atomic<int> state{(int)SendState::Off};
        std::atomic<int> fetched{0}, wrong{0};
        std::atomic<int> listen_fd{-1};
        std::atomic<int> udp_fd{-1};      // the discovery answer, on the same port number (UDP)
        std::string served_id;            // the id discovery asks about (under mutex)
        std::string served_address;       // this device's address as shown on its screen
        std::atomic<bool> running{false};
        std::chrono::steady_clock::time_point until;
        std::thread server;

        uint64_t code_bits(const std::string& code) {
            uint64_t n = 0;
            for (char c : code) n = (n << 5) | (uint64_t)(std::strchr(alphabet, c) - alphabet);
            return n;
        }

        bool send_all(int fd, const char* p, size_t n) {
            while (n > 0) {
                ssize_t k = blitz::net::send_some(fd, p, n);
                if (k <= 0) return false;
                p += k;
                n -= (size_t)k;
            }
            return true;
        }

        void answer(int fd, const char* status, const std::vector<uint8_t>& body) {
            std::string head = std::string("HTTP/1.0 ") + status + "\r\nContent-Type: application/octet-stream\r\nCache-Control: no-store\r\n"
                               "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
            if (send_all(fd, head.data(), head.size()) && !body.empty()) send_all(fd, (const char*)body.data(), body.size());
        }

        // One request. Every byte comes from the network: nothing here may throw or wait for ever.
        void handle(int fd) {
            blitz::net::harden(fd);
            blitz::net::set_timeouts(fd, 5000);
            std::string req;
            char buf[1024];
            while (req.find("\r\n\r\n") == std::string::npos && req.size() < 8192) {
                ssize_t n = blitz::net::recv_some(fd, buf, sizeof buf);
                if (n <= 0) break;
                req.append(buf, (size_t)n);
            }
            std::string line = req.substr(0, req.find("\r\n"));
            std::string want;
            std::vector<uint8_t> body;
            {
                std::lock_guard lock{mutex};
                want = "GET " + served_path + " ";
                body = served;
            }
            if (line.rfind(want, 0) == 0) {
                answer(fd, "200 OK", body);
                fetched++;
                state = (int)SendState::Fetched;
                fprintf(stderr, "[wifi sync] the saves were fetched\n");
            }
            else {
                answer(fd, "404 Not Found", {});
                if (line.rfind("GET /nbsave/", 0) == 0 && ++wrong >= max_wrong_tries) {
                    fprintf(stderr, "[wifi sync] %d wrong codes: stopped\n", wrong.load());
                    state = (int)SendState::TooManyTries;
                    running = false;
                }
            }
            blitz::net::close_socket(fd);
        }

        int udp_socket() {
            if (!blitz::net::start()) return -1;
#ifdef _WIN32
            SOCKET u = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            return u == INVALID_SOCKET ? -1 : (int)u;
#else
            return ::socket(AF_INET, SOCK_DGRAM, 0);
#endif
        }

        int recv_from(int fd, char* buf, size_t size, sockaddr_in& from) {
#ifdef _WIN32
            int len = sizeof(from);
            return ::recvfrom((SOCKET)fd, buf, (int)size, 0, (sockaddr*)&from, &len);
#else
            socklen_t len = sizeof(from);
            return (int)::recvfrom(fd, buf, size, 0, (sockaddr*)&from, &len);
#endif
        }

        void send_to(int fd, const std::string& msg, const sockaddr_in& to) {
#ifdef _WIN32
            ::sendto((SOCKET)fd, msg.data(), (int)msg.size(), 0, (const sockaddr*)&to, sizeof(to));
#else
            ::sendto(fd, msg.data(), msg.size(), 0, (const sockaddr*)&to, sizeof(to));
#endif
        }

        // A discovery question: "NBSYNC? <id>". Answered only for this sender's own id (which only the code makes), and a
        // wrong id counts as a wrong try, as on the TCP side.
        void answer_probe(int ufd) {
            char buf[256];
            sockaddr_in from{};
            int n = recv_from(ufd, buf, sizeof(buf) - 1, from);
            if (n <= 0) return;
            std::string msg(buf, (size_t)n), id;
            {
                std::lock_guard lock{mutex};
                id = served_id;
            }
            if (msg == "NBSYNC? " + id) {
                send_to(ufd, "NBSYNC! " + id, from);
                fprintf(stderr, "[wifi sync] answered a device looking for these saves\n");
            }
            else if (msg.rfind("NBSYNC? ", 0) == 0 && ++wrong >= max_wrong_tries) {
                state = (int)SendState::TooManyTries;
                running = false;
            }
        }

        void serve_loop() {
            while (running) {
                if (std::chrono::steady_clock::now() > until) {
                    state = (int)SendState::TimedOut;
                    running = false;
                    fprintf(stderr, "[wifi sync] ten minutes up: stopped\n");
                    break;
                }
                int lfd = listen_fd;
                if (lfd < 0) break;
                int ufd = udp_fd;
                fd_set r;
                FD_ZERO(&r);
                FD_SET(lfd, &r);
                if (ufd >= 0) FD_SET(ufd, &r);
                timeval t{0, 250000};
#ifdef _WIN32
                int n = ::select(0, &r, nullptr, nullptr, &t);
#else
                int n = ::select(std::max(lfd, ufd) + 1, &r, nullptr, nullptr, &t);
#endif
                if (n <= 0) continue;
                if (ufd >= 0 && FD_ISSET(ufd, &r)) answer_probe(ufd);
                if (FD_ISSET(lfd, &r)) {
                    int fd = blitz::net::accept_one(lfd);
                    if (fd >= 0) handle(fd);
                }
            }
            int lfd = listen_fd.exchange(-1);
            if (lfd >= 0) blitz::net::close_socket(lfd);
            int ufd = udp_fd.exchange(-1);
            if (ufd >= 0) blitz::net::close_socket(ufd);
        }

        bool parse_ip(const std::string& ip, int out[4]) {
            char extra;
            return std::sscanf(ip.c_str(), "%d.%d.%d.%d%c", &out[0], &out[1], &out[2], &out[3], &extra) == 4 &&
                   std::all_of(out, out + 4, [](int v) { return v >= 0 && v <= 255; });
        }
    }

    std::string make_code(uint8_t ip3, uint8_t ip4, int port) {
        std::random_device rd;
        uint64_t secret = ((uint64_t)rd() << 32 | rd()) & ((1ull << 40) - 1);
        uint64_t n = (uint64_t)ip3 << 52 | (uint64_t)ip4 << 44 | (uint64_t)((port - first_port) & 15) << 40 | secret;
        std::string out(code_chars, '0');
        for (size_t i = 0; i < code_chars; i++) out[code_chars - 1 - i] = alphabet[(n >> (5 * i)) & 31];
        return out;
    }

    bool parse_code(const std::string& typed, Code& out) {
        std::string c;
        for (char ch : typed) {
            if (ch == ' ' || ch == '-') continue;
            if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
            if (ch == 'O') ch = '0';
            if (ch == 'I' || ch == 'L') ch = '1';
            if (ch == 0 || std::strchr(alphabet, ch) == nullptr) return false;
            c += ch;
        }
        if (c.size() != code_chars) return false;
        uint64_t n = code_bits(c);
        out.text = c;
        out.ip3 = (uint8_t)(n >> 52);
        out.ip4 = (uint8_t)(n >> 44);
        out.port = first_port + (int)((n >> 40) & 15);
        return true;
    }

    std::string group_code(const std::string& code) {
        std::string out;
        for (size_t i = 0; i < code.size(); i++) {
            if (i > 0 && i % 4 == 0) out += '-';
            out += code[i];
        }
        return out;
    }

    cloud::Keys derive(const std::string& code) {
        // Argon2i: 40 secret bits are few, so each guess at a sealed bundle someone caught on the network costs 4 MiB
        // and three passes (tens of milliseconds) instead of a hash.
        cloud::Keys k;
        constexpr uint32_t blocks = 4096;
        std::vector<uint8_t> work((size_t)blocks * 1024);
        static const char salt[] = "new blitz wifi sync v1";
        crypto_argon2_config config{CRYPTO_ARGON2_I, blocks, 3, 1};
        crypto_argon2_inputs inputs{(const uint8_t*)code.data(), (const uint8_t*)salt, (uint32_t)code.size(), (uint32_t)sizeof(salt) - 1};
        uint8_t out[48];
        crypto_argon2(out, sizeof out, work.data(), config, inputs, crypto_argon2_no_extras);
        std::memcpy(k.key.data(), out, 32);
        k.id = blitzsha::hex(out + 32, 16);
        crypto_wipe(out, sizeof out);
        return k;
    }

    bool start_sending(const std::vector<uint8_t>& zip, const std::string& local_ip, std::string& code, std::string& why) {
        stop_sending();
        int ip[4];
        if (!parse_ip(local_ip, ip)) {
            why = "This device isn't on a network. Join the same Wi-Fi as the other device.";
            return false;
        }
        int fd = blitz::net::open_tcp();
        if (fd < 0) {
            why = "Couldn't open a connection.";
            return false;
        }
        blitz::net::harden(fd);
        blitz::net::reuse_port(fd);
        int port = 0;
        for (int p = first_port; p < first_port + port_count; p++) {
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
            addr.sin_port = htons((uint16_t)p);
            if (::bind(fd, (sockaddr*)&addr, sizeof(addr)) == 0) {
                port = p;
                break;
            }
        }
        if (port == 0 || ::listen(fd, 4) != 0) {
            blitz::net::close_socket(fd);
            why = "Couldn't open a port for the other device.";
            return false;
        }
        code = make_code((uint8_t)ip[2], (uint8_t)ip[3], port);
        cloud::Keys k = derive(code);
        // Discovery: the same port number over UDP (a device that can't reach the address the code names asks the whole
        // network). Without it the code and the typed address still work.
        int ufd = udp_socket();
        if (ufd >= 0) {
            blitz::net::harden(ufd);
            sockaddr_in uaddr{};
            uaddr.sin_family = AF_INET;
            uaddr.sin_addr.s_addr = htonl(INADDR_ANY);
            uaddr.sin_port = htons((uint16_t)port);
            if (::bind(ufd, (sockaddr*)&uaddr, sizeof(uaddr)) != 0) {
                blitz::net::close_socket(ufd);
                ufd = -1;
            }
        }
        udp_fd = ufd;
        {
            std::lock_guard lock{mutex};
            served_id = k.id;
            served_address = local_ip;
            served = cloud::seal(k, 0, zip);
            served_path = "/nbsave/" + k.id;
            until = std::chrono::steady_clock::now() + std::chrono::minutes(minutes_open);
        }
        fetched = 0;
        wrong = 0;
        listen_fd = fd;
        state = (int)SendState::Serving;
        running = true;
        server = std::thread(serve_loop);
        fprintf(stderr, "[wifi sync] serving %zu bytes on port %d for %d minutes\n", zip.size(), port, minutes_open);
        return true;
    }

    void stop_sending() {
        running = false;
        if (server.joinable()) server.join();
        int lfd = listen_fd.exchange(-1);
        if (lfd >= 0) blitz::net::close_socket(lfd);
        int ufd = udp_fd.exchange(-1);
        if (ufd >= 0) blitz::net::close_socket(ufd);
        std::lock_guard lock{mutex};
        served_id.clear();
        if (!served.empty()) crypto_wipe(served.data(), served.size());
        served.clear();
        served_path.clear();
        int s = state;
        if (s == (int)SendState::Serving || s == (int)SendState::Fetched) state = (int)SendState::Off;
    }

    SendState send_state() { return (SendState)state.load(); }
    int times_fetched() { return fetched; }
    int seconds_left() {
        if (!running) return 0;
        std::lock_guard lock{mutex};
        auto left = std::chrono::duration_cast<std::chrono::seconds>(until - std::chrono::steady_clock::now()).count();
        return left > 0 ? (int)left : 0;
    }

    Got fetch_from(const std::string& host, const Code& code) {
        Got g;
        cloud::Keys k = derive(code.text);
        int fd = blitz::net::open_tcp();
        if (fd < 0) {
            g.why = "Couldn't open a connection.";
            return g;
        }
        blitz::net::harden(fd);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t)code.port);
        if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1 || blitz::net::connect_within(fd, addr, 4000) != 0) {
            blitz::net::close_socket(fd);
            g.why = "The other device didn't answer. Check it still shows the code, and that both are on the same Wi-Fi.";
            fprintf(stderr, "[wifi sync] no answer from %s:%d\n", host.c_str(), code.port);
            return g;
        }
        blitz::net::set_timeouts(fd, 10000);
        std::string req = "GET /nbsave/" + k.id + " HTTP/1.0\r\nHost: " + host + "\r\n\r\n";
        send_all(fd, req.data(), req.size());
        std::string got;
        char buf[16384];
        for (ssize_t n; (n = blitz::net::recv_some(fd, buf, sizeof buf)) > 0 && got.size() < max_body;) got.append(buf, (size_t)n);
        blitz::net::close_socket(fd);
        size_t end = got.find("\r\n\r\n");
        if (got.rfind("HTTP/1.0 200", 0) != 0 || end == std::string::npos) {
            g.why = got.rfind("HTTP/1.0 404", 0) == 0 ? "That code doesn't match. Check it on the other device." : "The other device's answer was cut off. Try again.";
            return g;
        }
        cloud::Bytes blob(got.begin() + (long)end + 4, got.end());
        cloud::Opened o = cloud::open(k, blob);
        if (!o.ok) {
            g.why = "The saves didn't arrive whole. Try again.";
            return g;
        }
        g.ok = true;
        g.zip = std::move(o.plain);
        fprintf(stderr, "[wifi sync] got %zu bytes from %s:%d\n", g.zip.size(), host.c_str(), code.port);
        return g;
    }

    std::string sending_address() {
        std::lock_guard lock{mutex};
        return running ? served_address : std::string();
    }

    std::string discover(const Code& code, const std::string& target, int wait_ms) {
        cloud::Keys k = derive(code.text);
        int ufd = udp_socket();
        if (ufd < 0) return {};
        blitz::net::harden(ufd);
        blitz::net::set_int_option(ufd, SOL_SOCKET, SO_BROADCAST, 1);
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons((uint16_t)code.port);
        if (inet_pton(AF_INET, target.c_str(), &to.sin_addr) != 1) {
            blitz::net::close_socket(ufd);
            return {};
        }
        const std::string ask = "NBSYNC? " + k.id, want = "NBSYNC! " + k.id;
        std::string found;
        auto until_t = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
        for (int tries = 0; tries < 3 && found.empty(); tries++) {
            send_to(ufd, ask, to);
            while (found.empty()) {
                auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until_t - std::chrono::steady_clock::now()).count();
                if (left <= 0) break;
                long slice = std::min<long>((long)left, (long)(wait_ms / 3 + 1));
                fd_set r;
                FD_ZERO(&r);
                FD_SET(ufd, &r);
                timeval t{(int)(slice / 1000), (int)((slice % 1000) * 1000)};
#ifdef _WIN32
                int n = ::select(0, &r, nullptr, nullptr, &t);
#else
                int n = ::select(ufd + 1, &r, nullptr, nullptr, &t);
#endif
                if (n <= 0) break;  // this try's slice is over: ask again
                char buf[256];
                sockaddr_in from{};
                int got = recv_from(ufd, buf, sizeof buf, from);
                if (got > 0 && std::string(buf, (size_t)got) == want) {
                    char ip[INET_ADDRSTRLEN] = {};
                    inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
                    found = ip;
                }
            }
        }
        blitz::net::close_socket(ufd);
        if (!found.empty()) fprintf(stderr, "[wifi sync] found the other device at %s\n", found.c_str());
        return found;
    }

    Got fetch_at(const std::string& typed, const std::string& address) {
        Got g;
        Code c;
        int ip[4];
        if (!parse_code(typed, c)) {
            g.why = "That isn't the code: it has 12 letters and numbers.";
            return g;
        }
        if (!parse_ip(address, ip)) {
            g.why = "That isn't an address: it looks like 192.168.1.20, under the code on the other device.";
            return g;
        }
        return fetch_from(address, c);
    }

    Got fetch(const std::string& typed, const std::string& my_ip) {
        Got g;
        Code c;
        if (!parse_code(typed, c)) {
            g.why = "That isn't the code: it has 12 letters and numbers.";
            return g;
        }
        int ip[4];
        if (!parse_ip(my_ip, ip)) {
            g.why = "This device isn't on a network. Join the same Wi-Fi as the other device.";
            return g;
        }
        // First the address the code names (this network's first two numbers, the code's last two); then, if nothing
        // answers there, ask the whole network who has these saves (the devices may be on different subnets of one home
        // network, or a 10.x network). The player can still type the address the other device shows.
        const std::string named = std::to_string(ip[0]) + "." + std::to_string(ip[1]) + "." + std::to_string(c.ip3) + "." + std::to_string(c.ip4);
        Got g1 = fetch_from(named, c);
        if (g1.ok || g1.why.find("didn't answer") == std::string::npos) return g1;
        std::string found = discover(c, "255.255.255.255", 1500);
        if (!found.empty() && found != named) return fetch_from(found, c);
        g1.no_answer = true;
        return g1;
    }
}
