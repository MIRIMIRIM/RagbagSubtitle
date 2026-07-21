// Copyright (c) 2026
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "ragbag/subtitle_plugin_api.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <iterator>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

using InitFunction = int32_t (*)(RagbagSubtitleHostApiV1 const*, RagbagSubtitlePluginApiV1*);

void Log(void*, RagbagSubtitleLogLevelV1 level, char const *message) {
	std::cerr << "[plugin " << static_cast<int>(level) << "] " << (message ? message : "") << '\n';
}

#ifdef _WIN32
using LibraryHandle = HMODULE;

LibraryHandle OpenLibrary(char const *path) {
	return LoadLibraryA(path);
}

InitFunction ResolveInit(LibraryHandle library) {
	return reinterpret_cast<InitFunction>(GetProcAddress(library, "ragbag_subtitle_decoder_init_v1"));
}

void CloseLibrary(LibraryHandle library) {
	if (library)
		FreeLibrary(library);
}
#else
using LibraryHandle = void*;

LibraryHandle OpenLibrary(char const *path) {
	return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}

InitFunction ResolveInit(LibraryHandle library) {
	return reinterpret_cast<InitFunction>(dlsym(library, "ragbag_subtitle_decoder_init_v1"));
}

void CloseLibrary(LibraryHandle library) {
	if (library)
		dlclose(library);
}
#endif

} // namespace

