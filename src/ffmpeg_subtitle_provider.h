// Copyright (c) 2026
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "ragbag/subtitle_plugin_api.h"

#include <memory>
#include <string>

namespace ragbag {

class FfmpegBitmapSubtitleDecoder;

std::unique_ptr<FfmpegBitmapSubtitleDecoder> CreateFfmpegBitmapSubtitleDecoder();

class FfmpegBitmapSubtitleDecoder {
public:
    virtual ~FfmpegBitmapSubtitleDecoder() = default;
    virtual char const *LastError() const = 0;
    virtual int BeginStream(RagbagSubtitleStreamInfoV1 const *stream) = 0;
    virtual int PushPacket(RagbagSubtitlePacketV1 const *packet) = 0;
    virtual int EndStream() = 0;
    virtual int RenderAt(int64_t time_ns, RagbagSubtitleRenderTargetV1 *target, RagbagSubtitleRenderResultV1 *result) = 0;
};

} // namespace ragbag
