# MFX50 解码/编码分离 API 设计

本文说明新的接口边界。目标是把旧的一体式文件转码接口拆成：

```text
Device + Decoder + Policy + Encoder + Transcoder
```

旧接口仍可用于离线文件压测；业务实时接入优先使用 `Encoder`，直接接收外部硬解后的 NV12 GPU surface。

这里不是只保留外部硬解一种方式。新的 split API 明确保留两条入口：

```text
入口 A：工程侧已有硬解，直接注入 NV12 GPU surface
  H.264/H.265 packet -> 你方/业务方硬解 -> GPU NV12 surface -> MFX50 Encoder

入口 B：MFX50 自动硬解
  H.264/H.265 packet -> MFX50 Decoder -> GPU NV12 surface -> MFX50 Encoder
```

文档里提到 `mfxFrameSurface1* -> MFX50_Surface{ type = MFX50_SURFACE_ONEVPL }`，是在说明入口 A：当工程侧已经拿到了 oneVPL GPU surface 时，如何把它交给 encoder。入口 B 对应的是 `mfx50_decoder.h`，当前已经接入 oneVPL decode，可由 `mfx50_decoder_push_packet()` 输出 NV12 GPU surface。

注意：这里说的“注入 NV12”不是传裸帧字节数组，而是传 NV12 GPU surface 的句柄。`NV12` 是像素格式，`MFX50_SURFACE_ONEVPL` / `MFX50_SURFACE_VAAPI` / `MFX50_SURFACE_DMABUF` 才说明这帧 NV12 在哪里、怎么和 B570 encoder 共享。

## 1. 头文件归属

| 头文件 | 责任 |
| --- | --- |
| `mfx50_device.h` | 管理 B570 设备、VA Display、oneVPL session 等共享上下文。 |
| `mfx50_decoder.h` | 输入 H.264/H.265 packet，输出 NV12 GPU surface。 |
| `mfx50_encoder.h` | 输入 NV12 GPU surface，输出 HEVC/H.265 packet。 |
| `mfx50_surface.h` | 定义 surface、packet、surface 引用/释放规则。 |
| `include/mfx50_policy.h` | 保持独立，输入 features/metadata/frame，输出 `MFX50_EncodeDecision`。 |
| `mfx50_transcoder.h` | 旧 `MFX50_RunInputList()` / `MFX50_RunSingleInput()` 文件转码兼容接口。 |

## 2. Device 必须是共同父对象

`Decoder` 和 `Encoder` 都必须挂在同一个 `MFX50_Device` 下：

```c
MFX50_Device* device = NULL;
mfx50_device_create(&device_cfg, &device);

mfx50_decoder_create(device, &dec_cfg, &decoder);
mfx50_encoder_create(device, &enc_cfg, &encoder);
```

这样才能保证它们使用同一张 B570、同一个 VA Display / oneVPL device。不要让 Decoder 和 Encoder 各自偷偷创建 device，否则 decoder 输出的 GPU surface 不一定能被 encoder 零拷贝消费。

## 3. 工程侧已有硬解

如果工程师已经在自己的模块中完成硬解，业务链路应跳过 MFX50 Decoder：

```text
业务方拉流/硬解
  -> NV12 GPU surface
  -> mfx50_encoder_push_surface()
  -> mfx50_encoder_poll_packet()
```

C 调用形态：

```c
MFX50_Device* device = NULL;
MFX50_Encoder* encoder = NULL;

mfx50_device_create(&device_cfg, &device);
mfx50_encoder_create(device, &enc_cfg, &encoder);

for (;;) {
    MFX50_Surface surface = make_external_nv12_surface();
    MFX50_EncodeDecision decision = {};

    mfx50_policy_submit_features(policy_stream, &features);
    mfx50_policy_get_decision(policy_stream, &decision);

    mfx50_encoder_push_surface(encoder, &surface, &decision);

    MFX50_Packet packet = {};
    while (mfx50_encoder_poll_packet(encoder, &packet) == MFX50_OK) {
        write_packet(packet.data, packet.data_size);
        mfx50_packet_release(&packet);
    }
}
```

外部 NV12 注入时按硬解输出类型选择 surface 类型：

```text
oneVPL 硬解输出 mfxFrameSurface1*
  -> MFX50_Surface.type = MFX50_SURFACE_ONEVPL

VAAPI 硬解输出 VASurfaceID
  -> MFX50_Surface.type = MFX50_SURFACE_VAAPI

跨模块/跨进程传 DMA-BUF fd
  -> MFX50_Surface.type = MFX50_SURFACE_DMABUF
```

当前已经接入的是 `MFX50_SURFACE_ONEVPL` 的 `mfxFrameSurface1*` GPU surface 编码路径。`MFX50_SURFACE_VAAPI` 和 `MFX50_SURFACE_DMABUF` ABI 已留，编码 adapter 待接。

## 4. 使用 MFX50 自动硬解

如果需要 MFX50 负责解码：

