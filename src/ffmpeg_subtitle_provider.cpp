// Copyright (c) 2026
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "ffmpeg_subtitle_provider.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
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
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
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

// PGS and most FFmpeg bitmap subtitles decode to a palettized (PAL8) image: a
// byte index per pixel plus a small RGBA palette. Keeping that native layout in
// memory (index + palette) instead of expanding to 4 bytes/pixel BGRA cuts the
// resident footprint by ~4x. The palette is stored pre-multiplied so blend math
// is unchanged.
struct BitmapRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int visible_x0 = 0;
    int visible_y0 = 0;
    int visible_x1 = 0;
    int visible_y1 = 0;
    std::vector<uint8_t> indices;  // width * height entries, PAL8 index into palette
    BgraPixel palette[256] = {};
};

struct SubtitleEvent {
    int64_t start_ns = 0;
    int64_t end_ns = std::numeric_limits<int64_t>::max();
    int authored_width = 0;
    int authored_height = 0;
    std::vector<BitmapRect> rects;
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

using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;

std::string AvError(int error_code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(error_code, buffer, sizeof(buffer));
    return buffer;
}

constexpr AVRational kNanosecondTimeBase = { 1, 1000000000 };

struct SupportedCodec {
    char const *stable_id;
    AVCodecID codec_id;
};

constexpr SupportedCodec kSupportedCodecs[] = {
    { "hdmv-pgs", AV_CODEC_ID_HDMV_PGS_SUBTITLE },
    { "dvd-subtitle", AV_CODEC_ID_DVD_SUBTITLE },
};

SupportedCodec const *FindSupportedCodec(char const *stable_id) {
    if (!stable_id)
        return nullptr;
    for (auto const& codec : kSupportedCodecs) {
        if (std::strcmp(codec.stable_id, stable_id) == 0)
            return &codec;
    }
    return nullptr;
}

int64_t NsToAvTime(int64_t value) {
    if (value == RAGBAG_SUBTITLE_TIMESTAMP_UNKNOWN)
        return AV_NOPTS_VALUE;
    return av_rescale_q(value, kNanosecondTimeBase, AV_TIME_BASE_Q);
}

int64_t AvTimeToNs(int64_t value) {
    if (value == AV_NOPTS_VALUE)
        return RAGBAG_SUBTITLE_TIMESTAMP_UNKNOWN;
    return av_rescale_q(value, AV_TIME_BASE_Q, kNanosecondTimeBase);
}

int64_t SaturatingAdd(int64_t left, int64_t right) {
    if (right > 0 && left > std::numeric_limits<int64_t>::max() - right)
        return std::numeric_limits<int64_t>::max();
    if (right < 0 && left < std::numeric_limits<int64_t>::min() - right)
        return std::numeric_limits<int64_t>::min();
    return left + right;
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
    if (!rect->data[0] || !rect->data[1] || rect->linesize[0] < rect->w || rect->nb_colors <= 0)
        return false;

    out.x = rect->x;
    out.y = rect->y;
    out.width = rect->w;
    out.height = rect->h;

    int const colors = std::min(rect->nb_colors, 256);
    uint8_t visible_palette[256] = {};
    for (int i = 0; i < colors; ++i) {
        out.palette[i] = PremultiplyPaletteEntry(rect->data[1] + static_cast<size_t>(i) * 4);
        visible_palette[i] = out.palette[i].a != 0 ? 1 : 0;
    }

    // PAL8 index plane, copied verbatim. resize (not assign) so the buffer is
    // only touched once; the loop below fully populates every byte.
    out.indices.resize(static_cast<size_t>(out.width) * out.height);
    int visible_x0 = out.width;
    int visible_y0 = out.height;
    int visible_x1 = 0;
    int visible_y1 = 0;
    for (int y = 0; y < out.height; ++y) {
        auto const *src = rect->data[0] + static_cast<ptrdiff_t>(y) * rect->linesize[0];
        auto *dst = out.indices.data() + static_cast<size_t>(y) * out.width;
        std::memcpy(dst, src, static_cast<size_t>(out.width));
        for (int x = 0; x < out.width; ++x) {
            if (!visible_palette[dst[x]])
                continue;
            visible_x0 = std::min(visible_x0, x);
            visible_y0 = std::min(visible_y0, y);
            visible_x1 = std::max(visible_x1, x + 1);
            visible_y1 = std::max(visible_y1, y + 1);
        }
    }

    if (visible_x1 <= visible_x0 || visible_y1 <= visible_y0)
        return false;

    out.visible_x0 = visible_x0;
    out.visible_y0 = visible_y0;
    out.visible_x1 = visible_x1;
    out.visible_y1 = visible_y1;
    return true;
}

void ClearTarget(RagbagSubtitleRenderTargetV1& target) {
    if (!target.plane0 || target.width <= 0 || target.height <= 0)
        return;

    // Fast path: packed rows (no padding, top-down) -> one memset over the
    // whole plane. Otherwise clear each row individually (also handles negative
    // strides for bottom-up targets).
    if (target.stride0 == target.width * 4 && target.stride0 > 0) {
        std::memset(target.plane0, 0,
            static_cast<size_t>(target.stride0) * static_cast<size_t>(target.height));
        return;
    }

    for (int y = 0; y < target.height; ++y) {
        auto *row = target.plane0 + static_cast<ptrdiff_t>(y) * target.stride0;
        std::memset(row, 0, static_cast<size_t>(target.width) * 4);
    }
}

void BlendPixel(BgraPixel src, uint8_t *dst) {
    if (!src.a)
        return;
    if (src.a == 255) {
        dst[0] = src.b;
        dst[1] = src.g;
        dst[2] = src.r;
        dst[3] = 255;
        return;
    }

    uint32_t const inv_alpha = 255 - src.a;
    dst[0] = static_cast<uint8_t>(src.b + (dst[0] * inv_alpha + 127) / 255);
    dst[1] = static_cast<uint8_t>(src.g + (dst[1] * inv_alpha + 127) / 255);
    dst[2] = static_cast<uint8_t>(src.r + (dst[2] * inv_alpha + 127) / 255);
    dst[3] = static_cast<uint8_t>(src.a + (dst[3] * inv_alpha + 127) / 255);
}

void StorePixel(BgraPixel src, uint8_t *dst) {
    dst[0] = src.b;
    dst[1] = src.g;
    dst[2] = src.r;
    dst[3] = src.a;
}

int ScaleBoundaryToTarget(int64_t value, int target_extent, int authored_extent) {
    long double const scaled = static_cast<long double>(value)
        * static_cast<long double>(target_extent)
        / static_cast<long double>(authored_extent);
    if (scaled <= 0.0L)
        return 0;
    if (scaled >= static_cast<long double>(target_extent))
        return target_extent;
    return static_cast<int>(std::ceil(scaled));
}

// Blends one authored bitmap rect into the target. When the authored and target
// canvases match the rect is sampled 1:1; otherwise it is scaled with
// nearest-neighbour sampling. Nearest is appropriate for palette graphics
// (PGS/VOBsub/DVB): bilinear on pre-multiplied color+alpha would smear edges
// and bleed transparent palette entries.
bool BlendRect(BitmapRect const& rect, RagbagSubtitleRenderTargetV1& target, int authored_width, int authored_height, bool copy_to_clear_target) {
    if (authored_width <= 0 || authored_height <= 0)
        return false;

    bool const identity = target.width == authored_width && target.height == authored_height;

    auto const map_x = [&](int64_t value) {
        return identity
            ? static_cast<int>(std::clamp<int64_t>(value, 0, target.width))
            : ScaleBoundaryToTarget(value, target.width, authored_width);
    };
    auto const map_y = [&](int64_t value) {
        return identity
            ? static_cast<int>(std::clamp<int64_t>(value, 0, target.height))
            : ScaleBoundaryToTarget(value, target.height, authored_height);
    };

    // Map absolute authored-canvas edges rather than scaling rect origin and
    // size independently. This keeps adjacent objects contiguous and makes the
    // sampling grid consistent across the entire composition.
    int const whole_x0 = map_x(rect.x);
    int const whole_y0 = map_y(rect.y);
    int const whole_x1 = map_x(static_cast<int64_t>(rect.x) + rect.width);
    int const whole_y1 = map_y(static_cast<int64_t>(rect.y) + rect.height);
    int const visible_x0 = map_x(static_cast<int64_t>(rect.x) + rect.visible_x0);
    int const visible_y0 = map_y(static_cast<int64_t>(rect.y) + rect.visible_y0);
    int const visible_x1 = map_x(static_cast<int64_t>(rect.x) + rect.visible_x1);
    int const visible_y1 = map_y(static_cast<int64_t>(rect.y) + rect.visible_y1);

    int const x0 = std::max(whole_x0, visible_x0);
    int const y0 = std::max(whole_y0, visible_y0);
    int const x1 = std::min(whole_x1, visible_x1);
    int const y1 = std::min(whole_y1, visible_y1);
    if (x0 >= x1 || y0 >= y1)
        return false;

    auto const *palette = rect.palette;
    bool visible = false;

    if (identity) {
        for (int y = y0; y < y1; ++y) {
            auto *dst_row = target.plane0 + static_cast<ptrdiff_t>(y) * target.stride0;
            auto const *src_row = rect.indices.data() + static_cast<size_t>(y - rect.y) * rect.width;
            for (int x = x0; x < x1; ++x) {
                auto const& src = palette[src_row[x - rect.x]];
                if (!visible && src.a != 0)
                    visible = true;
                if (copy_to_clear_target)
                    StorePixel(src, dst_row + static_cast<ptrdiff_t>(x) * 4);
                else
                    BlendPixel(src, dst_row + static_cast<ptrdiff_t>(x) * 4);
            }
        }
    } else {
        // Nearest-neighbour sampling uses the global canvas grid so all rects
        // agree on which authored pixel corresponds to a target pixel.
        for (int y = y0; y < y1; ++y) {
            auto *dst_row = target.plane0 + static_cast<ptrdiff_t>(y) * target.stride0;
            int const authored_y = static_cast<int>(
                static_cast<int64_t>(y) * authored_height / target.height);
            int const src_y = authored_y - rect.y;
            if (src_y < 0 || src_y >= rect.height)
                continue;
            auto const *src_row = rect.indices.data() + static_cast<size_t>(src_y) * rect.width;
            for (int x = x0; x < x1; ++x) {
                int const authored_x = static_cast<int>(
                    static_cast<int64_t>(x) * authored_width / target.width);
                int const src_x = authored_x - rect.x;
                if (src_x < 0 || src_x >= rect.width)
                    continue;
                auto const& src = palette[src_row[src_x]];
                if (!visible && src.a != 0)
                    visible = true;
                if (copy_to_clear_target)
                    StorePixel(src, dst_row + static_cast<ptrdiff_t>(x) * 4);
                else
                    BlendPixel(src, dst_row + static_cast<ptrdiff_t>(x) * 4);
            }
        }
    }

    return visible;
}

class FfmpegBitmapSubtitleDecoderImpl final : public FfmpegBitmapSubtitleDecoder {
    std::string last_error;
    std::vector<SubtitleEvent> events;
    std::vector<int64_t> prefix_max_event_end;
    std::vector<int64_t> change_times;
    CodecContextPtr codec;
    std::vector<uint8_t> codec_private;
    int fallback_canvas_width = 0;
    int fallback_canvas_height = 0;
    AVCodecID codec_id = AV_CODEC_ID_NONE;
    bool stream_finished = false;

