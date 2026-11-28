// Unit tests for Cloud Sync and Sync over Wi-Fi (src/game/save_sync_cloud.cpp, src/main/save_sync_wifi.cpp;
// docs/SAVE_SYNC.md §7). Run with `make test-cloud` (part of `make test`):
//   build/tests/save_sync_cloud_test [<port of tools/save_sync_worker/test/local_server.mjs>]
// Without a port, the parts that need the sync server are skipped. With one, two "devices" (two app folders) sync
// through the Worker's own code: turn on, join, push, pull, conflicts both ways, an open pak, a lost race (412), a
// changed blob on the server, a wiped server.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/stat.h>

#include "../src/game/pak_guard.h"
#include "../src/game/save_sync_cloud.h"
#include "../src/main/net_compat.h"
#include "../src/main/save_sync_wifi.h"

namespace fs = std::filesystem;
using namespace blitz::savesync;
namespace cl = blitz::savesync::cloud;
using Bytes = std::vector<uint8_t>;

static int failures = 0, checks = 0;
#define CHECK(cond)                                                         \
    do {                                                                    \
        checks++;                                                           \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                     \
        }                                                                   \
    } while (0)

// ---- save fixtures (as tests/save_sync_test.cpp makes them) ------------------------------------------------------------
static const uint64_t season_id = 0x0EE109E6D63E3BF3ull;
static const std::string adv_folder = "original/advanced/blitz2026";

static uint32_t crc32(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}
static void put(Bytes& out, uint64_t v, int n) {
    for (int i = 0; i < n; i++) out.push_back((uint8_t)(v >> (8 * i)));
}
static Bytes adv(uint64_t id, uint32_t gen, uint8_t team, uint8_t week, int salt) {
    Bytes out{'B', 'L', 'Z', 'A', 'D', 'V', '1', 0};
    put(out, 2, 4);
    put(out, 'B', 4);
    put(out, id, 8);
    put(out, gen, 4);
    put(out, 0x5EED, 8);
    put(out, 0xF1F1, 8);
    for (int t = 0; t < 32; t++) {
        char name[4] = {'T', (char)('0' + t / 10), (char)('0' + t % 10), 0};
        if (t == 3) std::memcpy(name, "LAR", 4);
        out.insert(out.end(), name, name + 4);
    }
    put(out, 0x774, 4);
    for (uint32_t i = 0; i < 0x774; i++) out.push_back(i == 0 ? team : i == 1 ? week : (uint8_t)(i * 7 + salt));
    put(out, crc32(out.data(), out.size()), 4);
    return out;
}
static Bytes blitz_pak() {
    Bytes p(blitz::pakimg::pak_size, 0);
    for (int page = 5; page < 128; page++) {
        uint16_t next = page == 127 ? 1 : page + 1;
        p[0x100 + 2 * page] = next >> 8;
        p[0x100 + 2 * page + 1] = next & 0xFF;
    }
    const uint8_t dir[32] = {0x4E, 0x42, 0x49, 0x45, 0x35, 0x44, 0x00, 0x05, 0x02, 0, 0, 0, 0, 0, 0, 0,
                             0x1B, 0x25, 0x22, 0x2D, 0x33, 0x0F, 0x12, 0x10, 0x10, 0x10, 0, 0, 0, 0, 0, 0};
    std::memcpy(&p[0x300], dir, 32);
    for (uint32_t k = 0; k < 4; k++) p[0x500 + 0xF00 + k * 0x774] = p[0x500 + 0xF00 + k * 0x774 + 1] = 0xFF;
    return p;
}
static void put_stub(Bytes& p, int slot, uint64_t id, uint32_t gen, uint8_t team, uint8_t week, uint32_t crc) {
    uint32_t s = 0x500 + 0xF00 + slot * 0x774;
    std::memset(&p[s], 0, 0x20);
    p[s] = p[s + 1] = 0xFF;
    std::memcpy(&p[s + 4], "BZ26ADV", 8);
    for (int i = 0; i < 8; i++) p[s + 0xC + i] = (uint8_t)(id >> (8 * i));
    for (int i = 0; i < 4; i++) p[s + 0x14 + i] = (uint8_t)(gen >> (8 * i));
    p[s + 0x18] = team;
    p[s + 0x19] = week;
    for (int i = 0; i < 4; i++) p[s + 0x1C + i] = (uint8_t)(crc >> (8 * i));
}

static fs::path root;
static Env env_for(const std::string& name) {
    Env e;
    e.app = root / name;
    e.version = "1.0.5";
    e.build = "900";
    e.platform = "Test" + name;
    e.pack_installed = [](const std::string&) { return true; };
    fs::create_directories(e.app / "saves");
    return e;
}
static fs::path dir_of(const Env& e, const std::string& folder) { return e.app / "saves" / folder; }
// The game's season folder: one per pack, not next to Modern-on-original's pak (adv_save.cpp season_dir; 2026-11-27).
static fs::path season_dir(const Env& e) { return e.app / "saves" / "advanced" / "blitz2026" / "season"; }
static Bytes pak_of(const Env& e) { return read_file(dir_of(e, adv_folder) / pak_name); }

// A season Save as the game makes it: the season file first, then the pak's stub (the pak before kept in pak_history/).
static void save_season(const Env& e, uint32_t gen, int salt) {
    fs::path d = dir_of(e, adv_folder);
    Bytes a = adv(season_id, gen, 3, (uint8_t)(gen + 1), salt);
    write_file(season_dir(e) / adv_name(season_id, gen), a);
    bool had = false;
    Bytes pak = read_file(d / pak_name, &had);
    if (!had) pak = blitz_pak();
    else write_file(d / "pak_history" / ("blitz2000_pak1-20261127-10" + std::to_string(gen) + std::to_string(salt) + ".mpk"), pak);
    put_stub(pak, 0, season_id, gen, 3, (uint8_t)(gen + 1), crc32(a.data(), a.size() - 4));
    write_file(d / pak_name, pak);
}