```text
H.264/H.265 packet
  -> mfx50_decoder_push_packet()
  -> mfx50_decoder_poll_surface()
  -> policy
  -> mfx50_encoder_push_surface()
  -> mfx50_encoder_poll_packet()
```

关键是 decoder 输出的 `MFX50_Surface` 和 encoder 输入的 `MFX50_Surface` 是同一个对象语义，中间不要求转成裸帧字节数组。

当前 `mfx50_decoder.h` 已经定义自动硬解入口：

```c
mfx50_decoder_create(device, &dec_cfg, &decoder);
mfx50_decoder_push_packet(decoder, &packet);
mfx50_decoder_poll_surface(decoder, &surface);
```

实现状态：已接入 oneVPL 自动硬解。`mfx50_decoder_create()` 会准备共享 device/runtime；`mfx50_decoder_push_packet()` 会累积 H.264/H.265 Annex-B packet/码流并驱动 `DecodeHeader` / `DecodeFrameAsync`；如果 `dec_cfg.input_codec` 和 `packet.codec` 都为 `MFX50_CODEC_UNKNOWN`，会按 Annex-B 参数集自动识别 H.264/H.265；`mfx50_decoder_poll_surface()` 返回 `MFX50_SURFACE_ONEVPL` 的 NV12 GPU surface，可直接交给 `mfx50_encoder_push_surface()`。

## 5. Surface 类型

`MFX50_Surface` 支持以下类型：

| 类型 | 用途 | 当前实现状态 |
| --- | --- | --- |
| `MFX50_SURFACE_ONEVPL` | 同 oneVPL 体系传 `mfxFrameSurface1*` | 已实现：直接提交给 oneVPL encoder。要求 surface 与 encoder 使用同一个 device/session 体系。 |
| `MFX50_SURFACE_VAAPI` | 同进程 VAAPI 传 `VASurfaceID` + `VADisplay` | ABI 已定义，编码实现待接。 |
| `MFX50_SURFACE_DMABUF` | 跨进程/跨模块 DMA-BUF fd | ABI 已定义，标记 experimental。 |

第一版不要假装全部零拷贝已实现。工程集成时应先调用：

```c
MFX50_EncoderSurfaceSupport support = {};
mfx50_encoder_get_surface_support(device, &support);
```

以运行时返回值为准。

当前业务主推的 encode-only 路径是：

```text
mfxFrameSurface1* GPU NV12 surface
  -> MFX50_Surface{ type = MFX50_SURFACE_ONEVPL }
  -> mfx50_encoder_push_surface()
  -> mfx50_encoder_poll_packet()
```

这个路径要求业务方硬解输出的 `mfxFrameSurface1*` 与 MFX50 encoder 共享同一个 `MFX50_Device` / oneVPL session 体系。否则 surface 句柄不一定能被 encoder 正确消费。

完整可运行 demo：

```text
examples/split_codec/mfx50_split_file_transcode.cpp
bin/mfx50_split_file_transcode
```

该 demo 演示 MFX50 自动硬解路径。外部硬解直接注入 NV12 GPU surface 的场景需要接入业务方真实硬解输出的 `mfxFrameSurface1*`，包内不提供空实现 demo。

函数级说明见：

```text
docs/MFX50_CODEC_FUNCTION_REFERENCE.md
```

## 6. Surface 生命周期

`MFX50_Surface` 的生命周期规则如下：

1. 如果 encoder 需要在函数返回后继续使用外部 surface，必须先调用 `surface->add_ref(surface->ref_opaque)`。
2. encoder 完成编码后必须调用 `surface->release(surface->ref_opaque)`。
3. 业务方在不再使用自己的引用时调用 `mfx50_surface_release(&surface)`。
4. 如果外部 surface 没有提供 `add_ref/release`，encoder 只能同步消费；不能安全异步持有。

因此，工程师传入外部硬解 surface 时，需要把复用/释放规则接到 `add_ref/release` 回调上。

`mfx50_encoder_push_surface()` 当前实现为同步提交并等待 oneVPL encode 完成：函数返回后，encoder 不再持有传入的 `mfxFrameSurface1*`。后续如果改成异步持有，必须先通过 `add_ref/release` 接上引用生命周期。

## 7. Policy 独立

`Policy` 不塞进 Encoder 内部。正确关系是：

```text
surface/features/metadata
  -> mfx50_policy_get_decision()
  -> MFX50_EncodeDecision
  -> mfx50_encoder_push_surface(..., decision)
```

这样策略升级只影响 policy，不改变 encoder 的 surface 输入契约。

## 8. 旧 Transcoder 接口

旧文件接口迁到 `mfx50_transcoder.h`：

```c
MFX50_RunInputList(...)
MFX50_RunSingleInput(...)
```

它的定位是：

```text
码流文件输入
  -> 内部硬解
  -> 内部 NV12 surface
  -> policy/编码
  -> HEVC 文件输出
```

它不是纯 encoder API。

输入 codec 不再写死 H.264，支持：

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

为了兼容旧列表，未知扩展名默认仍按 H.264 处理。
