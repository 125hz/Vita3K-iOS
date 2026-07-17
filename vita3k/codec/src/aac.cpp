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

#include <codec/state.h>

#define DEBUG

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#ifndef VITA3K_PLATFORM_IOS
// Stock FFmpeg installs (vcpkg iOS) do not ship this private header; the iOS
// build uses the public send/receive API below instead.
#include <libavcodec/codec_internal.h>
#endif
}

#include <util/log.h>

AacDecoderState::AacDecoderState(uint32_t sample_rate, uint32_t channels) {
    codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    assert(codec);

    context = avcodec_alloc_context3(codec);
    assert(context);

    frame = av_frame_alloc();

    context->codec_type = AVMEDIA_TYPE_AUDIO;
    av_channel_layout_default(&context->ch_layout, channels);
    context->sample_rate = sample_rate;

    int err = avcodec_open2(context, codec, nullptr);
    assert(err == 0);

    swr = nullptr;
    int ret = swr_alloc_set_opts2(&swr,
        &context->ch_layout, AV_SAMPLE_FMT_S16, sample_rate,
        &context->ch_layout, AV_SAMPLE_FMT_FLTP, sample_rate,
        0, nullptr);
    assert(ret == 0);

    ret = swr_init(swr);
    assert(ret == 0);
}

AacDecoderState::~AacDecoderState() {
    av_frame_free(&frame);
    swr_free(&swr);
}

uint32_t AacDecoderState::get(DecoderQuery query) {
    switch (query) {
    case DecoderQuery::CHANNELS: return context->ch_layout.nb_channels;
    case DecoderQuery::BIT_RATE: return context->bit_rate;
    case DecoderQuery::SAMPLE_RATE: return context->sample_rate;
    default:
        return 0;
    }
}

bool AacDecoderState::send(const uint8_t *data, uint32_t size) {
    AVPacket *packet = av_packet_alloc();
    packet->data = const_cast<uint8_t *>(data);
    packet->size = size;

    av_frame_unref(frame);

#ifdef VITA3K_PLATFORM_IOS
    // Emit one AAC frame of silence into `frame` so `receive()` still produces
    // valid PCM and the guest movie/audio clock keeps advancing. Without this,
    // a single decode failure permanently wedged the avPlayer intro (the
    // endless-EAGAIN loop seen booting Persona 4 Golden on device).
    const auto emit_silence = [this]() {
        av_frame_unref(frame);
        frame->format = AV_SAMPLE_FMT_FLTP;
        av_channel_layout_copy(&frame->ch_layout, &context->ch_layout);
        frame->sample_rate = context->sample_rate;
        // AAC-LC produces 1024 samples per frame; SBR/HE-AAC would be 2048, but
        // a single short silent frame is a benign clock nudge either way.
        frame->nb_samples = 1024;
        if (av_frame_get_buffer(frame, 0) < 0)
            return false;
        av_samples_set_silence(frame->extended_data, 0, frame->nb_samples,
            frame->ch_layout.nb_channels, AV_SAMPLE_FMT_FLTP);
        return true;
    };

    // The public send/receive API requires the caller to drain decoded frames
    // before the decoder will accept a new packet. If a prior packet decoded
    // but its frame was never received (e.g. after an earlier error), the next
    // send returns EAGAIN forever unless we drain first, then resend.
    int err = avcodec_send_packet(context, packet);
    if (err == AVERROR(EAGAIN)) {
        avcodec_receive_frame(context, frame);
        av_frame_unref(frame);
        err = avcodec_send_packet(context, packet);
    }
    av_packet_free(&packet);

    if (err >= 0)
        err = avcodec_receive_frame(context, frame);

    if (err < 0) {
        LOG_WARN_ONCE("Aac decode error ({}); emitting silence. ffmpeg '{}', packet {} bytes, extradata {} bytes.",
            codec_error_name(err), av_version_info(), size, context->extradata_size);
        if (!emit_silence()) {
            LOG_WARN("Failed to allocate AAC silence frame: {}.", codec_error_name(err));
            return false;
        }
    }

    // The public API consumes the complete access unit handed in by
    // SceAudiodec, so the consumed elementary-stream size is the packet size.
    es_size_used = size;

    return true;
}
#else
    const FFCodec *ff_codec = ffcodec(codec);
    int got_frame;
    int len = ff_codec->cb.decode(context, frame, &got_frame, packet);
    assert(got_frame);

    av_packet_free(&packet);
    if (len < 0) {
        LOG_WARN("Error sending Aac packet: {}.", codec_error_name(len));
        return false;
    }

    es_size_used = static_cast<uint32_t>(len);

    return true;
}
#endif

bool AacDecoderState::receive(uint8_t *data, DecoderSize *size) {
    assert(frame->format == AV_SAMPLE_FMT_FLTP);

    if (data) {
        int ret = swr_convert(swr, &data, frame->nb_samples, const_cast<const uint8_t **>(frame->extended_data), frame->nb_samples);
        assert(ret >= 0);
    }

    if (size) {
        size->samples = frame->nb_samples;
    }

    return true;
}

uint32_t AacDecoderState::get_es_size() {
    return es_size_used;
}
