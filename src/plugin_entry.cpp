// Copyright (c) 2026
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "ffmpeg_subtitle_provider.h"

#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <new>

struct RagbagSubtitleDecoderV1 {
	std::unique_ptr<ragbag::FfmpegBitmapSubtitleDecoder> impl;
};

namespace {

RagbagSubtitleHostApiV1 g_host = {};
thread_local char g_abi_error[512] = {};

void ClearAbiError() noexcept {
	g_abi_error[0] = '\0';
}

void SetAbiError(char const *action, char const *detail) noexcept {
	std::snprintf(
		g_abi_error,
		sizeof(g_abi_error),
		"%s: %s",
		action ? action : "Ragbag subtitle decoder call failed",
		detail ? detail : "unknown error");
}

template<typename Callable>
int32_t GuardAbiCall(
	char const *action,
	RagbagSubtitleStatusV1 failure_status,
	Callable&& callable) noexcept {
	ClearAbiError();
	try {
		return callable();
	}
	catch (std::bad_alloc const&) {
		SetAbiError(action, "out of memory");
	}
	catch (std::exception const& err) {
		SetAbiError(action, err.what());
	}
	catch (...) {
		SetAbiError(action, "unknown C++ exception");
	}
	return static_cast<int32_t>(failure_status);
}

RagbagSubtitleDecoderDescriptorV1 const g_decoder_descriptor = {
	sizeof(RagbagSubtitleDecoderDescriptorV1),
	"ragbag.pgs",
	"Ragbag FFmpeg Bitmap Subtitle Decoder",
	"Ragbag/FFmpeg Bitmap",
	"hdmv-pgs;dvd-subtitle"
};

void Log(RagbagSubtitleLogLevelV1 level, char const *message) {
	if (g_host.log)
		g_host.log(g_host.user_data, level, message);
}

uint32_t GetDecoderCount() noexcept {
	return 1;
}

RagbagSubtitleDecoderDescriptorV1 const *GetDecoderDescriptor(uint32_t index) noexcept {
	return index == 0 ? &g_decoder_descriptor : nullptr;
}

int32_t CreateDecoder(char const *decoder_id, RagbagSubtitleDecoderV1 **out_decoder) noexcept {
	return GuardAbiCall("Creating subtitle decoder", RAGBAG_SUBTITLE_STATUS_OPEN_FAILED, [&]() -> int32_t {
		if (!out_decoder)
			return RAGBAG_SUBTITLE_STATUS_INVALID_ARGUMENT;
		*out_decoder = nullptr;
		if (!decoder_id || std::strcmp(decoder_id, g_decoder_descriptor.decoder_id) != 0)
			return RAGBAG_SUBTITLE_STATUS_INVALID_ARGUMENT;

		auto decoder = std::make_unique<RagbagSubtitleDecoderV1>();
		decoder->impl = ragbag::CreateFfmpegBitmapSubtitleDecoder();
		if (!decoder->impl) {
			SetAbiError("Creating subtitle decoder", "decoder factory returned no decoder");
			return RAGBAG_SUBTITLE_STATUS_OPEN_FAILED;
		}

		*out_decoder = decoder.release();
		return RAGBAG_SUBTITLE_STATUS_OK;
	});
}

void DestroyDecoder(RagbagSubtitleDecoderV1 *decoder) noexcept {
	try {
		delete decoder;
	}
	catch (std::exception const& err) {
		SetAbiError("Destroying subtitle decoder", err.what());
	}
	catch (...) {
		SetAbiError("Destroying subtitle decoder", "unknown C++ exception");
	}
}

char const *GetLastError(RagbagSubtitleDecoderV1 *decoder) noexcept {
	if (g_abi_error[0])
		return g_abi_error;
	try {
		if (!decoder || !decoder->impl)
			return "Invalid decoder.";
		return decoder->impl->LastError();
	}
	catch (std::exception const& err) {
		SetAbiError("Reading subtitle decoder error", err.what());
	}
	catch (...) {
		SetAbiError("Reading subtitle decoder error", "unknown C++ exception");
	}
	return g_abi_error;
}

int32_t BeginStream(RagbagSubtitleDecoderV1 *decoder, RagbagSubtitleStreamInfoV1 const *stream) noexcept {
	return GuardAbiCall("Opening subtitle packet stream", RAGBAG_SUBTITLE_STATUS_OPEN_FAILED, [&]() -> int32_t {
		return decoder && decoder->impl
			? decoder->impl->BeginStream(stream)
			: RAGBAG_SUBTITLE_STATUS_INVALID_ARGUMENT;
	});
}

int32_t PushPacket(RagbagSubtitleDecoderV1 *decoder, RagbagSubtitlePacketV1 const *packet) noexcept {
	return GuardAbiCall("Decoding subtitle packet", RAGBAG_SUBTITLE_STATUS_DECODE_FAILED, [&]() -> int32_t {
		return decoder && decoder->impl
			? decoder->impl->PushPacket(packet)
			: RAGBAG_SUBTITLE_STATUS_INVALID_ARGUMENT;
	});
}

int32_t EndStream(RagbagSubtitleDecoderV1 *decoder) noexcept {
	return GuardAbiCall("Finalizing subtitle packet stream", RAGBAG_SUBTITLE_STATUS_DECODE_FAILED, [&]() -> int32_t {
		return decoder && decoder->impl
			? decoder->impl->EndStream()
			: RAGBAG_SUBTITLE_STATUS_INVALID_ARGUMENT;
	});
}

int32_t RenderAt(RagbagSubtitleDecoderV1 *decoder, int64_t time_ns, RagbagSubtitleRenderTargetV1 *target, RagbagSubtitleRenderResultV1 *result) noexcept {
	return GuardAbiCall("Rendering subtitle bitmap", RAGBAG_SUBTITLE_STATUS_RENDER_FAILED, [&]() -> int32_t {
		return decoder && decoder->impl
			? decoder->impl->RenderAt(time_ns, target, result)
			: RAGBAG_SUBTITLE_STATUS_INVALID_ARGUMENT;
	});
}

} // namespace

