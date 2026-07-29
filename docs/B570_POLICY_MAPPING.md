# B570 oneVPL 映射说明

本文说明如何把 `libmfx50_policy.so` 的决策映射到 B570 oneVPL 编码参数。Policy SDK 本身不创建 oneVPL session，也不管理 GPU surface。

## 推荐热路径

```text
摄像头 H264/H265 packet
  -> oneVPL B570 硬解
  -> video memory surface
  -> 低频抽样得到 FrameFeatures
  -> mfx50_policy_get_decision()
  -> oneVPL B570 HEVC 硬编
```

## Decision 到 oneVPL 参数

| Decision 字段 | B570 oneVPL 映射 |
|---|---|
| `qpi` | CQP I-frame QP |
| `qpp` | CQP P-frame QP |
| `qpb` | CQP B-frame QP |
| `gop_size` | GOP size |
| `b_frame_dist` | B frame distance，例如 `dist=4` |
| `request_idr` | 场景切换时请求 IDR 或刷新 GOP |
| `rois[]` | oneVPL ROI delta QP 或 MBQP 映射 |
| `background_delta_qp` | 背景 ROI 正 delta QP，编码器不支持可忽略 |
| `denoise_strength` | 工程侧可选背景轻去噪，不进入核心 policy |

## Capability 降级

B570 example 创建 policy context 前，应设置 `MFX50_EncoderCaps`：

```c
caps.supports_b_frames = 1;
caps.supports_roi = 1;
caps.supports_dynamic_gop = 1;
caps.supports_idr_request = 1;
caps.supports_negative_delta_qp = 1;
caps.supports_positive_delta_qp = 1;
caps.supports_roi_delta_qp = 1;
caps.max_roi_count = 64;
caps.roi_alignment = 16;
caps.min_qp = 1;
caps.max_qp = 51;
caps.min_delta_qp = -16;
caps.max_delta_qp = 16;
caps.max_b_frame_dist = 4;
caps.max_gop_size = 300;
```

如果后端不支持某能力，policy 会在 `disabled_flags` 和 `reason` 中说明，上层可以退回全局 QP/GOP。

## 多卡

双 B570 和更多卡属于工程流水线层。推荐：

```text
每张 B570 一个 oneVPL worker/pipeline
每路一个 MFX50_PolicyStream
按 round-robin 或 least-load 分配摄像头
policy SDK 不参与设备调度
```

这样核心算法库仍然跨设备，B570 example 只负责展示 45 路/双卡映射方式。
