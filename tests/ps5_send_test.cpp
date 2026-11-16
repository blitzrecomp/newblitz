// Send to PS5 (src/main/ps5_send.h) without a console: `make test-ps5-send`.
//   ps5_send_test                      the connect timeout, the progress line, the manifest
//   ps5_send_test <ftp port> <root>    plus whole sends to a local FTP server serving <root> as the console's / (the
//                                      Makefile starts pyftpdlib from .venv), and the launch to a stand-in loader
#include "../src/main/ps5_send.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace blitz::ps5;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void write(const fs::path& p, const std::string& data) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << data;
}
static std::string read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

static void test_connect_timeout() {
    // Nothing at this address on the team's network: the connect has to give up in about 5 s, not the system's 75.
    int s = blitz::net::open_tcp();
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(1337);
    inet_pton(AF_INET, "192.168.0.250", &a.sin_addr);
    auto t0 = std::chrono::steady_clock::now();
    int err = blitz::net::connect_within(s, a, 5000);
    double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    blitz::net::close_socket(s);
    printf("dead address: error %d (%s) after %.2f s\n", err, strerror(err), took);
    CHECK(err != 0);
    CHECK(took < 6.0);

    // The same through the FTP client, as the app calls it.
    Ftp ftp;
    t0 = std::chrono::steady_clock::now();
    bool ok = ftp.open("192.168.0.250", 1337, 5000);
    took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("Ftp::open on the dead address: %s after %.2f s\n", ok ? "open?!" : "failed", took);
    CHECK(!ok && took < 6.0);

    // A machine that's there with nothing listening: refused at once.
    s = blitz::net::open_tcp();
    a.sin_port = htons(1);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    t0 = std::chrono::steady_clock::now();
    err = blitz::net::connect_within(s, a, 5000);
    took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    blitz::net::close_socket(s);
    printf("closed port: error %d after %.3f s\n", err, took);
    CHECK(err == ECONNREFUSED && took < 1.0);
}

static void test_progress() {
    CHECK(mb(18'400'000) == "18.4");
    CHECK(mb(44'774'212) == "44.8");
    CHECK(mb(0) == "0.0");
    Snapshot s{11, 23, 18'400'000, 45'000'000, 5, 300000};
    CHECK(eta_seconds(s) == -1);  // too early
    CHECK(progress_text(s) == "Sending 12 of 23 files \xC2\xB7 18.4 of 45.0 MB");
    s.elapsed = 60;               // average 306,667 B/s, recent 300,000: steady
    int eta = eta_seconds(s);
    printf("eta %d s: %s\n", eta, eta_text(eta).c_str());
    CHECK(eta > 85 && eta < 90);  // 26.6 MB at ~303 KB/s
    CHECK(progress_text(s) == "Sending 12 of 23 files \xC2\xB7 18.4 of 45.0 MB \xC2\xB7 about 1 minute left");
    s.recent_rate = 40000;        // slowed to 40 KB/s: not steady, no guess
    CHECK(eta_seconds(s) == -1);
    s.recent_rate = -1;
    CHECK(eta_seconds(s) == -1);
    CHECK(eta_text(30) == "less than a minute left");
    CHECK(eta_text(89) == "about 1 minute left");
    CHECK(eta_text(90) == "about 2 minutes left");
    CHECK(eta_text(17 * 60) == "about 17 minutes left");
    CHECK(eta_text(4000) == "over an hour left");
    Snapshot one{0, 1, 0, 2'000'000, 0, -1};
    CHECK(progress_text(one) == "Sending 1 of 1 file \xC2\xB7 0.0 of 2.0 MB");
    Snapshot done{23, 23, 45'000'000, 45'000'000, 100, 400000};
    CHECK(progress_text(done).rfind("Sending 23 of 23 files", 0) == 0);

    RateWindow w(15);
    CHECK(w.rate() < 0);
    for (int t = 0; t <= 30; t++) w.add(t, (uint64_t)t * 100000);
    CHECK(std::fabs(w.rate() - 100000) < 1);
    for (int t = 31; t <= 50; t++) w.add(t, 3'000'000 + (uint64_t)(t - 30) * 10000);  // slowed down
    CHECK(std::fabs(w.rate() - 10000) < 1);
}