RAGBAG_SUBTITLE_PLUGIN_EXPORT int32_t ragbag_subtitle_decoder_init_v1(
	RagbagSubtitleHostApiV1 const *host,
	RagbagSubtitlePluginApiV1 *plugin) {
	return GuardAbiCall("Initializing subtitle decoder plugin", RAGBAG_SUBTITLE_STATUS_OPEN_FAILED, [&]() -> int32_t {
		if (!plugin || plugin->struct_size < sizeof(RagbagSubtitlePluginApiV1))
			return RAGBAG_SUBTITLE_STATUS_INVALID_ARGUMENT;
		if (host) {
			if (host->struct_size < sizeof(RagbagSubtitleHostApiV1))
				return RAGBAG_SUBTITLE_STATUS_INVALID_ARGUMENT;
			if (host->api_version != RAGBAG_SUBTITLE_DECODER_API_VERSION)
				return RAGBAG_SUBTITLE_STATUS_UNSUPPORTED_API;
			g_host = *host;
		}

		auto const output_size = plugin->struct_size;
		std::memset(plugin, 0, output_size);
		plugin->struct_size = sizeof(RagbagSubtitlePluginApiV1);
		plugin->api_version = RAGBAG_SUBTITLE_DECODER_API_VERSION;
		plugin->plugin_id = "org.ragbag.subtitle.pgs";
		plugin->plugin_name = "Ragbag FFmpeg Bitmap Subtitle Decoder";
		plugin->plugin_version = "0.2.0-dev";
		plugin->plugin_license = "LGPL-2.1-or-later (plugin glue and statically linked FFmpeg)";
		plugin->get_decoder_count = GetDecoderCount;
		plugin->get_decoder_descriptor = GetDecoderDescriptor;
		plugin->create_decoder = CreateDecoder;
		plugin->destroy_decoder = DestroyDecoder;
		plugin->get_last_error = GetLastError;
		plugin->begin_stream = BeginStream;
		plugin->push_packet = PushPacket;
		plugin->end_stream = EndStream;
		plugin->render_at = RenderAt;

		Log(RAGBAG_SUBTITLE_LOG_INFO, "Initialized Ragbag packet-backed PGS/VobSub decoder.");
		return RAGBAG_SUBTITLE_STATUS_OK;
	});
}
