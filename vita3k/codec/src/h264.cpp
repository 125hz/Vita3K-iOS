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

#include <util/log.h>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <algorithm>
#include <cassert>

void copy_yuv_data_from_frame(AVFrame *frame, uint8_t *dest, const uint32_t width, const uint32_t height, bool is_p3) {
    for (size_t i = 0; i < height; i++) {
        memcpy(dest, &frame->data[0][frame->linesize[0] * i], width);
        dest += width;
    }

    if (is_p3) {
        for (size_t i = 0; i < height / 2; i++) {
            memcpy(dest, &frame->data[1][frame->linesize[1] * i], width / 2);
            dest += width / 2;
        }
        for (size_t i = 0; i < height / 2; i++) {
            memcpy(dest, &frame->data[2][frame->linesize[2] * i], width / 2);
            dest += width / 2;
        }
    } else {
        // p2 format, U and V are interleaved
        for (size_t i = 0; i < height / 2; i++) {
            const uint8_t *src_u = &frame->data[1][frame->linesize[1] * i];
            const uint8_t *src_v = &frame->data[2][frame->linesize[2] * i];
            for (size_t j = 0; j < width / 2; j++) {
                dest[0] = src_u[j];
                dest[1] = src_v[j];
                dest += 2;
            }
        }
    }
}

uint32_t H264DecoderState::buffer_size(DecoderSize size) {
    return size.width * size.height * 3 / 2;
}

uint32_t H264DecoderState::get(DecoderQuery query) {
    switch (query) {
    case DecoderQuery::WIDTH: return context->width;
    case DecoderQuery::HEIGHT: return context->height;
    default: return 0;
    }
}

bool H264DecoderState::send(const uint8_t *data, uint32_t size) {
    std::lock_guard<std::mutex> lock(codec_mutex);

    int error = 0;

    std::vector<uint8_t> au_frame(size + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(au_frame.data(), data, size);

    AVPacket *packet = av_packet_alloc();
    if (!packet) {
        LOG_ERROR("Error allocating H264 packet.");
        return false;
    }
    error = av_parser_parse2(
        parser, // AVCodecParserContext *s,
        context, // AVCodecContext *avctx,
        &packet->data, // uint8_t **poutbuf,
        &packet->size, // int *poutbuf_size,
        au_frame.data(), // const uint8_t *buf,
        size, // int buf_size,
        pts == ~0ull ? AV_NOPTS_VALUE : pts, // int64_t pts,
        dts == ~0ull ? AV_NOPTS_VALUE : dts, // int64_t dts,
        0 // int64_t pos
    );
    if (error < 0) {
        LOG_WARN("Error parsing H264 packet: {}.", codec_error_name(error));
        av_packet_free(&packet);
        return false;
    }

    packet->pts = parser->pts;
    packet->dts = parser->dts;

    error = avcodec_send_packet(context, packet);
    av_packet_free(&packet);
    if (error < 0) {
        LOG_WARN("Error sending H264 packet: {}.", codec_error_name(error));
        return false;
    }

    return true;
}

bool H264DecoderState::receive(uint8_t *data, DecoderSize *size) {
    AVFrame *frame = av_frame_alloc();

    int error = avcodec_receive_frame(context, frame);
    if (error < 0) {
        LOG_WARN("Error receiving H264 frame: {}.", codec_error_name(error));
        av_frame_free(&frame);
        return false;
    }

    if (data) {
        // Guard the copy against decoder output that does not match the
        // guest-requested planar-YUV420 layout or is smaller than requested.
        // Copying width_in*height_in unconditionally read past ffmpeg's planes
        // and hard-crashed the process on iOS as the P4G intro movie's first
        // frame decoded.
        const bool planar_yuv420 = (frame->format == AV_PIX_FMT_YUV420P
                                       || frame->format == AV_PIX_FMT_YUVJ420P)
            && frame->data[0] && frame->data[1] && frame->data[2];
        if (!planar_yuv420) {
            LOG_WARN("H264 frame format {} unsupported for YUV420 copy; dropping frame.",
                static_cast<int>(frame->format));
            av_frame_free(&frame);
            return false;
        }
        const uint32_t copy_width = std::min<uint32_t>(width_in, static_cast<uint32_t>(frame->width));
        const uint32_t copy_height = std::min<uint32_t>(height_in, static_cast<uint32_t>(frame->height));
        if (copy_width != width_in || copy_height != height_in)
            LOG_WARN_ONCE("H264 output {}x{} smaller than requested {}x{}; clamping copy.",
                frame->width, frame->height, width_in, height_in);
        copy_yuv_data_from_frame(frame, data, copy_width, copy_height, output_yuvp3);
    }

    if (size) {
        *size = { { static_cast<uint32_t>(context->width), static_cast<uint32_t>(context->height) } };
    }

    width_out = frame->width;
    height_out = frame->height;

    pts_out = frame->pts;

    av_frame_free(&frame);
    return true;
}

void H264DecoderState::configure(void *options) {
    auto *opt = static_cast<H264DecoderOptions *>(options);

    pts = static_cast<uint64_t>(opt->pts_upper) << 32u | static_cast<uint64_t>(opt->pts_lower);
    dts = static_cast<uint64_t>(opt->dts_upper) << 32u | static_cast<uint64_t>(opt->dts_lower);
}

void H264DecoderState::set_res(const uint32_t width, const uint32_t height) {
    width_in = width;
    height_in = height;
}

void H264DecoderState::get_res(uint32_t &width, uint32_t &height) {
    width = width_out;
    height = height_out;
}

void H264DecoderState::get_pts(uint32_t &upper, uint32_t &lower) {
    upper = pts_out >> 32u;
    lower = pts_out & 0xFFFFFFFF;
}

void H264DecoderState::set_output_format(bool is_yuv_p3) {
    this->output_yuvp3 = is_yuv_p3;
}

H264DecoderState::H264DecoderState(uint32_t width, uint32_t height) {
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    assert(codec);

    parser = av_parser_init(codec->id);
    assert(parser);
    parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;

    context = avcodec_alloc_context3(codec);
    assert(context);
    context->width = width;
    context->height = height;

#ifdef VITA3K_PLATFORM_IOS
    // Single-threaded decode on iOS: frame/slice threading spawns worker
    // pthreads whose faults bypass our fatal-signal logger (a silent process
    // death as the P4G movie started), and it multiplies the decoder's buffer
    // footprint right when memory is tightest during movie playback.
    context->thread_count = 1;
    context->thread_type = 0;
#endif

    int result = avcodec_open2(context, codec, nullptr);
    assert(result == 0);
}

H264DecoderState::~H264DecoderState() {
    av_parser_close(parser);
}