// ---- a small HTTP client for the local server (plain sockets, HTTP/1.0) -------------------------------------------------
static int server_port = 0;
static bool request(const std::string& method, const std::string& url, const std::string& body, std::string& out, int& status,
                    const std::string& bearer = "") {
    status = 0;
    out.clear();
    // http://127.0.0.1:<port>/<path>
    size_t slash = url.find('/', 7);
    std::string hostport = url.substr(7, slash - 7), path = url.substr(slash);
    int port = std::atoi(hostport.substr(hostport.find(':') + 1).c_str());
    int fd = blitz::net::open_tcp();
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (fd < 0 || blitz::net::connect_within(fd, a, 3000) != 0) {
        if (fd >= 0) blitz::net::close_socket(fd);
        return false;
    }
    blitz::net::set_timeouts(fd, 10000);
    std::string req = method + " " + path + " HTTP/1.0\r\nHost: 127.0.0.1\r\nContent-Length: " + std::to_string(body.size()) +
                      "\r\nContent-Type: application/octet-stream\r\n" +
                      (bearer.empty() ? std::string() : "Authorization: Bearer " + bearer + "\r\n") + "\r\n" + body;
    for (size_t sent = 0; sent < req.size();) {
        ssize_t n = blitz::net::send_some(fd, req.data() + sent, req.size() - sent);
        if (n <= 0) break;
        sent += (size_t)n;
    }
    std::string got;
    char buf[65536];
    for (ssize_t n; (n = blitz::net::recv_some(fd, buf, sizeof buf)) > 0;) got.append(buf, (size_t)n);
    blitz::net::close_socket(fd);
    size_t end = got.find("\r\n\r\n");
    if (got.size() < 12 || end == std::string::npos) return false;
    status = std::atoi(got.c_str() + 9);
    out = got.substr(end + 4);
    return status >= 200 && status < 300;
}

static int posts = 0;
static std::function<void()> before_next_post;  // a test's way to slip another device's write in first
static cl::Http http_client() {
    cl::Http h;
    h.get = [](const std::string& url, std::string& body, int& status) { return request("GET", url, "", body, status); };
    h.get_auth = [](const std::string& url, const std::string& bearer, std::string& body, int& status) {
        return request("GET", url, "", body, status, bearer);
    };
    h.post = [](const std::string& url, const std::string& body, int& status) {
        posts++;
        if (before_next_post) {
            auto f = before_next_post;
            before_next_post = nullptr;
            f();
        }
        std::string out;
        return request("POST", url, body, out, status);
    };
    return h;
}

static bool pak_open_now = false;
static cl::Hooks hooks(const Env& e) {
    cl::Hooks h;
    h.secrets = cl::file_secrets(e.app / "test_secret" / "code");
    h.folder_open = [](const fs::path&) { return pak_open_now; };
    h.apply = [](const fs::path& folder, bool& later, std::string& why) {
        later = false;
        Applied a = apply_staged(folder, write_pak_with_history);
        if (a == Applied::Failed) why = "failed";
        return a != Applied::Failed;
    };
    h.busy = [] { return false; };
    return h;
}

// ---- the tests --------------------------------------------------------------------------------------------------------
static void test_code_and_keys() {
    std::string a = cl::new_code(), b = cl::new_code();
    CHECK(a.size() == cl::code_chars && b.size() == cl::code_chars && a != b);
    CHECK(cl::normalize_code(a) == a);
    std::string grouped = cl::group_code(a);
    CHECK(grouped.size() == 24 + 5 && grouped[4] == '-');
    std::string lower = grouped;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    CHECK(cl::normalize_code(" " + lower + " ") == a);
    CHECK(cl::normalize_code("O0IL" + a.substr(4)) == "0011" + a.substr(4));  // read as the characters they look like
    CHECK(cl::normalize_code(a.substr(1)).empty());                            // too short
    CHECK(cl::normalize_code(a + "0").empty());                                // too long
    CHECK(cl::normalize_code("U" + a.substr(1)).empty());                      // not in the alphabet
    // The id is derived one way, the same every time, and says nothing about the key or the token.
    cl::Keys k = cl::derive(a, 1), k2 = cl::derive(a, 1), kb = cl::derive(b, 1), k_slot2 = cl::derive(a, 2);
    CHECK(k.id.size() == 64 && k.id == k2.id && k.key == k2.key && k.token == k2.token);
    CHECK(k.id != kb.id && k.key != kb.key);
    CHECK(k_slot2.id != k.id && k_slot2.key != k.key && k_slot2.token != k.token);  // each slot its own chain
    CHECK(cl::derive(a, 0).id == k.id && cl::derive(a, 9).id == k.id);              // out of range: Slot 1
    CHECK(k.id.find_first_not_of("0123456789abcdef") == std::string::npos);
    CHECK(k.token_hex() != k.id);
    CHECK(std::memcmp(k.key.data(), k.token.data(), 32) != 0);
    // Known answer: the derivation must never change, or every paired device loses its saves' address. Checked against
    // Python's hashlib: blake2b(b"new blitz cloud sync v1: slot 1: id", digest_size=32, key=b"0123456789ABCDEFGHJKMNPQ").
    CHECK(cl::derive("0123456789ABCDEFGHJKMNPQ", 1).id == "634537b06505f83b76441558729636bc67c708426027a6a2f7f5f891314b9276");
    CHECK(cl::derive("0123456789ABCDEFGHJKMNPQ", 2).id == "938df98ba0dc3960d9bf04a465b4283d2376fb82f06085bde6a99b44891e6b08");
}

static void test_seal() {
    cl::Keys k = cl::derive(cl::new_code(), 1), other = cl::derive(cl::new_code(), 1);
    Bytes plain(5000);
    for (size_t i = 0; i < plain.size(); i++) plain[i] = (uint8_t)(i * 13);
    Bytes s = cl::seal(k, 7, plain);
    CHECK(s.size() == plain.size() + cl::seal_overhead);
    CHECK(std::search(s.begin(), s.end(), plain.begin(), plain.begin() + 64) == s.end());  // no plaintext on the way
    CHECK(cl::seal(k, 7, plain) != s);                                                    // a fresh nonce each time
    cl::Opened o = cl::open(k, s);
    CHECK(o.ok && o.rev == 7 && o.plain == plain);
    CHECK(!cl::open(other, s).ok);  // another code
    for (size_t at : {(size_t)0, (size_t)5, (size_t)20, (size_t)40, (size_t)60, s.size() - 1}) {
        Bytes t = s;
        t[at] ^= 1;
        CHECK(!cl::open(k, t).ok);  // magic, revision, nonce, tag, ciphertext: any change is caught
    }
    Bytes cut(s.begin(), s.end() - 1);
    CHECK(!cl::open(k, cut).ok);
    CHECK(!cl::open(k, Bytes(10, 0)).ok);
    cl::Keys moved = k;
    moved.id = other.id;  // the same key filed under another id
    CHECK(!cl::open(moved, s).ok);
    Bytes empty_sealed = cl::seal(k, 1, {});
    CHECK(cl::open(k, empty_sealed).ok);
}