int main(int argc, char **argv) {
	if (argc != 2) {
		std::cerr << "usage: ragbag_subtitle_probe <plugin-path>\n";
		return 2;
	}

	auto library = OpenLibrary(argv[1]);
	if (!library) {
		std::cerr << "failed to load plugin\n";
		return 1;
	}

	auto init = ResolveInit(library);
	if (!init) {
		std::cerr << "plugin does not export ragbag_subtitle_decoder_init_v1\n";
		CloseLibrary(library);
		return 1;
	}

	RagbagSubtitleHostApiV1 host = {};
	host.struct_size = sizeof(host);
	host.api_version = RAGBAG_SUBTITLE_DECODER_API_VERSION;
	host.log = Log;

	RagbagSubtitlePluginApiV1 plugin = {};
	plugin.struct_size = sizeof(plugin);
	auto status = init(&host, &plugin);
	if (status != RAGBAG_SUBTITLE_STATUS_OK) {
		std::cerr << "plugin init failed: " << status << '\n';
		CloseLibrary(library);
		return 1;
	}

	std::cout << "plugin.id=" << (plugin.plugin_id ? plugin.plugin_id : "") << '\n';
	std::cout << "plugin.name=" << (plugin.plugin_name ? plugin.plugin_name : "") << '\n';
	std::cout << "plugin.version=" << (plugin.plugin_version ? plugin.plugin_version : "") << '\n';
	uint32_t const count = plugin.get_decoder_count ? plugin.get_decoder_count() : 0;
	std::cout << "decoder.count=" << count << '\n';
	bool advertises_vobsub = false;
	for (uint32_t index = 0; index < count; ++index) {
		auto const *descriptor = plugin.get_decoder_descriptor(index);
		if (!descriptor)
			continue;
		std::cout << "decoder[" << index << "].id=" << (descriptor->decoder_id ? descriptor->decoder_id : "") << '\n';
		std::cout << "decoder[" << index << "].codecs=" << (descriptor->codec_ids_semicolon ? descriptor->codec_ids_semicolon : "") << '\n';
		advertises_vobsub = advertises_vobsub
			|| (descriptor->codec_ids_semicolon
				&& std::strstr(descriptor->codec_ids_semicolon, "dvd-subtitle"));
	}
	if (!advertises_vobsub) {
		std::cerr << "decoder descriptor does not advertise dvd-subtitle\n";
		CloseLibrary(library);
		return 1;
	}

	if (count > 0) {
		auto const *descriptor = plugin.get_decoder_descriptor(0);
		RagbagSubtitleDecoderV1 *decoder = nullptr;
		status = plugin.create_decoder(descriptor->decoder_id, &decoder);
		RagbagSubtitleStreamInfoV1 stream = {};
		stream.struct_size = sizeof(stream);
		stream.codec_id = "hdmv-pgs";
		if (status == RAGBAG_SUBTITLE_STATUS_OK)
			status = plugin.begin_stream(decoder, &stream);

		// A 3x2 canvas with a 2x2 single-color PGS object, followed by a
		// zero-object presentation which clears it at two seconds.
		static uint8_t const pcs[] = {
			0x16, 0x00, 0x13,
			0x00, 0x03, 0x00, 0x02, 0x10, 0x00, 0x00, 0x80,
			0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		};
		static uint8_t const palette[] = {
			0x14, 0x00, 0x07, 0x00, 0x00, 0x01, 0xeb, 0x80, 0x80, 0xff
		};
		static uint8_t const object[] = {
			0x15, 0x00, 0x13,
			0x00, 0x00, 0x00, 0xc0, 0x00, 0x00, 0x0c,
			0x00, 0x02, 0x00, 0x02,
			0x01, 0x01, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00
		};
		static uint8_t const display_end[] = { 0x80, 0x00, 0x00 };
		static uint8_t const clear_pcs[] = {
			0x16, 0x00, 0x0b,
			0x00, 0x03, 0x00, 0x02, 0x10,
			0x00, 0x01, 0x00, 0x00, 0x00, 0x00
		};
		auto push = [&](int64_t pts_ns, uint8_t const *data, size_t size,
		                uint32_t flags = RAGBAG_SUBTITLE_PACKET_FLAG_NONE) {
			if (status != RAGBAG_SUBTITLE_STATUS_OK)
				return;
			RagbagSubtitlePacketV1 packet = {};
			packet.struct_size = sizeof(packet);
			packet.pts_ns = pts_ns;
			packet.dts_ns = RAGBAG_SUBTITLE_TIMESTAMP_UNKNOWN;
			packet.flags = flags;
			packet.payload = data;
			packet.payload_size = size;
			status = plugin.push_packet(decoder, &packet);
		};
		push(1000000000, pcs, sizeof(pcs));
		push(1000000000, palette, sizeof(palette));
		push(1000000000, object, sizeof(object));
		push(1000000000, display_end, sizeof(display_end));
		push(2000000000, clear_pcs, sizeof(clear_pcs));
		push(2000000000, display_end, sizeof(display_end));
		push(3000000000, pcs, sizeof(pcs));
		push(3000000000, palette, sizeof(palette));
		push(3000000000, object, sizeof(object));
		push(3000000000, display_end, sizeof(display_end));
		// A discontinuity must both end the decoded timeline at four seconds
		// and reset FFmpeg's retained PGS presentation/object state.
		push(4000000000, palette, sizeof(palette), RAGBAG_SUBTITLE_PACKET_FLAG_DISCONTINUITY);
		if (status == RAGBAG_SUBTITLE_STATUS_OK)
			status = plugin.end_stream(decoder);

		uint8_t invalid_pixels[24];
		std::fill(std::begin(invalid_pixels), std::end(invalid_pixels), uint8_t{0x5a});
		RagbagSubtitleRenderTargetV1 invalid_target = {};
		invalid_target.struct_size = sizeof(invalid_target);
		invalid_target.plane0 = invalid_pixels;
		invalid_target.stride0 = 4;
		invalid_target.width = 3;
		invalid_target.height = 2;
		RagbagSubtitleRenderResultV1 invalid_result = {};
		invalid_result.struct_size = sizeof(invalid_result);
		auto const invalid_status = status == RAGBAG_SUBTITLE_STATUS_OK
			? plugin.render_at(decoder, 1500000000, &invalid_target, &invalid_result)
			: status;
		bool const invalid_target_untouched = std::all_of(
			std::begin(invalid_pixels), std::end(invalid_pixels),
			[](uint8_t value) { return value == 0x5a; });

		uint8_t pixels[24];
		std::fill(std::begin(pixels), std::end(pixels), uint8_t{0x7d});
		RagbagSubtitleRenderTargetV1 target = {};
		target.struct_size = sizeof(target);
		target.plane0 = pixels;
		target.stride0 = 12;
		target.width = 3;
		target.height = 2;
		RagbagSubtitleRenderResultV1 result = {};
		result.struct_size = sizeof(result);
		if (status == RAGBAG_SUBTITLE_STATUS_OK)
			status = plugin.render_at(decoder, 1500000000, &target, &result);

		bool visible_pixels_are_opaque = true;
		bool transparent_column_is_clear = true;
		for (int y = 0; y < 2; ++y) {
			for (int x = 0; x < 2; ++x) {
				auto const *pixel = pixels + y * 12 + x * 4;
				visible_pixels_are_opaque = visible_pixels_are_opaque
					&& pixel[3] == 0xff
					&& (pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 0);
			}
			auto const *transparent = pixels + y * 12 + 8;
			transparent_column_is_clear = transparent_column_is_clear
				&& std::all_of(transparent, transparent + 4, [](uint8_t value) { return value == 0; });
		}

		uint8_t scaled_pixels[16];
		std::fill(std::begin(scaled_pixels), std::end(scaled_pixels), uint8_t{0x7d});
		RagbagSubtitleRenderTargetV1 scaled_target = {};
		scaled_target.struct_size = sizeof(scaled_target);
		scaled_target.plane0 = scaled_pixels;
		scaled_target.stride0 = 8;
		scaled_target.width = 2;
		scaled_target.height = 2;
		RagbagSubtitleRenderResultV1 scaled_result = {};
		scaled_result.struct_size = sizeof(scaled_result);
		if (status == RAGBAG_SUBTITLE_STATUS_OK)
			status = plugin.render_at(decoder, 1500000000, &scaled_target, &scaled_result);
		bool scaled_pixels_are_opaque = true;
		for (size_t offset = 3; offset < std::size(scaled_pixels); offset += 4)
			scaled_pixels_are_opaque = scaled_pixels_are_opaque && scaled_pixels[offset] == 0xff;

		std::fill(std::begin(pixels), std::end(pixels), uint8_t{0x7d});
		RagbagSubtitleRenderResultV1 clear_result = {};
		clear_result.struct_size = sizeof(clear_result);
		if (status == RAGBAG_SUBTITLE_STATUS_OK)
			status = plugin.render_at(decoder, 2000000000, &target, &clear_result);
		bool const cleared_pixels_are_zero = std::all_of(
			std::begin(pixels), std::end(pixels),
			[](uint8_t value) { return value == 0; });

		std::fill(std::begin(pixels), std::end(pixels), uint8_t{0x7d});
		RagbagSubtitleRenderResultV1 redisplayed_result = {};
		redisplayed_result.struct_size = sizeof(redisplayed_result);
		if (status == RAGBAG_SUBTITLE_STATUS_OK)
			status = plugin.render_at(decoder, 3500000000, &target, &redisplayed_result);

		std::fill(std::begin(pixels), std::end(pixels), uint8_t{0x7d});
		RagbagSubtitleRenderResultV1 discontinuity_result = {};
		discontinuity_result.struct_size = sizeof(discontinuity_result);
		if (status == RAGBAG_SUBTITLE_STATUS_OK)
			status = plugin.render_at(decoder, 4500000000, &target, &discontinuity_result);
		bool const discontinuity_pixels_are_zero = std::all_of(
			std::begin(pixels), std::end(pixels),
			[](uint8_t value) { return value == 0; });

		if (plugin.destroy_decoder)
			plugin.destroy_decoder(decoder);
		if (invalid_status != RAGBAG_SUBTITLE_STATUS_RENDER_FAILED || !invalid_target_untouched
			|| status != RAGBAG_SUBTITLE_STATUS_OK
			|| !visible_pixels_are_opaque || !transparent_column_is_clear
			|| result.has_visible_content == 0 || result.authored_width != 3 || result.authored_height != 2
			|| scaled_result.has_visible_content == 0 || !scaled_pixels_are_opaque
			|| clear_result.has_visible_content != 0 || !cleared_pixels_are_zero
			|| redisplayed_result.has_visible_content == 0
			|| discontinuity_result.has_visible_content != 0 || !discontinuity_pixels_are_zero) {
			std::cerr << "decoder lifecycle smoke test failed: status=" << status
				<< " invalid_status=" << invalid_status
				<< " invalid_untouched=" << invalid_target_untouched
				<< " identity_visible=" << visible_pixels_are_opaque
				<< " transparent_clear=" << transparent_column_is_clear
				<< " scaled_visible=" << scaled_pixels_are_opaque
				<< " clear_visible=" << clear_result.has_visible_content
				<< " clear_zero=" << cleared_pixels_are_zero
				<< " redisplayed_visible=" << redisplayed_result.has_visible_content
				<< " discontinuity_visible=" << discontinuity_result.has_visible_content
				<< " discontinuity_zero=" << discontinuity_pixels_are_zero
				<< '\n';
			CloseLibrary(library);
			return 1;
		}
		std::cout << "decoder.smoke=ok\n";

		RagbagSubtitleDecoderV1 *vobsub_decoder = nullptr;
		auto vobsub_status = plugin.create_decoder(descriptor->decoder_id, &vobsub_decoder);
		static char const vobsub_codec_private[] =
			"size: 720x480\n"
			"palette: 000000, ff0000, 00ff00, 0000ff, ffffff, 808080, 800000, 008000,"
			" 000080, 808000, 800080, 008080, c0c0c0, ff8080, 80ff80, 8080ff\n";
		RagbagSubtitleStreamInfoV1 vobsub_stream = {};
		vobsub_stream.struct_size = sizeof(vobsub_stream);
		vobsub_stream.codec_id = "dvd-subtitle";
		vobsub_stream.codec_private = reinterpret_cast<uint8_t const *>(vobsub_codec_private);
		vobsub_stream.codec_private_size = sizeof(vobsub_codec_private) - 1;
		if (vobsub_status == RAGBAG_SUBTITLE_STATUS_OK)
			vobsub_status = plugin.begin_stream(vobsub_decoder, &vobsub_stream);

		// Complete raw DVD SPU after the host has removed the IDX/SUB MPEG-PS
		// envelope. It contains one opaque red 2x2 bitmap displayed for 1024 ms.
		static uint8_t const vobsub_spu[] = {
			0x00, 0x24, 0x00, 0x06, 0x90, 0x90,
			0x00, 0x00, 0x00, 0x1e,
			0x03, 0x00, 0x10,
			0x04, 0x00, 0xf0,
			0x05, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01,
			0x06, 0x00, 0x04, 0x00, 0x05,
			0x01, 0xff,
			0x00, 0x5a, 0x00, 0x1e, 0x02, 0xff
		};
		RagbagSubtitlePacketV1 vobsub_packet = {};
		vobsub_packet.struct_size = sizeof(vobsub_packet);
		vobsub_packet.pts_ns = 1000000000;
		vobsub_packet.dts_ns = RAGBAG_SUBTITLE_TIMESTAMP_UNKNOWN;
		vobsub_packet.payload = vobsub_spu;
		vobsub_packet.payload_size = sizeof(vobsub_spu);
		if (vobsub_status == RAGBAG_SUBTITLE_STATUS_OK)
			vobsub_status = plugin.push_packet(vobsub_decoder, &vobsub_packet);
		if (vobsub_status == RAGBAG_SUBTITLE_STATUS_OK)
			vobsub_status = plugin.end_stream(vobsub_decoder);

		std::vector<uint8_t> vobsub_pixels(720 * 480 * 4, uint8_t{0x7d});
		RagbagSubtitleRenderTargetV1 vobsub_target = {};
		vobsub_target.struct_size = sizeof(vobsub_target);
		vobsub_target.plane0 = vobsub_pixels.data();
		vobsub_target.stride0 = 720 * 4;
		vobsub_target.width = 720;
		vobsub_target.height = 480;
		RagbagSubtitleRenderResultV1 vobsub_result = {};
		vobsub_result.struct_size = sizeof(vobsub_result);
		if (vobsub_status == RAGBAG_SUBTITLE_STATUS_OK) {
			vobsub_status = plugin.render_at(
				vobsub_decoder, 1500000000, &vobsub_target, &vobsub_result);
		}

		bool vobsub_red_bitmap = true;
		for (int y = 0; y < 2; ++y) {
			for (int x = 0; x < 2; ++x) {
				auto const *pixel = vobsub_pixels.data()
					+ static_cast<size_t>(y * vobsub_target.stride0 + x * 4);
				vobsub_red_bitmap = vobsub_red_bitmap
					&& pixel[0] == 0x00 && pixel[1] == 0x00
					&& pixel[2] == 0xff && pixel[3] == 0xff;
			}
		}
		auto const *outside_vobsub = vobsub_pixels.data() + 2 * 4;
		bool const vobsub_background_clear = std::all_of(
			outside_vobsub, outside_vobsub + 4,
			[](uint8_t value) { return value == 0; });
		if (plugin.destroy_decoder)
			plugin.destroy_decoder(vobsub_decoder);

		if (vobsub_status != RAGBAG_SUBTITLE_STATUS_OK
			|| vobsub_result.has_visible_content == 0
			|| vobsub_result.authored_width != 720 || vobsub_result.authored_height != 480
			|| vobsub_result.valid_until_ns != 2024000000
			|| !vobsub_red_bitmap || !vobsub_background_clear) {
			std::cerr << "VobSub smoke test failed: status=" << vobsub_status
				<< " visible=" << vobsub_result.has_visible_content
				<< " canvas=" << vobsub_result.authored_width << 'x' << vobsub_result.authored_height
				<< " valid_until=" << vobsub_result.valid_until_ns
				<< " red=" << vobsub_red_bitmap
				<< " background_clear=" << vobsub_background_clear
				<< '\n';
			CloseLibrary(library);
			return 1;
		}
		std::cout << "decoder.vobsub.smoke=ok\n";
	}

	CloseLibrary(library);
	return count > 0 ? 0 : 1;
}
