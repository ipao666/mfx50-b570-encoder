# Release Plan

## Positioning

Lead with the measurable rate-distortion result:

> x265-optimizer reduces GPU H.265 output size on real surveillance-style video while keeping SSIM and PSNR close to the baseline.

Do not lead with encoder concurrency. Concurrency is an implementation detail of the benchmark runner.

## Launch Checklist

- Keep only source code, benchmark summaries, and reproducibility metadata.
- Do not commit source videos, encoded videos, FFmpeg binaries, x265 binaries, or native build outputs.
- Link the benchmark report from the root README.
- Publish the exact command used to reproduce the benchmark.
- State the quality tradeoff honestly: 31.27% mean size reduction, -0.001814 mean SSIM delta, -0.974 dB mean PSNR delta.

## Suggested Announcement

```markdown
I open-sourced x265-optimizer, a GPU H.265 rate-distortion benchmark and optimization toolkit for real surveillance-style video.

On 10 real 5-minute samples, the improved NVENC configuration reduced output size by 31.27% on average while keeping the mean SSIM delta to -0.001814 and mean PSNR delta to -0.974 dB.

The repository includes the benchmark runner, CSV/JSON results, environment metadata, and a GitHub-friendly benchmark report.

Repo: <GitHub URL>
```

## Recommended GitHub Topics

`h265`, `hevc`, `nvenc`, `ffmpeg`, `video-compression`, `ssim`, `psnr`, `benchmark`, `gpu`, `rate-distortion`
