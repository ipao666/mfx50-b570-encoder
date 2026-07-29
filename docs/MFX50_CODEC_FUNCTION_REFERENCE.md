# MFX50 Encoder / Decoder 函数说明

本文对应 split codec API：

```text
include/mfx50_device.h
include/mfx50_surface.h
include/mfx50_decoder.h
include/mfx50_encoder.h
```

核心边界是：

```text
外部硬解路径：
  业务方硬解 -> NV12 GPU surface -> MFX50 Encoder -> HEVC/H.265 packet

MFX50 自动硬解路径：
  H.264/H.265 packet -> MFX50 Decoder -> NV12 GPU surface
  -> MFX50 Encoder -> HEVC/H.265 packet
```

这里的 `NV12` 是像素格式；`MFX50_SURFACE_ONEVPL` / `MFX50_SURFACE_VAAPI` / `MFX50_SURFACE_DMABUF` 才表示这帧 surface 的句柄类型和共享方式。

## 1. 设备对象

### `mfx50_device_default_config`

```c
MFX50_Status mfx50_device_default_config(MFX50_DeviceConfig* config);
```

填充默认设备配置，包括 ABI 版本、默认设备路径、默认 zero-copy 要求等。调用方可以在默认值基础上改：

```c
MFX50_DeviceConfig device_cfg;
mfx50_device_default_config(&device_cfg);
device_cfg.interop_type = MFX50_DEVICE_INTEROP_ONEVPL;
device_cfg.require_zero_copy = 1;
```

如果业务方已经有 oneVPL session 或 VA display，可以填：

```c
device_cfg.external_mfx_session = your_mfx_session;
device_cfg.external_va_display = your_va_display;
```

### `mfx50_device_create`

```c
MFX50_Status mfx50_device_create(
    const MFX50_DeviceConfig* config,
    MFX50_Device** out_device);
```

创建 `Decoder` 和 `Encoder` 的共同父对象。需要零拷贝传 surface 时，`Decoder` 和 `Encoder` 必须挂在同一个 `MFX50_Device` 上。

默认 `device_path` 是 `/dev/dri/renderD129`。内部会在创建 oneVPL session 前解析 render node，并设置 loader filter：

```text
mfxExtendedDeviceId.DRMRenderNodeNum = 129
```

这样可以避免工程师环境中 `MFXCreateSession failed: MFX_ERR_NOT_FOUND`，并保证 oneVPL session 和 VA display 绑定到同一张 B570。

### `mfx50_device_destroy`

```c
void mfx50_device_destroy(MFX50_Device* device);
```

释放设备对象。先销毁依赖它的 `Decoder` / `Encoder`，最后销毁 `Device`。

## 2. Surface 与 Packet

### `MFX50_Surface`

`MFX50_Surface` 描述一帧已经解出的 NV12 GPU surface。当前公开的 handle 类型：

```text
MFX50_SURFACE_ONEVPL  -> handle.onevpl.mfx_surface
MFX50_SURFACE_VAAPI   -> handle.vaapi.va_display + va_surface_id
MFX50_SURFACE_DMABUF  -> handle.dmabuf.fd / stride / offset / modifier
```

当前可直接提交 encoder 的实现路径是：

```text
MFX50_SURFACE_ONEVPL + NV12 + mfxFrameSurface1*
```

`MFX50_SURFACE_VAAPI` 和 `MFX50_SURFACE_DMABUF` 的 ABI 已保留，实际编码 adapter 后续接。

### `mfx50_surface_add_ref` / `mfx50_surface_release`

```c
MFX50_Status mfx50_surface_add_ref(const MFX50_Surface* surface);
void mfx50_surface_release(MFX50_Surface* surface);
```

如果调用方提供 `surface.add_ref` / `surface.release`，MFX50 可以通过这两个回调管理外部 surface 生命周期。当前 oneVPL encoder 路径是同步提交，`mfx50_encoder_push_surface()` 返回后不再持有传入 surface；回调字段为后续异步持有预留。

### `mfx50_packet_release`

```c
void mfx50_packet_release(MFX50_Packet* packet);
```

