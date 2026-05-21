#!/usr/bin/env python3
"""Run a GitHub-ready 5-minute rate-distortion benchmark.

This benchmark samples videos from a dataset, compares plain H.265/x265
against the scene-aware improved method, and writes CSV/JSON/Markdown output.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import re
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import asdict, dataclass
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
sys.path.insert(0, str(SRC / "scene_analyzer"))
from vibe_analyzer import SceneAnalyzer  # noqa: E402

OPT_SWITCHES = [
    "--split-early-exit",
    "--temporal-split-prior",
    "--content-pre-check",
    "--low-power-shutoff",
    "--low-power-partition",
    "--spatiotemporal-skip",
    "--adaptive-searchrange",
]


@dataclass
class VideoInfo:
    path: str
    name: str
    width: int
    height: int
    fps: float
    duration_s: float
    frames: int
    original_size_bytes: int


@dataclass
class MethodResult:
    output: str
    size_bytes: int
    encode_time_s: float
    ssim: float | None
    psnr: float | None
    psnr_y: float | None
    bitrate_kbps: float | None
    fps: float | None


@dataclass
class BenchmarkRow:
    sample_id: str
    source: str
    width: int
    height: int
    fps: float
    frames: int
    duration_s: float
    original_size_bytes: int
    baseline_output: str
    improved_output: str
    baseline_size_bytes: int
    improved_size_bytes: int
    baseline_bitrate_kbps: float | None
    improved_bitrate_kbps: float | None
    baseline_ssim: float | None
    improved_ssim: float | None
    ssim_delta: float | None
    baseline_psnr: float | None
    improved_psnr: float | None
    psnr_delta: float | None
    compression_ratio_baseline_vs_original: float | None
    compression_ratio_improved_vs_original: float | None
    improved_size_reduction_pct: float | None
    baseline_time_s: float
    improved_time_s: float
    analysis_time_s: float
    avg_scene_qp: float
    status: str
    error: str


def run(cmd: list[str], timeout: int | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, errors="replace", timeout=timeout, check=False)


def resolve_tool(value: str, fallback_name: str) -> str:
    p = Path(value)
    if p.exists():
        return str(p.resolve())
    found = shutil.which(value) or shutil.which(fallback_name)
    if found:
        return found
    raise FileNotFoundError(f"Tool not found: {value}")


def ffprobe_info(ffprobe: str, path: Path, seconds: int) -> VideoInfo | None:
    proc = run([
        ffprobe, "-v", "error",
        "-select_streams", "v:0",
        "-show_entries", "stream=width,height,r_frame_rate,avg_frame_rate,duration",
        "-show_entries", "format=duration",
        "-of", "json",
        str(path),
    ], timeout=60)
    if proc.returncode != 0:
        return None
    data = json.loads(proc.stdout)
    streams = data.get("streams") or []
    if not streams:
        return None
    stream = streams[0]
    width = int(stream.get("width") or 0)
    height = int(stream.get("height") or 0)
    rate_candidates = [stream.get("avg_frame_rate"), stream.get("r_frame_rate"), "25/1"]
    fps = 0.0
    for rate in rate_candidates:
        if not rate:
            continue
        if "/" in rate:
            num, den = rate.split("/", 1)
            fps = float(num) / max(float(den), 0.001)
        else:
            fps = float(rate)
        if 0 < fps <= 120:
            break
    if fps <= 0 or fps > 120:
        fps = 25.0
    duration = float(stream.get("duration") or data.get("format", {}).get("duration") or seconds)
    use_duration = min(duration, float(seconds))
    frames = max(1, int(round(use_duration * fps)))
    return VideoInfo(
        path=str(path),
        name=path.name,
        width=width,
        height=height,
        fps=fps,
        duration_s=use_duration,
        frames=frames,
        original_size_bytes=path.stat().st_size,
    )


def can_decode(ffmpeg: str, path: Path, timeout: int = 30) -> bool:
    proc = run([
        ffmpeg, "-hide_banner", "-loglevel", "error",
        "-t", "5",
        "-i", str(path),
        "-frames:v", "50",
        "-f", "null",
        "-",
    ], timeout=timeout)
    return proc.returncode == 0


def select_samples(dataset: Path, ffmpeg: str, ffprobe: str, count: int, seconds: int) -> list[VideoInfo]:
    videos = sorted(
        [p for p in dataset.rglob("*") if p.suffix.lower() in {".mp4", ".mkv", ".mov", ".avi"}],
        key=lambda p: str(p),
    )
    by_parent: dict[Path, list[Path]] = {}
    for video in videos:
        by_parent.setdefault(video.parent, []).append(video)

    selected: list[VideoInfo] = []
    for parent in sorted(by_parent):
        for candidate in by_parent[parent]:
            info = ffprobe_info(ffprobe, candidate, seconds)
            if info and info.width > 0 and info.height > 0 and info.duration_s >= min(30, seconds) and can_decode(ffmpeg, candidate):
                selected.append(info)
                break
        if len(selected) >= count:
            break

    if len(selected) < count:
        seen = {s.path for s in selected}
        for candidate in videos:
            if str(candidate) in seen:
                continue
            info = ffprobe_info(ffprobe, candidate, seconds)
            if info and info.width > 0 and info.height > 0 and info.duration_s >= min(30, seconds) and can_decode(ffmpeg, candidate):
                selected.append(info)
            if len(selected) >= count:
                break

    return selected[:count]


def parse_x265_csv(path: Path) -> dict[str, float]:
    if not path.exists():
        return {}
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    if len(lines) < 2:
        return {}
    header = [x.strip() for x in lines[0].split(",")]
    values = [x.strip() for x in lines[-1].split(",")]
    data = dict(zip(header, values))

    def f(key: str) -> float | None:
        try:
            value = data.get(key)
            return float(value) if value not in (None, "") else None
        except ValueError:
            return None

    return {
        "ssim": f("SSIM"),
        "psnr": f("Global PSNR"),
        "psnr_y": f("Y PSNR"),
        "bitrate_kbps": f("Bitrate"),
        "fps": f("FPS"),
    }


def ffmpeg_raw_cmd(ffmpeg: str, video: VideoInfo) -> list[str]:
    return [
        ffmpeg, "-hide_banner", "-loglevel", "error",
        "-i", video.path,
        "-t", f"{video.duration_s:.3f}",
        "-frames:v", str(video.frames),
        "-pix_fmt", "yuv420p",
        "-f", "rawvideo",
        "pipe:1",
    ]


def encode_x265(
    ffmpeg: str,
    x265: str,
    video: VideoInfo,
    output: Path,
    *,
    preset: str,
    crf: int | None,
    qpfile: Path | None,
    extra: list[str] | None = None,
    timeout: int = 7200,
) -> MethodResult:
    output.parent.mkdir(parents=True, exist_ok=True)
    csv_path = Path(str(output) + ".csv")
    log_path = Path(str(output) + ".log")
    for p in (output, csv_path, log_path):
        if p.exists():
            p.unlink()

    x265_cmd = [
        x265,
        "--input", "-",
        "--input-res", f"{video.width}x{video.height}",
        "--fps", f"{video.fps:.6f}",
        "--frames", str(video.frames),
        "--preset", preset,
        "--tune", "ssim",
        "--psnr",
        "--ssim",
        "--csv", str(csv_path),
        "-o", str(output),
    ]
    if crf is not None:
        x265_cmd += ["--crf", str(crf)]
    if qpfile is not None:
        x265_cmd += ["--qpfile", str(qpfile)]
    if extra:
        x265_cmd += extra

    start = time.perf_counter()
    ffmpeg_proc = subprocess.Popen(ffmpeg_raw_cmd(ffmpeg, video), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    x265_proc = subprocess.Popen(x265_cmd, stdin=ffmpeg_proc.stdout, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert ffmpeg_proc.stdout is not None
    ffmpeg_proc.stdout.close()
    try:
        _, x265_err = x265_proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        ffmpeg_proc.kill()
        x265_proc.kill()
        ffmpeg_proc.wait()
        x265_proc.wait()
        raise TimeoutError(f"x265 timeout: {video.name}")
    ffmpeg_err = ffmpeg_proc.stderr.read() if ffmpeg_proc.stderr is not None else b""
    ffmpeg_proc.wait(timeout=60)
    elapsed = time.perf_counter() - start

    log_path.write_bytes((ffmpeg_err or b"") + b"\n--- x265 stderr ---\n" + (x265_err or b""))
    if ffmpeg_proc.returncode not in (0, None) or x265_proc.returncode != 0:
        raise RuntimeError(f"encode failed ffmpeg={ffmpeg_proc.returncode} x265={x265_proc.returncode}; see {log_path}")

    metrics = parse_x265_csv(csv_path)
    return MethodResult(
        output=str(output),
        size_bytes=output.stat().st_size if output.exists() else 0,
        encode_time_s=elapsed,
        ssim=metrics.get("ssim"),
        psnr=metrics.get("psnr"),
        psnr_y=metrics.get("psnr_y"),
        bitrate_kbps=metrics.get("bitrate_kbps"),
        fps=metrics.get("fps"),
    )


def scene_analyze(
    ffmpeg: str,
    video: VideoInfo,
    qpfile: Path,
    *,
    base_qp: int,
    qp_scale: float,
    min_qp: int,
    max_qp: int,
) -> tuple[float, float]:
    start = time.perf_counter()
    analyzer = SceneAnalyzer(video.width, video.height)
    proc = subprocess.Popen(ffmpeg_raw_cmd(ffmpeg, video), stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    assert proc.stdout is not None
    y_size = video.width * video.height
    frame_size = int(y_size * 1.5)
    entries: list[str] = []
    qps: list[float] = []
    for frame_idx in range(video.frames):
        data = proc.stdout.read(frame_size)
        if len(data) < frame_size:
            break
        y_plane = np.frombuffer(data[:y_size], dtype=np.uint8).reshape(video.height, video.width)
        qp_map, _ = analyzer.process_frame(y_plane)
        raw_avg_qp = float(qp_map.mean())
        avg_qp = base_qp + (raw_avg_qp - base_qp) * qp_scale
        avg_qp = max(min_qp, min(max_qp, avg_qp))
        qps.append(avg_qp)
        frame_type = analyzer.get_recommended_frame_type()
        entries.append(f"{frame_idx} {frame_type} {int(round(avg_qp))}")
    proc.stdout.close()
    proc.wait(timeout=60)
    qpfile.parent.mkdir(parents=True, exist_ok=True)
    qpfile.write_text("\n".join(entries) + "\n", encoding="utf-8")
    return time.perf_counter() - start, (sum(qps) / len(qps) if qps else 0.0)


def pct_delta(new: float | None, old: float | None) -> float | None:
    if old in (None, 0) or new is None:
        return None
    return (new / old - 1.0) * 100.0


def ratio(original: int, encoded: int) -> float | None:
    if encoded <= 0:
        return None
    return original / encoded


def run_one(args, video: VideoInfo, sample_index: int) -> BenchmarkRow:
    sample_id = f"sample_{sample_index:02d}"
    sample_dir = Path(args.output_dir) / "artifacts" / sample_id
    try:
        baseline = encode_x265(
            args.ffmpeg, args.x265, video, sample_dir / f"{sample_id}_baseline.hevc",
            preset=args.preset, crf=args.crf, qpfile=None, timeout=args.timeout,
        )
        analysis_time = 0.0
        avg_scene_qp = 0.0
        if args.improved_mode == "scene":
            qpfile = sample_dir / f"{sample_id}_improved.qpfile"
            analysis_time, avg_scene_qp = scene_analyze(
                args.ffmpeg,
                video,
                qpfile,
                base_qp=args.crf,
                qp_scale=args.qp_scale,
                min_qp=args.min_qp,
                max_qp=args.max_qp,
            )
            improved = encode_x265(
                args.ffmpeg, args.x265, video, sample_dir / f"{sample_id}_improved.hevc",
                preset=args.preset, crf=None, qpfile=qpfile,
                extra=["--keyint", "300", "--min-keyint", "30", "--no-scenecut"],
                timeout=args.timeout,
            )
        else:
            improved = encode_x265(
                args.ffmpeg, args.x265, video, sample_dir / f"{sample_id}_improved.hevc",
                preset=args.preset, crf=args.crf, qpfile=None,
                extra=OPT_SWITCHES,
                timeout=args.timeout,
            )
        ssim_delta = None if baseline.ssim is None or improved.ssim is None else improved.ssim - baseline.ssim
        psnr_delta = None if baseline.psnr is None or improved.psnr is None else improved.psnr - baseline.psnr
        return BenchmarkRow(
            sample_id=sample_id,
            source=video.path,
            width=video.width,
            height=video.height,
            fps=video.fps,
            frames=video.frames,
            duration_s=video.duration_s,
            original_size_bytes=video.original_size_bytes,
            baseline_output=baseline.output,
            improved_output=improved.output,
            baseline_size_bytes=baseline.size_bytes,
            improved_size_bytes=improved.size_bytes,
            baseline_bitrate_kbps=baseline.bitrate_kbps,
            improved_bitrate_kbps=improved.bitrate_kbps,
            baseline_ssim=baseline.ssim,
            improved_ssim=improved.ssim,
            ssim_delta=ssim_delta,
            baseline_psnr=baseline.psnr,
            improved_psnr=improved.psnr,
            psnr_delta=psnr_delta,
            compression_ratio_baseline_vs_original=ratio(video.original_size_bytes, baseline.size_bytes),
            compression_ratio_improved_vs_original=ratio(video.original_size_bytes, improved.size_bytes),
            improved_size_reduction_pct=-(pct_delta(improved.size_bytes, baseline.size_bytes) or 0.0),
            baseline_time_s=baseline.encode_time_s,
            improved_time_s=improved.encode_time_s,
            analysis_time_s=analysis_time,
            avg_scene_qp=avg_scene_qp,
            status="ok",
            error="",
        )
    except Exception as exc:
        return BenchmarkRow(
            sample_id=sample_id,
            source=video.path,
            width=video.width,
            height=video.height,
            fps=video.fps,
            frames=video.frames,
            duration_s=video.duration_s,
            original_size_bytes=video.original_size_bytes,
            baseline_output="",
            improved_output="",
            baseline_size_bytes=0,
            improved_size_bytes=0,
            baseline_bitrate_kbps=None,
            improved_bitrate_kbps=None,
            baseline_ssim=None,
            improved_ssim=None,
            ssim_delta=None,
            baseline_psnr=None,
            improved_psnr=None,
            psnr_delta=None,
            compression_ratio_baseline_vs_original=None,
            compression_ratio_improved_vs_original=None,
            improved_size_reduction_pct=None,
            baseline_time_s=0.0,
            improved_time_s=0.0,
            analysis_time_s=0.0,
            avg_scene_qp=0.0,
            status="failed",
            error=str(exc),
        )


def mean(values: list[float | None]) -> float | None:
    nums = [v for v in values if v is not None and math.isfinite(v)]
    return sum(nums) / len(nums) if nums else None


def fmt(value: float | None, digits: int = 3) -> str:
    if value is None:
        return "N/A"
    return f"{value:.{digits}f}"


def write_outputs(output_dir: Path, rows: list[BenchmarkRow], env: dict) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    dict_rows = [asdict(r) for r in rows]
    (output_dir / "results.json").write_text(json.dumps(dict_rows, ensure_ascii=False, indent=2), encoding="utf-8")
    (output_dir / "environment.json").write_text(json.dumps(env, ensure_ascii=False, indent=2), encoding="utf-8")

    if dict_rows:
        with (output_dir / "results.csv").open("w", newline="", encoding="utf-8-sig") as f:
            writer = csv.DictWriter(f, fieldnames=list(dict_rows[0].keys()))
            writer.writeheader()
            writer.writerows(dict_rows)

    ok = [r for r in rows if r.status == "ok"]
    summary = {
        "total_samples": len(rows),
        "completed_samples": len(ok),
        "mean_improved_size_reduction_pct": mean([r.improved_size_reduction_pct for r in ok]),
        "mean_ssim_delta": mean([r.ssim_delta for r in ok]),
        "mean_psnr_delta": mean([r.psnr_delta for r in ok]),
        "mean_baseline_bitrate_kbps": mean([r.baseline_bitrate_kbps for r in ok]),
        "mean_improved_bitrate_kbps": mean([r.improved_bitrate_kbps for r in ok]),
        "mean_baseline_compression_ratio": mean([r.compression_ratio_baseline_vs_original for r in ok]),
        "mean_improved_compression_ratio": mean([r.compression_ratio_improved_vs_original for r in ok]),
    }
    (output_dir / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")

    lines = [
        "# 5-Minute Rate-Distortion Benchmark",
        "",
        "This benchmark compares plain H.265/x265 baseline encoding with the scene-aware improved method.",
        "",
        "## Summary",
        "",
        f"- Samples: {len(ok)}/{len(rows)} completed",
        f"- Mean improved size reduction vs baseline: {fmt(summary['mean_improved_size_reduction_pct'], 2)}%",
        f"- Mean SSIM delta: {fmt(summary['mean_ssim_delta'], 6)}",
        f"- Mean PSNR delta: {fmt(summary['mean_psnr_delta'], 3)} dB",
        f"- Mean baseline bitrate: {fmt(summary['mean_baseline_bitrate_kbps'], 1)} kbps",
        f"- Mean improved bitrate: {fmt(summary['mean_improved_bitrate_kbps'], 1)} kbps",
        "",
        "## Per-Sample Results",
        "",
        "| Sample | Resolution | Baseline KB | Improved KB | Size Reduction | Baseline SSIM | Improved SSIM | SSIM Delta | Baseline PSNR | Improved PSNR | PSNR Delta |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for r in rows:
        if r.status != "ok":
            lines.append(f"| {r.sample_id} | failed | | | | | | | | | |")
            continue
        lines.append(
            f"| {r.sample_id} | {r.width}x{r.height} | "
            f"{r.baseline_size_bytes / 1024:.1f} | {r.improved_size_bytes / 1024:.1f} | "
            f"{fmt(r.improved_size_reduction_pct, 2)}% | "
            f"{fmt(r.baseline_ssim, 6)} | {fmt(r.improved_ssim, 6)} | {fmt(r.ssim_delta, 6)} | "
            f"{fmt(r.baseline_psnr, 3)} | {fmt(r.improved_psnr, 3)} | {fmt(r.psnr_delta, 3)} |"
        )
    lines += [
        "",
        "## Method",
        "",
        "- Baseline: x265 H.265, same preset/CRF for all samples.",
        "- Improved: x265 custom optimization switches by default; `--improved-mode scene` uses scene-aware QP-file generation.",
        "- Each source is limited to the first 5 minutes.",
        "- Metrics come from x265 `--ssim --psnr --csv` output.",
        "",
        "## Files",
        "",
        "- `results.csv`: spreadsheet-friendly full results.",
        "- `results.json`: machine-readable full results.",
        "- `summary.json`: aggregate values for README badges/tables.",
        "- `environment.json`: reproducibility environment.",
        "- `artifacts/`: encoded files, x265 CSV files, logs, and qpfiles.",
    ]
    (output_dir / "README.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dataset", default=r"E:\Video_Compression\5.6")
    parser.add_argument("--output-dir", default=str(ROOT / "benchmarks" / "2026-05-20-5min-rate-distortion"))
    parser.add_argument("--ffmpeg", default=r"C:\tmp\ffmpeg\ffmpeg-8.1.1-essentials_build\bin\ffmpeg.exe")
    parser.add_argument("--ffprobe", default=r"C:\tmp\ffmpeg\ffmpeg-8.1.1-essentials_build\bin\ffprobe.exe")
    parser.add_argument("--x265", default=str(ROOT / "x265" / "bin" / "x265.exe"))
    parser.add_argument("--count", type=int, default=10)
    parser.add_argument("--seconds", type=int, default=300)
    parser.add_argument("--workers", type=int, default=2)
    parser.add_argument("--preset", default="medium")
    parser.add_argument("--crf", type=int, default=28)
    parser.add_argument("--improved-mode", choices=["switches", "scene"], default="switches")
    parser.add_argument("--qp-scale", type=float, default=0.22)
    parser.add_argument("--min-qp", type=int, default=24)
    parser.add_argument("--max-qp", type=int, default=34)
    parser.add_argument("--timeout", type=int, default=7200)
    args = parser.parse_args()

    args.ffmpeg = resolve_tool(args.ffmpeg, "ffmpeg")
    args.ffprobe = resolve_tool(args.ffprobe, "ffprobe")
    args.x265 = resolve_tool(args.x265, "x265")

    dataset = Path(args.dataset)
    output_dir = Path(args.output_dir)
    samples = select_samples(dataset, args.ffmpeg, args.ffprobe, args.count, args.seconds)
    if len(samples) < args.count:
        print(f"Only found {len(samples)} valid samples", file=sys.stderr)
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "samples.json").write_text(
        json.dumps([asdict(s) for s in samples], ensure_ascii=False, indent=2),
        encoding="utf-8",
    )

    env = {
        "date": time.strftime("%Y-%m-%d %H:%M:%S"),
        "dataset": str(dataset),
        "ffmpeg": args.ffmpeg,
        "ffprobe": args.ffprobe,
        "x265": args.x265,
        "count": args.count,
        "seconds": args.seconds,
        "workers": args.workers,
        "preset": args.preset,
        "crf": args.crf,
        "improved_mode": args.improved_mode,
        "qp_scale": args.qp_scale,
        "min_qp": args.min_qp,
        "max_qp": args.max_qp,
        "python": sys.version,
    }
    gpu = run(["nvidia-smi", "--query-gpu=name,driver_version,memory.total", "--format=csv,noheader"], timeout=15)
    env["gpu"] = gpu.stdout.strip() if gpu.returncode == 0 else ""

    rows: list[BenchmarkRow] = []
    with ThreadPoolExecutor(max_workers=max(1, args.workers)) as pool:
        futures = {
            pool.submit(run_one, args, sample, idx): sample
            for idx, sample in enumerate(samples)
        }
        for future in as_completed(futures):
            row = future.result()
            rows.append(row)
            print(f"{row.sample_id}: {row.status} {row.improved_size_reduction_pct if row.improved_size_reduction_pct is not None else ''}")

    rows.sort(key=lambda r: r.sample_id)
    write_outputs(output_dir, rows, env)
    print(output_dir)
    return 0 if all(r.status == "ok" for r in rows) else 1


if __name__ == "__main__":
    raise SystemExit(main())
