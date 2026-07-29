# B570 SDK 文件夹结构说明

```text
mfx50rt-b570-sdk-<version>-linux-x86_64/
  README.md
  MANIFEST.txt
  sha256sums.txt
  include/
    mfx50_realtime.h
    mfx50_realtime_algo.h
    mfx50rt.h
    mfx50_encoder.h
    mfx50_device.h
    mfx50_decoder.h
    mfx50_surface.h
    mfx50_transcoder.h
    mfx50_policy.h
    mfx50_types.h
  lib/
    libmfx50_realtime.so*
    libmfx50rt.so*
    libmfx50_encoder.so*
    libmfx50_policy.so*
  bin/
    sample_multi_transcode_b570
    mfx50rt_api_probe
    mfx50rt_stream_probe
    query_capabilities
    simple_transcode
    async_45_routes
    bench_real_45_files
    minimal_realtime_demo
    policy_decision_demo
    mfx50_split_file_transcode
  examples/
    b570_fastpath/
      b570_policy_env.sh
      run_45_from_manifest.sh
      run_45_from_h264_dir.sh
      run_45_from_mp4_dir.sh
      run_45_daynight_from_mp4_dir.sh
      run_single_mp4.sh
    split_codec/
      mfx50_split_file_transcode.cpp
  docs/
    ENGINEER_HANDOFF.md
    B570_BINARY_SDK_README.md
    B570_API_REFERENCE.md
    B570_VIDEO_COMPRESSION_FLOW.md
    B570_FILE_STRUCTURE.md
    B570_CURRENT_LIMITS.md
    B570_OPTIMIZATION_DECISION.md
    MFX50_SPLIT_CODEC_API.md
    MFX50_CODEC_FUNCTION_REFERENCE.md
    MFX50_SPLIT_CODEC_CHANGE_SUMMARY_20260603.md
    MFX50_TRANSCODER_API_AND_DEMO.md
    POLICY_SDK_README.md
    POLICY_SDK_API.md
    B570_POLICY_MAPPING.md
  validation/
    api_probe.log
    ENVIRONMENT.md
    minimal_realtime_demo.log
    results_size_first_45x1000.csv
    results_y_guard_45x1000.csv
    results_size_first_45_full.csv
    old_onevpl_20route_summary.txt
```

请先阅读 `docs/ENGINEER_HANDOFF.md`。

`sample_multi_transcode_b570` 是已经验证过的 B570 oneVPL 多路压缩二进制程序。它作为二进制 example 交付，不包含源码。

`run_45_from_manifest.sh` 是推荐的回归/工业示例入口。它读取 manifest，应用 policy profile 映射，导出 B570 算法环境变量，然后调用 `sample_multi_transcode_b570`。

`mfx50_device.h`、`mfx50_decoder.h`、`mfx50_surface.h`、`mfx50_encoder.h` 是新的 split codec API。它把 Device、Decoder、Surface、Encoder 的边界拆开，业务方可以直接把硬解得到的 NV12 GPU surface 交给 encoder；如果需要 MFX50 自动硬解，则走 `mfx50_decoder.h` 的 packet -> surface 入口。

`MFX50_CODEC_FUNCTION_REFERENCE.md` 是 encoder/decoder 函数说明。`bin/mfx50_split_file_transcode` 和 `examples/split_codec/mfx50_split_file_transcode.cpp` 是本次 split decoder 修复的完整可运行 demo，演示 MFX50 自动硬解入口：H.264/H.265 Annex-B packet -> Decoder -> NV12 oneVPL GPU surface -> Encoder。

`libmfx50_policy.so` 是跨设备 policy 库。工程师自建流水线时，可以直接调用它得到 QP/GOP/ROI 等算法决策。

`mfx50_realtime.h` 是保留的实时 C API 原型接口。

`mfx50rt.h` 是部分内部 demo 使用的底层 ABI，当前不作为首选商业集成接口。