static void test_decisions() {
    using cl::Next;
    cl::SlotState s;
    s.rev = 3;
    s.fp = "abc";
    CHECK(cl::next_step(3, s, "abc") == Next::Nothing);
    CHECK(cl::next_step(3, s, "def") == Next::Push);     // saved here since
    CHECK(cl::next_step(4, s, "abc") == Next::Pull);     // another device sent
    CHECK(cl::next_step(4, s, "def") == Next::Pull);     // both: pull first, phase 1 decides
    CHECK(cl::next_step(2, s, "abc") == Next::Pull);     // the server went back (restored): look before writing
    CHECK(cl::next_step(0, s, "abc") == Next::Push);     // gone from the server (90 days, or turned off): send again
    CHECK(cl::next_step(3, s, "") == Next::Nothing);     // no saves here
    cl::SlotState fresh;
    CHECK(cl::next_step(0, fresh, "") == Next::Nothing);
    CHECK(cl::next_step(0, fresh, "abc") == Next::Push);  // turned on: the first push
    CHECK(cl::next_step(5, fresh, "") == Next::Pull);     // joined

    Plan p;
    for (Verdict v : {Verdict::Same, Verdict::New, Verdict::Newer, Verdict::Older, Verdict::Conflict, Verdict::Newer}) {
        FolderPlan f;
        f.verdict = v;
        f.folder = v == Verdict::Newer && p.folders.size() == 5 ? "open" : "x" + std::to_string(p.folders.size());
        p.folders.push_back(f);
    }
    auto open = [](const std::string& folder) { return folder == "open"; };
    cl::Split a = cl::split_pull(p, open, true);
    CHECK((a.take == std::vector<size_t>{1, 2}));
    CHECK((a.keep == std::vector<size_t>{3}));
    CHECK((a.ask == std::vector<size_t>{4}));
    CHECK((a.wait == std::vector<size_t>{5}));  // newer, but its pak is open: next start
    cl::Split b = cl::split_pull(p, open, false);
    CHECK(b.ask.empty() && (b.wait == std::vector<size_t>{4, 5}));  // nobody to ask: the conflict waits
}

static void test_wifi() {
    using namespace blitz::savesync::wifi;
    std::string c = make_code(17, 200, first_port + 9);
    Code parsed;
    CHECK(c.size() == code_chars && parse_code(c, parsed));
    CHECK(parsed.ip3 == 17 && parsed.ip4 == 200 && parsed.port == first_port + 9);
    std::string g = group_code(c);
    CHECK(parse_code(g, parsed) && parsed.text == c);
    CHECK(!parse_code(c.substr(1), parsed));
    CHECK(make_code(17, 200, first_port + 9) != c);  // the secret part is random
    CHECK(derive(c).id == derive(c).id && derive(c).id.size() == 32 && derive(c).key != derive(make_code(1, 2, first_port)).key);

    Bytes zip(70000);
    for (size_t i = 0; i < zip.size(); i++) zip[i] = (uint8_t)(i ^ (i >> 8));
    std::string code, why;
    CHECK(!start_sending(zip, "", code, why) && !why.empty());  // not on a network
    if (!start_sending(zip, "127.0.0.1", code, why)) {
        fprintf(stderr, "  wifi: couldn't serve on loopback (%s): skipped\n", why.c_str());
        return;
    }
    CHECK(send_state() == SendState::Serving && seconds_left() > 500);
    Code mine;
    CHECK(parse_code(code, mine) && mine.ip3 == 0 && mine.ip4 == 1);
    Got got = fetch_from("127.0.0.1", mine);
    CHECK(got.ok && got.zip == zip);
    CHECK(send_state() == SendState::Fetched && times_fetched() == 1);
    // The same address and port with another secret: refused, and nothing comes back.
    Code wrong = mine;
    std::string w = make_code(0, 1, mine.port);
    CHECK(parse_code(w, wrong));
    Got bad = fetch_from("127.0.0.1", wrong);
    CHECK(!bad.ok && bad.zip.empty() && bad.why.find("doesn't match") != std::string::npos);
    for (int i = 1; i < max_wrong_tries && send_state() != SendState::TooManyTries; i++) fetch_from("127.0.0.1", wrong);
    usleep(300000);
    CHECK(send_state() == SendState::TooManyTries);
    Got after = fetch_from("127.0.0.1", mine);  // stopped: even the right code gets nothing now
    CHECK(!after.ok);
    stop_sending();
    // fetch() fills in the first two numbers of the address from this device's own.
    CHECK(start_sending(zip, "127.0.0.1", code, why));
    Got via = fetch(code, "127.0.0.5");
    CHECK(via.ok && via.zip == zip);
    stop_sending();
    CHECK(!fetch(code, "127.0.0.5").ok);  // stopped
}

// Finding the sender when the code's address doesn't reach it: the discovery answer (asked directly here, as a broadcast
// would reach it) and a typed address.
static void test_wifi_find() {
    using namespace blitz::savesync::wifi;
    Bytes zip(5000, 7);
    std::string code, why;
    if (!start_sending(zip, "127.0.0.1", code, why)) return;
    CHECK(sending_address() == "127.0.0.1");
    Code c;
    CHECK(parse_code(code, c));
    CHECK(discover(c, "127.0.0.1", 1500) == "127.0.0.1");
    Code other = c;
    CHECK(parse_code(make_code(c.ip3, c.ip4, c.port), other));
    CHECK(discover(other, "127.0.0.1", 600).empty());  // another code: no answer
    Got g = fetch_at(code, "127.0.0.1");
    CHECK(g.ok && g.zip == zip);
    CHECK(!fetch_at(code, "192.168.1").ok);
    CHECK(!fetch_at("XYZ", "127.0.0.1").ok);
    stop_sending();
    CHECK(sending_address().empty() && discover(c, "127.0.0.1", 400).empty());
}

