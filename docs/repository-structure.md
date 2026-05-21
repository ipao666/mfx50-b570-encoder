# Repository Structure

This is the GitHub-ready structure.

```text
x265-optimizer/
  README.md
  LICENSE
  requirements.txt
  tools/
    run_gpu_h265_benchmark.py
    run_rate_distortion_benchmark.py
  src/
    benchmark and pipeline utilities
  benchmarks/
    2026-05-20-gpu-h265-rate-distortion/
      README.md
      results.csv
      results.json
      summary.json
      environment.json
  docs/
    assets/
      benchmark-summary.svg
```

Large or sensitive files are intentionally excluded:

- source videos
- encoded `.mp4` / `.hevc` artifacts
- FFmpeg binaries
- x265 binaries and upstream source trees
- Visual Studio/CMake build outputs

The benchmark can regenerate ignored artifacts locally.
