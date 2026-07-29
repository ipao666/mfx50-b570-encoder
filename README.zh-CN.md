# MFX50 B570 Encoder

[English](README.md)

MFX50 B570 Encoder 是面向 Intel B570 硬件的 C/C++ 视频转码 SDK 与参考实现。项目提供实时处理流水线、自适应 QP 与 ROI 策略模块、oneVPL 集成、命令行诊断工具和验证测试。

## 特性

- 通过 oneVPL 将 H.264 输入转码为 H.265/HEVC
- 面向 B570 的多路实时转码能力
- 自适应 QP、MBQP、ROI、场景分析和质量保护策略
- 供集成使用的 C/C++ 公开 API
- JSON 配置、命令行示例、打包脚本和测试

## 运行环境

- Linux x86_64
- Intel B570 GPU，以及可用的 DRM render node
- oneVPL 运行时与开发库（`vpl` 或 `mfx-gen`）
- libva 和 libva-drm 开发库
- CMake 3.16 或更高版本，以及支持 C++17 的编译器

可选的 RTSP/UDP 示例还需要 FFmpeg 开发库。

## 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

如果目标机器上的 B570 使用不同的 render node，请在运行示例前设置 `DEVICE`：

```bash
export DEVICE=/dev/dri/renderD129
```

## 快速开始

仓库提供基础转码示例，以及位于 `examples/b570_fastpath/` 的 B570 运行脚本。

```bash
export LD_LIBRARY_PATH="$PWD/build:$LD_LIBRARY_PATH"
./build/bench_real_45_files \
  video_list_1.txt 0 65536 1 onevpl /tmp/mfx50_out_ \
  none target_90_ssim_guard 60 120 0 "" \
  "width=1280,height=720,fps_num=25,fps_den=1"
```

更多信息请参阅：

- [视频压缩流程](docs/B570_VIDEO_COMPRESSION_FLOW.md)
- [当前限制与验证结果](docs/B570_CURRENT_LIMITS.md)
- [后端说明](docs/BACKEND_GUIDE.md)

## 目录结构

```text
include/       公开头文件
src/           运行时、策略、算法和后端实现
examples/      集成示例与 B570 运行脚本
tools/         诊断与转码命令行工具
tests/         C/C++ 测试程序
configs/       运行配置示例
docs/          API、算法与运行文档
packaging/     SDK 打包辅助脚本
```

## 说明

本仓库仅包含源码，不含测试视频、生成媒体、预编译二进制或厂商运行时库。硬件能力和画质结果取决于驱动、oneVPL 实现、输入视频以及运行配置。

## 许可证

本项目采用 [MIT License](LICENSE)。
