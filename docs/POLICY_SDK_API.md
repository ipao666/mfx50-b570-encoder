# MFX50 Policy SDK 接口说明

`libmfx50_policy.so` 是跨设备算法决策库。它不解码、不编码、不 mux、不管理 GPU surface，只根据输入特征、metadata 和编码器能力输出压缩决策。

## 通用约定

- 所有结构体的 `struct_size` 必须填 `sizeof(结构体名)`。
- `MFX50_PolicyConfig.api_version` 必须填 `MFX50_POLICY_API_VERSION`。
- `reserved[]` 必须清零，留给后续 ABI 扩展。
- 一个 `MFX50_PolicyContext` 可以创建多路 `MFX50_PolicyStream`。
- 每个摄像头/视频路应使用独立 `MFX50_PolicyStream`。
- `submit_frame` 是可选低分辨率/抽样帧输入，不要求每帧传完整 YUV。
- `get_decision` 可以在只有 features、只有 metadata、或两者都有时调用。

## 状态码

| 状态码 | 含义 |
| --- | --- |
| `MFX50_OK` | 成功 |
| `MFX50_ERR_INVALID_PARAM` | 空指针、结构体尺寸错误、参数越界 |
| `MFX50_ERR_UNSUPPORTED` | 当前配置或能力不支持 |
| `MFX50_ERR_NOT_READY` | 输入不足或状态尚不可决策 |
| `MFX50_ERR_NO_MEMORY` | 内存不足 |
| `MFX50_ERR_BAD_STATE` | 调用顺序错误 |
| `MFX50_ERR_VERSION_MISMATCH` | ABI 版本不匹配 |
| `MFX50_ERR_BUFFER_TOO_SMALL` | 输出 buffer 不够 |

## 生命周期 API

### `mfx50_policy_get_version`

```c
MFX50_Status mfx50_policy_get_version(MFX50_Version* out_version);
```

参数：

| 参数 | 方向 | 含义 |
| --- | --- | --- |
| `out_version` | 输出 | 调用前设置 `struct_size`，返回 API 版本和库版本 |

### `mfx50_policy_create`

```c
MFX50_Status mfx50_policy_create(const MFX50_PolicyConfig* config,
                                 MFX50_PolicyContext** out_context);
```

创建全局 policy 上下文。

参数：

| 参数 | 方向 | 含义 |
| --- | --- | --- |
| `config` | 输入 | 全局配置，包含模式、目标路数、目标 fps、编码器能力 |
| `out_context` | 输出 | 返回 context handle，成功后由 `mfx50_policy_destroy` 释放 |

### `mfx50_policy_destroy`

```c
void mfx50_policy_destroy(MFX50_PolicyContext* context);
```

释放 context。调用前应先销毁所有 stream。

### `mfx50_policy_create_stream`

```c
MFX50_Status mfx50_policy_create_stream(MFX50_PolicyContext* context,
                                        const MFX50_StreamConfig* config,
                                        MFX50_PolicyStream** out_stream);
```

创建一路流的算法状态。

参数：

| 参数 | 方向 | 含义 |
| --- | --- | --- |
| `context` | 输入 | 全局 context |
| `config` | 输入 | 单路配置，包含路号、分辨率、fps、输入格式 |
| `out_stream` | 输出 | 返回 stream handle，由 `mfx50_policy_destroy_stream` 释放 |

### `mfx50_policy_destroy_stream`

```c
void mfx50_policy_destroy_stream(MFX50_PolicyStream* stream);
```

释放单路状态。

### `mfx50_policy_reset_stream`

```c
MFX50_Status mfx50_policy_reset_stream(MFX50_PolicyStream* stream,
                                       MFX50_ResetMode mode);
```

重置一路流。

| `mode` | 含义 |
| --- | --- |
| `MFX50_RESET_SOFT` | 清 ROI/tracking 短历史，保留配置 |
| `MFX50_RESET_HARD` | 清全部单路状态 |
| `MFX50_RESET_SCENE_CUT` | 场景切换，清短历史并倾向请求 IDR |

