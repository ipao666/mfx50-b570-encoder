# MFX50 Realtime Flush Fix 2026-06-09

## Problem

Short H.264 streams with delayed/reordered frames could lose tail frames when using
the realtime API path:

```text
MFX50RT_Create -> MFX50RT_PushPacket -> MFX50RT_Flush
```

The issue was in the realtime pipeline drain logic, not in HybridTSRQ/adaptive QP.
At end of stream, `drainPipeline()` entered decoder drain with `bs == NULL` before
all bytes remaining in `inputBs` were consumed by `DecodeFrameAsync`.

## Fix

`drainPipeline()` now drains in two phases:

1. Continue decoding any remaining `inputBs.DataLength` bytes with the real
   bitstream.
2. Only after the input bitstream is consumed, call decoder drain with `bs == NULL`
   to flush delayed frames.

This keeps the existing public ABI unchanged.

## Validation

Sample short video:

```text
input frames: 338
old realtime output: 327
fixed realtime output: 338
fixed adaptive MBQP output: 338
decode check errors: 0
```

10-minute B570 disk sample:

```text
input frames: 15004
fixed realtime output: 15004
decode check errors: 0
```

The adaptive path was verified with the original 2026-06-02 default parameters:

```text
profile=target_90_ssim_guard
gop_size=60
idr_interval=120
b_frames=0
force_mbqp_pattern=none
expert_options=none
actual_encode_control=MBQP
mbqp_applied_frames=337
```
