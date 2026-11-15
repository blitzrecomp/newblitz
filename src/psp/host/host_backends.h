// What psp_main.cpp needs from the platform layer. psp-host: host_backends.cpp; the PSP: psp_threads.cpp, psp_mem.cpp,
// psp_rom.cpp, psp_input.cpp, psp_overlay.cpp and psp_stdio.cpp (§2). Both: backends_common.cpp (null renderer, silent
// audio, script input).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ultramodern/renderer_context.hpp"
#include "librecomp/rsp.hpp"

namespace host {
    // Stack sizes (docs/PSP_PORT.md §4.1). Game threads: the fixed ones (scheduler, audio, ...) and the game's processes.
    struct StackSizes {
        uint32_t runtime = 64 * 1024;
        uint32_t game_start = 256 * 1024;
        uint32_t game_fixed = 64 * 1024;
        uint32_t game_process = 32 * 1024;
    };
    void set_stack_sizes(const StackSizes& sizes);
    const StackSizes& stack_sizes();
    bool is_fixed_game_thread(int id, int pri);

    struct ThreadCounts { int created, alive, max_alive, alive_game, max_alive_game; };
    ThreadCounts thread_counts();
    void stack_report(bool all);   // [stack] lines: every thread (all) or those over 50%

    uint8_t* rdram();              // null before recomp::start allocates it

    // stalls: misses that took over 20 ms; stall_ms: their total; waits: reads that waited for another thread's load
    struct RomStats { uint64_t hits, misses; double worst_ms; uint64_t stalls = 0; double stall_ms = 0; uint64_t waits = 0; };
    bool open_rom(const std::string& path, uint64_t expected_xxh3);
    RomStats rom_stats();

    // display_lists: tasks sent (ticks); frames: update_screen calls (VIs); shown: frames presented (GE renderer);
    // skipped: lists not drawn (presentation skip)
    struct GfxStats { uint64_t display_lists, frames, dummy, shown = 0, skipped = 0; };
    std::unique_ptr<ultramodern::renderer::RendererContext> create_null_renderer(uint8_t* rdram, ultramodern::renderer::WindowHandle, bool);
    GfxStats gfx_stats();
    void gfx_count(int display_lists, int frames, int shown, int skipped, int dummy);   // the renderers' counters
    bool gfx_mode_ge();            // the GE renderer is up (else the null renderer)
    void set_gfx_mode_ge(bool on);

    struct AudioStats { uint64_t frames, dropped; uint32_t rate; };
    AudioStats audio_stats();
    AudioStats silent_audio_stats();   // backends_common.cpp's silent sink (audio v0)
    // psp_audio.cpp (audio v1): the [perf] fields (audio_ms=<avg per task>(cpu|off) ...) since the last call
    std::string audio_perf_fields(double dt);
    bool audio_v1();                   // n_aspMain runs and the output is on (no noaudio switch)
    void audio_init(bool noaudio);     // psp_audio.cpp: before recomp::start
    RspUcodeFunc* audio_microcode();   // the audio task's microcode: n_aspMain (timed), or v0's "done at once"
    uint32_t pending_frames_now();     // frames queued for the output (ring + the block playing)

    int load_input_script(const std::string& path);
    bool add_input_line(const std::string& line, double at = -1);  // at -1: "[+secs|secs] <what> [hold]" from now (live)
    bool script_input(int controller, uint16_t* buttons, float* x, float* y);   // the script alone
    bool get_input(int controller, uint16_t* buttons, float* x, float* y);      // the platform's: pad + script

    int capture_output();          // fds 1 and 2 into psp_log; returns a dup of the original stdout for the stdout sink

#if defined(__PSP__)
    // psp_threads.cpp
    void init_threads(int pool_slots, uint32_t pool_slot_bytes);  // the game processes' pool, made at boot
    std::string thread_dump();     // every kernel thread: name, status, priority, wait, stack use (the HANG block, stat)
    int suspend_thread_named(const char* name);   // PSP: suspends every thread with that kernel name; how many (the HANG path)
    std::string gfx_breadcrumb();                 // the renderer's stage, how long it's been in it, the walker's position
    struct PoolStats { int slots, busy, max_busy, fallbacks; uint32_t slot_bytes, worst_used; };
    PoolStats pool_stats();
    // psp_mem.cpp
    struct MemStats { int free_kb, max_kb, heap_kb, heap_used_kb; bool guards_ok; uint32_t guard_low_bad, guard_high_bad; };
    MemStats mem_stats();          // also checks the RDRAM guards
    bool peek(uint32_t addr, uint32_t len, std::string& out);
    // psp_input.cpp
    void init_input();
    // psp_overlay.cpp: the pspDebugScreen text (§4.5 v0)
    void start_overlay(const std::string& title);
    std::string overlay_status();  // the status block it shows (also for stat)
    // overlay modes over the GE's frame (psp_overlay.cpp; the null renderer always shows the full debug screen)
    enum { OVERLAY_OFF = 0, OVERLAY_HUD = 1, OVERLAY_LOG = 2 };
    int overlay_mode();
    void set_overlay_mode(int mode);
    const char* overlay_mode_name(int mode);
    int overlay_mode_from(const std::string& text);   // -1 if it isn't one
    void overlay_use_ge(bool on);  // the GE renderer draws the overlay into its frames; the overlay thread stops drawing
    struct OverlayLine { std::string text; uint32_t color; };
    std::vector<OverlayLine> overlay_log_lines(int max);   // the newest log lines, as the overlay shows them
    // psp_gfx.cpp: the GE renderer (§4.5 M7)
    enum { PRESENT_30 = 0, PRESENT_AUTO = 1, PRESENT_20 = 2 };
    std::unique_ptr<ultramodern::renderer::RendererContext> create_psp_renderer(uint8_t* rdram, ultramodern::renderer::WindowHandle, bool);
    int present_mode();
    void set_present_mode(int mode);
    const char* present_mode_name(int mode);
    int present_mode_from(const std::string& text);   // -1 if it isn't one
    std::string gfx_perf_fields(double dt);            // the renderer's part of [perf] since the last call
    // psp_threads.cpp: the stack that is fullest (from the marks already taken: cheap)
    int stack_high(std::string& name);                 // percent
    // psp_mem.cpp: the heap's real high-water (newlib's most memory ever taken from sbrk)
    int heap_high_kb();
    // psp_input.cpp: synthetic pad presses (PSP buttons, before the map; the `pad` command)
    bool add_pad_line(const std::string& line, std::string& err);
    // psp_stdio.cpp: newlib's stdout and stderr into the log ring
    // psp_main.cpp
    uint64_t audio_tasks();
#endif

    // psp_main.cpp
    uint64_t vi_count();
    double game_seconds();         // since the game started (VIs / 60 in lockstep)
    bool lockstep();
}

RspExitReason host_silent_aspMain(uint8_t* rdram, uint32_t ucode_addr);
namespace host::silent {
    bool reset(uint32_t output_freq);
    void set_frequency(uint32_t freq);
    void queue_samples(int16_t* audio_data, size_t sample_count);
    size_t get_frames_remaining();
    void set_mute(bool mute);
    uint32_t pending_frames();
    uint32_t last_buffer_frames();
    uint32_t game_rate();
}