    int SetError(RagbagSubtitleStatusV1 status, std::string message) {
        last_error = std::move(message);
        return static_cast<int>(status);
    }

    int OpenCodecContext(RagbagSubtitleStatusV1 failure_status, char const *action) {
        AVCodec const *decoder = avcodec_find_decoder(codec_id);
        if (!decoder)
            return SetError(failure_status, std::string(action) + ": FFmpeg bitmap subtitle decoder is unavailable.");

        CodecContextPtr next_codec(avcodec_alloc_context3(decoder));
        if (!next_codec)
            return SetError(failure_status, std::string(action) + ": failed to allocate FFmpeg codec context.");

        next_codec->pkt_timebase = AV_TIME_BASE_Q;
        if (codec_id == AV_CODEC_ID_DVD_SUBTITLE
            && fallback_canvas_width > 0 && fallback_canvas_height > 0) {
            // DVD/VobSub SPUs carry rectangle coordinates but no canvas size.
            // Keep the host fallback available unless IDX/Matroska extradata
            // supplies an authoritative size line during codec init.
            next_codec->width = fallback_canvas_width;
            next_codec->height = fallback_canvas_height;
        }
        if (!codec_private.empty()) {
            auto const allocation_size = codec_private.size() + AV_INPUT_BUFFER_PADDING_SIZE;
            next_codec->extradata = static_cast<uint8_t *>(av_mallocz(allocation_size));
            if (!next_codec->extradata)
                return SetError(failure_status, std::string(action) + ": failed to allocate subtitle codec private data.");
            std::memcpy(next_codec->extradata, codec_private.data(), codec_private.size());
            next_codec->extradata_size = static_cast<int>(codec_private.size());
        }

        int const result = avcodec_open2(next_codec.get(), decoder, nullptr);
        if (result < 0)
            return SetError(failure_status, std::string(action) + ": " + AvError(result));

        codec = std::move(next_codec);
        return RAGBAG_SUBTITLE_STATUS_OK;
    }

