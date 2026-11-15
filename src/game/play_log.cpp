// <app folder>/plays.log: one line per play (both apps). The first piece of per-play autosave: after a
// crash it shows where the game was. Written from a hook where every play-outcome path of the game loop
// joins (ovl_a 0x80301E0C; vanilla ovl_a_func_802FEF08, team+ tp_game_process), so the state already
// describes the next play while "previous snap" and "end reason" still describe the one just run.
// Addresses are the same in both ROMs except the team abbreviations (build/analysis/playlog, NOTES.md).
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#if !defined(_WIN32) && !defined(__PROSPERO__)
#include <unistd.h>
#endif

#include "recomp.h"
#include "util/file.h"

namespace {
    constexpr uint32_t phase = 0x802EA420;     // 2 = playing
    constexpr uint32_t quarter = 0x802EA424;   // 0-3 Q1-Q4, 4-6 OT1-OT3
    constexpr uint32_t game_clock = 0x802EA428;    // minutes << 16 | seconds << 8 | tick
    constexpr uint32_t team_slot = 0x802EA434; // [2]
    constexpr uint32_t points = 0x802EA43C;    // [2][7] per quarter
    constexpr uint32_t offense = 0x802EA47C;   // side with the ball
    constexpr uint32_t spot = 0x802EA48C;      // yards from the offense's own goal line
    constexpr uint32_t prev_spot = 0x802EA490; // snap spot of the play just run
    constexpr uint32_t down = 0x802EA498;
    constexpr uint32_t marker = 0x802EA49C;    // first-down marker (100 = goal to go)
    constexpr uint32_t play_kind = 0x802EA508; // 0 scrimmage, 1 kickoff, 4 extra point
    constexpr uint32_t play_flags = 0x802EA510;// 0x40 = pass
    constexpr uint32_t end_reason = 0x802EA514;
#if BLITZ_TEAMPLUS
    constexpr uint32_t abbr[2] = {0x802E9448, 0x802E94AC};
#else
    constexpr uint32_t abbr[2] = {0x802E9458, 0x802E94BC};
#endif
    constexpr std::uintmax_t log_cap = 1024 * 1024;

    int32_t rd(uint8_t* rdram, uint32_t a) { return MEM_W(0, (gpr)(int32_t)a); }

    std::string text(uint8_t* rdram, uint32_t a, int n) {
        std::string s;
        for (int i = 0; i < n; i++) {
            char c = (char)MEM_B(0, (gpr)(int32_t)(a + i));
            if (c == 0) break;
            s += c;
        }
        return s;
    }

    int total(uint8_t* rdram, int side) {
        int t = 0;
        for (int q = 0; q < 7; q++) t += rd(rdram, points + side * 0x1C + q * 4);
        return t;
    }

    std::string last_game;  // teams of the game being logged
    int last_offense = -1;  // side that had the ball for the play just logged
}

extern "C" void blitz_redo_play_ended(uint8_t* rdram);  // savestate.cpp
extern "C" void blitz_capture_event(const char* fields);  // capture.cpp: BLITZ_CAPTURE_EVENTS
extern "C" void blitz_ds_play(const char* result, const char* situation, int offense_side, const char* team, int spot, int kind, int quarter, int clock_seconds, int turnover, int has_gain, int gain);  // ds_view.cpp

// BLITZ DS, at play select: the score and the next snap, straight from the game. Laid out as ds_view.cpp's LiveState.
namespace {
    struct DsLiveState {
        char team[2][8];
        int points[2];
        int quarter, clock_seconds, offense;
        char clock[24], situation[48];
        int spot, marker, kind;
        int valid;
    };
}