释放 `mfx50_encoder_poll_packet()` 输出 packet 里附带的内部引用。工程代码拿到 packet 后，写出或拷走数据，再调用这个函数。

## 3. Encoder 函数

### `mfx50_encoder_default_config`

```c
MFX50_Status mfx50_encoder_default_config(MFX50_EncoderConfig* config);
```

填充默认编码配置。常改字段：

```c
encoder_cfg.width = 1920;
encoder_cfg.height = 1080;
encoder_cfg.fps_num = 30;
encoder_cfg.fps_den = 1;
encoder_cfg.input_format = MFX50_PIXFMT_NV12;
encoder_cfg.output_codec = MFX50_CODEC_HEVC;
encoder_cfg.qpi = 32;
encoder_cfg.qpp = 32;
encoder_cfg.qpb = 32;
```

### `mfx50_encoder_get_surface_support`

```c
MFX50_Status mfx50_encoder_get_surface_support(
    MFX50_Device* device,
    MFX50_EncoderSurfaceSupport* out_support);
```

查询当前 encoder 能直接接收哪些 surface 句柄类型。工程集成时先查这个接口，不要只看头文件枚举。

当前实现返回的主路径是：

```text
supports_onevpl_surface = 1
supports_vaapi_surface  = 0
supports_dmabuf         = 0
```

### `mfx50_encoder_create`

```c
MFX50_Status mfx50_encoder_create(
    MFX50_Device* device,
    const MFX50_EncoderConfig* config,
    MFX50_Encoder** out_encoder);
```

在共享 `MFX50_Device` 下创建 encode-only encoder。这个接口不负责解码，输入必须是已经解出的 NV12 GPU surface。

### `mfx50_encoder_push_surface`

```c
MFX50_Status mfx50_encoder_push_surface(
    MFX50_Encoder* encoder,
    const MFX50_Surface* surface,
    const MFX50_EncodeDecision* decision);
```

提交一帧 NV12 GPU surface 给 encoder。`decision` 来自 policy；如果暂时不用复杂策略，也可以传 `NULL` 或只填 QP 字段。

oneVPL 注入示例：

```c
MFX50_Surface surface = {0};
surface.struct_size = sizeof(surface);
surface.api_version = MFX50_DEVICE_API_VERSION;
surface.type = MFX50_SURFACE_ONEVPL;
surface.pixel_format = MFX50_PIXFMT_NV12;
surface.width = width;
surface.height = height;
surface.pts = pts;
surface.handle.onevpl.mfx_surface = mfx_surface;

mfx50_encoder_push_surface(encoder, &surface, &decision);
```

### `mfx50_encoder_poll_packet`

```c
MFX50_Status mfx50_encoder_poll_packet(
    MFX50_Encoder* encoder,
    MFX50_Packet* out_packet);
```

拉取编码后的 HEVC/H.265 packet。返回：

```text
MFX50_OK         拿到一个 packet
MFX50_ERR_AGAIN  当前没有可读 packet
其他错误码       需要查看 mfx50_encoder_get_last_error()
```

成功拿到 packet 后：

```c
write_packet(packet.data, packet.data_size);
mfx50_packet_release(&packet);
```

### `mfx50_encoder_flush`

```c
MFX50_Status mfx50_encoder_flush(MFX50_Encoder* encoder);
```

通知 encoder 输入结束，并排出内部延迟 packet。调用后继续 `mfx50_encoder_poll_packet()`，直到返回 `MFX50_ERR_AGAIN` 或错误。

### `mfx50_encoder_get_last_error`

```c
const char* mfx50_encoder_get_last_error(MFX50_Encoder* encoder);
```

返回详细错误字符串，用于定位设备初始化、surface 类型、像素格式或 oneVPL 调用问题。

### `mfx50_encoder_destroy`

```c
void mfx50_encoder_destroy(MFX50_Encoder* encoder);
```

释放 encoder。已经 poll 出来的 packet 仍按 packet 自己的释放规则处理。

## 4. Decoder 函数

### `mfx50_decoder_default_config`

```c
MFX50_Status mfx50_decoder_default_config(MFX50_DecoderConfig* config);
```

填充默认解码配置。常改字段：