    void TruncateTimelineAt(int64_t time_ns) {
        for (auto& event : events) {
            if (event.start_ns <= time_ns && event.end_ns > time_ns)
                event.end_ns = time_ns;
        }
    }

    int DecodeSubtitlePacket(AVPacket *packet) {
        AVSubtitle subtitle = {};
        int got_subtitle = 0;
        int result = avcodec_decode_subtitle2(codec.get(), &subtitle, &got_subtitle, packet);
        if (result < 0)
            return SetError(RAGBAG_SUBTITLE_STATUS_DECODE_FAILED, "FFmpeg subtitle decode failed: " + AvError(result));
        if (!got_subtitle)
            return RAGBAG_SUBTITLE_STATUS_OK;

        int64_t base_ns = AvTimeToNs(subtitle.pts);
        if (base_ns == RAGBAG_SUBTITLE_TIMESTAMP_UNKNOWN)
            base_ns = AvTimeToNs(packet->pts);
        if (base_ns == RAGBAG_SUBTITLE_TIMESTAMP_UNKNOWN)
            base_ns = AvTimeToNs(packet->dts);
        if (base_ns == RAGBAG_SUBTITLE_TIMESTAMP_UNKNOWN)
            base_ns = 0;

        SubtitleEvent event;
        event.start_ns = SaturatingAdd(base_ns, static_cast<int64_t>(subtitle.start_display_time) * 1000000);
        if (subtitle.end_display_time > subtitle.start_display_time && subtitle.end_display_time != UINT32_MAX) {
            event.end_ns = SaturatingAdd(base_ns, static_cast<int64_t>(subtitle.end_display_time) * 1000000);
        }
        else if (packet->duration > 0) {
            event.end_ns = SaturatingAdd(event.start_ns, AvTimeToNs(packet->duration));
        }

        // PGS publishes its composition canvas while DVD/VobSub obtains it
        // from size metadata in codec private data or the host fallback above.
        event.authored_width = codec->width > 0 ? codec->width : fallback_canvas_width;
        event.authored_height = codec->height > 0 ? codec->height : fallback_canvas_height;

        for (unsigned i = 0; i < subtitle.num_rects; ++i) {
            BitmapRect rect;
            if (ConvertBitmapRect(subtitle.rects[i], rect))
                event.rects.push_back(std::move(rect));
        }

        avsubtitle_free(&subtitle);

        // A PGS display set replaces the previous composition, including an
        // empty display set used to clear it. Do not invent a five-second tail.
        if (!events.empty() && events.back().end_ns > event.start_ns)
            events.back().end_ns = event.start_ns;

        if (event.rects.empty())
            return RAGBAG_SUBTITLE_STATUS_OK;

        events.push_back(std::move(event));
        return RAGBAG_SUBTITLE_STATUS_OK;
    }

public:
    char const *LastError() const override {
        return last_error.c_str();
    }

