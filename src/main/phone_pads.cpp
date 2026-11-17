// Phones as controllers (Settings → Players → Phone Controllers; off by default).
//
// While on, the app serves a touch controller page on the local network (port 8770). A phone that
// opens it (the Players tab shows the address and a QR code) connects back with a WebSocket and
// becomes a virtual controller: an SDL virtual joystick, so it joins like a plugged-in pad (the next
// free player, Players tab, Identify, Swap, Make Player 1). Up to four phones. Messages from the page:
//   "s <buttons> <x> <y>"   N64 button bits (A 0x8000, B 0x4000, Z 0x2000, Start 0x1000) and the stick (-1..1)
//   "c <command>"           its ··· menu: save, load, redo, slot+, slot-, p1 (Be player 1)
// and to it: "v <page version>" (an older page reloads), "p <player>" (1-4, 0 = waiting), "r 1|0"
// (rumble, for phones that can vibrate), "m menu|select|play|offense|defense" (button labels) and
// "t <status>" (a toast). Anyone on the same network can join while it's on, which is why it's a switch.
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <cctype>
#include "net_compat.h"

#define SDL_MAIN_HANDLED
#include "SDL2/SDL.h"

#include "phone_page.h"
#include "game_moment.h"
#if BLITZ_MODERN
#include "wide_logo_bytes.h"
#endif

namespace blitz {
    int port_of_joystick(SDL_JoystickID id);  // controllers.cpp: 0-3, or -1
    bool port_rumbling(int port);
    void swap_ports(int a, int b);
    unsigned latest_status(std::string& out);  // notices.cpp
}
extern "C" void blitz_savestate_request(int op);   // 1 save, 2 load (current slot)
extern "C" void blitz_savestate_slot_step(int delta);
extern "C" void blitz_redo_request();

namespace {
    constexpr int max_phones = 4;
    constexpr int first_port = 8770;

    // A phone keeps its slot (and so its virtual controller and player) across reconnects: the page
    // sends a random id it keeps (ws?id=...). A new connection with a known id replaces the old one,
    // which may still look open (a locked phone's socket lingers), and a phone that leaves keeps its
    // controller for a while, so locking the screen doesn't give its player away.
    constexpr uint32_t linger_ms = 10000;

    struct Phone {
        std::atomic<bool> connected{false};
        std::atomic<uint32_t> state{0};       // buttons << 16 | (x + 128) << 8 | (y + 128)
        std::atomic<int> player{0};           // shown on the phone
        std::atomic<int> generation{0};       // the connection being served
        std::atomic<uint32_t> left_at{0};     // SDL ticks when it went away (0 = here)
        std::atomic<int> command{0};          // from the page's menu, run on the main thread
        std::mutex token_mutex;
        std::string token;
        SDL_Joystick* joystick = nullptr;     // main thread only
    };
    enum Command { cmd_none, cmd_save, cmd_load, cmd_redo, cmd_slot_next, cmd_slot_prev, cmd_player1 };
    std::array<Phone, max_phones> phones;
    std::atomic<bool> running{false};
    std::atomic<int> listen_fd{-1};
    std::atomic<int> http_port{0};
    // This run's id, made at start-up and put in the QR address (?g=...). A page keeps it and sends it back on every
    // connect, so a tab left open from another New Blitz (same port on another machine) is told so, not attached.
    std::string session_id;

    // ---- SHA-1 and base64, for the WebSocket handshake ----
    std::string sha1(const std::string& s) {
        uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
        std::string m = s;
        uint64_t bits = (uint64_t)s.size() * 8;
        m += (char)0x80;
        while (m.size() % 64 != 56) m += (char)0;
        for (int i = 7; i >= 0; i--) m += (char)(bits >> (i * 8));
        auto rol = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
        for (size_t off = 0; off < m.size(); off += 64) {
            uint32_t w[80];
            for (int i = 0; i < 16; i++) {
                w[i] = (uint8_t)m[off + 4 * i] << 24 | (uint8_t)m[off + 4 * i + 1] << 16 | (uint8_t)m[off + 4 * i + 2] << 8 | (uint8_t)m[off + 4 * i + 3];
            }
            for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
            uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
            for (int i = 0; i < 80; i++) {
                uint32_t f, k;
                if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
                else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
                else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
                else { f = b ^ c ^ d; k = 0xCA62C1D6; }
                uint32_t t = rol(a, 5) + f + e + k + w[i];
                e = d; d = c; c = rol(b, 30); b = a; a = t;
            }
            h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
        }
        std::string out;
        for (uint32_t v : h) for (int i = 3; i >= 0; i--) out += (char)(v >> (i * 8));
        return out;
    }