## 输入 API

### `mfx50_policy_submit_features`

```c
MFX50_Status mfx50_policy_submit_features(MFX50_PolicyStream* stream,
                                          const MFX50_FrameFeatures* features);
```

提交低成本帧特征。这是推荐主输入。

关键字段：

| 字段 | 含义 |
| --- | --- |
| `frame_index` | 帧序号 |
| `pts` | 时间戳 |
| `mean_y` | Y 平均亮度，用于昼夜/曝光/profile 判断 |
| `edge_density` | 边缘密度，用于复杂度判断 |
| `motion_score` | 运动强度，用于车辆/动态场景判断 |
| `dark_ratio` | 暗部比例 |
| `overexposed_ratio` | 过曝比例 |
| `road_y_variance` | 道路区域纹理/亮度方差 |
| `scene_cut_score` | 场景切换分数 |
| `recent_bitrate` | 最近码率反馈 |
| `recent_compression_percent` | 最近压缩率反馈 |
| `recent_encode_fps` | 最近编码 fps |
| `recent_latency_ms` | 最近延迟 |
| `recent_drop_frames` | 最近丢帧数 |

### `mfx50_policy_submit_metadata`

```c
MFX50_Status mfx50_policy_submit_metadata(MFX50_PolicyStream* stream,
                                          const MFX50_Metadata* metadata);
```

提交外部目标信息。metadata 只作为建议，policy 会结合内部规则、面积预算和编码器能力融合。

目标类型：

| 类型 | 含义 |
| --- | --- |
| `MFX50_OBJECT_VEHICLE` | 车辆 |
| `MFX50_OBJECT_PLATE` | 车牌 |
| `MFX50_OBJECT_SIGN` | 标牌 |
| `MFX50_OBJECT_TEXT` | 文字 |
| `MFX50_OBJECT_ROAD` | 道路 |
| `MFX50_OBJECT_BACKGROUND` | 背景 |

`MFX50_MetadataObject` 字段：

| 字段 | 含义 |
| --- | --- |
| `x/y/w/h` | 像素坐标 |
| `confidence` | 置信度 |
| `track_id` | 跟踪 ID，没有可填 `-1` |
| `timestamp` | 时间戳 |
| `priority` | 外部优先级 |
| `suggested_delta_qp` | 外部建议 delta QP |

### `mfx50_policy_submit_frame`

```c
MFX50_Status mfx50_policy_submit_frame(MFX50_PolicyStream* stream,
                                       const MFX50_AnalyzeFrame* frame);
```

提交可选分析帧。建议只传低分辨率或抽样帧。

字段：

| 字段 | 含义 |
| --- | --- |
| `width/height` | 分析帧分辨率 |
| `pts/frame_index` | 时间戳和帧号 |
| `y_plane/y_stride` | Y 平面 |
| `uv_plane/uv_stride` | NV12 UV 平面，可为空 |
| `pixel_format` | `NV12/YUV420/GRAY` |
| `is_lowres` | `1` 表示低分辨率分析帧 |

## 决策 API

### `mfx50_policy_get_decision`

```c
MFX50_Status mfx50_policy_get_decision(MFX50_PolicyStream* stream,
                                       MFX50_EncodeDecision* decision);
```

输出编码决策。

字段：

| 字段 | 含义 |
| --- | --- |
| `decision_id` | 决策递增 ID |
| `profile_id` | 当前 profile |
| `qpi/qpp/qpb` | I/P/B 帧基础 QP |
| `gop_size` | GOP |
| `b_frame_dist` | B 帧距离 |
| `request_idr` | 是否请求 IDR |
| `reuse_previous_frame` | 是否建议静态帧复用 |
| `background_delta_qp` | 背景正 delta QP |
| `denoise_strength` | 背景去噪建议强度 |
| `applied_flags` | 已启用策略 |
| `disabled_flags` | 因能力不足关闭的策略 |
| `reason` | 降级/决策说明 |
| `roi_count` | ROI 数量 |
| `rois[]` | ROI 列表 |

