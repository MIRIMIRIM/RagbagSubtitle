// Copyright (c) 2026
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "ragbag/subtitle_plugin_api.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

using InitFunction = int32_t (*)(RagbagSubtitleHostApiV0 const *, RagbagSubtitlePluginApiV0 *);

void HostLog(void *, RagbagSubtitleLogLevelV0 level, char const *message) {
    std::cerr << "[plugin " << static_cast<int>(level) << "] " << (message ? message : "") << '\n';
}

#ifdef _WIN32
std::wstring Utf8ToWide(std::string const& value) {
    if (value.empty())
        return {};
    int const needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.c_str(), -1, nullptr, 0);
    if (needed <= 0)
        return {};
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.c_str(), -1, wide.data(), needed))
        return {};
    wide.pop_back();
    return wide;
}

std::string LastLoadError() {
    DWORD const err = GetLastError();
    return "Windows error " + std::to_string(static_cast<unsigned long>(err));
}
#else
std::string LastLoadError() {
    auto const *err = dlerror();
    return err ? err : "unknown dlopen error";
}
#endif

class DynamicLibrary {
#ifdef _WIN32
    HMODULE handle = nullptr;
#else
    void *handle = nullptr;
#endif

public:
    explicit DynamicLibrary(std::string const& path) {
#ifdef _WIN32
        auto wide = Utf8ToWide(path);
        handle = wide.empty() ? nullptr : LoadLibraryW(wide.c_str());
#else
        handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
    }

    ~DynamicLibrary() {
#ifdef _WIN32
        if (handle)
            FreeLibrary(handle);
#else
        if (handle)
            dlclose(handle);
#endif
    }

    explicit operator bool() const { return handle != nullptr; }

    InitFunction ResolveInit() const {
        if (!handle)
            return nullptr;
#ifdef _WIN32
        return reinterpret_cast<InitFunction>(GetProcAddress(handle, "ragbag_subtitle_plugin_init_v0"));
#else
        return reinterpret_cast<InitFunction>(dlsym(handle, "ragbag_subtitle_plugin_init_v0"));
#endif
    }
};

void PrintDescriptor(RagbagSubtitleProviderDescriptorV0 const& descriptor) {
    std::cout << "provider.id=" << (descriptor.provider_id ? descriptor.provider_id : "") << '\n';
    std::cout << "provider.name=" << (descriptor.display_name ? descriptor.display_name : "") << '\n';
    std::cout << "provider.debug=" << (descriptor.debug_name ? descriptor.debug_name : "") << '\n';
    std::cout << "provider.extensions=" << (descriptor.extensions_semicolon ? descriptor.extensions_semicolon : "") << '\n';
    std::cout << "provider.codecs=" << (descriptor.codec_names_semicolon ? descriptor.codec_names_semicolon : "") << '\n';
    std::cout << "provider.capabilities=" << descriptor.capabilities << '\n';
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "usage: ragbag_subtitle_probe <plugin-path> [subtitle-file]\n";
        return 2;
    }

    DynamicLibrary library(argv[1]);
    if (!library) {
        std::cerr << "failed to load plugin: " << LastLoadError() << '\n';
        return 1;
    }

    auto init = library.ResolveInit();
    if (!init) {
        std::cerr << "plugin does not export ragbag_subtitle_plugin_init_v0\n";
        return 1;
    }

    RagbagSubtitleHostApiV0 host = {};
    host.struct_size = sizeof(host);
    host.api_version = RAGBAG_SUBTITLE_PLUGIN_API_VERSION;
    host.log = HostLog;

    RagbagSubtitlePluginApiV0 plugin = {};
    plugin.struct_size = sizeof(plugin);
    auto status = init(&host, &plugin);
    if (status != RAGBAG_SUBTITLE_STATUS_OK) {
        std::cerr << "plugin init failed: " << status << '\n';
        return 1;
    }

    std::cout << "plugin.id=" << (plugin.plugin_id ? plugin.plugin_id : "") << '\n';
    std::cout << "plugin.name=" << (plugin.plugin_name ? plugin.plugin_name : "") << '\n';
    std::cout << "plugin.version=" << (plugin.plugin_version ? plugin.plugin_version : "") << '\n';
    std::cout << "plugin.license=" << (plugin.plugin_license ? plugin.plugin_license : "") << '\n';

    uint32_t const count = plugin.get_provider_count ? plugin.get_provider_count() : 0;
    std::cout << "provider.count=" << count << '\n';
    for (uint32_t i = 0; i < count; ++i) {
        auto const *descriptor = plugin.get_provider_descriptor ? plugin.get_provider_descriptor(i) : nullptr;
        if (descriptor)
            PrintDescriptor(*descriptor);
    }

    if (argc >= 3 && count > 0 && plugin.create_provider && plugin.open_file && plugin.destroy_provider) {
        auto const *descriptor = plugin.get_provider_descriptor(0);
        RagbagSubtitleProviderV0 *provider = nullptr;
        status = plugin.create_provider(descriptor->provider_id, &provider);
        if (status == RAGBAG_SUBTITLE_STATUS_OK) {
            RagbagSubtitleVideoInfoV0 video = {};
            video.struct_size = sizeof(video);
            status = plugin.open_file(provider, argv[2], &video);
            std::cout << "open.status=" << status << '\n';
            if (status != RAGBAG_SUBTITLE_STATUS_OK && plugin.get_last_error)
                std::cout << "open.error=" << plugin.get_last_error(provider) << '\n';
            plugin.destroy_provider(provider);
        }
        else {
            std::cout << "create.status=" << status << '\n';
        }
    }

    return 0;
}