static void test_manifest() {
    Manifest m;
    m["/data/homebrew/PPSA99961/eboot.bin"] = Entry{std::string(64, 'a'), 44774212};
    m["/data/homebrew/PPSA99961/art/a file.wms"] = Entry{std::string(64, 'b'), 51542};
    Manifest back;
    CHECK(parse_manifest(manifest_text(m), back));
    CHECK(back.size() == 2);
    CHECK(back["/data/homebrew/PPSA99961/art/a file.wms"].size == 51542);
    CHECK(back["/data/homebrew/PPSA99961/eboot.bin"].sha == std::string(64, 'a'));
    CHECK(!parse_manifest("something else\n", back));
    CHECK(!parse_manifest("", back));
}

// A stand-in ELF loader: takes one payload; then, as a fresh start of the game would, moves blitz.log (or not).
static std::thread loader(int& port, const fs::path& app, bool restart, std::string& got) {
    int l = blitz::net::open_tcp();
    blitz::net::reuse_port(l);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    bind(l, (sockaddr*)&a, sizeof(a));
    socklen_t len = sizeof(a);
    getsockname(l, (sockaddr*)&a, &len);
    port = ntohs(a.sin_port);
    listen(l, 1);
    return std::thread([l, app, restart, &got] {
        int c = blitz::net::accept_one(l);
        char buf[4096];
        ssize_t n;
        while ((n = blitz::net::recv_some(c, buf, sizeof(buf))) > 0) got.append(buf, (size_t)n);
        blitz::net::close_socket(c);
        blitz::net::close_socket(l);
        if (restart) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            fs::rename(app / "blitz.log", app / "blitz.prev.log");
            write(app / "blitz.log", "[log] Blitz2026Recompiled started again\n");
        }
    });
}