static void test_two_devices() {
    const std::string base = "http://127.0.0.1:" + std::to_string(server_port) + "/";
    cl::Http http = http_client();
    Env A = env_for("A"), B = env_for("B");
    cl::Hooks h = hooks(A), hb = hooks(B);
    cl::State sa, sb;

    // A plays a season, turns Cloud Sync on: the first push. A's league settings differ from B's.
    save_season(A, 1, 10);
    CHECK(h.secrets.put(sa.code = cl::new_code()));
    const std::string rules_a = "{\"quarter_minutes\": 3}", rules_b = "{\"quarter_minutes\": 2}";
    write_file(A.app / "rules.json", Bytes(rules_a.begin(), rules_a.end()));
    write_file(B.app / "rules.json", Bytes(rules_b.begin(), rules_b.end()));
    sa.on = true;
    cl::Outcome o = cl::sync_once(A, http, base, sa, h, false);
    CHECK(o.result == cl::Result::Pushed && sa.cur().rev == 1);
    CHECK(cl::load_state(A.app, &h.secrets).cur().rev == 1 && cl::load_state(A.app, &h.secrets).code == sa.code);
    CHECK(cl::load_state(A.app, nullptr).code.empty() && cl::load_state(A.app, nullptr).on);  // the file has no code
    o = cl::sync_once(A, http, base, sa, h, false);
    CHECK(o.result == cl::Result::UpToDate);

    // B joins with the code as someone might type it, and gets A's season.
    std::string typed = cl::group_code(sa.code);
    std::transform(typed.begin(), typed.end(), typed.begin(), ::tolower);
    o = cl::join(B, http, base, sb, hb, typed);
    CHECK(o.result == cl::Result::Pulled && sb.cur().rev == 1 && sb.on);
    CHECK(pak_of(B) == pak_of(A));
    CHECK(fs::exists(season_dir(B) / adv_name(season_id, 1)));
    // Their league settings came with the saves, for the next start (rules.json as it is until then).
    CHECK(o.message.find("league settings") != std::string::npos);
    CHECK(read_file(B.app / "rules.json") == Bytes(rules_b.begin(), rules_b.end()));
    CHECK(read_file(B.app / staging / "settings" / "rules.json") == Bytes(rules_a.begin(), rules_a.end()));
    int before = posts;
    o = cl::sync_once(B, http, base, sb, hb, false);
    CHECK(o.result == cl::Result::UpToDate && posts == before);  // no echo back to the cloud

    // A wrong or mistyped code.
    cl::State nobody;
    CHECK(cl::join(B, http, base, nobody, hb, cl::new_code()).result == cl::Result::Failed && !nobody.on);
    CHECK(cl::join(B, http, base, nobody, hb, "1234").result == cl::Result::Failed);

    // B plays on (gen 2) before restarting and pushes: its bundle carries the settings it took, not its old ones, so A
    // isn't offered B's old league back. Then B restarts (the staged settings go in).
    save_season(B, 2, 20);
    o = cl::sync_once(B, http, base, sb, hb, false);
    CHECK(o.result == cl::Result::Pushed && sb.cur().rev == 2);
    o = cl::sync_once(A, http, base, sa, h, false);
    CHECK(o.result == cl::Result::Pulled && sa.cur().rev == 2 && pak_of(A) == pak_of(B));
    CHECK(o.message.find("league settings") == std::string::npos && !fs::exists(A.app / staging / "settings"));
    CHECK(apply_staged_settings(B.app) == 1 && read_file(B.app / "rules.json") == Bytes(rules_a.begin(), rules_a.end()));
    CHECK(read_file(B.app / "rules.json.before-sync") == Bytes(rules_b.begin(), rules_b.end()));
    CHECK(o.device == "TestB " + device_label(B).substr(6));
    CHECK(cl::sync_once(A, http, base, sa, h, false).result == cl::Result::UpToDate);

    // An open pak: what's newer waits for the next start, and nothing is pushed over it.
    save_season(A, 3, 30);
    CHECK(cl::sync_once(A, http, base, sa, h, false).result == cl::Result::Pushed && sa.cur().rev == 3);
    Bytes b_before = pak_of(B);
    pak_open_now = true;
    o = cl::sync_once(B, http, base, sb, hb, false);
    CHECK(o.result == cl::Result::Waiting && pak_of(B) == b_before && sb.cur().rev == 2);
    pak_open_now = false;
    o = cl::sync_once(B, http, base, sb, hb, false);
    CHECK(o.result == cl::Result::Pulled && sb.cur().rev == 3 && pak_of(B) == pak_of(A));

    // Both play gen 4 apart: A pushes first; B (no one to ask) waits and sends nothing over A's.
    save_season(A, 4, 40);
    save_season(B, 4, 41);
    Bytes a4 = pak_of(A), b4 = pak_of(B);
    CHECK(cl::sync_once(A, http, base, sa, h, false).result == cl::Result::Pushed && sa.cur().rev == 4);
    before = posts;
    o = cl::sync_once(B, http, base, sb, hb, false);
    CHECK(o.result == cl::Result::Waiting && posts == before && pak_of(B) == b4 && sb.cur().rev == 3);
    CHECK(o.message.find("Both devices") == 0);
    // With the player there: asked; Keep Mine sends B's up, A's kept as the previous copy on the server.
    o = cl::sync_once(B, http, base, sb, hb, true);
    CHECK(o.result == cl::Result::Ask && o.server_rev == 4);
    int asked = 0;
    for (FolderPlan& f : o.plan.folders) {
        if (f.verdict == Verdict::Conflict) {
            asked++;
            f.take = false;  // Keep Mine
        }
    }
    CHECK(asked == 1);
    cl::Outcome fin = cl::finish_pull(B, http, base, sb, hb, o);
    CHECK(fin.result == cl::Result::Pushed && sb.cur().rev == 5 && pak_of(B) == b4);
    // A now sees a conflict too; Use Theirs takes B's, and A's own gen 4 goes to pak_history.
    o = cl::sync_once(A, http, base, sa, h, true);
    CHECK(o.result == cl::Result::Ask);
    for (FolderPlan& f : o.plan.folders) {
        if (f.verdict == Verdict::Conflict) f.take = true;  // Use Theirs
    }
    fin = cl::finish_pull(A, http, base, sa, h, o);
    CHECK(fin.result == cl::Result::Pulled && sa.cur().rev == 5 && pak_of(A) == b4);
    bool kept = false;
    for (const auto& e : fs::directory_iterator(dir_of(A, adv_folder) / "pak_history")) {
        kept = kept || (e.path().extension() == ".mpk" && read_file(e.path()) == a4);
    }
    CHECK(kept);  // Controller Pak Backups can put A's own gen 4 back
    CHECK(cl::sync_once(A, http, base, sa, h, false).result == cl::Result::UpToDate);
    CHECK(cl::sync_once(B, http, base, sb, hb, false).result == cl::Result::UpToDate);

    // A race: B's write lands between A's look and A's write. A's write is turned down (412), A pulls B's and,
    // since both changed the season, waits; B's saves stay on the server.
    save_season(A, 6, 60);
    save_season(B, 6, 61);
    before_next_post = [&] { CHECK(cl::sync_once(B, http, base, sb, hb, false).result == cl::Result::Pushed); };
    o = cl::sync_once(A, http, base, sa, h, false);
    CHECK(o.result == cl::Result::Waiting && sb.cur().rev == 6 && sa.cur().rev == 5);
    std::string body;
    int status = 0;
    CHECK(request("GET", base + "v1/" + cl::derive(sa.code, 1).id + "/rev", "", body, status) && body.find("\"rev\":6") != std::string::npos);

    // A blob changed on the server (or a wrong one served): refused, nothing here touched.
    {
        cl::Keys k = cl::derive(sa.code, 1);
        cl::Keys fake = k;
        fake.key[0] ^= 1;  // sealed with another key, under the same id and token
        Bytes junk = cl::seal(fake, 7, Bytes(100, 1));
        CHECK(cl::put(http, base, k, 6, junk) == 201);
        Bytes a_before = pak_of(A);
        o = cl::sync_once(A, http, base, sa, h, true);
        CHECK(o.result == cl::Result::Failed && pak_of(A) == a_before && sa.cur().rev == 5);
    }

    // The server lost it all (90 days untouched, or Turn Off and Delete on another device): the next sync sends it again.
    {
        cl::Keys k = cl::derive(sa.code, 1);
        CHECK(cl::remove(http, base, cl::derive(cl::new_code(), 1)) == 200);  // another code's: nothing there, fine
        cl::Keys wrong_token = k;
        wrong_token.token[0] ^= 1;
        CHECK(cl::remove(http, base, wrong_token) == 403);
        CHECK(cl::remove(http, base, k) == 200);
        CHECK(cl::head(http, base, k).rev == 0);
        sb.cur().fp.clear();
        o = cl::sync_once(B, http, base, sb, hb, false);
        CHECK(o.result == cl::Result::Pushed && sb.cur().rev == 1);
    }

    // The game saving right now: nothing read, nothing sent.
    cl::Hooks busy = hb;
    busy.busy = [] { return true; };
    CHECK(cl::sync_once(B, http, base, sb, busy, false).result == cl::Result::Busy);
    // Off: nothing at all.
    cl::State off;
    CHECK(cl::sync_once(B, http, base, off, hb, false).result == cl::Result::Off);
    // No server: offline, and the state says so.
    cl::State dead = sb;
    CHECK(cl::sync_once(B, http, "http://127.0.0.1:1/", dead, h, false).result == cl::Result::Offline && !dead.last_error.empty());
}

