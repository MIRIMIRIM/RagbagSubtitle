# RagbagSubtitle

RagbagSubtitle is a packet-backed bitmap subtitle decoder loaded by Aegisub for its secondary subtitle strip. Aegisub owns files, Matroska containers, track selection, demuxing, timestamps, and packet delivery. Ragbag owns only codec decode state, the decoded bitmap timeline, and rendering into host-provided storage.

The C API is intentionally versioned as `v1`. It remains an internal contract shared with Aegisub and may change while the two sides mature together.

## Current shape

- One dynamic library target: `ragbag-subtitle-ffmpeg`.
- One FFmpeg bitmap decoder descriptor advertising `hdmv-pgs` and `dvd-subtitle`.
- Input: host-demuxed PGS segments or complete raw DVD/VobSub SPU packets with
  nanosecond timestamps. External `.idx` / `.sub` file pairs remain host-owned.
- Decode: the libavcodec PGS and DVD subtitle decoders; libavformat is deliberately not linked
  (libavutil remains a libavcodec dependency).
- Output: a fully overwritten host-owned BGRA8 premultiplied target.
- Scope: Aegisub secondary subtitles only. This is not a document or primary-subtitle renderer API.

## Build

The repository uses a vcpkg manifest with minimal FFmpeg features:

```json
ffmpeg[avcodec]
```

On this machine the expected vcpkg root is `F:/vcpkg`. The Windows preset uses
`x64-windows-static` so the FFmpeg libraries are statically linked into the
plugin DLL. The runtime artifact is therefore intended to be a single plugin DLL
plus system DLLs already provided by Windows, not a bundle of `avcodec-*.dll`
and `avutil-*.dll`.

```powershell
$cmake = 'C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
& $cmake --preset windows-vcpkg
& $cmake --build --preset windows-vcpkg --config RelWithDebInfo
```

For a preinstalled package without pkg-config, set `RAGBAG_FFMPEG_ROOT` to the
package root containing `include/libavcodec` and `lib` (or `lib64`). CMake
resolves the platform-native `avcodec` and `avutil` library
files explicitly and fails during configuration if any are missing. A broad
`find_package(FFMPEG)` fallback is intentionally not used because such modules
may silently add libavformat and break the decoder-only boundary.

The installed plugin is intended to live under Aegisub's executable-relative `runtimes` directory, matching the existing `native_library` app-local loading style.

```powershell
Copy-Item build/windows-vcpkg/RelWithDebInfo/ragbag-subtitle-ffmpeg.dll `
  F:/GitHub/Aegisub_wangqr/build-dir/RelWithDebInfo/runtimes/
```

You can verify that the plugin does not depend on FFmpeg DLLs with:

```powershell
dumpbin /dependents build/windows-vcpkg/RelWithDebInfo/ragbag-subtitle-ffmpeg.dll
```

When `BUILD_TESTING` is enabled, the decoder lifecycle probe is registered with
CTest and covers invalid stride rejection, transparent clearing, non-integer
canvas scaling, empty-display clearing, discontinuity handling, and a complete
VobSub SPU with IDX-style canvas and palette metadata:

```powershell
& $cmake --build --preset windows-vcpkg --config RelWithDebInfo
ctest --test-dir build/windows-vcpkg -C RelWithDebInfo --output-on-failure
```

The expected non-CRT dependencies are Windows system libraries such as
`Secur32.dll`, `ncrypt.dll`, `CRYPT32.dll`, `WS2_32.dll`, `ole32.dll`,
`USER32.dll`, and `KERNEL32.dll`.

## Licensing

The plugin glue and statically linked FFmpeg components are distributed under
**LGPL-2.1-or-later**. Static linking of FFmpeg means the combined plugin DLL is
an LGPL-covered work. Redistribution must comply with LGPL obligations:
- Provide license notices and a copy of LGPL-2.1.
- Provide FFmpeg source or a written offer for source for at least three years.
- Ensure recipients can relink or replace the LGPL-covered library components.
This repository currently treats the static single-DLL build as a local
development artifact; release packaging must make an explicit licensing decision
before shipping binaries.

## Decoder model

The v1 ABI accepts a complete packet stream before random-access rendering begins. Packet payload memory is borrowed only for the duration of `push_packet`; the decoder retains its own timeline. Sessions are single-threaded, while independent sessions may run concurrently. File extensions and container codec IDs are intentionally absent from the decoder contract. A `dvd-subtitle` packet is one complete raw DVD SPU after the host has removed the `.sub` MPEG-PS/PES envelope; its codec-private data contains the VobSub `size:` and `palette:` header lines.

### Resolution and scaling

PGS carries an authored composition resolution in its presentation composition segment. VobSub supplies it through the IDX or Matroska CodecPrivate `size:` line. The FFmpeg decoder exposes that value through its codec context and Ragbag records it with each decoded event. The host-provided fallback is used only for streams without a declared canvas. When the target differs from the authored canvas, Ragbag uses nearest-neighbour sampling so palette edges are not blurred.

## HDR and color

The v1 output is deliberately fixed to SDR-style premultiplied BGRA8 because it is consumed by Aegisub's secondary subtitle strip. HDR tone mapping, gamut mapping, and primary-video composition are outside this decoder's scope.
