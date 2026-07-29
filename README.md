# MFX50 B570 Encoder

[简体中文](README.zh-CN.md)

MFX50 B570 Encoder is a C/C++ video transcoding SDK and reference implementation for Intel B570 hardware. It provides a realtime pipeline, adaptive QP and ROI policy components, oneVPL integration, command-line probes, and validation tests.

## Features

- H.264 input to H.265/HEVC output through oneVPL
- B570-oriented multi-route realtime transcoding
- Adaptive QP, MBQP, ROI, scene analysis, and quality-guard policies
- Public C and C++ APIs for integration
- JSON configuration, CLI examples, packaging scripts, and tests

## Requirements

- Linux x86_64
- Intel B570 GPU and a usable DRM render node
- oneVPL runtime/development library (`vpl` or `mfx-gen`)
- libva and libva-drm development libraries
- CMake 3.16 or newer and a C++17 compiler

Optional RTSP/UDP demos require FFmpeg development libraries.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

If the target host exposes the B570 through a different render node, set `DEVICE` before running the examples:

```bash
export DEVICE=/dev/dri/renderD129
```

## Quick Start

The repository includes a basic transcode example and B570-oriented scripts under `examples/b570_fastpath/`.

```bash
export LD_LIBRARY_PATH="$PWD/build:$LD_LIBRARY_PATH"
./build/bench_real_45_files \
  video_list_1.txt 0 65536 1 onevpl /tmp/mfx50_out_ \
  none target_90_ssim_guard 60 120 0 "" \
  "width=1280,height=720,fps_num=25,fps_den=1"
```

See [docs/B570_VIDEO_COMPRESSION_FLOW.md](docs/B570_VIDEO_COMPRESSION_FLOW.md), [docs/B570_CURRENT_LIMITS.md](docs/B570_CURRENT_LIMITS.md), and [docs/BACKEND_GUIDE.md](docs/BACKEND_GUIDE.md) for architecture, validated constraints, and backend behavior.

## Repository Layout

```text
include/       Public headers
src/           Runtime, policy, algorithm, and backend implementation
examples/      Integration examples and B570 run scripts
tools/         Diagnostic and transcoding command-line tools
tests/         C and C++ test programs
configs/       Runtime configuration examples
docs/          API, algorithm, and operational documentation
packaging/     SDK packaging helpers
```

## Notes

This repository contains source code only. It does not include video samples, generated media, prebuilt binaries, or vendor runtime libraries. Hardware capability and quality results depend on the installed driver, oneVPL implementation, input material, and runtime configuration.

## License

Released under the [MIT License](LICENSE).
