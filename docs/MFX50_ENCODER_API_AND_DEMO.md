# MFX50 Transcoder/Benchmark API 与调用 Demo

本文说明 `mfx50_transcoder.h` 里的离线/压测兼容接口怎么用，并给出一个最小 C++ 调用示例。

注意：这组接口不是纯 encoder，也不是逐帧实时接口。它的定位是 **B570 多路离线压缩验证 / benchmark 封装**。内部流程是：

```text
读取输入列表
  -> 生成 oneVPL sample_multi_transcode 使用的 .par 参数文件
  -> 调用 sample_multi_transcode 或 sample_multi_transcode_b570
  -> 解析运行日志
  -> 返回每路 FPS 和整体统计
```

如果是业务实时接入，优先看 `docs/MFX50_SPLIT_CODEC_API.md`：新的 `mfx50_encoder.h` 已经定义为 NV12 GPU surface -> packet 的 encode-only API。旧的 `MFX50_RunInputList()` / `MFX50_RunSingleInput()` 归到 `mfx50_transcoder.h`。

## 1. 头文件与链接

业务代码包含：

```c
#include "mfx50_transcoder.h"
```

链接动态库：

```bash
-lmfx50_encoder
```

如果库不在系统默认搜索路径，需要设置：

```bash
export LD_LIBRARY_PATH=/path/to/sdk/lib:$LD_LIBRARY_PATH
```

## 2. 生命周期接口

```c
int MFX50_DefaultConfig(MFX50_Config* config);
MFX50_Handle MFX50_Create(const MFX50_Config* config);
void MFX50_Close(MFX50_Handle h);
```

调用顺序：

```text
MFX50_DefaultConfig
  -> 修改配置
  -> MFX50_Create
  -> MFX50_RunInputList 或 MFX50_RunSingleInput
  -> MFX50_GetStats / MFX50_GetRouteStats
  -> MFX50_Close
```

说明：

- `MFX50_DefaultConfig()` 必须先调用，用于填默认值。
- `MFX50_Create()` 会复制配置并创建内部上下文。
- `MFX50_Close()` 释放上下文。

## 3. 配置结构 `MFX50_Config`

```c
typedef struct MFX50_Config {
    int route_count;
    int frames_per_route;
    int fps_num;
    int fps_den;
    int initial_qp;
    int initial_gop;
    int async_depth;
    int device_count;
    MFX50_DeviceRoute devices[MFX50_MAX_DEVICES];
    int write_outputs;
    const char* sample_path;
    int enable_internal_roi;
    int enable_quality_guard;
    int enable_motion_idr;
} MFX50_Config;
```

字段说明：

| 字段 | 含义 |
| --- | --- |
| `route_count` | 总路数，例如 45 路。必须等于所有 `devices[i].route_count` 之和。 |
| `frames_per_route` | 每路处理多少帧，对应 oneVPL 参数 `-n`。 |
| `fps_num/fps_den` | 目标帧率，例如 `30/1` 表示 30fps。 |
| `initial_qp` | 初始 CQP 值。当前 wrapper 会把 I/P/B 都设为这个值。 |
| `initial_gop` | GOP 大小，对应 oneVPL 参数 `-gop_size`。 |
| `async_depth` | oneVPL sample 的异步深度，对应 `-async`。 |
| `device_count` | 使用几个 render node。 |
| `devices[]` | 每个设备的路径和分配路数。 |
| `write_outputs` | `1` 表示写出 HEVC 文件；`0` 表示输出到 `null`，只测吞吐。 |
| `sample_path` | 可选，指定 `sample_multi_transcode_b570` 路径。不填则从 `PATH` 找 `sample_multi_transcode`。 |
| `enable_internal_roi` | 当前接口保留字段，wrapper 本身不实现 ROI。 |
| `enable_quality_guard` | 当前接口保留字段，wrapper 本身不计算质量指标。 |
| `enable_motion_idr` | 当前接口保留字段，wrapper 本身不做逐帧 IDR 控制。 |

默认配置是历史 50 路双设备压测形态：

```text
44 路 -> /dev/dri/renderD129
 6 路 -> /dev/dri/renderD128
```

单张 B570 45 路建议这样配置：

```c
MFX50_Config cfg;
MFX50_DefaultConfig(&cfg);

cfg.route_count = 45;
cfg.device_count = 1;
cfg.devices[0].device_path = "/dev/dri/renderD129";
cfg.devices[0].route_count = 45;
```

## 4. 运行接口

### 4.1 多输入列表

```c
int MFX50_RunInputList(MFX50_Handle h,
                       const char* input_list_path,
                       const char* output_dir);
```

`input_list_path` 是文本文件，每行一个 H.264/H.265 AnnexB 输入文件路径。空行和 `#` 开头的行会被忽略。

示例：

```text
/data/ch000.h264
h265:/data/ch001.hevc
auto:/data/ch002.h265
```

支持的写法：

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

未知扩展名为了兼容旧列表，默认仍按 H.264 处理。

如果 `route_count = 45`，列表里至少需要 45 个输入。

### 4.2 单输入复用到多路

