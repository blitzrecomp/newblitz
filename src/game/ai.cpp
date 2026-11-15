// The N64 audio interface as the game sees it. Blitz's audio thread (0x8000B218) paces itself the
// way libmus does on hardware: each retrace it skips its turn while the AI's two-buffer DMA FIFO is
// full, and otherwise sizes the next buffer from osAiGetLength (552 samples, or 368/736 to correct
// drift). The runtime's osAiGetStatus always reports "not full", so the game rendered a 552-sample
// buffer every retrace: 1.5x real time (see NOTES "Audio").
//
// Model: the host queue stands in for the FIFO. The newest buffer is "next" and whatever is queued
// beyond it is the buffer "playing", so the FIFO is full while more than the newest buffer is
// still queued. A fixed margin of one retrace's worth of samples stays queued on top of that, so
// host thread jitter can't starve the device.
#include "recomp.h"

#include "blitz_audio.h"

namespace {
    constexpr uint32_t AI_STATUS_FIFO_FULL = 0x80000000;
    constexpr uint32_t AI_STATUS_DMA_BUSY = 0x40000000;

    // Game-rate frames the emulated FIFO holds: the host queue minus the safety margin.
    uint32_t fifo_frames() {
        uint32_t pending = blitz::audio::pending_frames();
        uint32_t margin = blitz::audio::game_rate() / 60;
        return pending > margin ? pending - margin : 0;
    }
}

// u32 osAiGetStatus(void)
extern "C" void blitz_osAiGetStatus(uint8_t* rdram, recomp_context* ctx) {
    uint32_t queued = fifo_frames();
    uint32_t status = 0;
    if (queued > blitz::audio::last_buffer_frames()) {
        status = AI_STATUS_FIFO_FULL | AI_STATUS_DMA_BUSY;
    }
    else if (queued > 0) {
        status = AI_STATUS_DMA_BUSY;
    }
    ctx->r2 = (gpr)(int32_t)status;
}

// u32 osAiGetLength(void): bytes left in the buffer playing now (4 bytes per stereo frame).
extern "C" void blitz_osAiGetLength(uint8_t* rdram, recomp_context* ctx) {
    uint32_t queued = fifo_frames();
    uint32_t next = blitz::audio::last_buffer_frames();
    uint32_t playing = queued > next ? queued - next : queued;
    ctx->r2 = (gpr)(int32_t)(playing * 4);
}
