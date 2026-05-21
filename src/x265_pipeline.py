#!/usr/bin/env python3
"""
x265 Power/Speed Optimization Benchmark Runner
Pipe-based streaming: ffmpeg -> x265, ZERO YUV temp files.
Compares: Baseline (all optimizations OFF) vs Optimized (7 power/speed switches ON).
"""

import subprocess
import os
import sys
import time
import re
import argparse
import logging
from pathlib import Path
import numpy as np
import yaml

logger = logging.getLogger("x265_pipeline")


def _get_project_root() -> Path:
    return Path(__file__).resolve().parent.parent


def _load_config() -> dict:
    config_path = _get_project_root() / "configs" / "default.yaml"
    with open(config_path) as f:
        return yaml.safe_load(f)


def _resolve_ffmpeg(cfg: dict) -> str:
    import shutil
    ffmpeg_path = cfg.get("ffmpeg", {}).get("path", "ffmpeg")
    if Path(ffmpeg_path).exists():
        return ffmpeg_path
    found = shutil.which(ffmpeg_path)
    if found:
        return found
    raise FileNotFoundError(f"FFmpeg not found: {ffmpeg_path}")


_cfg = _load_config()
ROOT = _get_project_root()
X265 = str(ROOT / "x265" / "bin" / "x265.exe")
FFMPEG = _resolve_ffmpeg(_cfg)
RESULTS = ROOT / _cfg.get("output", {}).get("base_dir", "output") / "bench_results"
TEMP = ROOT / _cfg.get("output", {}).get("base_dir", "output") / "temp"

OPT_SWITCHES = (
    "--split-early-exit "
    "--temporal-split-prior "
    "--content-pre-check "
    "--low-power-shutoff "
    "--low-power-partition "
    "--spatiotemporal-skip "
    "--adaptive-searchrange"
)

TEST_VIDEOS = ["0001.mp4", "0002.mp4", "0003.mp4", "0004.mp4"]


def get_video_info(path: str) -> dict:
    r = subprocess.run(
        f'"{FFMPEG}" -i "{path}" 2>&1',
        shell=True, capture_output=True, timeout=60
    )
    out = (r.stdout or b'').decode('utf-8', errors='replace') +           (r.stderr or b'').decode('utf-8', errors='replace')
    info = {}
    m = re.search(r'Stream #\d+:\d+.*Video:.*,\s*(\d+)x(\d+)', out)
    if m:
        info['w'], info['h'] = int(m.group(1)), int(m.group(2))
    else:
        m = re.search(r',\s*(\d+)x(\d+)', out)
        if m:
            info['w'], info['h'] = int(m.group(1)), int(m.group(2))
    m = re.search(r'(\d+\.?\d*)\s*fps', out)
    if m:
        info['fps'] = float(m.group(1))
    m = re.search(r'Duration:\s*(\d+):(\d+):(\d+\.\d+)', out)
    if m:
        info['duration'] = float(m.group(1)) * 3600 + float(m.group(2)) * 60 + float(m.group(3))
    fm = re.search(r'(\d+)\s*frames', out)
    if fm:
        info['nb_frames'] = int(fm.group(1))
    elif 'duration' in info and 'fps' in info:
        info['nb_frames'] = int(info['duration'] * info['fps'])
    return info

OPT_SWITCHES = (
    "--split-early-exit "
    "--temporal-split-prior "
    "--content-pre-check "
    "--low-power-shutoff "
    "--low-power-partition "
    "--spatiotemporal-skip "
    "--adaptive-searchrange"
)

TEST_VIDEOS = ["0001.mp4", "0002.mp4", "0003.mp4", "0004.mp4"]