```c
int MFX50_RunSingleInput(MFX50_Handle h,
                         const char* input_path,
                         const char* output_dir);
```

这个接口会把同一个输入文件复制给所有路。它适合 smoke test，不适合作为真实 45 路摄像头压测结论。

## 5. 返回值

`MFX50_RunInputList()` 和 `MFX50_RunSingleInput()` 的返回值：

| 返回值 | 含义 |
| ---: | --- |
| `0` | 运行成功，并且所有路达到目标 FPS。 |
| `1` | 运行完成，但至少一路低于目标 FPS。 |
| `<0` | 参数错误、调用 sample 失败、日志解析失败或其他错误。 |

错误详情用：

```c
const char* MFX50_GetLastError(MFX50_Handle h);
```

## 6. 统计接口

```c
int MFX50_GetStats(MFX50_Handle h, MFX50_Stats* stats);
int MFX50_GetRouteStats(MFX50_Handle h,
                        int route_index,
                        MFX50_RouteStats* stats);
```

`MFX50_Stats` 主要字段：

| 字段 | 含义 |
| --- | --- |
| `requested_routes` | 请求路数。 |
| `completed_routes` | 成功完成的路数。 |
| `routes_below_target_fps` | 低于目标 FPS 的路数。 |
| `all_routes_realtime` | 是否所有路都达到目标 FPS。 |
| `target_fps` | 目标 FPS。 |
| `min_route_fps` | 最低单路 FPS。 |
| `avg_route_fps` | 平均单路 FPS。 |
| `max_route_fps` | 最高单路 FPS。 |
| `common_time_sec` | oneVPL sample 输出的 common transcoding time。 |
| `wall_seconds` | wrapper 统计的墙钟时间。 |
| `par_path` | 自动生成的 `.par` 文件路径。 |
| `log_path` | sample 运行日志路径。 |
| `summary_path` | wrapper 生成的 JSON summary 路径。 |

`MFX50_RouteStats` 主要字段：

| 字段 | 含义 |
| --- | --- |
| `route_id` | 路号。 |
| `passed` | sample 日志里该路是否通过。 |
| `frames` | 该路处理帧数。 |
| `seconds` | 该路耗时。 |
| `fps` | 该路 FPS。 |
| `device_path` | 该路使用的 render node。 |

## 7. 最小调用 Demo

源码位置：

```text
examples/mfx50_encoder_minimal_demo.cpp
```

构建：

```bash
cd /path/to/mfx50_source
cmake -S . -B build
cmake --build build --target mfx50_encoder_minimal_demo -j$(nproc)
```

运行：

```bash
./build/mfx50_encoder_minimal_demo \
  /path/to/input_list.txt \
  /tmp/mfx50_encoder_demo \
  /path/to/sample_multi_transcode_b570 \
  45 \
  1000
```

参数说明：

| 参数 | 含义 |
| --- | --- |
| `input_list.txt` | 输入列表，每行一个 `.h264` / `.h265` / `.hevc` 文件，也可用 `h264:` / `h265:` / `hevc:` 前缀。 |
| `output_dir` | 输出目录，会生成 `.par`、日志、summary JSON 和可选 HEVC 输出。 |
| `sample_multi_transcode_b570` | 可选，指定 B570 sample 二进制路径。传空字符串则从 `PATH` 找。 |
| `route_count` | 可选，默认 45。 |
| `frames_per_route` | 可选，默认 1000。 |

Demo 里默认：

```c
cfg.device_count = 1;
cfg.devices[0].device_path = "/dev/dri/renderD129";
cfg.devices[0].route_count = route_count;
cfg.write_outputs = 1;
```

如果只想测吞吐、不想写输出文件，可以把：

```c
cfg.write_outputs = 1;
```

改成：

```c
cfg.write_outputs = 0;
```

## 8. 运行后输出

运行成功后，demo 会打印类似：

```text
version=0.1.0
completed_routes=45/45
target_fps=30.000 min_fps=32.100 avg_fps=32.400 max_fps=32.700
all_routes_realtime=1 routes_below_target=0
par=/tmp/mfx50_encoder_demo/mfx50_routes.par
log=/tmp/mfx50_encoder_demo/run.log
summary=/tmp/mfx50_encoder_demo/mfx50_summary.json
```

重点看：

- `completed_routes` 是否等于请求路数。
- `min_fps` 是否大于目标 FPS。
- `all_routes_realtime` 是否为 `1`。
- `log` 是否有 sample 的完整运行日志。
- `par` 是否能复现本次 oneVPL 参数。

## 9. 适用边界

适合：

- B570 多路离线压测。
- 验证 `libmfx50_encoder.so` 里的兼容 transcoder 符号是否能被 C/C++ 工程调用。
- 生成 `.par` 和日志，便于工程师复现。
- 快速确认 45 路是否达到目标 FPS。

不适合：

- RTSP 实时接入。
- 逐帧 push/poll 实时编码。
- SDK 内部算法决策展示。
- ROI / MBQP / policy 细节公开。

一句话：`mfx50_transcoder.h` 是 **离线压测/文件转码兼容接口**；新的 `mfx50_encoder.h` 才是 encode-only API 边界。
