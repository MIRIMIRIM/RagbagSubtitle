// Copyright (c) 2026
// SPDX-License-Identifier: MIT

#include "ffmpeg_subtitle_provider.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4244)
#endif
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/rational.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
}

namespace ragbag {
namespace {

struct BgraPixel {
    uint8_t b = 0;
    uint8_t g = 0;
    uint8_t r = 0;
    uint8_t a = 0;
};

struct BitmapRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    std::vector<BgraPixel> pixels;
};

struct SubtitleEvent {
    double start = 0.0;
    double end = 0.0;
    std::vector<BitmapRect> rects;
};

struct FormatContextDeleter {
    void operator()(AVFormatContext *ctx) const {
        if (ctx)
            avformat_close_input(&ctx);
    }
};

struct CodecContextDeleter {
    void operator()(AVCodecContext *ctx) const {
        avcodec_free_context(&ctx);
    }
};

struct PacketDeleter {
    void operator()(AVPacket *packet) const {
        av_packet_free(&packet);
    }
};

using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;

std::string AvError(int error_code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(error_code, buffer, sizeof(buffer));
    return buffer;
}

double TimestampToSeconds(int64_t value, AVRational time_base) {
    if (value == AV_NOPTS_VALUE)
        return std::numeric_limits<double>::quiet_NaN();
    return static_cast<double>(value) * av_q2d(time_base);
}

bool IsFiniteTime(double value) {
    return value == value
        && value > -std::numeric_limits<double>::infinity()
        && value < std::numeric_limits<double>::infinity();
}

BgraPixel PremultiplyPaletteEntry(uint8_t const *entry) {
    uint32_t const b = entry[0];
    uint32_t const g = entry[1];
    uint32_t const r = entry[2];
    uint32_t const a = entry[3];
    return {
        static_cast<uint8_t>((b * a + 127) / 255),
        static_cast<uint8_t>((g * a + 127) / 255),
        static_cast<uint8_t>((r * a + 127) / 255),
        static_cast<uint8_t>(a)
    };
}

bool ConvertBitmapRect(AVSubtitleRect const *rect, BitmapRect& out) {
    if (!rect || rect->type != SUBTITLE_BITMAP || rect->w <= 0 || rect->h <= 0)
        return false;
    if (!rect->data[0] || !rect->data[1] || rect->linesize[0] <= 0 || rect->nb_colors <= 0)
        return false;

    out.x = rect->x;
    out.y = rect->y;
    out.width = rect->w;
    out.height = rect->h;
    out.pixels.assign(static_cast<size_t>(out.width) * out.height, {});

    BgraPixel palette[256] = {};
    int const colors = std::min(rect->nb_colors, 256);
    for (int i = 0; i < colors; ++i)
        palette[i] = PremultiplyPaletteEntry(rect->data[1] + static_cast<size_t>(i) * 4);

    for (int y = 0; y < out.height; ++y) {
        auto const *src = rect->data[0] + static_cast<ptrdiff_t>(y) * rect->linesize[0];
        auto *dst = out.pixels.data() + static_cast<size_t>(y) * out.width;
        for (int x = 0; x < out.width; ++x)
            dst[x] = palette[src[x]];
    }

    return true;
}

void ClearTarget(RagbagSubtitleOverlayTargetV0& target) {
    if (!target.plane0 || target.width <= 0 || target.height <= 0)
        return;

    for (int y = 0; y < target.height; ++y) {
        auto *row = target.plane0 + static_cast<ptrdiff_t>(y) * target.stride0;
        std::memset(row, 0, static_cast<size_t>(target.width) * 4);
    }
}

void AddDirtyRect(RagbagSubtitleOverlayTargetV0& target, int x, int y, int width, int height) {
    if (!target.dirty_rects || target.dirty_rect_count >= target.dirty_rect_capacity)
        return;
    target.dirty_rects[target.dirty_rect_count++] = { x, y, width, height };
}

void BlendPixel(BgraPixel src, uint8_t *dst) {
    if (!src.a)
        return;

    uint32_t const inv_alpha = 255 - src.a;
    dst[0] = static_cast<uint8_t>(std::min<uint32_t>(255, src.b + (dst[0] * inv_alpha + 127) / 255));
    dst[1] = static_cast<uint8_t>(std::min<uint32_t>(255, src.g + (dst[1] * inv_alpha + 127) / 255));
    dst[2] = static_cast<uint8_t>(std::min<uint32_t>(255, src.r + (dst[2] * inv_alpha + 127) / 255));
    dst[3] = static_cast<uint8_t>(std::min<uint32_t>(255, src.a + (dst[3] * inv_alpha + 127) / 255));
}

void BlendRect(BitmapRect const& rect, RagbagSubtitleOverlayTargetV0& target) {
    int const x0 = std::max(0, rect.x);
    int const y0 = std::max(0, rect.y);
    int const x1 = std::min(target.width, rect.x + rect.width);
    int const y1 = std::min(target.height, rect.y + rect.height);
    if (x0 >= x1 || y0 >= y1)
        return;

    bool visible = false;
    for (int y = y0; y < y1; ++y) {
        auto *dst_row = target.plane0 + static_cast<ptrdiff_t>(y) * target.stride0;
        auto const *src_row = rect.pixels.data() + static_cast<size_t>(y - rect.y) * rect.width;
        for (int x = x0; x < x1; ++x) {
            auto const src = src_row[x - rect.x];
            visible = visible || src.a != 0;
            BlendPixel(src, dst_row + static_cast<ptrdiff_t>(x) * 4);
        }
    }

    if (visible) {
        target.has_visible_content = 1;
        AddDirtyRect(target, x0, y0, x1 - x0, y1 - y0);
    }
}

class FfmpegSubtitleProviderImpl final : public FfmpegSubtitleProvider {
    std::string last_error;
    std::vector<SubtitleEvent> events;