    std::string base64(const std::string& s) {
        static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        for (size_t i = 0; i < s.size(); i += 3) {
            uint32_t n = (uint8_t)s[i] << 16 | (i + 1 < s.size() ? (uint8_t)s[i + 1] << 8 : 0) | (i + 2 < s.size() ? (uint8_t)s[i + 2] : 0);
            out += t[n >> 18 & 63];
            out += t[n >> 12 & 63];
            out += i + 1 < s.size() ? t[n >> 6 & 63] : '=';
            out += i + 2 < s.size() ? t[n & 63] : '=';
        }
        return out;
    }

    bool send_all(int fd, const std::string& s) {
        size_t sent = 0;
        while (sent < s.size()) {
            ssize_t n = blitz::net::send_some(fd, s.data() + sent, s.size() - sent);
            if (n <= 0) return false;
            sent += (size_t)n;
        }
        return true;
    }

    // Short messages only (under 126 bytes; longer ones are cut at a character boundary, since a
    // text frame that isn't valid UTF-8 makes the browser drop the connection).
    bool send_text(int fd, std::string msg) {
        if (msg.size() > 125) {
            size_t n = 125;
            while (n > 0 && ((uint8_t)msg[n] & 0xC0) == 0x80) n--;
            msg.resize(n);
        }
        std::string f;
        f += (char)0x81;
        f += (char)msg.size();
        return send_all(fd, f + msg);
    }

    // Reads one client frame (masked); false on close or error.
    bool read_frame(int fd, std::string& out, int& opcode) {
        uint8_t hdr[2];
        if (blitz::net::recv_some(fd, (char*)hdr, 2, MSG_WAITALL) != 2) return false;
        opcode = hdr[0] & 0x0F;
        uint64_t len = hdr[1] & 0x7F;
        if (len == 126) {
            uint8_t e[2];
            if (blitz::net::recv_some(fd, (char*)e, 2, MSG_WAITALL) != 2) return false;
            len = (uint64_t)e[0] << 8 | e[1];
        }
        else if (len == 127) {
            return false;  // never this big
        }
        uint8_t mask[4] = {0, 0, 0, 0};
        if ((hdr[1] & 0x80) && blitz::net::recv_some(fd, (char*)mask, 4, MSG_WAITALL) != 4) return false;
        if (len > 4096) return false;
        out.resize(len);
        if (len && blitz::net::recv_some(fd, out.data(), len, MSG_WAITALL) != (ssize_t)len) return false;
        for (size_t i = 0; i < len; i++) out[i] ^= mask[i % 4];
        return true;
    }

    void serve_controller(int fd, Phone& phone, int gen) {
        phone.state = (0u << 16) | (128u << 8) | 128u;
        {
            std::lock_guard t{phone.token_mutex};
            phone.connected = true;
            phone.left_at = 0;
        }
        fprintf(stderr, "[phones] a phone connected\n");
        int sent_player = -1, sent_rumble = -1, sent_mode = -1;
        send_text(fd, "v " + std::to_string(phone_page_version));  // an older page reloads itself
        send_text(fd, "g " + session_id);  // a page opened without ?g= keeps this for its reconnects
        std::string status_text;
        unsigned sent_status = blitz::latest_status(status_text);
        blitz::net::set_recv_timeout(fd, 100);  // wake every 100 ms to report the player and rumble
        while (running && phone.generation == gen) {
            std::string msg;
            int op = 0;
            uint8_t peek;
            ssize_t ready = blitz::net::recv_some(fd, (char*)&peek, 1, MSG_PEEK);
            if (ready == 0) break;
            if (ready > 0) {
                if (!read_frame(fd, msg, op) || op == 8) break;
                if (op == 9) {  // ping: pong
                    send_all(fd, std::string("\x8A\x00", 2));
                }
                else if (op == 1 && msg.size() > 2 && msg[0] == 'c') {
                    static const std::pair<const char*, Command> names[] = {{"save", cmd_save}, {"load", cmd_load},
                        {"redo", cmd_redo}, {"slot+", cmd_slot_next}, {"slot-", cmd_slot_prev}, {"p1", cmd_player1}};
                    for (auto& [n, c] : names) if (msg.compare(2, std::string::npos, n) == 0) phone.command = c;
                }
                else if (op == 1 && msg.size() > 2 && msg[0] == 's') {
                    unsigned b = 0;
                    float x = 0, y = 0;
                    if (sscanf(msg.c_str() + 2, "%u %f %f", &b, &x, &y) == 3) {
                        int ix = std::clamp((int)(x * 127.0f), -127, 127), iy = std::clamp((int)(y * 127.0f), -127, 127);
                        phone.state = (b & 0xFFFF) << 16 | (uint32_t)(ix + 128) << 8 | (uint32_t)(iy + 128);
                    }
                }
            }
            else if (!blitz::net::timed_out()) {
                break;
            }
            int p = phone.player;
            if (p != sent_player) {
                if (!send_text(fd, "p " + std::to_string(p))) break;
                sent_player = p;
            }
            int r = p > 0 && blitz::port_rumbling(p - 1) ? 1 : 0;
            if (r != sent_rumble) {
                if (!send_text(fd, std::string("r ") + (r ? "1" : "0"))) break;
                sent_rumble = r;
            }
            // Menus, play select, or a play; in a play, offense or defense by this phone's team.
            int mode = (int)blitz::moment_for(p - 1);  // no player yet: a play is just "play"
            if (mode != sent_mode) {
                if (!send_text(fd, std::string("m ") + blitz::moment_text((blitz::GameMoment)mode).key)) break;
                sent_mode = mode;
            }
            unsigned serial = blitz::latest_status(status_text);
            if (serial != sent_status) {
                sent_status = serial;
                if (!send_text(fd, "t " + status_text)) break;
            }
        }
        // Not if a newer connection from the same phone replaced this one (checked and cleared together, so a
        // reconnect can't be marked gone by the old socket).
        std::lock_guard t{phone.token_mutex};
        if (phone.generation == gen) {
            phone.connected = false;
            phone.left_at = std::max<uint32_t>(1, SDL_GetTicks());
            fprintf(stderr, "[phones] a phone left\n");
        }
    }

