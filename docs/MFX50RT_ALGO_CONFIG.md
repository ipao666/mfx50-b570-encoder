# MFX50RT Algorithm Configuration

Date: 2026-05-30

This document records the first v0.5 algorithm integration slice. It is
additive and defaults to the v0.4 realtime SDK behavior.

## Public API

New public header:

```text
mfx50_realtime_algo.h
```

New functions:

```c
int MFX50RT_DefaultAlgoConfig(MFX50RT_AlgoConfig* cfg);
int MFX50RT_SetAlgoConfig(MFX50RT_Handle h, const MFX50RT_AlgoConfig* cfg);
int MFX50RT_GetAlgoConfig(MFX50RT_Handle h, MFX50RT_AlgoConfig* cfg);
int MFX50RT_GetAlgoCaps(MFX50RT_Handle h, MFX50RT_AlgoCaps* caps);
```

`MFX50RT_GetAlgoCaps` currently reports build-level capabilities and accepts a
null handle. `SetAlgoConfig` and `GetAlgoConfig` require a valid handle.

## Default Behavior

All algorithm switches are disabled by default:

```text
enable_preprocess = 0
enable_smooth_scale = 0
enable_pre_denoise = 0
enable_scene_analyzer = 0
enable_adaptive_profile = 0
enable_adaptive_qp = 0
enable_mbqp = 0
```

The preprocess stage uses `enable_preprocess` as a master gate. Smooth-scale
and pre-denoise only run when the master gate and the relevant sub-switch are
both enabled.

## Current Capabilities

Implemented in this slice:

```text
supports_preprocess = 1
supports_scene_analyzer = 1
supports_adaptive_qp = 0
supports_mbqp = 0
supports_hevc_mbqp = 0
supports_runtime_qp_ctrl = 0
```

The preprocess implementation is a CPU scalar NV12 in-place path adapted from
`212-linux/src/preprocess/preprocess_scalar.cpp`. It maps the decoded oneVPL
surface with `mfxFrameSurfaceInterface::Map(MFX_MAP_READ_WRITE)`, runs the
selected filter, then unmaps before `EncodeFrameAsync`.

The scene analyzer is a log-only v0.5 path. It maps the decoded surface for
read, samples the Y plane, updates `scene_analyzed_frames` and
`avg_scene_analyze_ms`, and does not change encoded output.

If mapping is not available or the mapped surface is not a valid NV12 view, the
SDK skips preprocessing, increments `fallback_frames`, logs one warning if a log
callback exists, and continues encoding.

## New Profiles

The following aggressive probe profiles are exposed for validation only:

```text
MFX50_PROFILE_COMPRESS_90_PROBE_A = 34/40/46
MFX50_PROFILE_COMPRESS_90_PROBE_B = 35/41/47
MFX50_PROFILE_COMPRESS_90_PROBE_C = 36/42/48
MFX50_PROFILE_COMPRESS_90_PROBE_D = 36/43/49
```

They are not default profiles and require the same 20-sample validation matrix
before any external quality claim.

HybridTSRQ also exposes three SDK-facade tuning profiles through the JSON
`algorithm.profile` field or `expert_options_json`:

```text
target90_v1_moderate
target90_v2_aggressive
target90_v3_extreme_guarded
```

These profiles raise the temporal anchor QP and background/flat-background MBQP
values while keeping ROI deltas comparatively protected. They are validation
profiles, not final defaults. Reports should include `qp_p50`, `qp_p90`,
`qp_p95`, `high_qp_block_ratio`, `background_block_ratio`, and
`roi_block_ratio` before choosing a production default.

## Probe Usage

Default behavior:

```bash
./build_native_rt/mfx50rt_stream_probe \
  --input /path/input.h264 \
  --input-format annexb \
  --output /tmp/out.h265 \
  --profile quality \
  --strict-realtime
```

Smooth-scale probe:

```bash
./build_native_rt/mfx50rt_stream_probe \
  --input /path/input.h264 \
  --input-format annexb \
  --output /tmp/out_smooth.h265 \
  --profile compress90b \
  --algo-preprocess \
  --smooth-scale 30 \
  --strict-realtime
```

Pre-denoise probe:

```bash
./build_native_rt/mfx50rt_stream_probe \
  --input /path/input.h264 \
  --input-format annexb \
  --output /tmp/out_denoise.h265 \
  --profile compress90b \
  --algo-preprocess \
  --pre-denoise 30 \
  --strict-realtime
```

The probe prints algorithm stats such as:

```text
stats_preprocess_frames
stats_smooth_scale_frames
stats_pre_denoise_frames
stats_avg_preprocess_ms
stats_active_algo_flags
```