extern "C" void blitz_ds_read_state(uint8_t* rdram, void* out_ptr) {
    DsLiveState& out = *static_cast<DsLiveState*>(out_ptr);
    out.valid = 0;
    if (rd(rdram, phase) != 2) return;  // not a game being played
    static const char* quarters[] = {"Q1", "Q2", "Q3", "Q4", "OT1", "OT2", "OT3"};
    static const char* downs[] = {"?", "1st", "2nd", "3rd", "4th"};
    for (int i = 0; i < 2; i++) {
        snprintf(out.team[i], sizeof(out.team[i]), "%s", text(rdram, abbr[i], 4).c_str());
        out.points[i] = total(rdram, i);
    }
    int q = rd(rdram, quarter), c = rd(rdram, game_clock), d = rd(rdram, down), los = rd(rdram, spot), mk = rd(rdram, marker), kind = rd(rdram, play_kind);
    out.quarter = q;
    out.clock_seconds = ((c >> 16) & 0xFF) * 60 + ((c >> 8) & 0xFF);
    out.offense = rd(rdram, offense) & 1;
    snprintf(out.clock, sizeof(out.clock), "%s %d:%02d", quarters[q >= 0 && q <= 6 ? q : 0], (c >> 16) & 0xFF, (c >> 8) & 0xFF);
    std::string situation = kind == 1 ? "kickoff" : kind == 4 ? "extra point" :
        std::string(downs[d >= 1 && d <= 4 ? d : 0]) + " & " + (mk >= 100 ? "goal" : std::to_string(mk - los)) +
        (los > 50 ? " at opp " + std::to_string(100 - los) : " at own " + std::to_string(los));
    snprintf(out.situation, sizeof(out.situation), "%s", situation.c_str());
    out.spot = los;
    out.marker = mk;
    out.kind = kind;
    out.valid = 1;
}

namespace blitz { uint8_t* monitor_rdram(); }  // dev_monitor.cpp

// For crash reports: which game this was, and where the chosen teams' records point.
// The image loader (func_80014B54) reads its palette from its loaded file, at the buffer plus the image's size. A buffer
// that isn't in RAM means the file didn't load: say which, since the read that follows brings the game down.
// The game's file open (func_80001B64) is null for an id outside 0x0A000001..the table's count, or with no free handle.
namespace {
    thread_local uint32_t loading_id = 0, loading_caller = 0;
}
extern "C" void blitz_loader_enter(uint32_t id, uint32_t caller) {
    loading_id = id;
    loading_caller = caller;
}
extern "C" void blitz_board_loaded(uint32_t id, uint32_t pixels);  // board_digits.cpp
extern "C" void blitz_board_image(uint32_t pixels) { blitz_board_loaded(loading_id, pixels); }
extern "C" void blitz_loader_opened(uint8_t* rdram, uint32_t file) {
    if (file != 0) return;
    uint32_t toc = (uint32_t)rd(rdram, 0x8005BD88u);
    uint32_t count = toc >= 0x80000000u && toc < 0x80800000u ? (uint32_t)rd(rdram, toc + 0xC) : 0;
    fprintf(stderr, "[loader] file %08X won't open (index %d of %u, called from %08X)\n", loading_id, (int)(loading_id - 0x0A000001u), count, loading_caller);
    for (uint32_t table : {0x80720E80u, 0x80721380u}) {
        fprintf(stderr, "[loader] %08X:", table);
        for (uint32_t i = 0; i < 10; i++) fprintf(stderr, " %08X", (uint32_t)rd(rdram, table + i * 4));
        fprintf(stderr, "\n");
    }
    fflush(stderr);
}

extern "C" void blitz_crash_details(int fd) {
#if defined(_WIN32) || defined(__PROSPERO__)
    (void)fd;
#else
    uint8_t* rdram = blitz::monitor_rdram();
    if (rdram == nullptr) {
        dprintf(fd, "[crash] no game memory yet\n");
        return;
    }
    int t0 = rd(rdram, team_slot), t1 = rd(rdram, team_slot + 4);
    dprintf(fd, "[crash] phase %d quarter %d teams %d %d\n", rd(rdram, phase), rd(rdram, quarter), t0, t1);
    for (int t : {t0, t1}) {
        if (t >= 0 && t < 64) dprintf(fd, "[crash] team %d: roster pointer %08X\n", t, (uint32_t)rd(rdram, 0x80720000u + (uint32_t)t * 0x64 + 0x58));
    }
    dprintf(fd, "[crash] team 0: %08X %08X, team 31: %08X\n", (uint32_t)rd(rdram, 0x80720000u), (uint32_t)rd(rdram, 0x80720058u), (uint32_t)rd(rdram, 0x80720000u + 31 * 0x64 + 0x58));
#endif
}