static void test_sends(int ftp_port, const fs::path& root) {
    fs::path here = root.parent_path() / "computer";
    fs::remove_all(here);
    fs::path kit = here / "PPSA99961";
    write(kit / "eboot.bin", std::string(300000, 'E'));
    write(kit / "sce_sys/param.json", "{\"titleId\":\"PPSA99961\"}");
    for (int i = 0; i < 5; i++) write(kit / ("art/files/team" + std::to_string(i) + ".wms"), std::string(51542, (char)('a' + i)));
    write(here / "launch.elf", std::string(1000, 'L'));
    write(here / "blitz2000.n64.us.1.1.z64", std::string(200000, 'R'));
    write(here / "paks/season2026.nbpak", std::string(120000, 'P'));
    write(here / "season.json", "{\"computer\":\"season\"}");
    write(here / "rules.json", "{\"computer\":\"rules\"}");

    fs::path title = root / "data/homebrew/PPSA99961", drop = root / "data/blitzrecomp", app = drop / "Blitz2026Recompiled";
    Job job;
    job.host = "127.0.0.1";
    job.ftp_port = ftp_port;
    job.title_dir = kit;
    job.rom = here / "blitz2000.n64.us.1.1.z64";
    job.rom_name = "blitz2000.n64.us.1.1.z64";
    job.pak = here / "paks/season2026.nbpak";
    job.setup = {here / "season.json", here / "rules.json"};
    Progress progress;
    std::vector<std::string> lines;
    auto log = [&](const std::string& l) {
        lines.push_back(l);
        printf("  [ps5] %s\n", l.c_str());
    };

    // 1. A fresh console: everything goes, setup and all.
    printf("send 1: a fresh console\n");
    Result r = send(job, progress, log);
    CHECK(r.stage == Result::Ok && r.setup_ok && !r.set_up_already);
    CHECK(r.files_sent == 7 + 1 + 1 + 2);  // the app, ROM, pak, season and rules
    CHECK(read(title / "eboot.bin") == read(kit / "eboot.bin"));
    CHECK(read(title / "art/files/team3.wms") == read(kit / "art/files/team3.wms"));
    CHECK(read(drop / "blitz.z64") == read(job.rom));
    CHECK(read(drop / "season2026.nbpak") == read(job.pak));
    CHECK(read(drop / "rules.json") == read(here / "rules.json"));  // the drop folder: the game takes them in itself
    CHECK(read(drop / "builder_done") == "1\n");
    CHECK(!fs::exists(app / "rules.json") && !fs::exists(app / "season.json"));
    CHECK(fs::exists(title / ".newblitz_manifest"));
    CHECK(!fs::exists(title / "eboot.bin.part"));

    // The console starts New Blitz: it takes the ROM, installs the pak, makes its settings (and its own Rules).
    fs::create_directories(app);
    fs::rename(drop / "blitz.z64", app / "blitz2000.n64.us.1.1.z64");
    write(app / "paks/season2026.nbpak", read(drop / "season2026.nbpak"));
    fs::remove(drop / "season2026.nbpak");
    for (const char* f : {"builder_done", "rules.json", "season.json"}) fs::remove(drop / f);
    write(app / "general.json", "{}");
    write(app / "rules.json", "{\"console\":\"rules\"}");
    write(app / "season.json", "{\"console\":\"season\"}");
    write(app / "blitz.log", "[log] Blitz2026Recompiled 0.9.5 started\nlots of lines\n");

    // 2. Nothing changed: nothing goes, and the console's own setup stays.
    printf("send 2: nothing changed, the console set up\n");
    r = send(job, progress, log);
    CHECK(r.stage == Result::Ok && r.set_up_already);
    CHECK(r.files_sent == 0);
    CHECK(read(app / "rules.json") == "{\"console\":\"rules\"}");
    CHECK(read(app / "season.json") == "{\"console\":\"season\"}");
    CHECK(!fs::exists(drop / "builder_done") && !fs::exists(drop / "rules.json") && !fs::exists(drop / "season.json"));
    CHECK(!fs::exists(drop / "blitz.z64"));
    CHECK(!fs::exists(drop / "season2026.nbpak"));

    // 3. A new eboot.bin and team logo of the same sizes, and a new pak: those three go.
    printf("send 3: same-size changes\n");
    write(kit / "eboot.bin", std::string(300000, 'F'));
    write(kit / "art/files/team1.wms", std::string(51542, 'z'));
    write(here / "paks/season2026.nbpak", std::string(120000, 'Q'));  // same size, other bytes
    r = send(job, progress, log);
    CHECK(r.files_sent == 3);
    CHECK(read(title / "eboot.bin") == std::string(300000, 'F'));
    CHECK(read(title / "art/files/team1.wms") == std::string(51542, 'z'));
    CHECK(read(drop / "season2026.nbpak") == std::string(120000, 'Q'));
    CHECK(read(app / "rules.json") == "{\"console\":\"rules\"}");

    // 4. A file changed on the console behind the manifest's back (another size): it goes again.
    printf("send 4: a file changed on the console\n");
    write(title / "sce_sys/param.json", "{}");
    r = send(job, progress, log);
    CHECK(r.files_sent == 1);
    CHECK(read(title / "sce_sys/param.json") == read(kit / "sce_sys/param.json"));

    // 5. No manifest (a console 0.9.x sent to): every app file goes once.
    printf("send 5: no manifest\n");
    fs::remove(title / ".newblitz_manifest");
    r = send(job, progress, log);
    CHECK(r.files_sent == 7);

    // 6. Send and Start, the game restarting: the launch payload arrives and the moved log is seen.
    printf("send 6: launch, the game starts again\n");
    std::string got;
    int lport = 0;
    std::thread t = loader(lport, app, true, got);
    job.launch_elf = here / "launch.elf";
    job.loader_port = lport;
    job.start_wait = 6;
    r = send(job, progress, log);
    t.join();
    CHECK(r.launched && got == read(job.launch_elf));
    CHECK(r.restarted == 1);

    // 7. The copy already running ignores the launch: its log doesn't move.
    printf("send 7: launch, the old copy stays\n");
    got.clear();
    t = loader(lport, app, false, got);
    job.loader_port = lport;
    job.start_wait = 4;
    r = send(job, progress, log);
    t.join();
    CHECK(r.launched && r.restarted == 0);

    // 8. No loader listening: on the PS5, start it from the home screen.
    printf("send 8: no loader\n");
    job.loader_port = 1;
    r = send(job, progress, log);
    CHECK(r.stage == Result::Ok && !r.launched);

    // 9. No FTP server: refused, at once.
    printf("send 9: no FTP server\n");
    Job none = job;
    none.ftp_port = 1;
    auto t0 = std::chrono::steady_clock::now();
    r = send(none, progress, log);
    CHECK(r.stage == Result::Refused);
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1));

    // The progress the screen showed, from a send with something in it (the whole app again).
    fs::remove(title / ".newblitz_manifest");
    job.launch_elf.clear();
    std::atomic<bool> running{true};
    std::string seen;
    std::thread watch([&] {
        while (running) {
            std::string t = progress.text();
            if (t.rfind("Sending", 0) == 0) seen = t;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });
    r = send(job, progress, log);
    running = false;
    watch.join();
    printf("progress seen: %s\n", seen.c_str());
    CHECK(seen.find(" of 7 files") != std::string::npos);
    CHECK(progress.text().empty());
}

int main(int argc, char** argv) {
    test_progress();
    test_manifest();
    if (argc >= 3) test_sends(atoi(argv[1]), argv[2]);
    if (!getenv("PS5_TEST_NO_NETWORK")) test_connect_timeout();
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
