# RagbagSubtitle

RagbagSubtitle is a development playground for dynamic subtitle providers intended to be loaded by Aegisub through its native library loader. The first provider is a generic FFmpeg/libavformat subtitle provider. It is not PGS-specific: it declares the file extensions and codec families it can attempt, then demuxes and decodes through FFmpeg.

The C API is intentionally versioned as `v0`. It is allowed to change aggressively while the Aegisub host side and the plugin mature.

## Current shape

- One dynamic library target: `ragbag-subtitle-ffmpeg`.
- One provider descriptor: `ragbag.ffmpeg.subtitle`.
- Input: file-backed subtitles through libavformat.
- Decode: libavcodec subtitle decoders.
- Output: BGRA8 premultiplied overlay rects rendered into a host-provided target.
- Initial target formats: PGS/SUP and other FFmpeg bitmap subtitle formats as they become wired up.

## Build

The repository uses a vcpkg manifest with minimal FFmpeg features:

```json
ffmpeg[avcodec,avformat]
```

On this machine the expected vcpkg root is `F:/vcpkg`. The Windows preset uses
`x64-windows-static` so the FFmpeg libraries are statically linked into the
plugin DLL. The runtime artifact is therefore intended to be a single plugin DLL
plus system DLLs already provided by Windows, not a bundle of `avcodec-*.dll`,
`avformat-*.dll`, and `avutil-*.dll`.

```powershell
$cmake = 'C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
& $cmake --preset windows-vcpkg
& $cmake --build --preset windows-vcpkg --config RelWithDebInfo
```

The installed plugin is intended to live under Aegisub's executable-relative `subtitle_plugins` directory, matching the existing `native_library` app-local loading style.

```powershell
Copy-Item build/windows-vcpkg/RelWithDebInfo/ragbag-subtitle-ffmpeg.dll `
  F:/GitHub/Aegisub_wangqr/build-relwithdebinfo-x64/RelWithDebInfo/subtitle_plugins/
```

You can verify that the plugin does not depend on FFmpeg DLLs with:

```powershell
dumpbin /dependents build/windows-vcpkg/RelWithDebInfo/ragbag-subtitle-ffmpeg.dll
```

The expected non-CRT dependencies are Windows system libraries such as
`Secur32.dll`, `ncrypt.dll`, `CRYPT32.dll`, `WS2_32.dll`, `ole32.dll`,
`USER32.dll`, and `KERNEL32.dll`.

## Licensing note

The plugin glue is MIT-licensed, but the FFmpeg build installed by vcpkg declares
`LGPL-2.1-or-later`. Static linking can impose additional LGPL obligations for
redistribution, including providing license notices, FFmpeg source or written
source offer as applicable, and a practical way for recipients to relink or
replace the LGPL-covered components. This repository currently treats the static
single-DLL build as a local development artifact; release packaging must make an
explicit licensing decision before shipping binaries.

## Provider model

A plugin does not need to register one factory per subtitle format. A single provider may declare multiple extensions/codecs and dispatch internally. Register separate provider descriptors only when the host should expose distinct user choices, when dependencies differ, or when lifecycle/rendering behavior differs enough to matter.

## HDR and color

The plugin should decode subtitle data and provide color metadata when available. HDR tone mapping, gamut mapping, subtitle brightness policy, and compose-before/after-tonemap decisions belong in the Aegisub renderer/compositor layer because that layer knows the video frame, output target, and user settings.
