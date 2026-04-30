// Copyright (c) 2026
// SPDX-License-Identifier: MIT

#pragma once

#include "ragbag/subtitle_plugin_api.h"

#include <memory>
#include <string>

namespace ragbag {

class FfmpegSubtitleProvider;

std::unique_ptr<FfmpegSubtitleProvider> CreateFfmpegSubtitleProvider();

class FfmpegSubtitleProvider {
public:
    virtual ~FfmpegSubtitleProvider() = default;
    virtual char const *LastError() const = 0;
    virtual int OpenFile(char const *path_utf8, RagbagSubtitleVideoInfoV0 const *video_hint) = 0;
    virtual int RenderOverlay(RagbagSubtitleRenderRequestV0 const *request, RagbagSubtitleOverlayTargetV0 *target) = 0;
};

} // namespace ragbag
