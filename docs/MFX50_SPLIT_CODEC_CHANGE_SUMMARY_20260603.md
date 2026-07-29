# MFX50 解码/编码拆分改动汇总

日期：2026-06-03

## 1. 目标

本轮改动的目标是把原来“一体式解码+编码”的文件转码接口，拆成更清楚的几层：

```text
Device + Decoder + Encoder + Policy + Transcoder
```

核心方向：

- 旧接口保留为文件转码/压测接口。
- 新业务接口走 encode-only。
- 工程师自己硬解后，可以把 GPU NV12 surface 交给 MFX50 encoder。
- MFX50 自动硬解入口也保留在 `mfx50_decoder.h`，用于 packet -> GPU NV12 surface。

换句话说，对外保留两种业务入口：

```text
入口 A：直接注入 NV12 GPU surface
  你方/业务方硬解 -> NV12 GPU surface -> mfx50_encoder_push_surface()

入口 B：MFX50 自动硬解
  H.264/H.265 packet -> mfx50_decoder_push_packet() -> mfx50_decoder_poll_surface()
  -> NV12 GPU surface -> mfx50_encoder_push_surface()
```

这里的“NV12 注入”指的是注入 GPU surface 句柄，不是裸帧字节数组。

## 2. 新增接口文件

新增公共头文件：

```text
mfx50_api.h
mfx50_device.h
mfx50_surface.h
mfx50_decoder.h
mfx50_encoder.h
mfx50_transcoder.h
```

接口职责：

| 文件 | 作用 |
| --- | --- |
| `mfx50_device.h` | 管 B570 设备、VA Display、oneVPL session 等共享上下文。 |
| `mfx50_surface.h` | 定义 `MFX50_Surface`、`MFX50_Packet`、surface 引用和释放规则。 |
| `mfx50_decoder.h` | 已实现 packet -> GPU NV12 surface 的 decoder API。 |
| `mfx50_encoder.h` | 新 encode-only API，主入口是 `mfx50_encoder_push_surface()`。 |
| `mfx50_transcoder.h` | 旧 `MFX50_RunInputList()` / `MFX50_RunSingleInput()` 兼容接口。 |

## 3. 旧接口重新归类

原来的：

```c
MFX50_RunInputList(...)
MFX50_RunSingleInput(...)
```

现在归到 `mfx50_transcoder.h`，语义是：

```text
码流文件输入
  -> 内部硬解
  -> 内部 NV12 surface
  -> 内部硬编
  -> HEVC 输出
```

它不是纯 encoder API。

旧 demo 已调整为包含：

```c
#include "mfx50_transcoder.h"
```

## 4. H.264 写死问题修复

原来 `mfx50_encoder.cpp` 里生成 `.par` 时写死：

```text
-i::h264
```

现在旧 transcoder 输入支持 H.264/H.265：

```text
xxx.h264       -> H.264
xxx.264        -> H.264
xxx.h265       -> H.265
xxx.hevc       -> H.265
h264:/path/a   -> 强制 H.264
h265:/path/b   -> 强制 H.265
hevc:/path/c   -> 强制 H.265
auto:/path     -> 按扩展名识别
```

为了兼容旧 input list，未知扩展名默认仍按 H.264 处理。

## 5. 新 Encoder API 当前状态

新增 encoder API：

```c
mfx50_encoder_default_config(...)
mfx50_encoder_get_surface_support(...)
mfx50_encoder_create(...)
mfx50_encoder_push_surface(...)
mfx50_encoder_poll_packet(...)
mfx50_encoder_flush(...)
mfx50_encoder_destroy(...)
```

当前公开支持状态：

```text
MFX50_SURFACE_ONEVPL   支持，输入 mfxFrameSurface1*
MFX50_SURFACE_VAAPI    预留，暂未实现
MFX50_SURFACE_DMABUF   预留/experimental，暂未实现
```

`mfx50_encoder_get_surface_support()` 当前返回：

```text
onevpl=1
vaapi=0
dmabuf=0
```

## 6. 外部硬解 oneVPL GPU Surface 路径

当前已经接入的主路径是：

```text
业务方硬解得到 mfxFrameSurface1*
  -> MFX50_Surface{ type = MFX50_SURFACE_ONEVPL }
  -> mfx50_encoder_push_surface()
  -> mfx50_encoder_poll_packet()
```

注意：

- `mfxFrameSurface1*` 必须来自同一个 `MFX50_Device` / oneVPL session 体系。
- 当前实现要求输入是 GPU/video-memory surface。
- `mfx50_encoder_push_surface()` 当前同步提交并等待编码完成，函数返回后不再持有传入 surface。

这不是说没有 MFX50 自动硬解接口。这里描述的是“工程侧已经硬解完成”的入口，也就是业务方手里已经有 `mfxFrameSurface1*` 时怎么交给 encoder。

MFX50 自动硬解入口保留在 `mfx50_decoder.h`：

```text
H.264/H.265 packet
  -> mfx50_decoder_push_packet()
  -> mfx50_decoder_poll_surface()
  -> MFX50_Surface
  -> mfx50_encoder_push_surface()
```

