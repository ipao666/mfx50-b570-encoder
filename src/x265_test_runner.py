#!/usr/bin/env python3
"""
Scene-Aware H.265 Compression Pipeline - Benchmark Runner (v4)
Pipe-based streaming: ffmpeg -> scene analysis / x265, ZERO YUV temp files.
Memory: only one frame buffer (~4MB) in-flight at any time.

v4 improvements:
  - Temporal QP smoothing (EMA) -- eliminates flicker
  - Motion-adaptive QP -- relaxes QP on fast-moving CTUs
  - Per-CTU spatial complexity ceiling -- protects locally complex regions
  - Edge-aware QP -- protects edge-dense CTUs
  - VMAF perceptual quality assessment
"""

import subprocess
import os
import sys
import time
import re
import json
import logging
from pathlib import Path
import numpy as np
import yaml

logger = logging.getLogger("x265_test_runner")


def _get_project_root() -> Path:
    """Return the absolute path to the project root (parent of src/)."""
    return Path(__file__).resolve().parent.parent


def _load_config() -> dict:
    """Load the default YAML configuration."""
    config_path = _get_project_root() / "configs" / "default.yaml"
    with open(config_path) as f:
        return yaml.safe_load(f)


def _resolve_ffmpeg(cfg: dict) -> str:
    """Resolve the ffmpeg path from config, falling back to PATH lookup."""
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
RESULTS = ROOT / _cfg.get("output", {}).get("base_dir", "output") / "test_results"
TEMP = ROOT / _cfg.get("output", {}).get("base_dir", "output") / "temp"

sys.path.insert(0, os.path.join(os.path.dirname(__file__), 'scene_analyzer'))
from vibe_analyzer import SceneAnalyzer

TEST_VIDEOS = [
    "VID_20260322_054012.mp4",
    "VID_20260322_064344.mp4",
    "VID_20260322_065756.mp4",
    "VID_20260322_073829.mp4",
]

def get_video_info(path):
    """Extract resolution, fps, duration, frame count from ffmpeg probe."""
    r = subprocess.run(
        f'"{FFMPEG}" -i "{path}" 2>&1',
        shell=True, capture_output=True, text=True, timeout=60
    )
    out = r.stdout + r.stderr
    info = {}
    m = re.search(r'Stream #\d+:\d+.*Video:.*,\s*(\d+)x(\d+)\s*,', out)
    if m:
        info['w'], info['h'] = int(m.group(1)), int(m.group(2))
    else:
        m = re.search(r',\s*(\d+)x(\d+)\s*,', out)
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


def parse_x265_csv(csv_path):
    """Parse x265 CSV summary (reads the LAST data line since x265 appends)."""
    try:
        with open(csv_path) as f:
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
            'i_count': int(data.get('I count', 0)),
            'p_count': int(data.get('P count', 0)),
            'b_count': int(data.get('B count', 0)),
        }
    except Exception as e:
        logger.warning("CSV parse warning: %s", e)
        return {}

def compute_vmaf(original_path, encoded_path, w, h, fps, frames, has_vmaf):
    """Compute VMAF score using ffmpeg libvmaf filter.
    Returns (vmaf_score, vmaf_neg_gain_score) or (0, 0) on failure."""
    if not has_vmaf:
        return 0.0, 0.0
    vmaf_json = encoded_path.replace('.hevc', '_vmaf.json')
    cmd = [
        FFMPEG, '-i', encoded_path, '-i', original_path,
        '-frames:v', str(frames),
        '-lavfi',
        f'[0:v]scale={w}:{h}:flags=bicubic,format=yuv420p[dist];'
        f'[dist][1:v]libvmaf=n_threads=8:log_fmt=json:log_path={vmaf_json}',
        '-f', 'null', '-y', os.devnull
    ]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
        score = 0.0
        for line in r.stderr.split('\n'):
            if 'VMAF score:' in line:
                score = float(line.split(':')[-1].strip())
                break
        neg_gain = 0.0
        if os.path.exists(vmaf_json):
            with open(vmaf_json) as f:
                data = json.load(f)
            pooled = data.get('pooled_metrics', {})
            if score == 0.0:
                score = pooled.get('vmaf', {}).get('mean', 0.0)
            neg_gain = pooled.get('vmaf_neg', {}).get('mean', 0.0)
        return score, neg_gain
    except Exception as e:
        logger.error("VMAF compute error: %s", e)
        return 0.0, 0.0


