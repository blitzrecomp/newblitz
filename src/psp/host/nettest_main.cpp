// psp-nettest: the PSP layer's log, HTTP and command code built for the Mac (as psp-host will use it), run against
// tools/psp/mock_worker.py. It plays both ends of M2(a) (docs/PSP_PORT.md §10) and prints one PASS/FAIL line per check:
//   psp-nettest [--app DIR] [--host0 DIR] [--base http://127.0.0.1:8788] [--serve SECS]
// It writes <app>/data/net.cfg for the mock (logs_base=<base>/mockpsp, hub_base=<base>/mockhub, a test cmd_key), then checks:
// the hub manifest GET, a kind=probe upload, a run log uploaded twice under one launch id (the mock keeps one file), a signed
// ping queued through the read key and answered in res/psp-ping, a replayed and an unsigned line rejected, URL key redaction in
// blitz.log, and the host0 mailbox. With --serve it then keeps polling for SECS seconds (tools/psp/remote.sh against it).
#include "psp_http.h"
#include "psp_log.h"
#include "psp_net.h"
#include "psp_remote.h"
#include "psp_sha256.h"
#include "psp_sys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

static int g_fail = 0;
static void result(const char* name, bool ok, const std::string& detail = "") {
    printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name, detail.empty() ? "" : ": ", detail.c_str());
    fflush(stdout);
    if (!ok) g_fail++;
}

static std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