def get_video_info(path: str) -> dict:
    """Extract resolution, fps, duration, frame count from ffmpeg probe."""
    r = subprocess.run(
        f'"{FFMPEG}" -i "{path}" 2>&1',
        shell=True, capture_output=True, timeout=60
    )
    out = (r.stdout or b'').decode('utf-8', errors='replace') + \
          (r.stderr or b'').decode('utf-8', errors='replace')
    info = {}
    m = re.search(r'Stream #\d+:\d+.*Video:.*,\s*(\d+)x(\d+)', out)
    if m:
        info['w'], info['h'] = int(m.group(1)), int(m.group(2))
    else:
        m = re.search(r',\s*(\d+)x(\d+)', out)
        if m:
            info['w'], info['h'] = int(m.group(1)), int(m.group(2))
    m = re.search(r'(\d+\.?\d*)\s*fps', out)
    if m:
        info['fps'] = float(m.group(1))
    m = re.search(r'Duration:\s*(\d+):(\d+):(\d+\.\d+)', out)
    if m:
        info['duration'] = float(m.group(1)) * 3600 + float(m.group(2)) * 60 + float(m.group(3))
    fm = re.search(r'(\d+)\s*frames', out)
    if fm:
        info['nb_frames'] = int(fm.group(1))
    elif 'duration' in info and 'fps' in info:
        info['nb_frames'] = int(info['duration'] * info['fps'])
    return info

def parse_x265_csv(csv_path: str) -> dict:
    """Parse x265 CSV summary (reads the LAST data line since x265 appends)."""
    try:
        with open(csv_path, encoding='utf-8', errors='replace') as f:
            lines = f.readlines()
        if len(lines) < 2:
            return {}
        header = lines[0].strip().split(', ')
        values = lines[-1].strip().split(', ')
        data = dict(zip(header, values))
        return {
            'ssim': float(data.get('SSIM', 0)),
            'psnr': float(data.get('Global PSNR', 0)),
            'psnr_y': float(data.get('Y PSNR', 0)),
            'bitrate': float(data.get('Bitrate', 0)),
            'elapsed': float(data.get('Elapsed Time', 0)),
            'fps': float(data.get('FPS', 0)),
            'i_qp': float(data.get('I ave-QP', 0)),
            'p_qp': float(data.get('P ave-QP', 0)),
            'b_qp': float(data.get('B ave-QP', 0)),
        }
    except Exception as e:
        logger.warning("CSV parse warning: %s", e)
        return {}