```c
decoder_cfg.input_codec = MFX50_CODEC_H264;  // 或 MFX50_CODEC_HEVC
decoder_cfg.width = width;
decoder_cfg.height = height;
decoder_cfg.annexb_input = 1;
decoder_cfg.low_latency = 1;
```

### `mfx50_decoder_create`

```c
MFX50_Status mfx50_decoder_create(
    MFX50_Device* device,
    const MFX50_DecoderConfig* config,
    MFX50_Decoder** out_decoder);
```

在共享 `MFX50_Device` 下创建 decoder。它的输出目标是 `MFX50_Surface`，用于直接交给 encoder。

### `mfx50_decoder_push_packet`

```c
MFX50_Status mfx50_decoder_push_packet(
    MFX50_Decoder* decoder,
    const MFX50_Packet* packet);
```

提交一个 H.264/H.265 access unit 或 packet。packet 示例：

```c
MFX50_Packet packet = {0};
packet.struct_size = sizeof(packet);
packet.api_version = MFX50_DEVICE_API_VERSION;
packet.codec = MFX50_CODEC_H264;
packet.data = encoded_data;
packet.data_size = encoded_size;
packet.pts = pts;

mfx50_decoder_push_packet(decoder, &packet);
```

当前实现状态：已接入 oneVPL 硬解。`mfx50_decoder_push_packet()` 会累积 H.264/H.265 Annex-B packet/码流，自动 `DecodeHeader`，并通过 `DecodeFrameAsync` 输出 NV12 oneVPL GPU surface。`dec_cfg.input_codec` 和 `packet.codec` 都为 `MFX50_CODEC_UNKNOWN` 时，会按 Annex-B 参数集自动识别 H.264/H.265。包数据不足以形成完整 header/frame 时返回 `MFX50_OK` 并等待后续 packet，不再返回 `MFX50_ERR_NOT_IMPLEMENTED`。

### `mfx50_decoder_poll_surface`

```c
MFX50_Status mfx50_decoder_poll_surface(
    MFX50_Decoder* decoder,
    MFX50_Surface* out_surface);
```

拉取解码出的 NV12 GPU surface。成功后可直接：

```c
mfx50_encoder_push_surface(encoder, &surface, &decision);
mfx50_surface_release(&surface);
```

没有 surface 可读时返回 `MFX50_ERR_AGAIN`；`mfx50_decoder_flush()` 后继续 poll，全部 surface 取完后返回 `MFX50_ERR_EOS`。成功取得的 surface 必须在 encoder 消费后调用 `mfx50_surface_release()`。

### `mfx50_decoder_flush`

```c
MFX50_Status mfx50_decoder_flush(MFX50_Decoder* decoder);
```

通知 decoder 输入结束。后续继续 poll surface，直到取完。

### `mfx50_decoder_get_last_error`

```c
const char* mfx50_decoder_get_last_error(MFX50_Decoder* decoder);
```

返回 decoder 的详细错误字符串。

### `mfx50_decoder_destroy`

```c
void mfx50_decoder_destroy(MFX50_Decoder* decoder);
```

释放 decoder。已经 poll 出来的 surface 按 `MFX50_Surface` 生命周期规则释放。

## 5. Demo

本次 SDK 交付的 split codec 完整可运行 demo：

```text
bin/mfx50_split_file_transcode
examples/split_codec/mfx50_split_file_transcode.cpp
```

运行方式：

```bash
ffmpeg -hide_banner -loglevel error \
  -i input.mp4 \
  -map 0:v:0 -an -sn -dn \
  -c:v copy -bsf:v hevc_mp4toannexb \
  -f hevc input.hevc -y

LD_LIBRARY_PATH=./lib ./bin/mfx50_split_file_transcode \
  input.hevc output.hevc h265 25 30 32
```

这个 demo 走完整链路：H.264/H.265 Annex-B packet -> MFX50 Decoder -> NV12 oneVPL GPU surface -> MFX50 Encoder -> HEVC packet。外部硬解直接注入 NV12 GPU surface 的集成方式见上面的 `mfx50_encoder_push_surface()` 说明，不在包内伪造空 surface demo。