    int SetError(RagbagSubtitleStatusV0 status, std::string message) {
        last_error = std::move(message);
        return static_cast<int>(status);
    }

    int DecodeSubtitlePacket(AVCodecContext *codec_ctx, AVStream const *stream, AVPacket *packet) {
        AVSubtitle subtitle = {};
        int got_subtitle = 0;
        int result = avcodec_decode_subtitle2(codec_ctx, &subtitle, &got_subtitle, packet);
        if (result < 0)
            return SetError(RAGBAG_SUBTITLE_STATUS_DECODE_FAILED, "FFmpeg subtitle decode failed: " + AvError(result));
        if (!got_subtitle)
            return RAGBAG_SUBTITLE_STATUS_OK;

        double base = TimestampToSeconds(packet->pts, stream->time_base);
        if (!IsFiniteTime(base))
            base = TimestampToSeconds(packet->dts, stream->time_base);
        if (subtitle.pts != AV_NOPTS_VALUE)
            base = static_cast<double>(subtitle.pts) / AV_TIME_BASE;
        if (!IsFiniteTime(base))
            base = 0.0;

        SubtitleEvent event;
        event.start = base + static_cast<double>(subtitle.start_display_time) / 1000.0;
        if (subtitle.end_display_time > subtitle.start_display_time && subtitle.end_display_time != UINT32_MAX)
            event.end = base + static_cast<double>(subtitle.end_display_time) / 1000.0;
        else
            event.end = event.start + 5.0;

        for (unsigned i = 0; i < subtitle.num_rects; ++i) {
            BitmapRect rect;
            if (ConvertBitmapRect(subtitle.rects[i], rect))
                event.rects.push_back(std::move(rect));
        }

        avsubtitle_free(&subtitle);

        if (event.rects.empty()) {
            if (!events.empty() && events.back().end > event.start)
                events.back().end = event.start;
            return RAGBAG_SUBTITLE_STATUS_OK;
        }

        if (!events.empty() && events.back().end > event.start)
            events.back().end = event.start;
        events.push_back(std::move(event));
        return RAGBAG_SUBTITLE_STATUS_OK;
    }

public:
    char const *LastError() const override {
        return last_error.c_str();
    }