// Where the code is kept: never in cloud_sync.json; the file fallback is 0600; an old file with the code in it is moved.
static void test_secrets() {
    Env e = env_for("secrets");
    cl::Secrets sec = cl::file_secrets(e.app / "secret" / "code");
    std::string code = cl::new_code(), got;
    CHECK(!sec.get(got));
    CHECK(sec.put(code) && sec.get(got) && got == code);
    struct stat sb;
    CHECK(::stat((e.app / "secret" / "code").c_str(), &sb) == 0 && (sb.st_mode & 0777) == 0600);
    sec.erase();
    CHECK(!sec.get(got));
    // A state file from before: its code goes to the secret store and out of the file.
    std::string old = "{\"on\": true, \"code\": \"" + cl::group_code(code) + "\", \"rev\": 4, \"fp\": \"x\"}";
    write_file(e.app / "cloud_sync.json", Bytes(old.begin(), old.end()));
    cl::State s = cl::load_state(e.app, &sec);
    CHECK(s.on && s.code == code && s.slot == 1 && s.cur().rev == 4 && s.cur().fp == "x");
    Bytes file = read_file(e.app / "cloud_sync.json");
    CHECK(std::string(file.begin(), file.end()).find(code.substr(0, 4)) == std::string::npos);
    CHECK(sec.get(got) && got == code);
    // Without the code (secure storage emptied): not on.
    sec.erase();
    CHECK(!cl::load_state(e.app, &sec).on);
    CHECK(cl::load_state(e.app, nullptr).on);  // the cheap read only says what the file says
}

// The push guard (a device never sends saves that don't carry the cloud's on, unasked) and Cloud Save Slots.
static void test_guards_and_slots() {
    const std::string base = "http://127.0.0.1:" + std::to_string(server_port) + "/";
    cl::Http http = http_client();
    Env C = env_for("C"), D = env_for("D");
    cl::Hooks hc = hooks(C), hd = hooks(D);
    cl::State sc, sd;
    const std::string code = cl::new_code();

    save_season(C, 1, 1);
    CHECK(hc.secrets.put(code));
    sc.on = true;
    sc.code = code;
    CHECK(cl::sync_once(C, http, base, sc, hc, false).result == cl::Result::Pushed && sc.cur().rev == 1);
    const Bytes gen1 = pak_of(C);
    save_season(C, 2, 2);
    CHECK(cl::sync_once(C, http, base, sc, hc, false).result == cl::Result::Pushed && sc.cur().rev == 2);
    const cl::Keys k1 = cl::derive(code, 1);

    // A backup put back (the pak as at gen 1): the cloud's gen 2 doesn't come before it. Nothing is sent unasked.
    write_file(dir_of(C, adv_folder) / pak_name, gen1);
    int before = posts;
    cl::Outcome o = cl::sync_once(C, http, base, sc, hc, false);
    CHECK(o.result == cl::Result::Waiting && posts == before && cl::head(http, base, k1).rev == 2);
    CHECK(o.message.find("don't carry on") != std::string::npos);
    o = cl::sync_once(C, http, base, sc, hc, true);
    CHECK(o.result == cl::Result::Ask && o.guard && o.asked.size() == 1 && posts == before);
    {
        const FolderPlan& f = o.plan.folders[o.asked[0]];
        CHECK(f.verdict == Verdict::Newer && !f.theirs.empty() && !f.mine.empty() && !f.theirs_when.empty() && !f.mine_when.empty());
        CHECK(o.plan.device == device_label(C));
    }
    // Use the Cloud's: gen 2 back on the device, nothing sent.
    o.plan.folders[o.asked[0]].take = true;
    cl::Outcome fin = cl::finish_pull(C, http, base, sc, hc, o);
    CHECK((fin.result == cl::Result::Pulled || fin.result == cl::Result::UpToDate) && cl::head(http, base, k1).rev == 2);
    CHECK(pak_of(C) != gen1);

    // An unrelated season (data reset, a new season started): asked; Keep This Device's sends it.
    fs::remove_all(dir_of(C, adv_folder));
    save_season(C, 1, 77);
    const Bytes unrelated = pak_of(C);
    o = cl::sync_once(C, http, base, sc, hc, true);
    CHECK(o.result == cl::Result::Ask && o.guard && o.plan.folders[o.asked[0]].verdict == Verdict::Conflict);
    for (size_t i : o.asked) o.plan.folders[i].take = false;  // Keep This Device's
    fin = cl::finish_pull(C, http, base, sc, hc, o);
    CHECK(fin.result == cl::Result::Pushed && sc.cur().rev == 3 && cl::head(http, base, k1).rev == 3);
    // Cancel (nothing answered): the next round asks again; nothing sent meanwhile.
    fs::remove_all(dir_of(C, adv_folder));
    save_season(C, 1, 78);
    CHECK(cl::sync_once(C, http, base, sc, hc, true).result == cl::Result::Ask);
    CHECK(cl::sync_once(C, http, base, sc, hc, true).result == cl::Result::Ask && cl::head(http, base, k1).rev == 3);
    write_file(dir_of(C, adv_folder) / pak_name, unrelated);  // back as it was synced
    fs::remove(season_dir(C) / adv_name(season_id, 1));
    write_file(season_dir(C) / adv_name(season_id, 1), adv(season_id, 1, 3, 2, 77));
    CHECK(cl::sync_once(C, http, base, sc, hc, false).result == cl::Result::UpToDate);

    // Slots: D joins (Slot 1). C moves to Slot 2 (empty): its saves go there; Slot 1 stays as it was.
    o = cl::join(D, http, base, sd, hd, code);
    CHECK(o.result == cl::Result::Pulled && sd.slot == 1 && pak_of(D) == unrelated);
    sc.slot = 2;
    o = cl::sync_once(C, http, base, sc, hc, true, true);
    CHECK(o.result == cl::Result::Pushed && sc.cur().rev == 1 && sc.slots[0].rev == 3);
    const cl::Keys k2 = cl::derive(code, 2);
    CHECK(cl::head(http, base, k2).rev == 1 && cl::head(http, base, k1).rev == 3);
    save_season(C, 2, 79);
    CHECK(cl::sync_once(C, http, base, sc, hc, false).result == cl::Result::Pushed && cl::head(http, base, k2).rev == 2);
    CHECK(cl::sync_once(D, http, base, sd, hd, false).result == cl::Result::UpToDate && pak_of(D) == unrelated);  // Slot 1: no change
    // D switches to Slot 2: newer saves there, but switching asks before anything on D changes.
    sd.slot = 2;
    o = cl::sync_once(D, http, base, sd, hd, true, true);
    CHECK(o.result == cl::Result::Ask && !o.guard && o.plan.folders[o.asked[0]].verdict == Verdict::Newer && pak_of(D) == unrelated);
    o.plan.folders[o.asked[0]].take = true;  // Use Slot 2's
    fin = cl::finish_pull(D, http, base, sd, hd, o);
    CHECK(fin.result == cl::Result::Pulled && pak_of(D) == pak_of(C) && sd.cur().rev == 2);
    // And back to Slot 1: D's saves carry Slot 1's on, but sending them would replace what Slot 1 keeps: asked.
    sd.slot = 1;
    o = cl::sync_once(D, http, base, sd, hd, true, true);
    CHECK(o.result == cl::Result::Ask && o.plan.folders[o.asked[0]].verdict == Verdict::Older && cl::head(http, base, k1).rev == 3);
    // Not switching (an ordinary sync), older saves in the cloud simply get this device's.
    CHECK(cl::split_pull(o.plan, nullptr, true, false).keep.size() == 1);
    // State per slot survives a reload.
    cl::State re = cl::load_state(C.app, &hc.secrets);
    CHECK(re.slot == 2 && re.slots[0].rev == 3 && re.slots[1].rev == 2 && re.code == code);
}