extern "C" void blitz_play_ended(uint8_t* rdram) {
    blitz_redo_play_ended(rdram);
    if (rd(rdram, phase) != 2) return;  // the attract demo runs the same loop
    std::string team[2] = {text(rdram, abbr[0], 4), text(rdram, abbr[1], 4)};
    int q = rd(rdram, quarter), c = rd(rdram, game_clock);
    int off = rd(rdram, offense) & 1, d = rd(rdram, down), los = rd(rdram, spot), mk = rd(rdram, marker);
    int prev = rd(rdram, prev_spot), why = rd(rdram, end_reason), kind = rd(rdram, play_kind);
    bool pass = (rd(rdram, play_flags) & 0x40) != 0;

    std::filesystem::path path = recompui::file::get_app_folder_path() / "plays.log";
    std::error_code ec;
    if (std::filesystem::file_size(path, ec) > log_cap) {
        std::filesystem::rename(path, recompui::file::get_app_folder_path() / "plays.prev.log", ec);
    }
    FILE* f = fopen(path.string().c_str(), "a");
    if (f == nullptr) return;
    std::string game = team[0] + " vs " + team[1] + " (slots " + std::to_string(rd(rdram, team_slot)) + ", " +
                       std::to_string(rd(rdram, team_slot + 4)) + ")";
    if (game != last_game) {
        last_offense = -1;
        time_t now = time(nullptr);
        char when[32];
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", localtime(&now));
        fprintf(f, "--- %s  %s\n", when, game.c_str());
        last_game = game;
    }
    static const char* quarters[] = {"Q1", "Q2", "Q3", "Q4", "OT1", "OT2", "OT3"};
    static const char* downs[] = {"?", "1st", "2nd", "3rd", "4th"};
    std::string result;
    // The spot is measured from the offense's own goal line, so a gain only means something when the
    // same side still has the ball; otherwise the play was a turnover, punt or turnover on downs.
    bool turnover = last_offense >= 0 && off != last_offense && kind == 0;
    last_offense = off;
    switch (why) {
        case 0: case 6: {
            if (turnover) {
                result = "change of possession";
                break;
            }
            int gain = los - prev;
            result = gain == 0 ? "no gain" : std::to_string(gain > 0 ? gain : -gain) + " yd " + (pass ? "pass" : "run") + (gain < 0 ? " for a loss" : "");
            break;
        }
        case 4: result = "return"; break;
        case 5: result = "incomplete"; break;
        case 15: result = "touchback"; break;
        default: result = "score or special (" + std::to_string(why) + ")"; break;
    }
    std::string situation = kind == 1 ? "kickoff" : kind == 4 ? "extra point" :
        std::string(downs[d >= 1 && d <= 4 ? d : 0]) + " & " + (mk >= 100 ? "goal" : std::to_string(mk - los)) +
        (los > 50 ? " at opp " + std::to_string(100 - los) : " at own " + std::to_string(los));
    fprintf(f, "%s %d:%02d  %s %d - %d %s  last: %-24s  next: %s ball, %s\n",
            quarters[q >= 0 && q <= 6 ? q : 0], (c >> 16) & 0xFF, (c >> 8) & 0xFF,
            team[0].c_str(), total(rdram, 0), total(rdram, 1), team[1].c_str(),
            result.c_str(), team[off].c_str(), situation.c_str());
    fclose(f);
    {
        // Trailer footage (tools/capture/cut.py): the play's end, with what made it worth a clip.
        static int prev_total[2] = {-1, -1};
        int now_total[2] = {total(rdram, 0), total(rdram, 1)};
        int scored = prev_total[0] < 0 ? 0 : (now_total[0] - prev_total[0]) + (now_total[1] - prev_total[1]);
        if (scored < 0) scored = 0;  // a new game
        prev_total[0] = now_total[0];
        prev_total[1] = now_total[1];
        char ev[384];
        snprintf(ev, sizeof(ev),
                 "\"kind\": \"play\", \"result\": \"%s\", \"gain\": %d, \"pass\": %d, \"turnover\": %d, \"scored\": %d, "
                 "\"play_kind\": %d, \"end_reason\": %d, \"quarter\": \"%s\", \"clock\": \"%d:%02d\", \"score\": \"%s %d - %d %s\"",
                 result.c_str(), (why == 0 || why == 6) && !turnover ? los - prev : 0, pass ? 1 : 0, turnover ? 1 : 0, scored,
                 kind, why, quarters[q >= 0 && q <= 6 ? q : 0], (c >> 16) & 0xFF, (c >> 8) & 0xFF,
                 team[0].c_str(), now_total[0], now_total[1], team[1].c_str());
        blitz_capture_event(ev);
    }
    // BLITZ DS's drive summary (ds_view.cpp).
    blitz_ds_play(result.c_str(), situation.c_str(), off, team[off].c_str(), los, kind, q, ((c >> 16) & 0xFF) * 60 + ((c >> 8) & 0xFF), turnover ? 1 : 0, why == 0 || why == 6 ? 1 : 0, los - prev);
}