ROI 字段：

| 字段 | 含义 |
| --- | --- |
| `x/y/w/h` | ROI 像素坐标 |
| `delta_qp` | 相对 QP；负数保护，正数更强压缩 |
| `type` | ROI 类型 |
| `priority` | 优先级，编码器 ROI 数不足时先保高优先级 |
| `source` | 内部、metadata 或融合 |
| `confidence` | 置信度 |
| `track_id` | 跟踪 ID |

## Option 接口

### `mfx50_policy_set_option`

```c
MFX50_Status mfx50_policy_set_option(MFX50_PolicyContext* context,
                                     const char* key,
                                     const char* value);
```

设置策略参数。普通工程接入不建议频繁使用，只在 `MFX50_MODE_CUSTOM` 或调试时使用。

### `mfx50_policy_get_option`

```c
MFX50_Status mfx50_policy_get_option(MFX50_PolicyContext* context,
                                     const char* key,
                                     char* value,
                                     size_t value_capacity);
```

读取当前策略参数。`value` 由调用方分配。

常用 key：

| key | 含义 |
| --- | --- |
| `vehicle.delta_qp` | 车辆 ROI delta QP |
| `plate.delta_qp` | 车牌 ROI delta QP |
| `sign_text.delta_qp` | 标牌/文字 ROI delta QP |
| `background.delta_qp` | 背景正 delta QP |
| `denoise.background_strength` | 背景轻去噪 |
| `gop.size` | GOP |
| `bframe.dist` | B 帧距离 |
| `roi.area_budget_percent` | ROI 面积预算 |

## 统计和日志

### `mfx50_policy_get_stats`

```c
MFX50_Status mfx50_policy_get_stats(MFX50_PolicyStream* stream,
                                    MFX50_PolicyStats* stats);
```

返回 policy 层统计，不包含 GPU 功耗、SSIM 或真实码率。

| 字段 | 含义 |
| --- | --- |
| `frames_seen` | 收到的帧特征/分析帧数 |
| `decisions_made` | 决策次数 |
| `current_profile_id` | 当前 profile |
| `avg_roi_count` | 平均 ROI 数 |
| `avg_roi_area_percent` | 平均 ROI 面积比例 |
| `idr_request_count` | 请求 IDR 次数 |
| `fallback_count` | 降级次数 |
| `disabled_flags_count` | 策略关闭计数 |
| `metadata_object_count` | 已接收 metadata 目标数 |

### `mfx50_policy_set_log_callback`

```c
MFX50_Status mfx50_policy_set_log_callback(MFX50_PolicyContext* context,
                                           MFX50_LogCallback callback,
                                           void* user_data);
```

设置日志回调。回调内不要做阻塞 IO。

```c
typedef void (*MFX50_LogCallback)(int32_t level,
                                  const char* message,
                                  void* user_data);
```

## 调用示例

```c
MFX50_PolicyConfig cfg = {0};
cfg.struct_size = sizeof(cfg);
cfg.api_version = MFX50_POLICY_API_VERSION;
cfg.mode = MFX50_MODE_TARGET_90;
cfg.target_streams = 45;
cfg.input_fps = 30.0f;
cfg.target_encode_fps = 30.0f;
cfg.encoder_caps.supports_b_frames = 1;
cfg.encoder_caps.supports_roi = 1;
cfg.encoder_caps.supports_negative_delta_qp = 1;
cfg.encoder_caps.max_roi_count = 64;
cfg.encoder_caps.roi_alignment = 16;
cfg.encoder_caps.min_qp = 1;
cfg.encoder_caps.max_qp = 51;

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
features.frame_index = 0;
features.mean_y = 104.0f;
features.edge_density = 0.18f;
features.motion_score = 0.12f;
mfx50_policy_submit_features(stream, &features);

MFX50_EncodeDecision decision = {0};
decision.struct_size = sizeof(decision);
mfx50_policy_get_decision(stream, &decision);

mfx50_policy_destroy_stream(stream);
mfx50_policy_destroy(ctx);
```