// ---- pairing: a 6-digit number in place of the code ---------------------------------------------------------------------
static void test_pairing_offline() {
    // Numbers: six digits, from the random source, every digit used.
    std::string seen;
    for (int i = 0; i < 200; i++) {
        std::string n = cl::new_pair_number();
        CHECK(n.size() == 6 && n.find_first_not_of("0123456789") == std::string::npos);
        seen += n;
    }
    for (char d = '0'; d <= '9'; d++) CHECK(seen.find(d) != std::string::npos);
    CHECK(cl::new_pair_number() != cl::new_pair_number() || cl::new_pair_number() != cl::new_pair_number());
    CHECK(cl::normalize_pair_number("482 915") == "482915");
    CHECK(cl::normalize_pair_number(" 482-915 ") == "482915");
    CHECK(cl::normalize_pair_number("48291").empty());
    CHECK(cl::normalize_pair_number("4829150").empty());
    CHECK(cl::normalize_pair_number("48291a").empty());
    CHECK(cl::group_pair_number("482915") == "482 915");
    // Known answers (Python: blake2b(b"id: 482915", digest_size=32, key=b"new blitz pairing v1"), and "proof: "): the
    // two derivations must never change, or a new app couldn't pair with an old one.
    cl::PairKeys k = cl::pair_keys("482915");
    CHECK(k.id == "db74604ac58f7c9b2046cee5cdda077148a912cb2bea7006a3b09eeffd3bb3f4");
    CHECK(k.proof_hex() == "a5a2d7279d4df97150b1f64bde3e577c8f60a17a68ea35595c826112c09f791b");
    CHECK(cl::pair_keys("482916").id != k.id && k.proof_hex() != k.id);
    // Sealing the code: opens with the number only, carries the slot, and any change is caught.
    const std::string code = cl::new_code();
    Bytes s = cl::seal_pairing("482915", code, 2);
    CHECK(std::search(s.begin(), s.end(), code.begin(), code.end()) == s.end());  // no code on the way
    CHECK(cl::seal_pairing("482915", code, 2) != s);                              // a fresh salt and nonce each time
    std::string got;
    int slot = 0;
    CHECK(cl::open_pairing("482915", s, got, slot) && got == code && slot == 2);
    CHECK(!cl::open_pairing("482916", s, got, slot));
    for (size_t at : {(size_t)0, (size_t)6, (size_t)30, (size_t)50, s.size() - 1}) {
        Bytes t = s;
        t[at] ^= 1;
        CHECK(!cl::open_pairing("482915", t, got, slot));
    }
    CHECK(!cl::open_pairing("482915", Bytes(s.begin(), s.end() - 1), got, slot));
}

// What "exactly as before" means for a device: its Cloud Sync state file, its stored code, its synced copies, its saves.
struct DeviceNow {
    cl::Snapshot snap;
    std::string saves;
    bool operator==(const DeviceNow& o) const {
        return snap.had_state == o.snap.had_state && snap.state == o.snap.state && snap.had_code == o.snap.had_code &&
               snap.code == o.snap.code && snap.copies == o.snap.copies && saves == o.saves;
    }
};
static DeviceNow device_now(const Env& e, const cl::Hooks& h) { return {cl::snapshot(e.app, h.secrets), saves_fingerprint(e)}; }

static void test_snapshot() {
    Env e = env_for("snap");
    cl::Secrets sec = cl::file_secrets(e.app / "secret" / "code");
    // Off, nothing stored: a change and a restore leave nothing behind.
    cl::Snapshot off = cl::snapshot(e.app, sec);
    CHECK(!off.had_state && !off.had_code && off.copies.empty());
    cl::State s;
    s.on = true;
    s.code = cl::new_code();
    CHECK(sec.put(s.code) && cl::store_state(e.app, s));
    cl::keep_synced_copy(e.app, 1, 3, Bytes(10, 7));
    cl::restore(e.app, sec, off);
    std::string got;
    CHECK(!fs::exists(e.app / "cloud_sync.json") && !sec.get(got) && !fs::exists(e.app / "cloud_sync"));
    // On, with a slot and copies: a change and a restore put each byte back.
    CHECK(sec.put(s.code) && cl::store_state(e.app, s));
    cl::keep_synced_copy(e.app, 1, 3, Bytes(10, 7));
    cl::Snapshot on = cl::snapshot(e.app, sec);
    CHECK(on.had_state && on.had_code && on.code == s.code && on.copies.size() == 2);
    cl::State t = s;
    t.slot = 3;
    t.code = cl::new_code();
    CHECK(sec.put(t.code) && cl::store_state(e.app, t));
    cl::keep_synced_copy(e.app, 3, 9, Bytes(5, 1));
    cl::restore(e.app, sec, on);
    cl::Snapshot back = cl::snapshot(e.app, sec);
    CHECK(back.state == on.state && back.code == on.code && back.copies == on.copies);
    CHECK(cl::load_state(e.app, &sec).slot == 1);
}

