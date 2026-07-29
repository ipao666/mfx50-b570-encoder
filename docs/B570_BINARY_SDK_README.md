# B570 二进制 SDK 使用说明

正式交付请先阅读 `docs/ENGINEER_HANDOFF.md`。本文保留为 B570 二进制包的补充说明。

## 交付目标

这个 SDK 面向交通监控实时压缩部署，核心目标是让工程师在不接触算法源码的情况下调用现有算法和 B570 oneVPL 多路流水线。

交付包只包含：

- `include/`：C/C++ 公开头文件。
- `lib/`：运行时动态库。
- `bin/`：SDK probe、接口 demo、B570 多路示例二进制。
- `examples/b570_fastpath/`：B570 多路运行示例脚本，默认 45 路，可指定路数。
- `docs/`：接口、目录、流程和限制说明。
- `validation/`：本机 B570 环境和 demo 验证记录。

交付包不包含 `src/`、`tools/legacy/sample_multi_transcode/src/` 等算法源码。

## 推荐接入方式

实时 45 路优先使用编码包输入，而不是先把摄像头转成 YUV 再送入 SDK。

推荐链路：

1. 摄像头 RTSP/GB28181 流进入工程系统。
2. 工程系统 demux 或硬解得到 H.264 access unit 包。
3. 调用 `MFX50RT_PushPacket` 或使用 B570 运行示例。
4. SDK/oneVPL 输出 HEVC 码流。
5. 工程系统 mux 成 MP4、FLV、TS 或继续推流。

不推荐链路：

- 每路先转完整 YUV 再跨接口传入。YUV 1080p30 的内存带宽和拷贝压力很大，45 路会明显吃 CPU/内存带宽；当前实时 C API 的 `MFX50RT_PushFrame` 也标记为未实现。

## B570 运行示例

B570 多路运行示例在：

```bash
examples/b570_fastpath/run_45_from_h264_dir.sh
examples/b570_fastpath/run_45_from_mp4_dir.sh
examples/b570_fastpath/run_45_daynight_from_mp4_dir.sh
examples/b570_fastpath/run_45_from_manifest.sh
examples/b570_fastpath/run_single_mp4.sh
```

H.264 AnnexB 输入：

```bash
export SAMPLE_MULTI_TRANSCODE=/path/to/sdk/bin/sample_multi_transcode_b570
export DEVICE=/dev/dri/renderD129
examples/b570_fastpath/run_45_from_h264_dir.sh /path/to/45_h264 /tmp/b570_45
```

默认运行 45 路。指定路数：

```bash
ROUTE_COUNT=16 examples/b570_fastpath/run_45_from_h264_dir.sh /path/to/16_h264 /tmp/b570_16
```

MP4 离线输入：

```bash
export SAMPLE_MULTI_TRANSCODE=/path/to/sdk/bin/sample_multi_transcode_b570
examples/b570_fastpath/run_45_from_mp4_dir.sh /path/to/45_mp4 /tmp/b570_45_mp4
```

指定路数：

```bash
ROUTE_COUNT=16 examples/b570_fastpath/run_45_from_mp4_dir.sh /path/to/16_mp4 /tmp/b570_16_mp4
```

单文件 MP4 离线压缩：

```bash
export SAMPLE_MULTI_TRANSCODE=/path/to/sdk/bin/sample_multi_transcode_b570
examples/b570_fastpath/run_single_mp4.sh input.mp4 output.mp4
```

日/夜质量优先 profile：

```bash
export SAMPLE_MULTI_TRANSCODE=/path/to/sdk/bin/sample_multi_transcode_b570
examples/b570_fastpath/run_45_daynight_from_mp4_dir.sh /path/to/45_mp4 /tmp/b570_daynight
```

默认日/夜策略：

- 白天 06:00-18:59：`QPI/QPP/QPB=36/38/44`，优先保证 SSIM/YUV。
- 夜间 19:00-05:59：`QPI/QPP/QPB=47/49/51`，优先压缩到 95%+。

可用 `DAY_START_HOUR`、`DAY_END_HOUR`、`DAY_QPI/DAY_QPP/DAY_QPB`、`NIGHT_QPI/NIGHT_QPP/NIGHT_QPB` 覆盖。

运行 manifest 回归：

```bash
export SAMPLE_MULTI_TRANSCODE=/path/to/sdk/bin/sample_multi_transcode_b570
export DEVICE=/dev/dri/renderD129
export RISK_CSV=/path/to/risk.csv
examples/b570_fastpath/run_45_from_manifest.sh /path/to/manifest.csv /tmp/b570_batch0 0 45
```

