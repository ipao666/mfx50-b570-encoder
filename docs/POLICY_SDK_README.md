# MFX50 Policy SDK 说明

`libmfx50_policy.so` 是跨设备的压缩算法决策库。它只回答“这一帧/这一段应该怎么压”，不直接负责视频解码、编码、RTSP、MP4、oneVPL session、GPU surface 或双卡调度。

## 交付内容

```text
mfx50_policy_sdk/
  include/
    mfx50_types.h
    mfx50_policy.h
  lib/
    libmfx50_policy.so
  examples/
    policy_decision_demo.c
  docs/
    POLICY_SDK_README.md
    POLICY_SDK_API.md
    B570_POLICY_MAPPING.md
    POLICY_SDK_FILE_STRUCTURE.md
```

评估脚本、SSIM 脚本、调参探针、测试视频不会放进 SDK 交付包。

## 推荐接入路径

实时摄像头或离线视频由工程侧完成解码/编码流水线：

```text
H264/H265 packet
  -> 工程师的硬件解码/编码流水线
  -> 提取低成本特征和外部 metadata
  -> 调用 libmfx50_policy.so
  -> 得到 QP / ROI / GOP / IDR / 背景QP / 去噪 / 帧复用决策
  -> 映射到具体编码器参数
```

B570 上建议参考我们的 oneVPL 路线：

```text
B570 硬解 -> 低频低分辨率特征 -> policy decision -> B570 硬编
```

不要把主路径做成“CPU 全分辨率 YUV 解码 + 每帧全图分析 + 再传 GPU 编码”。

## 最小调用流程

```c
MFX50_PolicyConfig cfg = {0};
cfg.struct_size = sizeof(cfg);
cfg.api_version = MFX50_POLICY_API_VERSION;
cfg.mode = MFX50_MODE_TARGET_90;
cfg.target_streams = 45;
cfg.input_fps = 30.0f;
cfg.target_encode_fps = 30.0f;

MFX50_PolicyContext* ctx = NULL;
mfx50_policy_create(&cfg, &ctx);

MFX50_StreamConfig scfg = {0};
scfg.struct_size = sizeof(scfg);
scfg.stream_id = 0;
scfg.width = 1920;
scfg.height = 1080;
scfg.fps = 30.0f;
scfg.input_codec = MFX50_CODEC_H264;
scfg.pixel_format = MFX50_PIXFMT_NV12;
scfg.is_realtime = 1;

MFX50_PolicyStream* stream = NULL;
mfx50_policy_create_stream(ctx, &scfg, &stream);

MFX50_FrameFeatures features = {0};
features.struct_size = sizeof(features);
features.mean_y = 104.0f;
features.edge_density = 0.18f;
mfx50_policy_submit_features(stream, &features);

MFX50_EncodeDecision decision = {0};
decision.struct_size = sizeof(decision);
mfx50_policy_get_decision(stream, &decision);

mfx50_policy_destroy_stream(stream);
mfx50_policy_destroy(ctx);
```

主路径推荐 `submit_features + submit_metadata + get_decision`。`submit_frame` 只作为可选增强输入，建议传低分辨率或抽样帧。