static void test_pairing() {
    const std::string base = "http://127.0.0.1:" + std::to_string(server_port) + "/";
    cl::Http http = http_client();
    Env P = env_for("P"), Q = env_for("Q"), R = env_for("R");
    cl::Hooks hp = hooks(P), hq = hooks(Q), hr = hooks(R);
    cl::State sp, sq, sr;

    // P turns Cloud Sync on (its saves go up) and offers a number for Q.
    save_season(P, 1, 90);
    cl::Outcome o = cl::turn_on(P, http, base, sp, hp);
    CHECK(o.result == cl::Result::Pushed && sp.on && sp.code.size() == cl::code_chars);
    cl::Offer offer = cl::offer_pairing(http, base, sp.code, sp.slot);
    CHECK(offer.ok && offer.number.size() == 6 && offer.id == cl::pair_keys(offer.number).id);
    bool live = false;
    CHECK(cl::pairing_live(http, base, offer, live) && live);
    // Q types it as a person might ("482 915"): it gets the code, never shown, and joins P's slot.
    cl::Claimed c = cl::claim_pairing(http, base, cl::group_pair_number(offer.number));
    CHECK(c.ok && c.code == sp.code && c.slot == 1);
    o = cl::join(Q, http, base, sq, hq, c.code, c.slot);
    CHECK(o.result == cl::Result::Pulled && sq.on && sq.code == sp.code && pak_of(Q) == pak_of(P));
    std::string kept;
    CHECK(hq.secrets.get(kept) && kept == sp.code);  // in the secret store, as a typed code would be
    // Once: the number is gone, and P's screen sees it taken.
    CHECK(!cl::claim_pairing(http, base, offer.number).ok);
    CHECK(cl::pairing_live(http, base, offer, live) && !live);

    // A wrong number: nothing, and the words say what to do.
    cl::Offer o2 = cl::offer_pairing(http, base, sp.code, sp.slot);
    std::string wrong = o2.number;
    wrong[5] = wrong[5] == '9' ? '0' : (char)(wrong[5] + 1);
    c = cl::claim_pairing(http, base, wrong);
    CHECK(!c.ok && c.status == 404 && c.message.find("didn't work") != std::string::npos && c.code.empty());
    CHECK(!cl::claim_pairing(http, base, "12345").ok);  // too short: never sent
    // Cancel on P: gone at once.
    CHECK(cl::pairing_live(http, base, o2, live) && live);
    cl::cancel_pairing(http, base, o2);
    CHECK(cl::pairing_live(http, base, o2, live) && !live);
    CHECK(cl::claim_pairing(http, base, o2.number).status == 404);
    // New Number: the old one stops working at once.
    cl::Offer o3 = cl::offer_pairing(http, base, sp.code, sp.slot);
    cl::Offer o4 = cl::offer_pairing(http, base, sp.code, sp.slot, &o3);
    CHECK(o4.ok && o4.number != o3.number && o4.owner == o3.owner);
    CHECK(cl::pairing_live(http, base, o3, live) && !live && cl::claim_pairing(http, base, o3.number).status == 404);
    cl::cancel_pairing(http, base, o4);

    // A number from a device syncing with Slot 2 brings the joiner to Slot 2, even an empty one (R's saves go there).
    sp.slot = 2;
    cl::store_state(P.app, sp);
    cl::Offer o5 = cl::offer_pairing(http, base, sp.code, sp.slot);
    c = cl::claim_pairing(http, base, o5.number);
    CHECK(c.ok && c.slot == 2);
    save_season(R, 1, 91);
    o = cl::join(R, http, base, sr, hr, c.code, c.slot);
    CHECK(o.result == cl::Result::Pushed && sr.slot == 2 && cl::head(http, base, cl::derive(sp.code, 2)).rev == 1);
    // A typed full code still finds the first slot with saves (the fallback).
    cl::State sr2;
    Env R2 = env_for("R2");
    cl::Hooks hr2 = hooks(R2);
    o = cl::join(R2, http, base, sr2, hr2, cl::group_code(sp.code));
    CHECK(o.result == cl::Result::Pulled && sr2.slot == 1);
    // Nowhere to reach: no number, said plainly.
    cl::Offer dead = cl::offer_pairing(http, "http://127.0.0.1:1/", sp.code, 1);
    CHECK(!dead.ok && dead.message.find("can't be reached") != std::string::npos);
}

