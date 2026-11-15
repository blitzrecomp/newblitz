// Host audio output. The game's 16-bit stereo buffers (osAiSetNextBuffer, 22050 Hz) are converted
// to float and fed through one persistent SDL_AudioStream to the device rate, then queued on an
// SDL audio device. A persistent stream keeps the resampler's filter state and time base across
// buffers, so buffer boundaries don't click. The game's own rate control (src/game/ai.cpp) keeps
// the queue short; dropping whole buffers above max_queued_ms is only a safety valve.
// If the device goes away (headphones unplugged, AirPods disconnected), the next buffer finds it
// stopped and the current default device is opened instead, retried once a second until one opens;
// the game keeps running silently meanwhile. BLITZ_AUDIO_DROP_TEST=<s> closes the device after s
// seconds to exercise that path.
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#define SDL_MAIN_HANDLED
#include "SDL2/SDL.h"

#include "recompui/config.h"
#include "recompui/recompui.h"

#include "blitz_audio.h"

namespace {
    constexpr uint32_t channels = 2;
    constexpr uint32_t output_bytes_per_frame = channels * sizeof(float);
    constexpr uint32_t max_queued_ms = 200;

    std::mutex audio_mutex;
    SDL_AudioDeviceID device = 0;
    SDL_AudioStream* stream = nullptr;
    std::atomic<uint32_t> sample_rate{48000};
    std::atomic<uint32_t> output_sample_rate{48000};
    std::atomic<uint32_t> last_frames{0};
    std::atomic_bool host_mute{false};
    uint32_t dropped_buffers = 0;
    // BLITZ_AUDIO_DUMP=<file>: also write everything queued on the device there (raw float32 stereo
    // at the device rate) for offline checks.
    FILE* dump_file = nullptr;
    uint32_t last_reopen_try = 0;  // SDL ticks

    void rebuild_stream();