当前实现状态：`Decoder` 已接入 oneVPL decode。`mfx50_decoder_push_packet()` 不再返回 `MFX50_ERR_NOT_IMPLEMENTED`，而是累积 H.264/H.265 Annex-B packet/码流，完成 `DecodeHeader` / `DecodeFrameAsync` 后由 `mfx50_decoder_poll_surface()` 输出 NV12 oneVPL GPU surface。

## 7. 新增实现文件

新增：

```text
mfx50_codec_api.cpp
```

目前包含：

- `MFX50_Device` 基础创建/销毁。
- `MFX50_Decoder` API 壳。
- `MFX50_Encoder` API。
- oneVPL `mfxFrameSurface1*` surface encode 的最小实现。
- `MFX50_Packet` 输出队列与释放函数。

## 8. 新增文档

新增：

```text
docs/MFX50_SPLIT_CODEC_API.md
```

内容包括：

- Device 必须作为 Decoder/Encoder 共同父对象。
- 业务方自己硬解时的调用链。
- 使用 MFX50 解码时的调用链。
- Surface 类型说明。
- Surface 生命周期与同步/异步持有规则。
- Policy 仍保持独立。
- 旧 Transcoder 接口归属。

更新：

```text
docs/MFX50_ENCODER_API_AND_DEMO.md
```

现在说明它实际是 `Transcoder/Benchmark API`，不是纯 encoder。

## 9. 新增示例

新增：

```text
tools/mfx50_split_file_transcode.cpp
```

这个 demo 是完整可运行示例，用于验证：

```text
H.264/H.265 Annex-B packet
  -> MFX50 Decoder
  -> NV12 oneVPL GPU surface
  -> MFX50 Encoder
  -> HEVC packet
```

外部硬解直接注入 NV12 GPU surface 的集成方式保留在 `mfx50_encoder_push_surface()` 接口说明中，不在 SDK 包里伪造空 surface demo。

## 10. 新增测试

新增：

```text
tests/test_split_codec_headers.c
tests/test_transcoder_input_codecs.cpp
```

测试覆盖：

- 新 split headers 可以 C/C++ 编译链接。
- `Device / Decoder / Encoder` 基础创建流程。
- `mfx50_encoder_get_surface_support()` 返回当前支持矩阵。
- 旧 transcoder 输入列表能正确生成 `-i::h264` / `-i::h265`。

## 11. CMake 与打包脚本更新

更新 `CMakeLists.txt`：

- `libmfx50_encoder.so` 增加 `mfx50_codec_api.cpp`。
- `libmfx50_encoder.so` 链接 oneVPL / VAAPI 相关库。
- 安装新增头文件：

```text
mfx50_api.h
mfx50_device.h
mfx50_surface.h
mfx50_decoder.h
mfx50_encoder.h
mfx50_transcoder.h
```

更新 `packaging/make_b570_binary_sdk.sh`：

- SDK 包会复制新增头文件。
- SDK 包会复制 `docs/MFX50_SPLIT_CODEC_API.md`。
- SDK 包会复制 `docs/MFX50_CODEC_FUNCTION_REFERENCE.md`。
- SDK 包会复制 `docs/MFX50_SPLIT_CODEC_CHANGE_SUMMARY_20260603.md`。
- SDK 包会把旧 `docs/MFX50_ENCODER_API_AND_DEMO.md` 作为 `docs/MFX50_TRANSCODER_API_AND_DEMO.md` 放入包内。
- SDK 包会带完整可运行的 `mfx50_split_file_transcode` demo。

## 12. 验证结果

在台式机 `100.108.202.16` 的 `/tmp` 临时目录验证过：

```text
split_codec_headers        passed
transcoder_input_codecs     passed
mfx50_split_file_transcode  HEVC Annex-B -> Decoder -> Encoder 实测通过
```

split file transcode 复验项：

```text
date:   2026-06-04
host:   100.108.202.16 / admi-MS-7E34
input:  /tmp/mfx50_selfcheck_input.hevc
output: /tmp/mfx50_selfcheck_output.hevc

input_chunks=1
decoded_surfaces=252
encoded_packets=252
TOOL_RC=0

ffprobe:
codec_name=hevc
width=1920
height=1080
nb_read_frames=252
```

## 13. 已生成小包

本地：

```text
D:\mfx50_core_code_20260531\combination\deliverables\mfx50_split_codec_api_20260603.tar.gz
D:\mfx50_core_code_20260531\combination\deliverables\mfx50_split_codec_api_20260603.zip
```

台式机：

```text
/home/admi/combination/dist/mfx50_split_codec_api_20260603.tar.gz
/home/admi/combination/dist/mfx50_split_codec_api_20260603.zip
```

小包内容：

```text
include/
docs/
examples/transcoder_input_list.example.txt
CMakeLists.txt
README.md
```

## 14. 还没做的下一步

还需要根据工程师实际硬解输出类型继续接：

1. 如果硬解输出是 `mfxFrameSurface1*`：
   - 直接用当前 `MFX50_SURFACE_ONEVPL` 路径做真实联调。

2. 如果硬解输出是 `VASurfaceID`：
   - 继续实现 `MFX50_SURFACE_VAAPI` adapter。

3. 如果跨进程传 surface：
   - 再实现 DMA-BUF fd / stride / offset / modifier 路径。

当前最关键的待确认项：

```text
工程师硬解输出到底是 mfxFrameSurface1*，还是 VASurfaceID，还是 DMA-BUF fd。
```