def pipe_encode(video_path: str, output: str, w: int, h: int, fps: float,
                frames: int, preset: str = "medium",
                crf: int = 28, tune: str = "ssim", extra: str = "",
                timeout: int = 1800) -> tuple:
    """Encode via pipe: ffmpeg | x265 using native Popen pipe chaining."""
    params = f'--preset {preset} --tune {tune} --psnr --ssim --crf {crf} '
    if extra:
        params += extra + ' '
    ffmpeg_cmd = (
        f'"{FFMPEG}" -i "{video_path}" -frames:v {frames} '
        f'-pix_fmt yuv420p -f rawvideo pipe:1'
    )
    x265_cmd = (
        f'"{X265}" --input - --input-res {w}x{h} --fps {fps} '
        f'--frames {frames} {params}'
        f'--csv "{output}.csv" -o "{output}"'
    )
    t0 = time.time()
    csv_path = Path(f"{output}.csv")
    if csv_path.exists():
        csv_path.unlink()
    ffmpeg_proc = subprocess.Popen(
        ffmpeg_cmd, shell=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL
    )
    x265_proc = subprocess.Popen(
        x265_cmd, shell=True, stdin=ffmpeg_proc.stdout,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    ffmpeg_proc.stdout.close()
    try:
        x265_out, x265_err = x265_proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        ffmpeg_proc.kill()
        x265_proc.kill()
        ffmpeg_proc.wait()
        x265_proc.wait()
        return "TIMEOUT", time.time() - t0
    ffmpeg_proc.wait()
    elapsed = time.time() - t0
    return (x265_out or b'').decode('utf-8', errors='replace') + \
           (x265_err or b'').decode('utf-8', errors='replace'), elapsed

def run_benchmark() -> None:
    """Run the multi-video baseline vs optimized benchmark."""
    RESULTS.mkdir(parents=True, exist_ok=True)
    logger.info("=" * 100)
    logger.info("x265 Power/Speed Optimization Benchmark (Pipe Streaming)")
    logger.info("=" * 100)
    logger.info("  x265:    %s", X265)
    logger.info("  ffmpeg:  %s", FFMPEG)
    logger.info("  Results: %s", RESULTS)
    logger.info("  Baseline:   CRF=28, medium, tune=ssim")
    logger.info("  Optimized:  +%s", OPT_SWITCHES)
    logger.info("")
    all_rows = []
    for video_name in TEST_VIDEOS:
        video_path = Path(video_name)
        if not video_path.exists():
            logger.warning("SKIP: %s not found at %s", video_name, video_path)
            continue
        logger.info("=" * 70)
        logger.info("VIDEO: %s", video_name)
        logger.info("=" * 70)
        info = get_video_info(str(video_path))
        if not info or 'w' not in info:
            logger.warning("  Failed to get video info")
            continue
        w, h, fps_val = info['w'], info['h'], info.get('fps', 30)
        frames = info.get('nb_frames') or int(info.get('duration', 10) * fps_val)
        logger.info("  %dx%d, %.1ffps, %d frames (%.1fs)",
                    w, h, fps_val, frames, info.get('duration', 0))
        logger.info("  Mode: PIPE streaming (zero YUV temp files)")
        stem = Path(video_name).stem
        logger.info("  [1/2] Baseline encode (CRF 28, medium, all optimizations OFF)...")
        b_out = RESULTS / f"{stem}_baseline.hevc"
        blog, btime = pipe_encode(str(video_path), str(b_out), w, h, fps_val, frames)
        b_csv = parse_x265_csv(str(b_out) + ".csv")
        b_size = b_out.stat().st_size if b_out.exists() else 0
        logger.info("    SSIM=%.6f  PSNR=%.2fdB  FPS=%.1f  Size=%.0fKB  Bitrate=%.0fkbps  Time=%.1fs",
                    b_csv.get('ssim', 0), b_csv.get('psnr', 0),
                    b_csv.get('fps', 0), b_size / 1024,
                    b_csv.get('bitrate', 0), btime)
        logger.info("  [2/2] Optimized encode (CRF 28, medium, 7 switches ON)...")
        o_out = RESULTS / f"{stem}_optimized.hevc"
        olog, otime = pipe_encode(str(video_path), str(o_out), w, h, fps_val, frames, extra=OPT_SWITCHES)
        o_csv = parse_x265_csv(str(o_out) + ".csv")
        o_size = o_out.stat().st_size if o_out.exists() else 0
        logger.info("    SSIM=%.6f  PSNR=%.2fdB  FPS=%.1f  Size=%.0fKB  Bitrate=%.0fkbps  Time=%.1fs",
                    o_csv.get('ssim', 0), o_csv.get('psnr', 0),
                    o_csv.get('fps', 0), o_size / 1024,
                    o_csv.get('bitrate', 0), otime)
        fps_speedup = o_csv.get('fps', 0) / max(b_csv.get('fps', 0), 0.001)
        time_ratio = otime / max(btime, 0.001)
        ssim_delta = (o_csv.get('ssim', 0) or 0) - (b_csv.get('ssim', 0) or 0)
        size_pct = o_size / max(b_size, 1) * 100
        logger.info("    SUMMARY: FPS x%.2f  SSIM %+.6f  Size %.1f%%  Time %.2fx",
                    fps_speedup, ssim_delta, size_pct, time_ratio)
        all_rows.append({
            'video': video_name, 'res': f"{w}x{h}", 'frames': frames,
            'b_time': btime, 'b_size_kb': b_size / 1024,
            'b_ssim': b_csv.get('ssim'), 'b_psnr': b_csv.get('psnr'),
            'b_psnr_y': b_csv.get('psnr_y'), 'b_bitrate': b_csv.get('bitrate'),
            'b_fps': b_csv.get('fps'),
            'b_i_qp': b_csv.get('i_qp'), 'b_p_qp': b_csv.get('p_qp'), 'b_b_qp': b_csv.get('b_qp'),
            'o_time': otime, 'o_size_kb': o_size / 1024,
            'o_ssim': o_csv.get('ssim'), 'o_psnr': o_csv.get('psnr'),
            'o_psnr_y': o_csv.get('psnr_y'), 'o_bitrate': o_csv.get('bitrate'),
            'o_fps': o_csv.get('fps'),
            'o_i_qp': o_csv.get('i_qp'), 'o_p_qp': o_csv.get('p_qp'), 'o_b_qp': o_csv.get('b_qp'),
        })

    if not all_rows:
        logger.warning("No tests completed!")
        return
    logger.info("")
    logger.info("=" * 130)
    logger.info("FINAL BENCHMARK RESULTS")
    logger.info("=" * 130)

    def sf(v):
        return f"{v:.6f}" if v else "N/A"
    def pf(v):
        return f"{v:.2f}" if v else "N/A"
    def bf(v):
        return f"{v:.0f}" if v else "N/A"

    lines = []
    lines.append("=" * 140)
    lines.append("x265 Power/Speed Optimization - Benchmark Results (PIPE STREAMING)")
    lines.append(f"Date: {time.strftime('%Y-%m-%d %H:%M:%S')}  |  ZERO YUV temp files")
    lines.append("=" * 140)
    lines.append("")
    lines.append(f"{'Video':<32} {'Mode':<12} {'SSIM':<10} {'PSNR':<10} {'PSNR-Y':<10} {'FPS':<8} {'Bitrate':<10} {'Size(KB)':<10} {'Time(s)':<8} {'I/P/B QP':<22}")
    lines.append("-" * 140)
    for r in all_rows:
        lines.append(
            f"{r['video']:<32} {'BASELINE':<12} "
            f"{sf(r['b_ssim']):<10} {pf(r['b_psnr']):<10} {pf(r['b_psnr_y']):<10} "
            f"{pf(r['b_fps']):<8} {bf(r['b_bitrate']):<10} "
            f"{r['b_size_kb']:<10.1f} {r['b_time']:<8.1f} "
            f"{pf(r['b_i_qp'])}/{pf(r['b_p_qp'])}/{pf(r['b_b_qp'])}"
        )
        lines.append(
            f"{'':<32} {'OPTIMIZED':<12} "
            f"{sf(r['o_ssim']):<10} {pf(r['o_psnr']):<10} {pf(r['o_psnr_y']):<10} "
            f"{pf(r['o_fps']):<8} {bf(r['o_bitrate']):<10} "
            f"{r['o_size_kb']:<10.1f} {r['o_time']:<8.1f} "
            f"{pf(r['o_i_qp'])}/{pf(r['o_p_qp'])}/{pf(r['o_b_qp'])}"
        )
        fps_ratio = r['o_fps'] / max(r['b_fps'], 0.001) if r['b_fps'] and r['o_fps'] else 0
        time_ratio = r['o_time'] / max(r['b_time'], 0.001) if r['b_time'] > 0 else 0
        size_pct = r['o_size_kb'] / max(r['b_size_kb'], 0.001) * 100 if r['b_size_kb'] > 0 else 0
        ssim_delta = (r['o_ssim'] or 0) - (r['b_ssim'] or 0)
        lines.append(f"  -> FPS x{fps_ratio:.2f} | Time {time_ratio:.2f}x | Size {size_pct:.1f}% | SSIM {ssim_delta:+.6f}")
        lines.append("-" * 140)
    lines.append("")
    lines.append("AGGREGATED RESULTS")
    lines.append("=" * 84)

    def safe_mean(vals):
        vals = [x for x in vals if x is not None and x > 0]
        return sum(vals) / len(vals) if vals else 0

    metrics = ['b_ssim', 'o_ssim', 'b_psnr', 'o_psnr', 'b_psnr_y', 'o_psnr_y',
               'b_bitrate', 'o_bitrate', 'b_size_kb', 'o_size_kb',
               'b_time', 'o_time', 'b_fps', 'o_fps']
    avgs = {m: safe_mean([r[m] for r in all_rows]) for m in metrics}
    lines.append(f"{'Metric':<30} {'Baseline':<18} {'Optimized':<18} {'Delta':<18}")
    lines.append("-" * 84)
    lines.append(f"{'SSIM':<30} {avgs['b_ssim']:<18.6f} {avgs['o_ssim']:<18.6f} {avgs['o_ssim'] - avgs['b_ssim']:<+18.6f}")
    lines.append(f"{'PSNR (dB)':<30} {avgs['b_psnr']:<18.2f} {avgs['o_psnr']:<18.2f} {avgs['o_psnr'] - avgs['b_psnr']:<+18.2f}")
    lines.append(f"{'PSNR-Y (dB)':<30} {avgs['b_psnr_y']:<18.2f} {avgs['o_psnr_y']:<18.2f} {avgs['o_psnr_y'] - avgs['b_psnr_y']:<+18.2f}")
    lines.append(f"{'FPS':<30} {avgs['b_fps']:<18.1f} {avgs['o_fps']:<18.1f} {(avgs['o_fps'] / max(avgs['b_fps'], 0.001) - 1) * 100:<+17.1f}%")
    lines.append(f"{'Bitrate (kbps)':<30} {avgs['b_bitrate']:<18.1f} {avgs['o_bitrate']:<18.1f} {(avgs['o_bitrate'] / max(avgs['b_bitrate'], 0.001) - 1) * 100:<+17.1f}%")
    lines.append(f"{'Size (KB)':<30} {avgs['b_size_kb']:<18.1f} {avgs['o_size_kb']:<18.1f} {(avgs['o_size_kb'] / max(avgs['b_size_kb'], 0.001) - 1) * 100:<+17.1f}%")
    lines.append(f"{'Encode Time (s)':<30} {avgs['b_time']:<18.1f} {avgs['o_time']:<18.1f} {(avgs['o_time'] / max(avgs['b_time'], 0.001) - 1) * 100:<+17.1f}%")
    lines.append("")
    lines.append("OPTIMIZATION SWITCHES ENABLED:")
    lines.append(f"  {OPT_SWITCHES}")
    lines.append("")
    lines.append("ENCODER:")
    lines.append("  x265 HEVC encoder version 4.0+1-6318f22 [Windows][GCC 15.2.0][64 bit] 8bit")
    lines.append("")
    lines.append("ENCODE SETTINGS:")
    lines.append("  Baseline:   --preset medium --crf 28 --tune ssim --psnr --ssim")
    lines.append("  Optimized:  same + 7 power/speed optimization switches")
    lines.append("")
    lines.append("MEMORY/DISK:")
    lines.append("  YUV temp files: ZERO (pipe streaming ffmpeg -> x265)")
    lines.append("  Peak RAM: ~4-12 MB (single YUV frame buffer in-flight)")
    result_text = '\n'.join(lines)
    logger.info(result_text)
    table_path = RESULTS / "comparison_results.txt"
    with open(table_path, 'w', encoding='utf-8') as f:
        f.write(result_text)
    logger.info("Results saved to: %s", table_path)

def print_available_optimizations() -> None:
    """Print the list of available optimization flags."""
    print("Available x265 low-power optimization flags:")
    for flag in OPT_SWITCHES.split():
        print(f"  {flag}")
    print()
    print("These flags can be passed to --optimizations")


def parse_args(argv=None) -> argparse.Namespace:
    """Parse command-line arguments."""
    parser = argparse.ArgumentParser(
        description="x265 scene-aware encoding pipeline"
    )
    parser.add_argument("--input", required=True, help="Input video file")
    parser.add_argument("--output-dir", default=str(RESULTS),
                        help="Output directory for encoded files")
    parser.add_argument("--preset",
                        default=_cfg.get("x265", {}).get("preset", "medium"),
                        help="x265 preset")
    parser.add_argument("--crf", type=int, default=28, help="x265 CRF value")
    parser.add_argument("--optimizations", nargs="*", default=[],
                        help="Low-power optimization flags to enable")
    parser.add_argument("--list-optimizations", action="store_true",
                        help="List available optimization flags")
    parser.add_argument("--verbose", "-v", action="store_true",
                        help="Enable debug logging")
    return parser.parse_args(argv)


def main(argv=None) -> int:
    """CLI entry point for the x265 encoding pipeline."""
    args = parse_args(argv)
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s [%(levelname)s] %(message)s",
    )
    if args.list_optimizations:
        print_available_optimizations()
        return 0
    video_path = Path(args.input)
    if not video_path.exists():
        logger.error("Input video not found: %s", video_path)
        return 1
    info = get_video_info(str(video_path))
    if not info or 'w' not in info:
        logger.error("Failed to get video info for: %s", video_path)
        return 1
    w, h, fps_val = info['w'], info['h'], info.get('fps', 30)
    frames = info.get('nb_frames') or int(info.get('duration', 10) * fps_val)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    stem = video_path.stem
    preset = args.preset
    crf = args.crf
    extra = " ".join(args.optimizations) if args.optimizations else OPT_SWITCHES
    logger.info("Baseline encode (preset=%s, crf=%d, all optimizations OFF)...", preset, crf)
    b_out = output_dir / f"{stem}_baseline.hevc"
    blog, btime = pipe_encode(str(video_path), str(b_out), w, h, fps_val, frames,
                               preset=preset, crf=crf, tune="ssim")
    b_csv = parse_x265_csv(str(b_out) + ".csv")
    b_size = b_out.stat().st_size if b_out.exists() else 0
    logger.info("  Baseline: SSIM=%.6f  PSNR=%.2fdB  FPS=%.1f  Size=%.0fKB  Time=%.1fs",
                b_csv.get('ssim', 0), b_csv.get('psnr', 0),
                b_csv.get('fps', 0), b_size / 1024, btime)
    logger.info("Optimized encode (preset=%s, crf=%d, extra switches ON)...", preset, crf)
    o_out = output_dir / f"{stem}_optimized.hevc"
    olog, otime = pipe_encode(str(video_path), str(o_out), w, h, fps_val, frames,
                               preset=preset, crf=crf, tune="ssim", extra=extra)
    o_csv = parse_x265_csv(str(o_out) + ".csv")
    o_size = o_out.stat().st_size if o_out.exists() else 0
    logger.info("  Optimized: SSIM=%.6f  PSNR=%.2fdB  FPS=%.1f  Size=%.0fKB  Time=%.1fs",
                o_csv.get('ssim', 0), o_csv.get('psnr', 0),
                o_csv.get('fps', 0), o_size / 1024, otime)
    fps_speedup = o_csv.get('fps', 0) / max(b_csv.get('fps', 0), 0.001)
    ssim_delta = (o_csv.get('ssim', 0) or 0) - (b_csv.get('ssim', 0) or 0)
    size_pct = o_size / max(b_size, 1) * 100
    time_ratio = otime / max(btime, 0.001)
    logger.info("  SUMMARY: FPS x%.2f  SSIM %+.6f  Size %.1f%%  Time %.2fx",
                fps_speedup, ssim_delta, size_pct, time_ratio)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
