# Benchmark Analysis

The public benchmark is stored in:

`benchmarks/2026-05-20-gpu-h265-rate-distortion`

## Dataset

- 10 real surveillance-style video samples
- 5 minutes per sample
- GPU concurrent H.265/NVENC encoding
- Source videos are not included in the repository

## Compared Methods

Baseline:

- `hevc_nvenc`
- preset `p4`
- tune `hq`
- `cq=28`

Improved:

- `hevc_nvenc`
- preset `p7`
- tune `hq`
- `cq=34`
- spatial AQ enabled
- temporal AQ enabled
- B-reference mode enabled

## Aggregate Result

| Metric | Value |
| --- | ---: |
| Completed samples | 10 / 10 |
| Mean size reduction | 31.27% |
| Mean SSIM delta | -0.001814 |
| Mean PSNR delta | -0.974 dB |
| Baseline bitrate | 1389.9 kbps |
| Improved bitrate | 898.8 kbps |

## Interpretation

The improved configuration produced materially smaller H.265 files while keeping objective quality close to the baseline on average. The result should be described as a rate-distortion tradeoff, not as lossless or quality-identical compression.

The most accurate public claim is:

> On 10 real 5-minute samples, the improved GPU H.265 configuration reduced output size by 31.27% on average, with mean SSIM delta -0.001814 and mean PSNR delta -0.974 dB.

## Reproducibility

Run:

```bash
python tools/run_gpu_h265_benchmark.py ^
  --dataset E:\Video_Compression\5.6 ^
  --output-dir benchmarks\local-gpu-h265-rate-distortion ^
  --count 10 ^
  --seconds 300 ^
  --workers 4 ^
  --baseline-cq 28 ^
  --improved-cq 34
```
