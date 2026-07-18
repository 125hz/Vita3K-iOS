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

#include <cstring>
#include <iterator>

AacDecoderState::AacDecoderState(uint32_t sample_rate, uint32_t channels) {
    codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    assert(codec);

    context = avcodec_alloc_context3(codec);
    assert(context);

    frame = av_frame_alloc();

    context->codec_type = AVMEDIA_TYPE_AUDIO;
    av_channel_layout_default(&context->ch_layout, channels);
    context->sample_rate = sample_rate;

#ifdef VITA3K_PLATFORM_IOS
    // The desktop build decodes AAC through libavcodec's private ff_codec
    // callback, which accepts a bare raw access unit. vcpkg's iOS ffmpeg does
    // not ship codec_internal.h, so we use the public send/receive API — and
    // that path rejects a raw AU with no extradata (AVERROR_INVALIDDATA on
    // ffmpeg 8.1, exactly what Persona 4 Golden's intro movie hit). SceAudiodec
    // provides the stream configuration out of band (sample rate + channels),
    // so synthesise the 2-byte AudioSpecificConfig the decoder needs, matching
    // what an MP4 'esds' box would carry.
    static const int aac_freq_table[] = { 96000, 88200, 64000, 48000, 44100,
        32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350 };
    int freq_index = -1;
    for (int i = 0; i < static_cast<int>(std::size(aac_freq_table)); ++i) {
        if (aac_freq_table[i] == static_cast<int>(sample_rate)) {
            freq_index = i;
            break;
        }
    }
    if (freq_index >= 0 && channels >= 1 && channels <= 7) {
        constexpr int aac_lc_object_type = 2;
        uint8_t asc[2];
        asc[0] = static_cast<uint8_t>((aac_lc_object_type << 3) | (freq_index >> 1));
        asc[1] = static_cast<uint8_t>(((freq_index & 1) << 7) | (channels << 3));
        context->extradata = static_cast<uint8_t *>(
            av_mallocz(sizeof(asc) + AV_INPUT_BUFFER_PADDING_SIZE));
        if (context->extradata) {
            memcpy(context->extradata, asc, sizeof(asc));
            context->extradata_size = sizeof(asc);
        }
    } else {
        LOG_WARN("AAC: no AudioSpecificConfig for sample_rate={} channels={}; "
                 "raw access units may fail to decode.", sample_rate, channels);
    }
#endif

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
    const uint8_t *payload_data = data;
    uint32_t payload_size = size;
    uint32_t framing_size = 0;

#ifdef VITA3K_PLATFORM_IOS
    // P4G's SceAudiodec buffer is not a bare AAC AU: it starts with a
    // little-endian 32-bit payload length (device capture: DE 04 00 00 =
    // 1246 bytes) and the rest of the 1536-byte ES buffer is padding/next
    // data. Strip that wrapper before handing the AU to FFmpeg and report the
    // four framing bytes as consumed with it.
    if (size >= 8 && data[2] == 0 && data[3] == 0) {
        const uint32_t declared_size = static_cast<uint32_t>(data[0])
            | (static_cast<uint32_t>(data[1]) << 8)
            | (static_cast<uint32_t>(data[2]) << 16)
            | (static_cast<uint32_t>(data[3]) << 24);
        if (declared_size >= 8 && declared_size <= size - 4) {
            payload_data = data + 4;
            payload_size = declared_size;
            framing_size = 4;
            LOG_INFO_ONCE("AAC iOS length-prefixed AU: input={} declared_payload={} framing={} bytes.",
                size, payload_size, framing_size);
        }
    }
#endif

    packet->data = const_cast<uint8_t *>(payload_data);
    packet->size = static_cast<int>(payload_size);

    av_frame_unref(frame);

#ifdef VITA3K_PLATFORM_IOS
    // sceAudiodec supplies the capacity of its ES buffer, not necessarily the
    // exact length of the first compressed access unit. The old private FFmpeg
    // decode callback returned the number of bytes it consumed; the public API
    // does not. Ask FFmpeg's public AAC parser for the first frame boundary so
    // avPlayer advances by one AU instead of discarding the entire 1536-byte
    // buffer (the P4G FilterAu stall seen on device).
    uint32_t parsed_consumed = framing_size + payload_size;
    AVCodecParserContext *parser = av_parser_init(AV_CODEC_ID_AAC);
    if (parser) {
        uint8_t *parsed_data = nullptr;
        int parsed_size = 0;
        const int consumed = av_parser_parse2(parser, context, &parsed_data, &parsed_size,
            payload_data, static_cast<int>(payload_size), AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (consumed > 0 && consumed <= static_cast<int>(payload_size) && parsed_size > 0) {
            packet->data = parsed_data;
            packet->size = parsed_size;
            parsed_consumed = framing_size + static_cast<uint32_t>(consumed);
        }
    }
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
    const int decode_packet_size = packet->size;
    av_packet_free(&packet);

    if (err >= 0)
        err = avcodec_receive_frame(context, frame);
    if (parser)
        av_parser_close(parser);

    if (err < 0) {
        // With valid extradata this should no longer happen for P4G's raw AUs;
        // keep the silent frame only as a last resort so one bad packet can't
        // wedge the avPlayer clock, and log it so it is visible if it recurs.
        LOG_WARN_ONCE("Aac decode error ({}); emitting silence. ffmpeg '{}', input {} bytes, decoded packet {} bytes, "
                      "extradata {} bytes, head {:02X} {:02X} {:02X} {:02X}.",
            codec_error_name(err), av_version_info(), size, decode_packet_size, context->extradata_size,
            size > 0 ? data[0] : 0, size > 1 ? data[1] : 0, size > 2 ? data[2] : 0, size > 3 ? data[3] : 0);
        if (!emit_silence()) {
            LOG_WARN("Failed to allocate AAC silence frame: {}.", codec_error_name(err));
            return false;
        }
        es_size_used = parsed_consumed;
        return true;
    }

    LOG_INFO_ONCE("AAC decode ok on iOS (ffmpeg '{}', extradata {} bytes, {} samples/frame).",
        av_version_info(), context->extradata_size, frame->nb_samples);

    // Advance the guest ES read pointer by the bytes actually consumed. For an
    // ADTS stream that is the 13-bit frame length in the header; for a raw AU
    // (the MP4/avPlayer case) the whole packet is one access unit.
    es_size_used = parsed_consumed;
    if (payload_size >= 7 && payload_data[0] == 0xFF && (payload_data[1] & 0xF6) == 0xF0) {
        const uint32_t adts_frame_length =
            (static_cast<uint32_t>(payload_data[3] & 0x03) << 11)
            | (static_cast<uint32_t>(payload_data[4]) << 3)
            | (static_cast<uint32_t>(payload_data[5]) >> 5);
        if (adts_frame_length >= 7 && adts_frame_length <= payload_size)
            es_size_used = framing_size + adts_frame_length;
    }

    LOG_INFO_ONCE("AAC iOS frame boundary: input={} packet={} consumed={} bytes.",
        size, decode_packet_size, es_size_used);

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
