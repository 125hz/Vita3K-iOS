// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include "audio/impl/sdl_audio.h"
#include "util/log.h"
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_hints.h>

#include <algorithm>
#include <cstdint>

#define SDL_CHECK_EXT(condition, ret)                         \
    do {                                                      \
        if (!(condition)) {                                   \
            LOG_ERROR("SDL audio error: {}", SDL_GetError()); \
            return ret;                                       \
        }                                                     \
    } while (0)

#define SDL_CHECK(f_call) SDL_CHECK_EXT(f_call, {})
#define SDL_CHECK_VOID(f_call) SDL_CHECK_EXT(f_call, )
#define SDL_CHECK_NEG(f_call) SDL_CHECK_EXT((f_call) >= 0, {})

static int get_threshold_samples(const int device_buffer_samples) {
    return 4 * device_buffer_samples;
}

void SDLCALL SDLAudioAdapter::thread_wakeup_callback(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount) {
    assert(userdata != nullptr);
    assert(stream != nullptr);
    SDLAudioOutPort *port = static_cast<SDLAudioOutPort *>(userdata);
    port->adapter.state.device_pulls.fetch_add(1, std::memory_order_relaxed);
    const int samples_available = port->adapter.get_rest_sample(*port);
    if (samples_available < get_threshold_samples(port->adapter.device_buffer_samples) || additional_amount > 0) {
        port->cond_var.notify_one();
    }
}

SDLAudioAdapter::SDLAudioAdapter(AudioState &audio_state)
    : AudioAdapter(audio_state) {}

SDLAudioAdapter::~SDLAudioAdapter() {
    if (device_id > 0) {
        SDL_CloseAudioDevice(device_id);
    }
}

bool SDLAudioAdapter::init() {
    // A 512-frame callback is low latency on desktop, but device traces show
    // iOS underrunning it while JIT and MoltenVK share the CPU. Match the iOS
    // audio-session preference (about 21 ms) for stable playback.
#if defined(VITA3K_PLATFORM_IOS)
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "1024");
    // Opening with no spec makes SDL fall back to the device's default spec,
    // and its CoreAudio backend then pushes that rate onto the audio session -
    // overriding the 48 kHz vita3k_ios_configure_audio_session() just asked
    // for, and landing on SDL's 44.1 kHz default. Guest audio is 48 kHz, so
    // that costs a resample down to 44.1 kHz in SDL and another back up to
    // 48 kHz in the AudioQueue converter.
    //
    // SDL_HINT_AUDIO_FREQUENCY cannot fix this: PrepareAudioFormat only reads
    // it when the incoming freq is 0, and the default spec already carries a
    // rate. Request the rate explicitly instead. Leaving format and channels
    // zeroed lets SDL fill in its own defaults for those, as before.
    SDL_AudioSpec want = {};
    want.freq = 48000;
    device_id = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &want);
#else
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "512");
    device_id = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
#endif
    SDL_CHECK_EXT(device_id > 0, false);
    return true;
}

void SDLAudioAdapter::switch_state(const bool pause) {
    if (pause)
        SDL_CHECK_VOID(SDL_PauseAudioDevice(device_id));
    else
        SDL_CHECK_VOID(SDL_ResumeAudioDevice(device_id));
}

AudioOutPortPtr SDLAudioAdapter::open_port(int nb_channels, int freq, int nb_sample) {
    SDL_AudioSpec src_spec = {
        .format = SDL_AUDIO_S16LE,
        .channels = nb_channels,
        .freq = freq
    };
    SDL_CHECK(SDL_GetAudioDeviceFormat(device_id, &dst_spec, &device_buffer_samples));
    const AudioStreamPtr stream(SDL_CreateAudioStream(&src_spec, &dst_spec), SDL_DestroyAudioStream);
    SDL_CHECK(stream);
    auto port = std::make_shared<SDLAudioOutPort>(stream, *this);
    port->channels = nb_channels;
    port->freq = freq;
    port->len = nb_sample;
    port->len_microseconds = (nb_sample * 1'000'000ULL) / freq;
    port->len_bytes = nb_sample * nb_channels * sizeof(int16_t);
    SDL_CHECK(SDL_BindAudioStream(device_id, stream.get()));
    SDL_CHECK(SDL_SetAudioStreamGetCallback(stream.get(), SDLAudioAdapter::thread_wakeup_callback, port.get()));
    LOG_INFO("SDL audio output opened: guest={} Hz/{} ch/{} frames, device={} Hz/{} ch/{} frames (format=0x{:X})",
        freq, nb_channels, nb_sample, dst_spec.freq, dst_spec.channels, device_buffer_samples,
        static_cast<unsigned>(dst_spec.format));
    switch_state(false);
    return port;
}

void SDLAudioAdapter::audio_output(AudioOutPort &out_port, const void *buffer) {
    if (out_port.stopping)
        return;

    //  Put audio to the port's stream and see how much is left to play.
    SDLAudioOutPort &port = static_cast<SDLAudioOutPort &>(out_port);
    state.output_calls.fetch_add(1, std::memory_order_relaxed);
    // If there's lots of audio left to play, stop this thread.
    // The audio callback will wake it up later when it's running out of data.
    const int samples_available = get_rest_sample(port);
    if (samples_available > get_threshold_samples(device_buffer_samples)) {
        std::unique_lock<std::mutex> lock(port.mutex);
        port.cond_var.wait_for(lock, std::chrono::microseconds(port.len_microseconds * 2));
        if (out_port.stopping)
            return;
    }
    SDL_CHECK_VOID(SDL_PutAudioStreamData(port.stream.get(), buffer, out_port.len_bytes));
}

void SDLAudioAdapter::set_volume(AudioOutPort &out_port, float volume) {
    SDL_CHECK_VOID(SDL_SetAudioStreamGain(static_cast<SDLAudioOutPort &>(out_port).stream.get(), volume));
}

int SDLAudioAdapter::get_rest_sample(AudioOutPort &out_port) {
    auto &port = static_cast<SDLAudioOutPort &>(out_port);
    // sceAudioOutGetRestSample asks how many guest frames have not played yet.
    // That is exactly what SDL_GetAudioStreamQueued reports: every frame put
    // into the stream and not yet pulled by the device, still in guest format.
    //
    // Do not add SDL_GetAudioStreamAvailable to this. The two are not disjoint
    // pools - Available walks the same stream->queue that Queued measures and
    // simply expresses it in the device format, so summing them reports twice
    // the real backlog. That both lies to audio-gated games and halves the
    // effective cushion in audio_output()'s threshold check, which lets the
    // queue dip below one device buffer and makes SDL pad the mix with
    // silence (the White Album 2 stutter captured on iOS).
    const int bytes_queued = SDL_GetAudioStreamQueued(port.stream.get());
    SDL_CHECK_NEG(bytes_queued);
    const int guest_frame_bytes = std::max(1, port.channels * static_cast<int>(sizeof(int16_t)));
    return bytes_queued / guest_frame_bytes;
}

void SDLAudioAdapter::wake_all_ports() {
    for (auto &[_, port_ptr] : state.out_ports) {
        auto &port = static_cast<SDLAudioOutPort &>(*port_ptr);
        {
            std::lock_guard<std::mutex> lock(port.mutex);
        }
        port.cond_var.notify_all();
    }
}