    int BeginStream(RagbagSubtitleStreamInfoV1 const *stream) override {
        events.clear();
        prefix_max_event_end.clear();
        change_times.clear();
        codec.reset();
        codec_private.clear();
        last_error.clear();
        fallback_canvas_width = 0;
        fallback_canvas_height = 0;
        codec_id = AV_CODEC_ID_NONE;
        stream_finished = false;

        auto const *supported_codec = stream && stream->struct_size >= sizeof(RagbagSubtitleStreamInfoV1)
            ? FindSupportedCodec(stream->codec_id)
            : nullptr;
        if (!supported_codec) {
            return SetError(RAGBAG_SUBTITLE_STATUS_INVALID_ARGUMENT, "Unsupported or invalid bitmap subtitle stream.");
        }
        if ((!stream->codec_private && stream->codec_private_size > 0)
            || stream->codec_private_size > static_cast<uint64_t>(std::numeric_limits<int>::max() - AV_INPUT_BUFFER_PADDING_SIZE)) {
            return SetError(RAGBAG_SUBTITLE_STATUS_INVALID_ARGUMENT, "Invalid subtitle codec private data.");
        }

        fallback_canvas_width = std::max(stream->fallback_canvas_width, 0);
        fallback_canvas_height = std::max(stream->fallback_canvas_height, 0);
        codec_id = supported_codec->codec_id;

        if (stream->codec_private_size > 0) {
            codec_private.assign(
                stream->codec_private,
                stream->codec_private + static_cast<size_t>(stream->codec_private_size));
        }

        return OpenCodecContext(RAGBAG_SUBTITLE_STATUS_OPEN_FAILED, "Failed to open subtitle decoder");
    }

