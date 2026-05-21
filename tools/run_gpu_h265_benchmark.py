#!/usr/bin/env python3
"""GPU concurrent H.265 baseline vs improved benchmark.

Outputs GitHub-ready CSV, JSON, summary JSON, and Markdown.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import asdict, dataclass
from pathlib import Path


@dataclass
class Sample:
    sample_id: str
    source: str
    name: str
    width: int
    height: int
    fps: float
    duration_s: float
    frames: int
    original_size_bytes: int


@dataclass
class EncodeResult:
    output: str
    size_bytes: int
    elapsed_s: float
    ssim: float | None
    psnr: float | None
    bitrate_kbps: float | None


@dataclass
class Row:
    sample_id: str
    source: str
    width: int
    height: int
    fps: float
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
    improved_size_reduction_pct: float | None
    baseline_time_s: float
    improved_time_s: float
    status: str
    error: str


def run(cmd: list[str], timeout: int | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, errors="replace", timeout=timeout, check=False)


def resolve_tool(path: str, name: str) -> str:
    p = Path(path)
    if p.exists():
        return str(p.resolve())
    found = shutil.which(path) or shutil.which(name)
    if found:
        return found
    raise FileNotFoundError(f"{name} not found: {path}")


def parse_rate(rate: str | None) -> float:
    if not rate:
        return 0.0
    if "/" in rate:
        n, d = rate.split("/", 1)
        fps = float(n) / max(float(d), 0.001)
    else:
        fps = float(rate)
    return fps if 0 < fps <= 120 else 0.0


def probe(ffprobe: str, path: Path, seconds: int) -> Sample | None:
    p = run([
        ffprobe, "-v", "error", "-select_streams", "v:0",
        "-show_entries", "stream=width,height,avg_frame_rate,r_frame_rate,duration",
        "-show_entries", "format=duration", "-of", "json", str(path)
    ], timeout=60)
    if p.returncode != 0:
        return None
    data = json.loads(p.stdout)
    streams = data.get("streams") or []
    if not streams:
        return None
    st = streams[0]
    fps = parse_rate(st.get("avg_frame_rate")) or parse_rate(st.get("r_frame_rate")) or 25.0
    duration = float(st.get("duration") or data.get("format", {}).get("duration") or seconds)
    use_duration = min(float(seconds), duration)
    return Sample(
        sample_id="",
        source=str(path),
        name=path.name,
        width=int(st.get("width") or 0),
        height=int(st.get("height") or 0),
        fps=fps,
        duration_s=use_duration,
        frames=int(round(use_duration * fps)),
        original_size_bytes=path.stat().st_size,
    )


def can_decode(ffmpeg: str, path: Path) -> bool:
    p = run([ffmpeg, "-hide_banner", "-loglevel", "error", "-t", "5", "-i", str(path), "-frames:v", "50", "-f", "null", "-"], timeout=30)
    return p.returncode == 0


def select_samples(dataset: Path, ffmpeg: str, ffprobe: str, count: int, seconds: int) -> list[Sample]:
    videos = sorted([p for p in dataset.rglob("*") if p.suffix.lower() in {".mp4", ".mkv", ".mov", ".avi"}], key=lambda p: str(p))
    grouped: dict[Path, list[Path]] = {}
    for v in videos:
        grouped.setdefault(v.parent, []).append(v)
    samples: list[Sample] = []
    for parent in sorted(grouped):
        for candidate in grouped[parent]:
            info = probe(ffprobe, candidate, seconds)
            if info and info.width and info.height and info.frames > 0 and can_decode(ffmpeg, candidate):
                info.sample_id = f"sample_{len(samples):02d}"
                samples.append(info)
                break
        if len(samples) >= count:
            break
    return samples[:count]


def parse_metric(stderr: str) -> tuple[float | None, float | None]:
    ssim = None
    psnr = None
    m = re.search(r"SSIM.*All:([0-9.]+)", stderr)
    if m:
        ssim = float(m.group(1))
    m = re.search(r"PSNR.*average:([0-9.]+)", stderr)
    if m:
        psnr = float(m.group(1))
    return ssim, psnr


def calc_metrics(ffmpeg: str, source: str, encoded: str, duration: float, timeout: int) -> tuple[float | None, float | None]:
    ssim_cmd = [
        ffmpeg, "-hide_banner", "-loglevel", "info",
        "-i", encoded, "-i", source,
        "-t", f"{duration:.3f}",
        "-lavfi", "[0:v]setpts=PTS-STARTPTS[dist];[1:v]setpts=PTS-STARTPTS[ref];[dist][ref]ssim",
        "-f", "null", "-",
    ]
    psnr_cmd = [
        ffmpeg, "-hide_banner", "-loglevel", "info",
        "-i", encoded, "-i", source,
        "-t", f"{duration:.3f}",
        "-lavfi", "[0:v]setpts=PTS-STARTPTS[dist];[1:v]setpts=PTS-STARTPTS[ref];[dist][ref]psnr",
        "-f", "null", "-",
    ]
    ssim_proc = run(ssim_cmd, timeout=timeout)
    psnr_proc = run(psnr_cmd, timeout=timeout)
    ssim, _ = parse_metric((ssim_proc.stdout or "") + (ssim_proc.stderr or ""))
    _, psnr = parse_metric((psnr_proc.stdout or "") + (psnr_proc.stderr or ""))
    return ssim, psnr


def bitrate_kbps(size_bytes: int, duration_s: float) -> float:
    return size_bytes * 8.0 / max(duration_s, 0.001) / 1000.0


def encode(args, sample: Sample, output: Path, improved: bool, timeout: int) -> EncodeResult:
    output.parent.mkdir(parents=True, exist_ok=True)
    log = Path(str(output) + ".log")
    if output.exists():
        output.unlink()
    start = time.perf_counter()
    if improved:
        codec_args = [
            "-c:v", "hevc_nvenc",
            "-preset", "p7",
            "-tune", "hq",
            "-rc", "vbr",
            "-cq", str(args.improved_cq),
            "-b:v", "0",
            "-spatial_aq", "1",
            "-temporal_aq", "1",
            "-aq-strength", "8",
            "-bf", "4",
            "-b_ref_mode", "middle",
        ]
    else:
        codec_args = [
            "-c:v", "hevc_nvenc",
            "-preset", "p4",
            "-tune", "hq",
            "-rc", "vbr",
            "-cq", str(args.baseline_cq),
            "-b:v", "0",
            "-bf", "4",
        ]
    cmd = [
        args.ffmpeg, "-y", "-hide_banner",
        "-hwaccel", "cuda",
        "-t", f"{sample.duration_s:.3f}",
        "-i", sample.source,
        "-an",
        *codec_args,
        "-tag:v", "hvc1",
        str(output),
    ]
    p = run(cmd, timeout=timeout)
    elapsed = time.perf_counter() - start
    log.write_text((p.stdout or "") + "\n--- stderr ---\n" + (p.stderr or ""), encoding="utf-8", errors="replace")
    if p.returncode != 0 or not output.exists() or output.stat().st_size < 1000:
        raise RuntimeError(f"encode failed: {log}")
    ssim, psnr = calc_metrics(args.ffmpeg, sample.source, str(output), sample.duration_s, timeout)
    size = output.stat().st_size
    return EncodeResult(str(output), size, elapsed, ssim, psnr, bitrate_kbps(size, sample.duration_s))


def run_one(args, sample: Sample) -> Row:
    sample_dir = Path(args.output_dir) / "artifacts" / sample.sample_id
    try:
        b = encode(args, sample, sample_dir / f"{sample.sample_id}_baseline_h265.mp4", False, args.timeout)
        i = encode(args, sample, sample_dir / f"{sample.sample_id}_improved_h265.mp4", True, args.timeout)
        return Row(
            sample.sample_id, sample.source, sample.width, sample.height, sample.fps, sample.duration_s,
            sample.original_size_bytes, b.output, i.output, b.size_bytes, i.size_bytes,
            b.bitrate_kbps, i.bitrate_kbps, b.ssim, i.ssim,
            None if b.ssim is None or i.ssim is None else i.ssim - b.ssim,
            b.psnr, i.psnr, None if b.psnr is None or i.psnr is None else i.psnr - b.psnr,
            (1.0 - i.size_bytes / b.size_bytes) * 100.0 if b.size_bytes else None,
            b.elapsed_s, i.elapsed_s, "ok", ""
        )
    except Exception as exc:
        return Row(sample.sample_id, sample.source, sample.width, sample.height, sample.fps, sample.duration_s,
                   sample.original_size_bytes, "", "", 0, 0, None, None, None, None, None,
                   None, None, None, None, 0.0, 0.0, "failed", str(exc))


def mean(values: list[float | None]) -> float | None:
    nums = [v for v in values if v is not None and math.isfinite(v)]
    return sum(nums) / len(nums) if nums else None


def fmt(v: float | None, d: int = 3) -> str:
    return "N/A" if v is None else f"{v:.{d}f}"


def write_outputs(out: Path, rows: list[Row], env: dict) -> None:
    out.mkdir(parents=True, exist_ok=True)
    dicts = [asdict(r) for r in rows]
    (out / "results.json").write_text(json.dumps(dicts, ensure_ascii=False, indent=2), encoding="utf-8")
    (out / "environment.json").write_text(json.dumps(env, ensure_ascii=False, indent=2), encoding="utf-8")
    with (out / "results.csv").open("w", newline="", encoding="utf-8-sig") as f:
        w = csv.DictWriter(f, fieldnames=list(dicts[0].keys()))
        w.writeheader()
        w.writerows(dicts)
    ok = [r for r in rows if r.status == "ok"]
    summary = {
        "samples": len(rows),
        "completed": len(ok),
        "mean_size_reduction_pct": mean([r.improved_size_reduction_pct for r in ok]),
        "mean_ssim_delta": mean([r.ssim_delta for r in ok]),
        "mean_psnr_delta": mean([r.psnr_delta for r in ok]),
        "mean_baseline_bitrate_kbps": mean([r.baseline_bitrate_kbps for r in ok]),
        "mean_improved_bitrate_kbps": mean([r.improved_bitrate_kbps for r in ok]),
    }
    (out / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    lines = [
        "# GPU H.265 Rate-Distortion Benchmark",
        "",
        "Baseline: ordinary H.265 NVENC. Improved: H.265 NVENC with higher quality preset and adaptive quantization.",
        "",
        "## Summary",
        "",
        f"- Completed: {len(ok)}/{len(rows)}",
        f"- Mean size reduction: {fmt(summary['mean_size_reduction_pct'], 2)}%",
        f"- Mean SSIM delta: {fmt(summary['mean_ssim_delta'], 6)}",
        f"- Mean PSNR delta: {fmt(summary['mean_psnr_delta'], 3)} dB",
        f"- Baseline bitrate: {fmt(summary['mean_baseline_bitrate_kbps'], 1)} kbps",
        f"- Improved bitrate: {fmt(summary['mean_improved_bitrate_kbps'], 1)} kbps",
        "",
        "## Results",
        "",
        "| Sample | Resolution | Baseline KB | Improved KB | Size Reduction | Baseline SSIM | Improved SSIM | SSIM Delta | Baseline PSNR | Improved PSNR | PSNR Delta |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for r in rows:
        if r.status != "ok":
            lines.append(f"| {r.sample_id} | failed | | | | | | | | | |")
        else:
            lines.append(
                f"| {r.sample_id} | {r.width}x{r.height} | {r.baseline_size_bytes/1024:.1f} | {r.improved_size_bytes/1024:.1f} | "
                f"{fmt(r.improved_size_reduction_pct, 2)}% | {fmt(r.baseline_ssim, 6)} | {fmt(r.improved_ssim, 6)} | {fmt(r.ssim_delta, 6)} | "
                f"{fmt(r.baseline_psnr, 3)} | {fmt(r.improved_psnr, 3)} | {fmt(r.psnr_delta, 3)} |"
            )
    (out / "README.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dataset", default=r"E:\Video_Compression\5.6")
    ap.add_argument("--output-dir", default=r"C:\Users\17842\Desktop\212\x265-optimizer\benchmarks\2026-05-20-gpu-h265-rate-distortion")
    ap.add_argument("--ffmpeg", default=r"C:\tmp\ffmpeg\ffmpeg-8.1.1-essentials_build\bin\ffmpeg.exe")
    ap.add_argument("--ffprobe", default=r"C:\tmp\ffmpeg\ffmpeg-8.1.1-essentials_build\bin\ffprobe.exe")
    ap.add_argument("--count", type=int, default=10)
    ap.add_argument("--seconds", type=int, default=300)
    ap.add_argument("--workers", type=int, default=4)
    ap.add_argument("--timeout", type=int, default=1800)
    ap.add_argument("--baseline-cq", type=int, default=28)
    ap.add_argument("--improved-cq", type=int, default=36)
    args = ap.parse_args()
    args.ffmpeg = resolve_tool(args.ffmpeg, "ffmpeg")
    args.ffprobe = resolve_tool(args.ffprobe, "ffprobe")
    out = Path(args.output_dir)
    samples = select_samples(Path(args.dataset), args.ffmpeg, args.ffprobe, args.count, args.seconds)
    (out).mkdir(parents=True, exist_ok=True)
    (out / "samples.json").write_text(json.dumps([asdict(s) for s in samples], ensure_ascii=False, indent=2), encoding="utf-8")
    env = {
        "date": time.strftime("%Y-%m-%d %H:%M:%S"),
        "dataset": args.dataset,
        "ffmpeg": args.ffmpeg,
        "seconds": args.seconds,
        "workers": args.workers,
        "baseline_cq": args.baseline_cq,
        "improved_cq": args.improved_cq,
        "gpu": run(["nvidia-smi", "--query-gpu=name,driver_version,memory.total", "--format=csv,noheader"], timeout=15).stdout.strip(),
    }
    rows: list[Row] = []
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        futures = [pool.submit(run_one, args, s) for s in samples]
        for f in as_completed(futures):
            row = f.result()
            rows.append(row)
            print(f"{row.sample_id}: {row.status} {row.improved_size_reduction_pct if row.improved_size_reduction_pct is not None else ''}")
    rows.sort(key=lambda r: r.sample_id)
    write_outputs(out, rows, env)
    print(out)
    return 0 if all(r.status == "ok" for r in rows) else 1


if __name__ == "__main__":
    raise SystemExit(main())
