# Contributing

Thanks for considering a contribution to x265-optimizer.

## Good first contributions

- Improve README examples for different FFmpeg/NVENC builds.
- Add input preflight checks for corrupted or unsupported video files.
- Improve result summaries for SSIM, PSNR, bitrate, and size reduction.
- Add tests around metric parsing and report generation.
- Add docs for specific NVIDIA GPU setups.

## Development setup

```bash
python -m venv .venv
.\.venv\Scripts\activate
python -m pip install -U pip
python -m pip install -r requirements.txt
python -m py_compile tools\run_gpu_h265_benchmark.py
```

## Pull request expectations

- Keep changes focused.
- Include tests for pure-Python behavior.
- For encoder changes, include the FFmpeg command, hardware details, and a small summary of the run.
- Do not commit large videos, generated `.hevc` files, local logs, or native build outputs.

## Benchmark reports

When reporting benchmark results, include:

- CPU, GPU, RAM, OS, driver version.
- FFmpeg version and encoder availability.
- Source clip count, resolution, FPS, duration, and whether sources were preflighted.
- Encoder preset, bitrate or CRF, and duration limit.
- Summary JSON plus representative failed logs.
