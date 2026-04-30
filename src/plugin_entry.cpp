// Copyright (c) 2026
// SPDX-License-Identifier: MIT

#include "ffmpeg_subtitle_provider.h"

#include <cstring>
#include <memory>

struct RagbagSubtitleProviderV0 {
    std::unique_ptr<ragbag::FfmpegSubtitleProvider> impl;
};

namespace {

RagbagSubtitleHostApiV0 g_host = {};

RagbagSubtitleProviderDescriptorV0 const g_provider_descriptor = {
    sizeof(RagbagSubtitleProviderDescriptorV0),
    "ragbag.ffmpeg.subtitle",
    "Ragbag FFmpeg Subtitle Provider",
    "Ragbag/FFmpeg",
    ".sup;.pgs;.idx;.sub;.mks;.mkv;.mp4;.webm",
    "hdmv_pgs_subtitle;dvd_subtitle;dvb_subtitle;xsub",
    RAGBAG_SUBTITLE_PROVIDER_CAP_FILE_INPUT
        | RAGBAG_SUBTITLE_PROVIDER_CAP_BITMAP_OUTPUT
        | RAGBAG_SUBTITLE_PROVIDER_CAP_DIRTY_RECTS
        | RAGBAG_SUBTITLE_PROVIDER_CAP_PREMULTIPLIED_BGRA8
};

void Log(RagbagSubtitleLogLevelV0 level, char const *message) {
    if (g_host.log)
        g_host.log(g_host.user_data, level, message);
}

uint32_t GetProviderCount() {
    return 1;
}

RagbagSubtitleProviderDescriptorV0 const *GetProviderDescriptor(uint32_t index) {
    return index == 0 ? &g_provider_descriptor : nullptr;
}

int32_t CreateProvider(char const *provider_id, RagbagSubtitleProviderV0 **out_provider) {
    if (!out_provider)
        return RAGBAG_SUBTITLE_STATUS_INVALID_ARGUMENT;
    *out_provider = nullptr;
    if (!provider_id || std::strcmp(provider_id, g_provider_descriptor.provider_id) != 0)
        return RAGBAG_SUBTITLE_STATUS_INVALID_ARGUMENT;

    auto provider = std::make_unique<RagbagSubtitleProviderV0>();
    provider->impl = ragbag::CreateFfmpegSubtitleProvider();
    if (!provider->impl)
        return RAGBAG_SUBTITLE_STATUS_OPEN_FAILED;

    *out_provider = provider.release();
    return RAGBAG_SUBTITLE_STATUS_OK;
}

void DestroyProvider(RagbagSubtitleProviderV0 *provider) {
    delete provider;
}

char const *GetLastError(RagbagSubtitleProviderV0 *provider) {
    if (!provider || !provider->impl)
        return "Invalid provider.";
    return provider->impl->LastError();
}

int32_t OpenFile(RagbagSubtitleProviderV0 *provider, char const *path_utf8, RagbagSubtitleVideoInfoV0 const *video_hint) {
    if (!provider || !provider->impl)
        return RAGBAG_SUBTITLE_STATUS_INVALID_ARGUMENT;
    return provider->impl->OpenFile(path_utf8, video_hint);
}

int32_t RenderOverlay(RagbagSubtitleProviderV0 *provider, RagbagSubtitleRenderRequestV0 const *request, RagbagSubtitleOverlayTargetV0 *target) {
    if (!provider || !provider->impl)
        return RAGBAG_SUBTITLE_STATUS_INVALID_ARGUMENT;
    return provider->impl->RenderOverlay(request, target);
}

} // namespace

RAGBAG_SUBTITLE_PLUGIN_EXPORT int32_t ragbag_subtitle_plugin_init_v0(
    RagbagSubtitleHostApiV0 const *host,
    RagbagSubtitlePluginApiV0 *plugin) {
    if (!plugin || plugin->struct_size < sizeof(RagbagSubtitlePluginApiV0))
        return RAGBAG_SUBTITLE_STATUS_INVALID_ARGUMENT;
    if (host && host->struct_size >= sizeof(RagbagSubtitleHostApiV0))
        g_host = *host;
    if (host && host->api_version != RAGBAG_SUBTITLE_PLUGIN_API_VERSION)
        return RAGBAG_SUBTITLE_STATUS_UNSUPPORTED_API;

    auto const output_size = plugin->struct_size;
    std::memset(plugin, 0, output_size);
    plugin->struct_size = sizeof(RagbagSubtitlePluginApiV0);
    plugin->api_version = RAGBAG_SUBTITLE_PLUGIN_API_VERSION;
    plugin->plugin_id = "org.ragbag.subtitle.ffmpeg";
    plugin->plugin_name = "Ragbag Subtitle FFmpeg Provider";
    plugin->plugin_version = "0.1.0-dev";
    plugin->plugin_license = "MIT for plugin glue; FFmpeg license depends on the linked FFmpeg build";
    plugin->get_provider_count = GetProviderCount;
    plugin->get_provider_descriptor = GetProviderDescriptor;
    plugin->create_provider = CreateProvider;
    plugin->destroy_provider = DestroyProvider;
    plugin->get_last_error = GetLastError;
    plugin->open_file = OpenFile;
    plugin->render_overlay = RenderOverlay;

    Log(RAGBAG_SUBTITLE_LOG_INFO, "Initialized Ragbag FFmpeg subtitle provider plugin.");
    return RAGBAG_SUBTITLE_STATUS_OK;
}