    int OpenFile(char const *path_utf8, RagbagSubtitleVideoInfoV0 const *) override {
        events.clear();
        last_error.clear();
        if (!path_utf8 || !*path_utf8)
            return SetError(RAGBAG_SUBTITLE_STATUS_INVALID_ARGUMENT, "Missing subtitle path.");

        AVFormatContext *raw_format = nullptr;
        int result = avformat_open_input(&raw_format, path_utf8, nullptr, nullptr);
        if (result < 0)
            return SetError(RAGBAG_SUBTITLE_STATUS_OPEN_FAILED, "FFmpeg failed to open input: " + AvError(result));
        FormatContextPtr format(raw_format);

        result = avformat_find_stream_info(format.get(), nullptr);
        if (result < 0)
            return SetError(RAGBAG_SUBTITLE_STATUS_OPEN_FAILED, "FFmpeg failed to read stream info: " + AvError(result));

        int subtitle_stream_index = av_find_best_stream(format.get(), AVMEDIA_TYPE_SUBTITLE, -1, -1, nullptr, 0);
        if (subtitle_stream_index < 0)
            return SetError(RAGBAG_SUBTITLE_STATUS_OPEN_FAILED, "No subtitle stream found in input.");

        AVStream *stream = format->streams[subtitle_stream_index];
        AVCodecParameters *codec_parameters = stream->codecpar;
        AVCodec const *decoder = avcodec_find_decoder(codec_parameters->codec_id);
        if (!decoder)
            return SetError(RAGBAG_SUBTITLE_STATUS_OPEN_FAILED, "No FFmpeg subtitle decoder is available for this stream.");

        CodecContextPtr codec(avcodec_alloc_context3(decoder));
        if (!codec)
            return SetError(RAGBAG_SUBTITLE_STATUS_OPEN_FAILED, "Failed to allocate FFmpeg codec context.");

        result = avcodec_parameters_to_context(codec.get(), codec_parameters);
        if (result < 0)
            return SetError(RAGBAG_SUBTITLE_STATUS_OPEN_FAILED, "Failed to copy codec parameters: " + AvError(result));
        codec->pkt_timebase = stream->time_base;

        result = avcodec_open2(codec.get(), decoder, nullptr);
        if (result < 0)
            return SetError(RAGBAG_SUBTITLE_STATUS_OPEN_FAILED, "Failed to open subtitle decoder: " + AvError(result));

        PacketPtr packet(av_packet_alloc());
        if (!packet)
            return SetError(RAGBAG_SUBTITLE_STATUS_OPEN_FAILED, "Failed to allocate FFmpeg packet.");

        while ((result = av_read_frame(format.get(), packet.get())) >= 0) {
            if (packet->stream_index == subtitle_stream_index) {
                int decode_status = DecodeSubtitlePacket(codec.get(), stream, packet.get());
                if (decode_status != RAGBAG_SUBTITLE_STATUS_OK) {
                    av_packet_unref(packet.get());
                    return decode_status;
                }
            }
            av_packet_unref(packet.get());
        }

        std::sort(events.begin(), events.end(), [](SubtitleEvent const& a, SubtitleEvent const& b) {
            return a.start < b.start;
        });

        if (events.empty())
            return SetError(RAGBAG_SUBTITLE_STATUS_DECODE_FAILED, "No bitmap subtitle events were decoded.");

        return RAGBAG_SUBTITLE_STATUS_OK;
    }

    int RenderOverlay(RagbagSubtitleRenderRequestV0 const *request, RagbagSubtitleOverlayTargetV0 *target) override {
        if (!request || !target || target->struct_size < sizeof(RagbagSubtitleOverlayTargetV0))
            return SetError(RAGBAG_SUBTITLE_STATUS_INVALID_ARGUMENT, "Invalid render request or overlay target.");
        if (target->pixel_format != RAGBAG_SUBTITLE_PIXEL_FORMAT_BGRA8 || target->alpha_mode != RAGBAG_SUBTITLE_ALPHA_PREMULTIPLIED)
            return SetError(RAGBAG_SUBTITLE_STATUS_RENDER_FAILED, "Only premultiplied BGRA8 overlay targets are supported.");
        if (!target->plane0 || target->width <= 0 || target->height <= 0 || target->stride0 == 0)
            return SetError(RAGBAG_SUBTITLE_STATUS_RENDER_FAILED, "Overlay target has invalid storage.");

        target->dirty_rect_count = 0;
        target->has_visible_content = 0;
        if (request->flags & RAGBAG_SUBTITLE_RENDER_CLEAR_TARGET) {
            ClearTarget(*target);
            AddDirtyRect(*target, 0, 0, target->width, target->height);
        }

        double const time = request->time_seconds;
        for (auto const& event : events) {
            if (time < event.start)
                break;
            if (time >= event.end)
                continue;
            for (auto const& rect : event.rects)
                BlendRect(rect, *target);
        }

        return RAGBAG_SUBTITLE_STATUS_OK;
    }
};

} // namespace

std::unique_ptr<FfmpegSubtitleProvider> CreateFfmpegSubtitleProvider() {
    return std::make_unique<FfmpegSubtitleProviderImpl>();
}

} // namespace ragbag