// Cancel anywhere: the device is exactly as it was (no code kept, no slot changed, no saves touched, nothing sent).
static void test_cancel() {
    const std::string base = "http://127.0.0.1:" + std::to_string(server_port) + "/";
    cl::Http http = http_client();
    Env E = env_for("E"), F = env_for("F");
    cl::Hooks he = hooks(E), hf = hooks(F);
    cl::State se, sf;
    save_season(E, 1, 100);
    save_season(F, 1, 101);  // F has its own, unrelated season: joining E's asks

    // Turn On cancelled before anything was sent: off, no code, nothing on the server.
    DeviceNow e0 = device_now(E, he);
    cl::Hooks no = he;
    no.commit = [] { return false; };
    cl::Outcome o = cl::turn_on(E, http, base, se, no);
    CHECK(o.result == cl::Result::Cancelled && !se.on && device_now(E, he) == e0);
    CHECK(!cl::load_state(E.app, &he.secrets).on && !cl::load_state(E.app, nullptr).on);
    // Turn On offline: the same.
    o = cl::turn_on(E, http, "http://127.0.0.1:1/", se, he);
    CHECK(o.result == cl::Result::Offline && !se.on && device_now(E, he) == e0);
    // Turn On for real.
    o = cl::turn_on(E, http, base, se, he);
    CHECK(o.result == cl::Result::Pushed && se.on);
    const cl::Keys k1 = cl::derive(se.code, 1);

    // Join cancelled before anything changed: F exactly as it was, Cloud Sync still off.
    DeviceNow f0 = device_now(F, hf);
    cl::Hooks fno = hf;
    fno.commit = [] { return false; };
    save_season(E, 2, 102);
    CHECK(cl::sync_once(E, http, base, se, he, false).result == cl::Result::Pushed);
    Env G = env_for("G");  // no saves: E's would simply come in, if not cancelled
    cl::Hooks hg = hooks(G), gno = hg;
    gno.commit = [] { return false; };
    DeviceNow g0 = device_now(G, hg);
    cl::State sg;
    o = cl::join(G, http, base, sg, gno, se.code, 1);
    CHECK(o.result == cl::Result::Cancelled && !sg.on && device_now(G, hg) == g0);
    // Join offline: as it was.
    o = cl::join(G, http, "http://127.0.0.1:1/", sg, hg, se.code, 1);
    CHECK(o.result == cl::Result::Offline && !sg.on && device_now(G, hg) == g0);
    // Join with a wrong code: as it was.
    o = cl::join(G, http, base, sg, hg, cl::new_code());
    CHECK(o.result == cl::Result::Failed && device_now(G, hg) == g0);

    // Join with a question (F's season and E's are unrelated): asked first, and nothing is kept until it's answered. Cancel
    // on the question (the screens drop the outcome): F exactly as it was.
    o = cl::join(F, http, base, sf, hf, se.code, 1);
    CHECK(o.result == cl::Result::Ask && o.pending.code == se.code && o.pending.slot == 1 && !o.pending.switching);
    CHECK(!sf.on && device_now(F, hf) == f0 && !cl::load_state(F.app, nullptr).on);
    // Answered, but cancelled before anything changed: still as it was.
    cl::Outcome asked = o;
    for (size_t i : asked.asked) asked.plan.folders[i].take = true;  // Use the Cloud's
    cl::Outcome fin = cl::finish_pull(F, http, base, sf, fno, asked);
    CHECK(fin.result == cl::Result::Cancelled && !sf.on && device_now(F, hf) == f0);
    // Answered for real: now the code is kept, F syncs with Slot 1 and has E's saves.
    fin = cl::finish_pull(F, http, base, sf, hf, asked);
    CHECK(fin.result == cl::Result::Pulled && sf.on && sf.code == se.code && sf.slot == 1 && pak_of(F) == pak_of(E));
    std::string kept;
    CHECK(hf.secrets.get(kept) && kept == se.code && cl::load_state(F.app, &hf.secrets).on);

    // Switching slots, cancelled: the slot is the one before, nothing sent to the new slot.
    const cl::Keys k2 = cl::derive(se.code, 2);
    DeviceNow f1 = device_now(F, hf);
    o = cl::switch_slot(F, http, base, sf, fno, 2);
    CHECK(o.result == cl::Result::Cancelled && sf.slot == 1 && device_now(F, hf) == f1 && cl::head(http, base, k2).rev == 0);
    // Offline: the same (an ordinary sync must never run with a slot the player didn't finish switching to).
    o = cl::switch_slot(F, http, "http://127.0.0.1:1/", sf, hf, 2);
    CHECK(o.result == cl::Result::Offline && sf.slot == 1 && device_now(F, hf) == f1);
    // E fills Slot 2 with other saves; F switching asks, and until it's answered F stays on Slot 1.
    fs::remove_all(dir_of(E, adv_folder));
    save_season(E, 1, 103);
    se.slot = 2;
    CHECK(cl::sync_once(E, http, base, se, he, true, true).result == cl::Result::Pushed && cl::head(http, base, k2).rev == 1);
    o = cl::switch_slot(F, http, base, sf, hf, 2);
    CHECK(o.result == cl::Result::Ask && o.pending.switching && o.pending.slot == 2 && sf.slot == 1);
    CHECK(device_now(F, hf) == f1 && cl::load_state(F.app, nullptr).slot == 1);
    // Answered (Keep This Device's): now F syncs with Slot 2 and its saves are there.
    for (size_t i : o.asked) o.plan.folders[i].take = false;
    fin = cl::finish_pull(F, http, base, sf, hf, o);
    CHECK(fin.result == cl::Result::Pushed && sf.slot == 2 && cl::load_state(F.app, nullptr).slot == 2 && cl::head(http, base, k2).rev == 2);
    CHECK(sf.code == se.code && cl::load_state(F.app, &hf.secrets).code == se.code);  // the code untouched by a switch

    // An ordinary sync cancelled before sending: nothing sent, the saves as they were.
    save_season(F, 3, 104);
    const uint64_t rev = cl::head(http, base, k2).rev;
    const std::string saves = saves_fingerprint(F);
    o = cl::sync_once(F, http, base, sf, fno, true);
    CHECK(o.result == cl::Result::Cancelled && cl::head(http, base, k2).rev == rev && saves_fingerprint(F) == saves);
    CHECK(cl::sync_once(F, http, base, sf, hf, true).result == cl::Result::Pushed);
    (void)k1;
}

// The guess limit, as the player meets it: past a few wrong numbers, "wait a minute" (make test-cloud's server allows
// PAIR_TRIES a minute; the Worker's own test checks the deployed 5).
static void test_pair_limit() {
    const std::string base = "http://127.0.0.1:" + std::to_string(server_port) + "/";
    cl::Http http = http_client();
    cl::Claimed c;
    int tries = 0;
    for (; tries < 200; tries++) {
        c = cl::claim_pairing(http, base, cl::new_pair_number());
        if (c.status != 404) break;
    }
    CHECK(c.status == 429 && !c.ok && c.message.find("Wait a minute") != std::string::npos);
    // And while limited, even the right number gets nothing (the limit is per connection, not per guess).
    cl::Offer o = cl::offer_pairing(http, base, cl::new_code(), 1);
    CHECK(!o.ok && o.status == 429);
}

int main(int argc, char** argv) {
    char tmpl[] = "/tmp/blitz-cloud-test-XXXXXX";
    root = mkdtemp(tmpl);
    if (argc > 1) server_port = std::atoi(argv[1]);
    fprintf(stderr, "code and keys\n");
    test_code_and_keys();
    fprintf(stderr, "seal and open\n");
    test_seal();
    fprintf(stderr, "decisions\n");
    test_decisions();
    fprintf(stderr, "wifi\n");
    test_wifi();
    test_wifi_find();
    fprintf(stderr, "secrets\n");
    test_secrets();
    fprintf(stderr, "pairing numbers and snapshots\n");
    test_pairing_offline();
    test_snapshot();
    if (server_port > 0) {
        fprintf(stderr, "two devices through the local sync server (port %d)\n", server_port);
        test_two_devices();
        fprintf(stderr, "guards and slots\n");
        test_guards_and_slots();
        fprintf(stderr, "pairing with a 6-digit number\n");
        test_pairing();
        fprintf(stderr, "cancel leaves each device as it was\n");
        test_cancel();
        fprintf(stderr, "the guess limit\n");
        test_pair_limit();
    }
    else fprintf(stderr, "two devices: skipped (no sync server port; make test-cloud starts one)\n");
    std::error_code ec;
    fs::remove_all(root, ec);
    fprintf(stderr, "%s: %d checks, %d failed\n", failures ? "FAIL" : "ok", checks, failures);
    return failures ? 1 : 0;
}
