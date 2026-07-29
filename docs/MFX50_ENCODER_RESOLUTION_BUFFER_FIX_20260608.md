# MFX50 Encoder 2304x1296 修复说明

## 结论

2304x1296 这类分辨率不是天然不支持。此次失败的直接原因不是可见分辨率没有凑成 32 倍数，而是编码首帧时 oneVPL 返回 `MFX_ERR_NOT_ENOUGH_BUFFER`，旧 SDK 没有把它当成“输出 bitstream buffer 需要扩容”处理。

## 已修复

- `MFX_ERR_NOT_ENOUGH_BUFFER` 现在会像 `MFX_ERR_MORE_BITSTREAM` 一样触发 bitstream buffer 扩容并重试。
- `mfx_status_name()` 增加 `MFX_ERR_NOT_ENOUGH_BUFFER`，错误日志不再只显示 `mfxStatus(-5)`。
- 外部 oneVPL surface 现在区分 backing/canvas 和 visible crop：
  - `MFX50_Surface.width/height` 表示外部 surface backing/canvas hint。
  - `MFX50_Surface.crop_w/crop_h` 表示真实可见画面。
- encoder 初始化失败日志会带上 visible、backing、source_info 尺寸，便于工程师定位是否是 surface 参数不一致。

## 工程师接入注意

如果业务方自己硬解后传入 oneVPL GPU surface，建议按下面方式表达尺寸：

```text
surface.width/height      = GPU surface backing/canvas 尺寸
surface.crop_w/crop_h     = 真实可见画面尺寸
mfxFrameSurface1.Info     = 尽量与真实 backing/crop 保持一致
```

例如可见画面为 `2304x1296`，如果上游硬解分配的 backing 为 `2304x1312`，则应填：

```text
width  = 2304
height = 1312
crop_w = 2304
crop_h = 1296
```

如果使用 MFX50 decoder 解码后直接送 MFX50 encoder，SDK 会沿用 decoder 输出的 oneVPL surface 信息。

## 已验证

在 B570 `/dev/dri/renderD129` 上生成 50 帧 `2304x1296` HEVC Annex-B 流，并通过：

```text
MFX50 decoder -> oneVPL NV12 GPU surface -> MFX50 encoder
```

验证结果：

```text
decoded_surfaces=50
encoded_packets=50
ffprobe: hevc,2304,1296,50
```