    std::atomic<int> handlers{0};  // open connections; more than a few phones' worth are turned away

    // Every byte here comes from the network: nothing may throw, block forever or kill the app.
    void handle(int fd) {
        blitz::net::set_int_option(fd, IPPROTO_TCP, TCP_NODELAY, 1);
        blitz::net::harden(fd);           // a phone that drops can't kill the app or follow a restart
        blitz::net::set_timeouts(fd, 5000);  // a request that stalls is dropped
        std::string req;
        char buf[2048];
        while (req.find("\r\n\r\n") == std::string::npos && req.size() < 16384) {
            ssize_t n = blitz::net::recv_some(fd, buf, sizeof(buf));
            if (n <= 0) { blitz::net::close_socket(fd); return; }
            req.append(buf, (size_t)n);
        }
        // Headers only: up to the blank line, never into a body.
        std::string head = req.substr(0, std::min(req.find("\r\n\r\n"), req.size())) + "\r\n";
        auto header = [&](const char* name) {
            std::string lower = head;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            std::string key = std::string("\r\n") + name + ":";
            size_t at = lower.find(key);
            if (at == std::string::npos) return std::string();
            size_t start = head.find_first_not_of(' ', at + key.size());
            size_t end = start == std::string::npos ? std::string::npos : head.find("\r\n", start);
            if (start == std::string::npos || end == std::string::npos) return std::string();
            return head.substr(start, end - start);
        };
        if (req.rfind("GET /ws", 0) == 0) {
            std::string key = header("sec-websocket-key");
            // Only this server's own page may connect: a browser sends its page's origin, and a web page from
            // anywhere else on the network can't drive the game.
            std::string origin = header("origin"), host = header("host");
            if (key.empty() || (!origin.empty() && origin != "http://" + host)) {
                send_all(fd, "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n");
                blitz::net::close_socket(fd);
                return;
            }
            // "?id=<token>&g=<game>": the request line's query, letters and digits only.
            std::string line = req.substr(0, std::min(req.find("\r\n"), req.size()));
            auto param = [&](const char* name) {
                std::string out, key = std::string(name) + "=";
                size_t q = line.find(std::string("?") + key);
                if (q == std::string::npos) q = line.find(std::string("&") + key);
                if (q == std::string::npos) return out;
                for (size_t i = q + 1 + key.size(); i < line.size() && std::isalnum((unsigned char)line[i]) && out.size() < 40; i++) out += line[i];
                return out;
            };
            std::string token = param("id"), game = param("g");
            if (!game.empty() && game != session_id) {
                // A page from another New Blitz: finish the handshake so it can be told, and take no slot.
                std::string accept = base64(sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
                send_all(fd, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n");
                send_text(fd, "x other");
                send_all(fd, std::string("\x88\x00", 2));
                blitz::net::close_socket(fd);
                return;
            }
            Phone* slot = nullptr;
            int gen = 0;
            {
                // Held only while a slot is chosen. It used to be held for the whole connection (serve_controller below), so
                // with one phone playing every other phone, and a phone coming back, waited at its handshake (PS5, 2026-11-16).
                static std::mutex choose_mutex;
                std::lock_guard choose{choose_mutex};
                if (!token.empty()) {  // this phone's own slot, even if its old connection looks open
                    for (Phone& p : phones) {
                        std::lock_guard t{p.token_mutex};
                        if (p.token == token) { slot = &p; break; }
                    }
                }
                if (slot == nullptr) {  // a free slot, preferring one no phone has used; never one a locked phone is coming back to
                    for (int pass = 0; pass < 2 && slot == nullptr; pass++) {
                        for (Phone& p : phones) {
                            std::lock_guard t{p.token_mutex};
                            bool lingering = p.left_at != 0 && SDL_GetTicks() - p.left_at < linger_ms;
                            if (!p.connected && !lingering && (pass == 1 || p.token.empty())) { slot = &p; break; }
                        }
                    }
                }
                if (slot != nullptr) {
                    std::lock_guard t{slot->token_mutex};
                    slot->token = token;
                    slot->connected = true;
                    gen = ++slot->generation;  // an older connection to this slot stops serving it
                }
            }
            if (slot == nullptr) {
                send_all(fd, "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n");
                blitz::net::close_socket(fd);
                return;
            }
            std::string accept = base64(sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
            send_all(fd, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n");
            serve_controller(fd, *slot, gen);
        }
#if BLITZ_MODERN
        else if (req.rfind("GET /logo.png", 0) == 0) {
            send_all(fd, "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nCache-Control: max-age=3600\r\nContent-Length: " +
                         std::to_string(sizeof(wide_logo_bytes)) + "\r\nConnection: close\r\n\r\n" +
                         std::string(wide_logo_bytes, sizeof(wide_logo_bytes)));
        }
#endif
        else {
            std::string body = phone_page_html;
            for (size_t at; (at = body.find("__VERSION__")) != std::string::npos;) body.replace(at, 11, std::to_string(phone_page_version));
            if (size_t at = body.find("__LABELS__"); at != std::string::npos) body.replace(at, 10, blitz::moment_text_js());
#if BLITZ_MODERN
            // BRANDING: phone page logo (icons/logo_wide_2026.png) / "Blitz" text
            const std::string brand = "<img src=\"/logo.png\" alt=\"Blitz '27\">";
#else
            const std::string brand = "Blitz";
#endif
            for (size_t at; (at = body.find("__BRAND__")) != std::string::npos;) body.replace(at, 9, brand);
            send_all(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\nContent-Length: " +
                         std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        }
        blitz::net::close_socket(fd);
    }

    void server_main() {
        while (running) {
            int fd = blitz::net::accept_one(listen_fd);
            if (fd < 0) {
                if (!running) break;
                SDL_Delay(100);  // e.g. out of file descriptors: don't spin
                continue;
            }
            if (handlers.load() >= 16) {  // four phones need four; the rest is someone flooding the port
                blitz::net::close_socket(fd);
                continue;
            }
            handlers++;
            try {
                std::thread([fd] {
                    try {
                        handle(fd);
                    } catch (...) {
                        blitz::net::close_socket(fd);
                    }
                    handlers--;
                }).detach();
            } catch (...) {
                handlers--;
                blitz::net::close_socket(fd);
            }
        }
    }
}

namespace blitz {
    // The address phones open, e.g. "http://192.168.1.20:8770/?g=0123abcd", or "" when off or offline.
    std::string phone_pads_url() {
        if (!running) return {};
        std::string ip = blitz::net::local_address();
        return ip.empty() ? std::string() : "http://" + ip + ":" + std::to_string(http_port.load()) + "/?g=" + session_id;
    }

    int phone_count() {
        int n = 0;
        for (Phone& p : phones) n += p.connected ? 1 : 0;
        return n;
    }

    void phone_pads_enable(bool on) {
        if (on == running) return;
        if (on) {
            int fd = blitz::net::open_tcp();
            if (fd < 0) return;
            blitz::net::harden(fd);  // Settings → Restart must not hand the port to the new app
            blitz::net::reuse_port(fd);
            int port = 0;
            for (int p = first_port; p < first_port + 10; p++) {
                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_addr.s_addr = htonl(INADDR_ANY);
                addr.sin_port = htons((uint16_t)p);
                if (::bind(fd, (sockaddr*)&addr, sizeof(addr)) == 0) { port = p; break; }
            }
            if (port == 0 || ::listen(fd, 8) != 0) {
                fprintf(stderr, "[phones] couldn't open a port for phone controllers\n");
                blitz::net::close_socket(fd);
                return;
            }
            listen_fd = fd;
            http_port = port;
            {
                std::random_device rd;
                char id[17];
                snprintf(id, sizeof(id), "%08x%08x", (unsigned)rd(), (unsigned)rd());
                session_id = id;
            }
            running = true;
            std::thread(server_main).detach();  // a joinable std::thread left at exit would abort the app
            fprintf(stderr, "[phones] phone controllers at %s\n", phone_pads_url().c_str());
        }
        else {
            running = false;
            blitz::net::shutdown_socket(listen_fd);  // accept() returns and server_main sees running == false
            blitz::net::close_socket(listen_fd);
            listen_fd = -1;
            fprintf(stderr, "[phones] phone controllers off\n");
        }
    }

    // Main thread, from controllers_update: a virtual joystick per connected phone, fed its state.
    void phone_pads_update() {
        for (int i = 0; i < max_phones; i++) {
            Phone& p = phones[i];
            if (p.connected && p.joystick == nullptr) {
                SDL_VirtualJoystickDesc desc{};
                desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
                desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
                desc.naxes = SDL_CONTROLLER_AXIS_MAX;
                desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
                std::string name = "Phone " + std::to_string(i + 1);
                desc.name = name.c_str();
                int index = SDL_JoystickAttachVirtualEx(&desc);
                p.joystick = index >= 0 ? SDL_JoystickOpen(index) : nullptr;
            }
            else if (!p.connected && p.joystick != nullptr && SDL_GetTicks() - p.left_at < linger_ms) {
                p.state = (0u << 16) | (128u << 8) | 128u;  // away: neutral until it's back or gives up its player
            }
            else if (!p.connected && p.joystick != nullptr) {
                SDL_JoystickID id = SDL_JoystickInstanceID(p.joystick);
                SDL_JoystickClose(p.joystick);
                p.joystick = nullptr;
                for (int k = 0; k < SDL_NumJoysticks(); k++) {
                    if (SDL_JoystickGetDeviceInstanceID(k) == id) SDL_JoystickDetachVirtual(k);
                }
                p.player = 0;
            }
            if (p.joystick == nullptr) continue;
            // Menu commands (save, load, redo, be player 1) only from a phone that is playing.
            int command = p.command.exchange(cmd_none);
            if (blitz::port_of_joystick(SDL_JoystickInstanceID(p.joystick)) < 0) command = cmd_none;
            switch (command) {
                case cmd_save: blitz_savestate_request(1); break;
                case cmd_load: blitz_savestate_request(2); break;
                case cmd_redo: blitz_redo_request(); break;
                case cmd_slot_next: blitz_savestate_slot_step(1); break;
                case cmd_slot_prev: blitz_savestate_slot_step(-1); break;
                case cmd_player1: {
                    int port = blitz::port_of_joystick(SDL_JoystickInstanceID(p.joystick));
                    if (port > 0) blitz::swap_ports(0, port);
                    break;
                }
                default: break;
            }
            uint32_t s = p.state;
            uint16_t b = (uint16_t)(s >> 16);
            float x = ((int)((s >> 8) & 0xFF) - 128) / 127.0f, y = ((int)(s & 0xFF) - 128) / 127.0f;
            SDL_Joystick* j = p.joystick;
            SDL_JoystickSetVirtualButton(j, SDL_CONTROLLER_BUTTON_A, (b & 0x8000) ? 1 : 0);
            SDL_JoystickSetVirtualButton(j, SDL_CONTROLLER_BUTTON_X, (b & 0x4000) ? 1 : 0);
            SDL_JoystickSetVirtualButton(j, SDL_CONTROLLER_BUTTON_START, (b & 0x1000) ? 1 : 0);
            // Released is the axis's bottom, not 0: SDL reads a virtual pad's trigger from the whole axis range, so 0 was
            // half pressed (turbo only stayed off because a press needs more than half).
            SDL_JoystickSetVirtualAxis(j, SDL_CONTROLLER_AXIS_TRIGGERLEFT, (b & 0x2000) ? 32767 : -32768);
            SDL_JoystickSetVirtualAxis(j, SDL_CONTROLLER_AXIS_LEFTX, (Sint16)std::clamp(x * 32767.0f, -32768.0f, 32767.0f));
            SDL_JoystickSetVirtualAxis(j, SDL_CONTROLLER_AXIS_LEFTY, (Sint16)std::clamp(-y * 32767.0f, -32768.0f, 32767.0f));
            int port = blitz::port_of_joystick(SDL_JoystickInstanceID(j));
            p.player = port >= 0 ? port + 1 : 0;
        }
    }
}
