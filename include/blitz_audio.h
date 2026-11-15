#pragma once
// Host audio output (src/main/audio.cpp) and what the emulated N64 audio interface needs from it
// (src/game/ai.cpp).
#include <cstddef>
#include <cstdint>

namespace blitz::audio {
    bool reset(uint32_t output_freq);
    void set_frequency(uint32_t freq);
    void queue_samples(int16_t* audio_data, size_t sample_count);
    size_t get_frames_remaining();

    // Silence, with everything else going on as usual (Instant Replay, while the game's stopped).
    void set_mute(bool mute);

    // Game-rate frames queued on the host and not yet played.
    uint32_t pending_frames();
    // Frames in the last buffer the game handed to osAiSetNextBuffer.
    uint32_t last_buffer_frames();
    // The game's sample rate (osAiSetFrequency).
    uint32_t game_rate();
}