    int PushPacket(RagbagSubtitlePacketV1 const *packet) override {
        if (!codec || stream_finished)
            return SetError(RAGBAG_SUBTITLE_STATUS_INVALID_ARGUMENT, "Bitmap subtitle stream is not accepting packets.");
        if (!packet || packet->struct_size < sizeof(RagbagSubtitlePacketV1)
            || (!packet->payload && packet->payload_size > 0)
            || packet->payload_size > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
            return SetError(RAGBAG_SUBTITLE_STATUS_INVALID_ARGUMENT, "Invalid bitmap subtitle packet.");
        }

        if (packet->flags & RAGBAG_SUBTITLE_PACKET_FLAG_DISCONTINUITY) {
            int64_t const discontinuity_ns = packet->pts_ns != RAGBAG_SUBTITLE_TIMESTAMP_UNKNOWN
                ? packet->pts_ns
                : packet->dts_ns;
            if (discontinuity_ns == RAGBAG_SUBTITLE_TIMESTAMP_UNKNOWN) {
                return SetError(
                    RAGBAG_SUBTITLE_STATUS_INVALID_ARGUMENT,
                    "A bitmap subtitle discontinuity requires a known PTS or DTS.");
            }
            auto const reset_status = OpenCodecContext(
                RAGBAG_SUBTITLE_STATUS_DECODE_FAILED,
                "Failed to reset subtitle decoder after discontinuity");
            if (reset_status != RAGBAG_SUBTITLE_STATUS_OK)
                return reset_status;
            TruncateTimelineAt(discontinuity_ns);
        }

        PacketPtr av_packet(av_packet_alloc());
        if (!av_packet)
            return SetError(RAGBAG_SUBTITLE_STATUS_OPEN_FAILED, "Failed to allocate FFmpeg packet.");

        int result = av_new_packet(av_packet.get(), static_cast<int>(packet->payload_size));
        if (result < 0)
            return SetError(RAGBAG_SUBTITLE_STATUS_OPEN_FAILED, "Failed to allocate FFmpeg packet payload: " + AvError(result));
        if (packet->payload_size > 0)
            std::memcpy(av_packet->data, packet->payload, static_cast<size_t>(packet->payload_size));
        av_packet->pts = NsToAvTime(packet->pts_ns);
        av_packet->dts = NsToAvTime(packet->dts_ns);
        av_packet->duration = packet->duration_ns > 0 ? NsToAvTime(packet->duration_ns) : 0;

        return DecodeSubtitlePacket(av_packet.get());
    }

    int EndStream() override {
        if (!codec || stream_finished)
            return SetError(RAGBAG_SUBTITLE_STATUS_INVALID_ARGUMENT, "Bitmap subtitle stream is not open.");

        stream_finished = true;
        std::stable_sort(events.begin(), events.end(), [](SubtitleEvent const& a, SubtitleEvent const& b) {
            if (a.start_ns != b.start_ns)
                return a.start_ns < b.start_ns;
            return a.end_ns < b.end_ns;
        });

        prefix_max_event_end.reserve(events.size());
        int64_t max_end = std::numeric_limits<int64_t>::min();
        for (auto const& event : events) {
            max_end = std::max(max_end, event.end_ns);
            prefix_max_event_end.push_back(max_end);
            change_times.push_back(event.start_ns);
            if (event.end_ns != std::numeric_limits<int64_t>::max())
                change_times.push_back(event.end_ns);
        }

        std::sort(change_times.begin(), change_times.end());
        change_times.erase(std::unique(change_times.begin(), change_times.end()), change_times.end());

        return RAGBAG_SUBTITLE_STATUS_OK;
    }