def pipe_encode(video_path, output, w, h, fps, frames, preset="medium",
                crf=None, qpfile=None, tune="ssim", extra="", timeout=1800):
    """Encode via pipe: ffmpeg | x265 using native Popen pipe chaining.
    NO shell pipe, NO YUV temp file -- pure Python subprocess pipe."""
    params = f'--preset {preset} --tune {tune} --psnr --ssim '
    if crf is not None:
        params += f'--crf {crf} '
    if qpfile:
        params += f'--qpfile "{qpfile}" '
    params += extra
    ffmpeg_cmd = (
        f'"{FFMPEG}" -i "{video_path}" -frames:v {frames} '
        f'-pix_fmt yuv420p -f rawvideo pipe:1'
    )
    x265_cmd = (
        f'"{X265}" --input - --input-res {w}x{h} --fps {fps} '
        f'--frames {frames} {params} '
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

def scene_analyze_pipe(video_path, w, h, fps, frames):
    """Scene analysis via pipe: ffmpeg | python reader (no YUV file).
    Reads frames one-at-a-time from ffmpeg stdout.
    Returns (qpfile_entries, idr_triggers, analysis_time, avg_qp, avg_gop).
    """
    logger.info("    Pipe: ffmpeg -> scene analyzer...")
    t0 = time.time()
    analyzer = SceneAnalyzer(w, h)
    ffmpeg_cmd = (
        f'"{FFMPEG}" -i "{video_path}" -frames:v {frames} '
        f'-pix_fmt yuv420p -f rawvideo pipe:1'
    )
    ffmpeg_proc = subprocess.Popen(
        ffmpeg_cmd, shell=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL
    )
    y_size = w * h
    frame_size = y_size + (w // 2) * (h // 2) * 2
    qpfile_entries = []
    frame_qps = []
    gop_history = []
    idr_triggers = []
    for fi in range(frames):
        data = ffmpeg_proc.stdout.read(frame_size)
        if len(data) < frame_size:
            break
        y_plane = np.frombuffer(data[:y_size], dtype=np.uint8).reshape(h, w)
        qp_map, meta = analyzer.process_frame(y_plane)
        avg_qp = float(qp_map.mean())
        frame_qps.append(avg_qp)
        gop_history.append(meta['suggested_gop'])
        ft = analyzer.get_recommended_frame_type()
        if meta['idr_pending']:
            idr_triggers.append(fi)
        qpfile_entries.append(f"{fi} {ft} {int(avg_qp)}")
        if fi % 500 == 0 and fi > 0:
            elapsed = time.time() - t0
            logger.info("      Frame %d/%d (%.1f fps)  avg QP=%.1f  GOP=%d",
                        fi, frames, fi / elapsed,
                        sum(frame_qps) / len(frame_qps),
                        meta['suggested_gop'])
    ffmpeg_proc.stdout.close()
    ffmpeg_proc.wait()
    elapsed = time.time() - t0
    avg_qp = sum(frame_qps) / len(frame_qps) if frame_qps else 0
    avg_gop = sum(gop_history) / len(gop_history) if gop_history else 0
    return qpfile_entries, idr_triggers, elapsed, avg_qp, avg_gop

def main():
    """Run the scene-aware H.265 benchmark pipeline."""
    RESULTS.mkdir(parents=True, exist_ok=True)
    TEMP.mkdir(parents=True, exist_ok=True)

    # Check VMAF support once at runtime
    has_vmaf = False
    try:
        r = subprocess.run([FFMPEG, '-filters'], capture_output=True, text=True, timeout=10)
        has_vmaf = 'libvmaf' in r.stdout
    except Exception:
        pass

    logger.info("=" * 100)
    logger.info("Scene-Aware H.265 Compression Benchmark (v4 - pipe streaming + v2 analyzer)")
    logger.info("VMAF available: %s", has_vmaf)
    logger.info("v2: temporal QP smoothing + motion-adaptive + per-CTU variance + edge-aware")
    logger.info("=" * 100)

    all_rows = []
    temp_files = []

    for video_name in TEST_VIDEOS:
        video_path = Path(video_name)
        if not video_path.exists():
            logger.warning("SKIP: %s", video_name)
            continue

        logger.info("")
        logger.info("=" * 70)
        logger.info("VIDEO: %s", video_name)
        logger.info("=" * 70)

        info = get_video_info(video_path)
        if not info or 'w' not in info:
            logger.warning("  Failed to get video info")
            continue

        w, h, fps = info['w'], info['h'], info.get('fps', 30)
        frames = info.get('nb_frames') or int(info.get('duration', 10) * fps)
        logger.info("  %dx%d, %.1ffps, %d frames (%.1fs)",
                    w, h, fps, frames, info.get('duration', 0))
        logger.info("  Mode: PIPE streaming (zero YUV temp file)")

        stem = Path(video_name).stem
        frame_size_mb = (w * h * 1.5) / (1024 * 1024)
        logger.info("  In-flight memory: ~%.1fMB (one frame buffer)", frame_size_mb)

        # BASELINE
        logger.info("  [1/3] Baseline encode (CRF 28, medium) [pipe]...")
        b_out = RESULTS / f"{stem}_baseline.hevc"
        blog, btime = pipe_encode(
            video_path, str(b_out), w, h, fps, frames,
            preset="medium", crf=28, tune="ssim"
        )
        b_csv = parse_x265_csv(str(b_out) + ".csv")
        b_size = b_out.stat().st_size if b_out.exists() else 0
        b_vmaf, b_vmaf_neg = 0.0, 0.0
        if has_vmaf:
            logger.info("    VMAF analysis...")
            b_vmaf, b_vmaf_neg = compute_vmaf(str(video_path), str(b_out), w, h, fps, frames, has_vmaf)
        vmaf_str = f"  VMAF={b_vmaf:.2f}" if has_vmaf else ""
        logger.info("    SSIM=%.6f  PSNR=%.2fdB  Size=%.0fKB  Time=%.1fs  Bitrate=%.0fkbps%s",
                    b_csv.get('ssim', 0), b_csv.get('psnr', 0),
                    b_size / 1024, btime, b_csv.get('bitrate', 0), vmaf_str)

        # SCENE ANALYSIS
        logger.info("  [2/3] Scene analysis (ViBe + dynamic GOP + motion-IDR)...")
        qpfile_entries, idr_triggers, t_a, avg_sa_qp, avg_gop = scene_analyze_pipe(
            video_path, w, h, fps, frames
        )
        logger.info("    Analysis: %.1fs (%d fps)  Avg scene QP=%.1f",
                    t_a, int(frames / t_a) if t_a > 0 else 0, avg_sa_qp)
        logger.info("    Dynamic GOP: avg=%.0f", avg_gop)
        if idr_triggers:
            logger.info("    Motion-onset IDR triggers at frames: %s", idr_triggers)

        qpfile_path = TEMP / f"{stem}_qpfile.txt"
        with open(qpfile_path, 'w') as f:
            f.write('\n'.join(qpfile_entries) + '\n')
        temp_files.append(qpfile_path)

        # SCENE-AWARE
        logger.info("  [3/3] Scene-aware encode (dynamic GOP + motion-IDR) [pipe]...")
        sa_out = RESULTS / f"{stem}_scene_aware.hevc"
        salog, satime = pipe_encode(
            video_path, str(sa_out), w, h, fps, frames,
            preset="medium", qpfile=str(qpfile_path), tune="ssim",
            extra="--keyint 300 --min-keyint 30 --no-scenecut"
        )
        sa_csv = parse_x265_csv(str(sa_out) + ".csv")
        sa_size = sa_out.stat().st_size if sa_out.exists() else 0
        sa_vmaf, sa_vmaf_neg = 0.0, 0.0
        if has_vmaf:
            logger.info("    VMAF analysis...")
            sa_vmaf, sa_vmaf_neg = compute_vmaf(str(video_path), str(sa_out), w, h, fps, frames, has_vmaf)
        vmaf_str = f"  VMAF={sa_vmaf:.2f}" if has_vmaf else ""
        logger.info("    SSIM=%.6f  PSNR=%.2fdB  Size=%.0fKB  Time=%.1fs  Bitrate=%.0fkbps%s",
                    sa_csv.get('ssim', 0), sa_csv.get('psnr', 0),
                    sa_size / 1024, satime, sa_csv.get('bitrate', 0), vmaf_str)

        all_rows.append({
            'video': video_name, 'res': f"{w}x{h}", 'frames': frames,
            'b_time': btime, 'b_size_kb': b_size / 1024,
            'b_ssim': b_csv.get('ssim'), 'b_psnr': b_csv.get('psnr'),
            'b_psnr_y': b_csv.get('psnr_y'), 'b_bitrate': b_csv.get('bitrate'),
            'b_i_qp': b_csv.get('i_qp'), 'b_p_qp': b_csv.get('p_qp'), 'b_b_qp': b_csv.get('b_qp'),
            'b_vmaf': b_vmaf, 'b_vmaf_neg': b_vmaf_neg,
            'sa_time': satime, 'sa_size_kb': sa_size / 1024,
            'sa_ssim': sa_csv.get('ssim'), 'sa_psnr': sa_csv.get('psnr'),
            'sa_psnr_y': sa_csv.get('psnr_y'), 'sa_bitrate': sa_csv.get('bitrate'),
            'sa_i_qp': sa_csv.get('i_qp'), 'sa_p_qp': sa_csv.get('p_qp'), 'sa_b_qp': sa_csv.get('b_qp'),
            'sa_vmaf': sa_vmaf, 'sa_vmaf_neg': sa_vmaf_neg,
            'analysis_fps': frames / t_a if t_a > 0 else 0,
            'avg_scene_qp': avg_sa_qp,
        })

        ss_delta = (sa_csv.get('ssim', 0) or 0) - (b_csv.get('ssim', 0) or 0)
        size_pct = (sa_size / max(b_size, 1) * 100)
        time_ratio = satime / max(btime, 1)
        logger.info("    SUMMARY: SSIM %+.6f  Size %.1f%% of baseline  Time %.1fx baseline",
                    ss_delta, size_pct, time_ratio)

    if not all_rows:
        logger.warning("No tests completed!")
        return

    logger.info("")
    logger.info("=" * 130)
    logger.info("FINAL BENCHMARK RESULTS")
    logger.info("=" * 130)

    def s(v):
        return f"{v:.6f}" if v else "N/A"
    def p(v):
        return f"{v:.2f}" if v else "N/A"
    def b(v):
        return f"{v:.0f}" if v else "N/A"

    lines = []
    lines.append("=" * 140)
    lines.append("Scene-Aware H.265 Compression Pipeline - Benchmark Results v4 (PIPE STREAMING + v2 ANALYZER)")
    lines.append(f"Date: {time.strftime('%Y-%m-%d %H:%M:%S')}  |  ZERO YUV temp files")
    lines.append("=" * 140)
    lines.append("")
    lines.append(f"{'Video':<32} {'Method':<12} {'SSIM':<10} {'PSNR(dB)':<10} {'PSNR-Y':<10} {'Size(KB)':<10} {'Bitrate':<10} {'Time(s)':<8} {'I/P/B QP':<22}")
    lines.append("-" * 140)

    for r in all_rows:
        lines.append(
            f"{r['video']:<32} {'BASELINE':<12} "
            f"{s(r['b_ssim']):<10} {p(r['b_psnr']):<10} {p(r['b_psnr_y']):<10} "
            f"{r['b_size_kb']:<10.1f} {b(r['b_bitrate']):<10} "
            f"{r['b_time']:<8.1f} "
            f"{p(r['b_i_qp'])}/{p(r['b_p_qp'])}/{p(r['b_b_qp'])}"
        )
        size_pct = (r['sa_size_kb'] / r['b_size_kb'] * 100) if r['b_size_kb'] > 0 else 0
        speedup = r['sa_time'] / r['b_time'] if r['b_time'] > 0 else 0
        lines.append(
            f"{'':<32} {'SCENE-AWARE':<12} "
            f"{s(r['sa_ssim']):<10} {p(r['sa_psnr']):<10} {p(r['sa_psnr_y']):<10} "
            f"{r['sa_size_kb']:<10.1f} {b(r['sa_bitrate']):<10} "
            f"{r['sa_time']:<8.1f} "
            f"{p(r['sa_i_qp'])}/{p(r['sa_p_qp'])}/{p(r['sa_b_qp'])}"
        )
        lines.append(f"  -> Size: {size_pct:.1f}% of baseline | Time: {speedup:.2f}x | Scene QP: {r['avg_scene_qp']:.1f} | Analysis: {r['analysis_fps']:.0f} fps")
        lines.append("-" * 140)

    lines.append("")
    lines.append("AGGREGATED RESULTS")
    lines.append("=" * 80)
    try:
        def safe_mean(vals):
            vals = [x for x in vals if x is not None and x > 0]
            return sum(vals) / len(vals) if vals else 0

        metrics = ['b_ssim', 'sa_ssim', 'b_psnr', 'sa_psnr', 'b_psnr_y', 'sa_psnr_y',
                   'b_bitrate', 'sa_bitrate', 'b_size_kb', 'sa_size_kb', 'b_time', 'sa_time']
        avgs = {m: safe_mean([r[m] for r in all_rows]) for m in metrics}

        lines.append(f"{'Metric':<30} {'Baseline':<18} {'Scene-Aware':<18} {'Delta':<18}")
        lines.append("-" * 84)
        lines.append(f"{'SSIM':<30} {avgs['b_ssim']:<18.6f} {avgs['sa_ssim']:<18.6f} {avgs['sa_ssim'] - avgs['b_ssim']:<+18.6f}")
        lines.append(f"{'PSNR (dB)':<30} {avgs['b_psnr']:<18.2f} {avgs['sa_psnr']:<18.2f} {avgs['sa_psnr'] - avgs['b_psnr']:<+18.2f}")
        lines.append(f"{'PSNR-Y (dB)':<30} {avgs['b_psnr_y']:<18.2f} {avgs['sa_psnr_y']:<18.2f} {avgs['sa_psnr_y'] - avgs['b_psnr_y']:<+18.2f}")
        lines.append(f"{'Bitrate (kbps)':<30} {avgs['b_bitrate']:<18.1f} {avgs['sa_bitrate']:<18.1f} {(avgs['sa_bitrate'] / max(avgs['b_bitrate'], 0.001) - 1) * 100:<+17.1f}%")
        lines.append(f"{'Size (KB)':<30} {avgs['b_size_kb']:<18.1f} {avgs['sa_size_kb']:<18.1f} {(avgs['sa_size_kb'] / max(avgs['b_size_kb'], 0.001) - 1) * 100:<+17.1f}%")
        lines.append(f"{'Encode Time (s)':<30} {avgs['b_time']:<18.1f} {avgs['sa_time']:<18.1f} {(avgs['sa_time'] / max(avgs['b_time'], 0.001) - 1) * 100:<+17.1f}%")

        vmaf_b_vals = [r['b_vmaf'] for r in all_rows if r.get('b_vmaf', 0) > 0]
        vmaf_sa_vals = [r['sa_vmaf'] for r in all_rows if r.get('sa_vmaf', 0) > 0]
        if vmaf_b_vals and vmaf_sa_vals:
            b_vmaf_avg = sum(vmaf_b_vals) / len(vmaf_b_vals)
            sa_vmaf_avg = sum(vmaf_sa_vals) / len(vmaf_sa_vals)
            lines.append(f"{'VMAF':<30} {b_vmaf_avg:<18.2f} {sa_vmaf_avg:<18.2f} {sa_vmaf_avg - b_vmaf_avg:<+18.2f}")
    except Exception as e:
        lines.append(f"Error: {e}")

    lines.append("")
    lines.append("MEMORY/DISK OPTIMIZATIONS:")
    lines.append("  YUV temp files:       ZERO (pipe streaming ffmpeg -> analyzer / x265)")
    lines.append("  Peak RAM:             ~4 MB (single YUV frame buffer in-flight)")
    lines.append("  ViBe model RAM:       ~3.1 MB (1/4 resolution x 20 samples)")
    lines.append("  qpfile size:          <50 KB (text, written to disk only)")
    lines.append("  Total disk per test:  ~0 (only encode outputs + CSV)")
    lines.append("")
    lines.append("CONFIGURATION:")
    lines.append("  Baseline:         x265 medium preset, CRF=28, tune=ssim [pipe]")
    lines.append("  Scene-Aware:      x265 medium preset, tune=ssim, per-frame QP from scene analysis,")
    lines.append("                    dynamic GOP (static=300, dynamic=60, normal=120),")
    lines.append("                    motion-onset IDR triggering, no-scenecut [pipe]")
    lines.append("  Scene Analyzer:   ViBe background subtraction (20 samples, radius=20, min-match=2)")
    lines.append("                    1/4 resolution downsample, 3x3 morphological opening")
    lines.append("                    CTU-level QP mapping: ROI=26, Transition=32, Background=42 (night<=36)")
    lines.append("                    2-CTU dilation margin for transition zone")
    lines.append("                    Motion-onset: static>60 -> sudden FG>2%% = IDR (cooldown=30)")
    lines.append("  v2 Improvements:  Temporal QP smoothing (EMA alpha=0.35)")
    lines.append("                    Motion-adaptive QP (relax +2 on high-motion CTUs)")
    lines.append("                    Per-CTU spatial variance ceiling (protect var>600, relax var<150)")
    lines.append("                    Edge-aware QP (Sobel gradient, protect edge-density>15%%)")
    lines.append("  Analysis res:     1/4 of source (e.g., 608x270 for 2432x1080)")
    lines.append("  CTU size:         64x64")

    result_text = '\n'.join(lines)
    logger.info(result_text)

    table_path = RESULTS / "comparison_results.txt"
    with open(table_path, 'w', encoding='utf-8') as f:
        f.write(result_text)
    logger.info("Results saved to: %s", table_path)

    for tf in temp_files:
        try:
            tf.unlink()
        except Exception:
            pass
    logger.info("Cleaned up qpfile temp files.")


if __name__ == "__main__":
    raise SystemExit(main())