这个入口会按 `risk.csv` 选择 `base_q36b48`、`day_guard_q34`、`risk_q35` 三档，并自动导出 B570 v3 验证使用的算法环境变量。

输出：

- H.265 elementary stream：`<output>/h265/ch000.h265 ...`
- MP4 wrapper 输出：`<output>/mp4/ch000.mp4 ...`
- oneVPL 参数文件：`<output>/b570_<路数>_routes.par`
- 转码日志：`<output>/logs/transcode.log`

说明：脚本名里的 `45` 表示默认验证路数，不表示只能跑 45 路。

## 关键参数

默认大小优先 profile：

```bash
QPI=36
QPP=38
QPB=48
GOP_SIZE=300
DIST=4
USAGE=veryfast
ROI_ONLY=1
ROI_ANALYZE_INTERVAL=30
ROI_DELTA_QP=-4
PLATE_ROI_DELTA_QP=-12
PLATE_ROI_MARGIN=24
MBQP_SAFE_MAX=38
STATIC_SKIP=0
```

含义：

- `QPI/QPP/QPB`：I/P/B 帧基础 CQP。数值越大，码率越低，质量越低。
- `GOP_SIZE`：最大 GOP，默认 300；配合 `AdaptiveI:on` 和 B 帧结构提高压缩效率。
- `DIST=4`：启用 IPB 结构，允许 B 帧提高压缩效率。
- `USAGE=veryfast`：B570 45 路实时路径使用速度优先。
- `ROI_ONLY=1`：只附加 ROI delta QP，不启用全帧 MBQP 图，降低 CPU 分析压力。
- `ROI_ANALYZE_INTERVAL=30`：ROI 分析复用间隔，减少每帧 CPU 计算。
- `ROI_DELTA_QP`：车辆/运动目标区域相对基础 QP 的保护量。
- `PLATE_ROI_DELTA_QP`：疑似车牌区域保护量。
- `PLATE_ROI_MARGIN`：车牌区域扩张像素。
- `STATIC_SKIP`：静态帧复用开关；当前验证中对日间高运动 tail 段收益不明显，默认关闭。
- `OUTPUT_FPS`：MP4 mux 输出帧率，默认 30。

Y 通道保护 profile 可把 `QPI/QPP/QPB` 改成 `36/38/44`。这会提高 SSIM 和 Y/U/V 最低值，但压缩率会下降。

## 重要边界

当前 45 路验证主路径是 `sample_multi_transcode_b570` 加 `examples/b570_fastpath` 脚本。

`libmfx50_policy.so` 是跨设备算法决策库，用于工程师自建流水线时输出 QP/GOP/ROI 决策。

`mfx50_realtime.h`/`libmfx50_realtime.so` 保留为 C API 原型和后续集成接口，不替代已经验证的 B570 运行示例。

## C API 调用示例

公开实时接口在 `include/mfx50_realtime.h`：

```c
MFX50RT_Config cfg;
MFX50RT_DefaultConfig(&cfg);
cfg.device_selector = "intel:dGPU:0";
cfg.route_count = 45;
cfg.async_mode = 1;
cfg.input_mode = MFX50_INPUT_ENCODED_PACKET;
cfg.input_codec = MFX50_CODEC_H264;
cfg.output_codec = MFX50_CODEC_HEVC;
cfg.qpi = 41;
cfg.qpp = 43;
cfg.qpb = 51;

MFX50RT_Handle h = NULL;
int rc = MFX50RT_Create(&cfg, &h);
```

送入每路 access unit：

```c
MFX50RT_Packet pkt = {0};
pkt.struct_size = sizeof(pkt);
pkt.stream_id = route_id;
pkt.data = data;
pkt.size = size;
pkt.pts = pts;
pkt.dts = dts;
pkt.is_keyframe = is_keyframe;
MFX50RT_PushPacket(h, &pkt);
```

拉取输出：

```c
uint8_t buffer[8 * 1024 * 1024];
MFX50RT_EncodedPacket out = {0};
out.struct_size = sizeof(out);
out.data = buffer;
out.capacity = sizeof(buffer);
rc = MFX50RT_PollPacket(h, &out);
```

结束：

```c
MFX50RT_Flush(h);
MFX50RT_Close(h);
```

## 其他资料

详细限制、profile 取舍和目录结构请继续阅读：

```text
docs/B570_CURRENT_LIMITS.md
docs/B570_OPTIMIZATION_DECISION.md
docs/B570_FILE_STRUCTURE.md
docs/MFX50_SPLIT_CODEC_API.md
```