    int RenderAt(int64_t time_ns, RagbagSubtitleRenderTargetV1 *target, RagbagSubtitleRenderResultV1 *render_result) override {
        if (!stream_finished || !target || !render_result
            || target->struct_size < sizeof(RagbagSubtitleRenderTargetV1)
            || render_result->struct_size < sizeof(RagbagSubtitleRenderResultV1)) {
            return SetError(RAGBAG_SUBTITLE_STATUS_INVALID_ARGUMENT, "Invalid render request or overlay target.");
        }
        if (!target->plane0 || target->width <= 0 || target->height <= 0 || target->stride0 == 0)
            return SetError(RAGBAG_SUBTITLE_STATUS_RENDER_FAILED, "Overlay target has invalid storage.");

        int64_t const row_bytes = static_cast<int64_t>(target->width) * 4;
        int64_t const stride_bytes = target->stride0 < 0
            ? -static_cast<int64_t>(target->stride0)
            : static_cast<int64_t>(target->stride0);
        if (row_bytes > stride_bytes
            || (target->height > 1
                && stride_bytes > static_cast<int64_t>(std::numeric_limits<ptrdiff_t>::max()) / (target->height - 1))) {
            return SetError(RAGBAG_SUBTITLE_STATUS_RENDER_FAILED, "Overlay target stride is smaller than its rows or exceeds addressable storage.");
        }

        ClearTarget(*target);
        render_result->has_visible_content = 0;
        render_result->authored_width = 0;
        render_result->authored_height = 0;

        // Events are sorted by start. Locate the first event that starts strictly
        // after the requested time via binary search. A prefix max-end index
        // then finds the first event that could still be active even if subtitle
        // events overlap. Active events are blended in authored (forward) order
        // so later-starting events compose on top, matching the original scan.
        auto upper = std::upper_bound(events.begin(), events.end(), time_ns,
            [](int64_t t, SubtitleEvent const& e) { return t < e.start_ns; });
        auto const upper_index = static_cast<size_t>(upper - events.begin());
        auto const first_index = static_cast<size_t>(
            std::upper_bound(prefix_max_event_end.begin(), prefix_max_event_end.begin() + upper_index, time_ns)
                - prefix_max_event_end.begin());
        auto first_active = events.begin() + static_cast<ptrdiff_t>(first_index);
        bool copy_to_clear_target = true;
        for (auto it = first_active; it != upper; ++it) {
            if (time_ns >= it->end_ns)
                continue;

            int authored_width = it->authored_width > 0 ? it->authored_width : target->width;
            int authored_height = it->authored_height > 0 ? it->authored_height : target->height;
            render_result->authored_width = authored_width;
            render_result->authored_height = authored_height;
            for (auto const& rect : it->rects) {
                if (BlendRect(rect, *target, authored_width, authored_height, copy_to_clear_target)) {
                    copy_to_clear_target = false;
                    render_result->has_visible_content = 1;
                }
            }
        }

        auto next_change = std::upper_bound(change_times.begin(), change_times.end(), time_ns);
        render_result->valid_until_ns = next_change == change_times.end()
            ? std::numeric_limits<int64_t>::max()
            : *next_change;
        render_result->valid_from_ns = next_change == change_times.begin()
            ? std::numeric_limits<int64_t>::min()
            : *std::prev(next_change);
        render_result->content_revision = static_cast<uint64_t>(next_change - change_times.begin());

        return RAGBAG_SUBTITLE_STATUS_OK;
    }
};

} // namespace

std::unique_ptr<FfmpegBitmapSubtitleDecoder> CreateFfmpegBitmapSubtitleDecoder() {
    return std::make_unique<FfmpegBitmapSubtitleDecoderImpl>();
}

} // namespace ragbag
