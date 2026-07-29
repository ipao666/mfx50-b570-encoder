# MFX50 Split Decoder 修复说明

日期：2026-06-04

## 修复目标

修复 split codec API 中 `mfx50_decoder_push_packet()` 返回 `MFX50_ERR_NOT_IMPLEMENTED` 的问题，让 MFX50 自动硬解入口真正可用：

```text
H.264/H.265 Annex-B packet/码流
  -> mfx50_decoder_push_packet()
  -> oneVPL DecodeHeader / DecodeFrameAsync
  -> mfx50_decoder_poll_surface()
  -> MFX50_SURFACE_ONEVPL NV12 GPU surface
  -> mfx50_encoder_push_surface()
```

## 主要改动

1. `mfx50_decoder_create()` 现在会准备共享 `MFX50_Device` runtime，校验 codec/config，并初始化 decoder 内部 bitstream buffer、async op 队列和输出 surface 队列。
2. `mfx50_decoder_push_packet()` 已接入 oneVPL decode，不再返回 `MFX50_ERR_NOT_IMPLEMENTED`。
3. `mfx50_decoder_poll_surface()` 返回可直接给 encoder 使用的 `MFX50_SURFACE_ONEVPL` surface，并带 `mfx50_surface_release()` 生命周期回调。
4. `mfx50_decoder_flush()` 支持 EOS 收尾；flush 后继续 poll，把硬件内部剩余帧取完，最终返回 `MFX50_ERR_EOS`。
5. 修复 decoder destroy 生命周期顺序：先释放未交付/未消费 surface，再 `MFXVideoDECODE_Close()`，避免 driver surface interface 失效后释放导致崩溃。
6. 新增 `tools/mfx50_split_file_transcode.cpp`，用于验证 split decoder -> encoder 链路。

## 已验证

在 Linux B570 机器 `100.108.202.16` 验证：

```text
cmake configure/build: OK
test_split_codec_headers: passed
```

实际码流复验结果：

```text
date:   2026-06-04
host:   100.108.202.16 / admi-MS-7E34
input:  /tmp/mfx50_selfcheck_input.hevc
output: /tmp/mfx50_selfcheck_output.hevc
tool:   mfx50_split_file_transcode

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

说明：输入由 `/home/admi/compress_out.mp4` 通过 FFmpeg `hevc_mp4toannexb` 抽成 HEVC Annex-B 码流。

## 使用示例

```bash
ffmpeg -hide_banner -loglevel error \
  -i input.mp4 \
  -map 0:v:0 -an -sn -dn \
  -c:v copy -bsf:v hevc_mp4toannexb \
  -f hevc input.hevc -y

LD_LIBRARY_PATH=./lib ./bin/mfx50_split_file_transcode \
  input.hevc output.hevc h265 25 30 32
```

RTSP/FFmpeg demux 场景应把每个 H.264/H.265 packet 填入 `MFX50_Packet` 后调用：

```c
mfx50_decoder_push_packet(decoder, &packet);
while (mfx50_decoder_poll_surface(decoder, &surface) == MFX50_OK) {
    mfx50_encoder_push_surface(encoder, &surface, &decision);
    mfx50_surface_release(&surface);
}
```
