// Commands both ways (docs/PSP_PORT.md §5.2-5.4).
//   host0: mailbox   the Mac (tools/psp/remote.sh --via host0) writes <host0>/cmd/<seq>.tmp and renames it to <seq>.txt; about once
//                    a second the PSP takes the lowest <seq>.txt, deletes it, runs each line and writes <host0>/res/<name>
//                    (tmp + rename; errors to res/error). No signing: it is a cable or the local emulator. <host0>/seen is
//                    rewritten every 5 s with the launch id and uptime.
//   Worker           only when <app>/data/net.cfg exists and the net is up: GET <logs_base>/cmd?pid=psp-<digits> every 10 s,
//                    every 2 s for 2 minutes after a command. A line is "<seq> <verb args> #<tag>", tag = the first 16 hex of
//                    HMAC-SHA256(cmd_key, "<seq>|<verb args>"); seq must be above the last one (kept in <app>/data/cmd_seq).
//                    Anything else is rejected and logged. Results go to POST <logs_base>/res/psp-<name>.
//   <app>/cmd.txt    read once at boot: its set lines only, then renamed to cmd.done.
// Verbs: ping, stat, log <name> [file], ls <name> [dir], get <name> <file>, shot <name>, set <key> <value>,
//        peek <name> <addr> <len>, exit; more with add_verb. File arguments are relative to the app folder, no "..".
// net.cfg values never reach a log: the keys are registered with psplog's redaction before anything else happens.
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

namespace pspremote {

struct NetCfg {
    bool present = false;
    std::string logs_base, hub_base, cmd_key, device = "psp1001";
    std::string home;           // "remote": every exit goes to NB Remote (PSP/GAME/NBREMOTE) instead of the XMB
    int wifi_profile = 1;
};
bool load_net_cfg(const std::string& path, NetCfg& out);
const NetCfg& net_cfg();

struct Options {
    std::string app_dir;
    std::string host0_dir;      // "host0:/blitz-psp" or a Mac folder; empty: no host0 mailbox
    bool worker = true;         // poll the Worker when net.cfg is there and the net is up
    int run_upload_first_s = 0; // >0: worker_upload_run() at this uptime, then every run_upload_every_s (the game: 60, 300)
    int run_upload_every_s = 0;
};
void init(const Options& opt);
void start();                   // the poll thread (priority 0x30)
void stop();

typedef bool (*VerbFn)(const std::vector<std::string>& args, std::string& out, void* ctx);
// named: the first argument is the result's name (as for log/ls/get/shot); otherwise the result is res/<verb>
void add_verb(const char* name, VerbFn fn, void* ctx, bool named);
void set_exit_hook(void (*fn)(void* ctx), void* ctx);
// Starts a New Blitz EBOOT in place of this one (sceKernelLoadExec): "" this game again, or "classic", "modern", "probe" (the
// kit's NBCLASSIC, NEWBLITZ, NBPROBE beside this app folder). Flushes the log first. Returns only if the load failed.
bool relaunch(const std::string& which);
// Leaves the game: with net.cfg's home=remote and PSP/GAME/NBREMOTE/EBOOT.PBP there, NB Remote is started in its place (so the
// PSP stays reachable over Wi-Fi), else back to the XMB. clean: a normal exit (the log shut, the running marker deleted);
// false keeps the marker, so the next launch files this run as dead. Does not return (on the Mac it exits).
[[noreturn]] void go_home(const char* why, bool clean = true);
void set_stat_hook(void (*fn)(std::string& out, void* ctx), void* ctx);
void set_peek(bool (*fn)(uint32_t addr, uint32_t len, std::string& out));
std::string setting(const std::string& key);   // what "set" stored (v1 stub: nothing reads these yet)

// One command line -> result name and body. ok=false puts the body in res/error instead.
void execute(const std::string& line, std::string& res_name, std::string& body, bool& ok);

// Worker client (net.cfg's logs_base)
bool worker_ready();
bool worker_upload(const char* kind, const std::string& body, std::string* reply_id, bool with_launch);
bool worker_upload_run();                // kind=run, launch=<id>: header line + 256 KB tail of blitz.log; replaces in place
bool worker_put_res(const std::string& name, const std::string& body);   // name without the psp- prefix
int worker_poll();                       // one cmd poll: lines run (>= 0) or -1
int upload_pending();                    // pending/*.log as kind=crash, each deleted after a 200
const std::string& worker_launch();      // Date minus uptime, fixed at the first HTTP reply ("" before)
std::string pid();                       // psp-<launch digits>
struct Counters { int polls, cmds, rejected, uploads, upload_fail, host0_cmds; };
Counters counters();

}  // namespace pspremote