int main(int argc, char** argv) {
    std::string app = "build/psp/nettest/app", host0 = "build/psp/nettest/host0/blitz-psp", base = "http://127.0.0.1:8788";
    int serve = 0;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--app")) app = argv[i + 1];
        else if (!strcmp(argv[i], "--host0")) host0 = argv[i + 1];
        else if (!strcmp(argv[i], "--base")) base = argv[i + 1];
        else if (!strcmp(argv[i], "--serve")) serve = atoi(argv[i + 1]);
    }
    const std::string key = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";   // test key, mock only
    pspsys::make_dirs(app + "/data");
    pspsys::make_dirs(host0);
    std::string cfg = "logs_base=" + base + "/mockpsp\nhub_base=" + base + "/mockhub\ncmd_key=" + key + "\nwifi_profile=1\ndevice=psp1001\n";
    pspsys::write_file(app + "/data/net.cfg", cfg.data(), cfg.size());
    pspsys::remove_file(app + "/data/cmd_seq");

    psplog::Options lo;
    lo.app_dir = app;
    lo.host0_dir = host0;
    lo.build = BLITZ_BUILD;
    lo.commit = BLITZ_COMMIT;
    lo.flavour = "nettest";
    psplog::init(lo);
    pspremote::Options ro;
    ro.app_dir = app;
    ro.host0_dir = host0;
    ro.worker = false;   // polls by hand below; --serve turns the thread on
    pspremote::init(ro);
    pspnet::start(1);
    result("net up", pspnet::wait_up(2000), pspnet::state_name(pspnet::state()));

    psphttp::Response r;
    bool ok = psphttp::get(base + "/mockhub/manifest.json", r);
    result("GET manifest", ok && r.status == 200 && r.body.find("blitz-rosters/1") != std::string::npos,
           "status " + std::to_string(r.status) + ", " + std::to_string(r.body.size()) + " bytes, " + std::to_string(r.ms) + " ms, Date " + r.date);
    result("Date parsed", r.date_utc > 1700000000, std::to_string(r.date_utc) + " vs wall " + std::to_string(pspsys::wall_clock()));

    std::string id;
    ok = pspremote::worker_upload("probe", "PROBE v1 nettest\nPROBE done pass=1 partial=0 fail=0 skip=0 secs=0.1\n", &id, false);
    result("POST kind=probe", ok && id.find("_psp1001_probe_") != std::string::npos, id);

    ok = pspremote::worker_upload_run();
    std::string list1, list2;
    psphttp::Response l;
    psphttp::get(base + "/mockread/list", l);
    list1 = l.body;
    PLOG("nettest", "a second run upload, same launch");
    bool ok2 = pspremote::worker_upload_run();
    psphttp::get(base + "/mockread/list", l);
    list2 = l.body;
    std::string run_id = pspremote::worker_launch() + "_psp1001_run";
    auto count = [&](const std::string& s) { size_t n = 0, p = 0; while ((p = s.find(run_id, p)) != std::string::npos) { n++; p++; } return n; };
    result("run log replaces in place", ok && ok2 && !pspremote::worker_launch().empty() && count(list1) == 1 && count(list2) == 1,
           "launch " + pspremote::worker_launch() + " (local " + psplog::launch_id() + "), copies after 1st/2nd: " +
               std::to_string(count(list1)) + "/" + std::to_string(count(list2)));

    // the Mac's side: a signed ping for this pid, through the read key
    std::string pid = pspremote::pid();
    auto queue = [&](const std::string& line) {
        psphttp::Response q;
        psphttp::post(base + "/mockread/cmd?box=psp", "@" + pid + " " + line, q);
        return q.status == 200;
    };
    std::string tag = pspsha::command_tag(key, "1001", "ping");
    queue("1001 ping #" + tag);
    int n = pspremote::worker_poll();
    psphttp::Response pong;
    psphttp::get(base + "/mockread/res/psp-ping", pong);
    // the mock keeps the last result under each name, so it must be this launch's pong
    result("signed ping round trip", n == 1 && pong.status == 200 && pong.body.compare(0, 5 + psplog::launch_id().size(), "pong " + psplog::launch_id()) == 0,
           trim(pong.body));

    pspremote::Counters before = pspremote::counters();
    queue("1001 ping #" + tag);                 // replayed
    queue("ping");                              // unsigned
    queue("1002 ping #0000000000000000");       // bad tag
    n = pspremote::worker_poll();
    pspremote::Counters after = pspremote::counters();
    result("replayed / unsigned / bad tag rejected", n == 0 && after.rejected - before.rejected == 3,
           std::to_string(after.rejected - before.rejected) + " rejected");

    std::string stag = pspsha::command_tag(key, "1003", "stat");
    queue("1003 stat #" + stag);
    n = pspremote::worker_poll();
    psphttp::Response st;
    psphttp::get(base + "/mockread/res/psp-stat", st);
    result("signed stat", n == 1 && st.body.find("launch " + psplog::launch_id() + " ") == 0, std::to_string(st.body.size()) + " bytes");

    // SHA-256 known answer (FIPS 180-4 "abc") and HMAC (RFC 4231 case 2)
    uint8_t h[32];
    pspsha::sha256("abc", 3, h);
    result("sha256 abc", pspsha::hex(h, 32) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    pspsha::hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, h);
    result("hmac rfc4231#2", pspsha::hex(h, 32) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    // redaction: the key never reaches blitz.log
    PLOG("nettest", "a URL with a key: %s/up?kind=x", (base + "/" + std::string(32, 'a')).c_str());
    psplog::flush();
    std::string logtext;
    pspsys::read_file(app + "/logs/blitz.log", logtext);
    result("keys redacted in blitz.log", logtext.find(key) == std::string::npos && logtext.find(std::string(32, 'a')) == std::string::npos &&
                                             logtext.find("/<KEY>/up") != std::string::npos);

    // host0 mailbox, both halves here
    pspsys::make_dir(host0 + "/cmd");
    pspsys::remove_file(host0 + "/res/ping");   // a result left by an earlier run must not pass for this one
    std::string c = "ping\n";
    pspsys::write_file(host0 + "/cmd/1.txt", c.data(), c.size());
    pspremote::start();
    std::string res;
    for (int i = 0; i < 40 && !pspsys::read_file(host0 + "/res/ping", res); i++) pspsys::sleep_ms(100);
    result("host0 cmd ping", res.compare(0, 5 + psplog::launch_id().size(), "pong " + psplog::launch_id()) == 0, trim(res));

    // net.cfg (its keys) can't be fetched by a command, whatever the channel
    std::string rn, rb;
    bool rok = true;
    pspremote::execute("get x data/net.cfg", rn, rb, rok);
    bool blocked = !rok && rb.find("cmd_key") == std::string::npos;
    pspremote::execute("log x data/NET.CFG", rn, rb, rok);
    blocked = blocked && !rok && rb.find("cmd_key") == std::string::npos;
    result("net.cfg refused to get/log", blocked, trim(rb));

    if (serve > 0) {
        printf("serving %d s: host0 %s, worker pid %s\n", serve, host0.c_str(), pid.c_str());
        fflush(stdout);
        uint64_t until = pspsys::now_us() + (uint64_t)serve * 1000000ull;
        while (pspsys::now_us() < until) { pspremote::worker_poll(); pspsys::sleep_ms(1000); }
    }
    pspremote::stop();
    psplog::shutdown("nettest done");
    printf("%s: %d failed\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}
