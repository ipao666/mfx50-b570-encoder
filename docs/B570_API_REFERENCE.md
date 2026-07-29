# B570 SDK API Reference

## Header choice

Use `mfx50_realtime.h` for current production examples. It is the interface used by `mfx50rt_stream_probe` and the packaged realtime demos.

`mfx50rt.h` is a newer lower-level ABI used by some internal probes. Keep it as an advanced interface unless the integration team explicitly wants to bind to it.

## `MFX50RT_DefaultConfig`

```c
int MFX50RT_DefaultConfig(MFX50RT_Config* cfg);
```

Initializes all fields and sets ABI/version sizes. Always call this before editing a config.

Important fields:

- `input_mode`: use `MFX50_INPUT_ENCODED_PACKET` for realtime H.264/H.265 packet input.
- `output_mode`: `MFX50_OUTPUT_POLL` or `MFX50_OUTPUT_CALLBACK`.
- `input_codec`: `MFX50_CODEC_H264` or `MFX50_CODEC_HEVC`.
- `output_codec`: current production output is `MFX50_CODEC_HEVC`.
- `device_selector`: `intel:dGPU:0` or `/dev/dri/renderD129` for B570.
- `route_count`: number of simultaneous streams.
- `async_mode`: set to `1` for realtime multi-route worker mode.
- `max_input_queue_packets`: per-route input queue size.
- `max_output_queue_packets`: output queue size.
- `drop_policy`: keep `MFX50RT_DROP_NONE` when frames must not be dropped.
- `gop`: GOP size.
- `gop_ref_dist`: reference distance. Values above 1 enable B-frame structure.
- `qpi/qpp/qpb`: base QP for I/P/B frames.

## `MFX50RT_Create`

```c
int MFX50RT_Create(const MFX50RT_Config* cfg, MFX50RT_Handle* out_handle);
```

Creates the realtime encoder context. One handle can contain multiple routes when `route_count > 1`.

Return values:

- `MFX50_OK`: created successfully.
- `MFX50_ERR_INVALID_ARG`: invalid config or route count.
- `MFX50_ERR_DEVICE`: oneVPL/VA device initialization failed.
- `MFX50_ERR_NOT_IMPLEMENTED`: requested input/output mode is not implemented.

## `MFX50RT_PushPacket`

```c
int MFX50RT_PushPacket(MFX50RT_Handle h, const MFX50RT_Packet* packet);
```

Pushes one encoded access unit into one route.

Packet fields:

- `struct_size`: `sizeof(MFX50RT_Packet)`.
- `stream_id`: route index, from `0` to `route_count - 1`.
- `data`, `size`: encoded access unit bytes.
- `pts`, `dts`: timestamps in caller-defined units.
- `is_keyframe`: non-zero for IDR/key access units.
- `end_of_stream`: non-zero to mark EOS.

If `async_mode=1` and the queue is full, the function can return `MFX50_ERR_BACKPRESSURE`. The caller should wait briefly and retry, or configure a drop policy if loss is acceptable.

## `MFX50RT_PollPacket`

```c
int MFX50RT_PollPacket(MFX50RT_Handle h, MFX50RT_EncodedPacket* out_packet);
```

Polls encoded output in pull mode.

Output packet fields:

- `data`: caller-provided buffer.
- `capacity`: buffer size.
- `size`: actual output bytes after success.
- `stream_id`: route id.
- `pts`, `dts`: output timestamps.
- `is_keyframe`: key packet marker.

Return values:

- `MFX50_OK`: output packet available.
- `MFX50_ERR_NO_OUTPUT`: no packet available now.
- `MFX50_ERR_BUFFER_TOO_SMALL`: enlarge `data` to at least returned `size`.

## Callback output

```c
int MFX50RT_SetOutputCallback(
    MFX50RT_Handle h,
    MFX50RT_OutputCallback cb,
    void* user_opaque);
```

Use callback mode when the integration wants SDK-driven output delivery. Keep callback work short; heavy file IO or muxing should be moved to the caller's own worker queue.

## Stats and diagnostics

```c
int MFX50RT_GetStats(MFX50RT_Handle h, MFX50RT_Stats* stats);
const char* MFX50RT_GetLastError(MFX50RT_Handle h);
const char* MFX50RT_StatusString(int code);
```

`MFX50RT_Stats` exposes packet counts, frame counts, fps, queue depth, input/output bytes, error counters, and MBQP/preprocess counters.

## Raw YUV input status

```c
int MFX50RT_PushFrame(MFX50RT_Handle h, const MFX50RT_RawFrame* frame);
```

The symbol is present but current realtime implementation returns `MFX50_ERR_NOT_IMPLEMENTED`. For 45-route B570 deployment, keep the pipeline on encoded packets or integrate raw frame ingestion directly inside a zero-copy oneVPL path before using it commercially.