    // With audio_mutex held.
    bool open_device(uint32_t output_freq) {
        SDL_AudioSpec desired{};
        desired.freq = (int)output_freq;
        desired.format = AUDIO_F32;
        desired.channels = channels;
        desired.samples = 0x100;
        SDL_AudioSpec obtained{};
        device = SDL_OpenAudioDevice(nullptr, false, &desired, &obtained, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
        if (device == 0) {
            return false;
        }
        output_sample_rate = (uint32_t)obtained.freq;
        SDL_PauseAudioDevice(device, 0);
        return true;
    }

    // With audio_mutex held, before each buffer: reopen a lost device.
    void check_device() {
        static const uint32_t drop_test_ms = [] {
            const char* e = getenv("BLITZ_AUDIO_DROP_TEST");
            return e != nullptr ? (uint32_t)(atof(e) * 1000) : 0u;
        }();
        static bool drop_tested = false;
        if (drop_test_ms != 0 && !drop_tested && SDL_GetTicks() > drop_test_ms) {
            drop_tested = true;
            fprintf(stderr, "[audio] drop test: closing the device\n");
            SDL_CloseAudioDevice(device);  // its status reads as stopped from now on, like a lost device
        }
        if (device != 0 && SDL_GetAudioDeviceStatus(device) != SDL_AUDIO_STOPPED) {
            return;
        }
        uint32_t now = SDL_GetTicks();
        if (now - last_reopen_try < 1000) {
            return;
        }
        last_reopen_try = now;
        if (device != 0) {
            fprintf(stderr, "[audio] output device lost; reopening the default device\n");
            SDL_CloseAudioDevice(device);
            device = 0;
        }
        uint32_t old_rate = output_sample_rate;
        if (open_device(old_rate)) {
            if (output_sample_rate != old_rate) rebuild_stream();
            fprintf(stderr, "[audio] output reopened at %u Hz\n", output_sample_rate.load());
        }
    }

    void rebuild_stream() {
        if (stream != nullptr) {
            SDL_FreeAudioStream(stream);
        }
        stream = SDL_NewAudioStream(AUDIO_F32, channels, sample_rate, AUDIO_F32, channels, output_sample_rate);
        if (stream == nullptr) {
            fprintf(stderr, "Error creating SDL audio stream: %s\n", SDL_GetError());
            throw std::runtime_error("Error creating SDL audio stream");
        }
    }

    uint32_t queued_output_frames() {
        return device != 0 ? SDL_GetQueuedAudioSize(device) / output_bytes_per_frame : 0;
    }
}

namespace blitz::audio {
    void queue_samples(int16_t* audio_data, size_t sample_count) {
        static std::vector<float> input;
        static std::vector<uint8_t> output;
        std::lock_guard lock{audio_mutex};
        last_frames = (uint32_t)(sample_count / channels);
        check_device();
        if (device == 0) {
            return;
        }

        if (queued_output_frames() * 1000ull / output_sample_rate > max_queued_ms) {
            if (dropped_buffers++ % 60 == 0) {
                fprintf(stderr, "[audio] host queue over %u ms, dropping buffers (%u so far)\n", max_queued_ms, dropped_buffers);
            }
            return;
        }

        // RDRAM's word byteswap also swaps each pair of 16-bit samples, so right comes first.
        // BLITZ_MUTE=1 (test runs; tools/live.sh sets it unless BLITZ_LIVE_SOUND=1): silent, everything else as usual.
        static const bool muted = getenv("BLITZ_MUTE") != nullptr && getenv("BLITZ_MUTE")[0] == '1';
        float volume = (muted || host_mute.load()) ? 0.0f : static_cast<float>(recompui::config::sound::get_main_volume()) / 100.0f;
        input.resize(sample_count);
        for (size_t i = 0; i + 1 < sample_count; i += channels) {
            input[i + 0] = audio_data[i + 1] * (0.5f / 32768.0f) * volume;
            input[i + 1] = audio_data[i + 0] * (0.5f / 32768.0f) * volume;
        }
        if (SDL_AudioStreamPut(stream, input.data(), (int)(input.size() * sizeof(float))) < 0) {
            fprintf(stderr, "[audio] SDL_AudioStreamPut: %s\n", SDL_GetError());
            return;
        }
        int available = SDL_AudioStreamAvailable(stream);
        if (available <= 0) {
            return;
        }
        output.resize(available);
        int got = SDL_AudioStreamGet(stream, output.data(), available);
        if (got > 0) {
            SDL_QueueAudio(device, output.data(), got);
            if (dump_file != nullptr) {
                fwrite(output.data(), 1, got, dump_file);
            }
        }
    }

    void set_mute(bool mute) {
        host_mute = mute;
    }

    uint32_t pending_frames() {
        return (uint32_t)((uint64_t)queued_output_frames() * sample_rate / output_sample_rate);
    }

    uint32_t last_buffer_frames() {
        return last_frames;
    }

    uint32_t game_rate() {
        return sample_rate;
    }

    // Runtime callback behind its own osAiGetLength; the game's osAiGetLength is src/game/ai.cpp.
    size_t get_frames_remaining() {
        return pending_frames();
    }

    void set_frequency(uint32_t freq) {
        std::lock_guard lock{audio_mutex};
        sample_rate = freq;
        rebuild_stream();
    }

    bool reset(uint32_t output_freq) {
        std::lock_guard lock{audio_mutex};
        if (!open_device(output_freq)) {
            std::string audio_error = std::string("No audio device could be found.\nError opening audio device: ") + SDL_GetError();
            recompui::message_box(audio_error.c_str());
            return false;
        }
        rebuild_stream();
        if (const char* path = getenv("BLITZ_AUDIO_DUMP")) {
            dump_file = fopen(path, "wb");
            fprintf(stderr, "[audio] dumping output (f32 stereo %u Hz) to %s\n", output_sample_rate.load(), path);
        }
        return true;
    }
}
