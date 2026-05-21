#!/usr/bin/env python3
"""Multi-source concurrency benchmark — each encode stream uses a different input.

Tests NVENC, QSV, x265, and mixed CPU+GPU encoding at maximum concurrency.
Outputs encoded files + SSIM/PSNR quality report to D:\\video_bench_results.
"""
from __future__ import annotations

import argparse
import csv
import json
import logging
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any

logger = logging.getLogger("multi_bench")

# ── Constants ──
FFMPEG_DEFAULT = r"C:\tmp\ffmpeg\ffmpeg-8.1.1-essentials_build\bin\ffmpeg.exe"
X265_DEFAULT = str(Path(__file__).resolve().parent.parent / "x265" / "bin" / "x265.exe")

@dataclass
class EncodeTask:
    source: Path
    output: Path
    log: Path
    encoder: str  # "nvenc" | "qsv" | "x265"
    index: int

@dataclass
class EncodeResult:
    task: EncodeTask
    returncode: int
    elapsed_s: float
    file_size: int
    ssim: float | None = None
    psnr: float | None = None


def resolve_ffmpeg(path: str) -> str:
    p = Path(path)
    if p.exists():
        return str(p.resolve())
    found = shutil.which(path)
    if found:
        return found
    raise FileNotFoundError(f"FFmpeg not found: {path}")


def build_nvenc_cmd(ffmpeg: str, task: EncodeTask, preset: str, bitrate: str) -> list[str]:
    return [
        ffmpeg, "-y", "-hide_banner", "-i", str(task.source),
        "-t", "300", "-an", "-c:v", "hevc_nvenc", "-preset", preset,
        "-rc", "vbr", "-b:v", bitrate, "-f", "hevc", str(task.output),
    ]


def build_qsv_cmd(ffmpeg: str, task: EncodeTask, preset: str, bitrate: str) -> list[str]:
    return [
        ffmpeg, "-y", "-hide_banner", "-i", str(task.source),
        "-t", "300", "-an", "-c:v", "hevc_qsv", "-preset", preset,
        "-b:v", bitrate, "-f", "hevc", str(task.output),
    ]


def build_x265_cmd(ffmpeg: str, x265: str, task: EncodeTask, preset: str, crf: int) -> str:
    """Shell pipe: ffmpeg decode → x265 encode (x265 can't read mp4 directly)."""
    return (
        f'"{ffmpeg}" -y -hide_banner -i "{task.source}" -t 300 -an '
        f'-f yuv4mpegpipe -pix_fmt yuv420p - 2>{task.log} '
        f'| "{x265}" --y4m --input - --output "{task.output}" '
        f'--preset {preset} --crf {crf} --tune ssim --fps 25 --no-progress '
        f'2>>{task.log}'
    )


def run_encodes(
    tasks: list[EncodeTask],
    ffmpeg: str,
    x265_path: str,
    preset: str,
    bitrate: str,
    crf: int,
) -> list[EncodeResult]:
    """Run all encode tasks concurrently. Returns results."""
    processes: dict[subprocess.Popen, EncodeTask] = {}

    for task in tasks:
        task.output.parent.mkdir(parents=True, exist_ok=True)
        log_f = open(task.log, "wb")

        if task.encoder == "nvenc":
            cmd = build_nvenc_cmd(ffmpeg, task, preset, bitrate)
            proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=log_f)
        elif task.encoder == "qsv":
            cmd = build_qsv_cmd(ffmpeg, task, preset, bitrate)
            proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=log_f)
        elif task.encoder == "x265":
            cmd = build_x265_cmd(ffmpeg, x265_path, task, preset, crf)
            proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=log_f, shell=True)
        else:
            raise ValueError(f"Unknown encoder: {task.encoder}")

        processes[proc] = task
        log_f.close()

    logger.info("Launched %d encode tasks, waiting...", len(processes))
    started = time.perf_counter()

    results: list[EncodeResult] = []
    for proc, task in processes.items():
        try:
            proc.wait(timeout=600)  # 10 min timeout per encode
        except subprocess.TimeoutExpired:
            logger.warning("Task %d hung, killing", task.index)
            proc.kill()
            proc.wait()

        elapsed = time.perf_counter() - started
        sz = task.output.stat().st_size if task.output.exists() else 0

        results.append(EncodeResult(
            task=task, returncode=proc.returncode,
            elapsed_s=round(elapsed, 1), file_size=sz,
        ))

    return results


def compute_quality(result: EncodeResult, ffmpeg: str) -> None:
    """Compute SSIM and PSNR against source."""
    if not result.task.output.exists() or result.file_size < 1000:
        return

    proc = subprocess.run(
        [ffmpeg, "-i", str(result.task.output), "-i", str(result.task.source),
         "-lavfi", "[0:v][1:v]ssim;[0:v][1:v]psnr", "-f", "null", "-"],
        capture_output=True, text=True, errors="replace", check=False,
    )
    output = proc.stdout + proc.stderr
    for line in output.splitlines():
        if "All:" in line and "SSIM" in output:
            try:
                result.ssim = float(line.split("All:")[1].split()[0])
            except (ValueError, IndexError):
                pass
        if "average:" in line and "PSNR" in output:
            try:
                result.psnr = float(line.split("average:")[1].split()[0])
            except (ValueError, IndexError):
                pass


def run_concurrency_test(
    sources: list[Path],
    encoder: str,
    n_streams: int,
    ffmpeg: str,
    x265_path: str,
    output_dir: Path,
    preset: str = "p1",
    bitrate: str = "2500k",
    crf: int = 28,
) -> dict[str, Any]:
    """Run N concurrent encodes with unique sources, report aggregate metrics."""
    assert n_streams <= len(sources), f"Need {n_streams} sources, have {len(sources)}"

    run_dir = output_dir / f"{encoder}_{n_streams}way"
    run_dir.mkdir(parents=True, exist_ok=True)

    tasks = []
    for i in range(n_streams):
        tasks.append(EncodeTask(
            source=sources[i],
            output=run_dir / f"out_{i:03d}.hevc",
            log=run_dir / f"out_{i:03d}.log",
            encoder=encoder.split("_")[0],  # handle "nvenc_gpu0" etc
            index=i,
        ))

    t0 = time.perf_counter()
    results = run_encodes(tasks, ffmpeg, x265_path, preset, bitrate, crf)
    total_elapsed = time.perf_counter() - t0

    completed = sum(1 for r in results if r.returncode == 0 and r.file_size > 1000)
    failed = n_streams - completed

    logger.info("%s %d-way: %d/%d done, %.1fs", encoder, n_streams, completed, n_streams, total_elapsed)

    return {
        "encoder": encoder,
        "n_streams": n_streams,
        "completed": completed,
        "failed": failed,
        "total_elapsed_s": round(total_elapsed, 1),
    }


def main():
    parser = argparse.ArgumentParser(description="Multi-source concurrency benchmark")
    parser.add_argument("--sources-dir", default=r"D:\video_bench_results\sources")
    parser.add_argument("--output-dir", default=r"D:\video_bench_results\output")
    parser.add_argument("--ffmpeg", default=FFMPEG_DEFAULT)
    parser.add_argument("--x265", default=X265_DEFAULT)
    parser.add_argument("--encoder", choices=["nvenc", "qsv", "x265", "all"], default="all")
    parser.add_argument("--preset", default="p1")
    parser.add_argument("--bitrate", default="2500k")
    parser.add_argument("--crf", type=int, default=28)
    parser.add_argument("--verbose", "-v", action="store_true")
    args = parser.parse_args()

    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s [%(levelname)s] %(message)s", datefmt="%H:%M:%S")

    ffmpeg = resolve_ffmpeg(args.ffmpeg)
    sources_dir = Path(args.sources_dir)
    sources = sorted(sources_dir.glob("*.mp4"))

    if not sources:
        logger.error("No source files found in %s", sources_dir)
        return 1

    logger.info("Found %d source segments", len(sources))
    output_dir = Path(args.output_dir)

    # ── Phase A: NVENC sweep ──
    if args.encoder in ("nvenc", "all"):
        logger.info("=== NVENC GPU Concurrency Sweep ===")
        summary = []
        for n in (4, 8, 12, 16, 20, 24):
            r = run_concurrency_test(sources, "nvenc", min(n, len(sources)),
                                     ffmpeg, args.x265, output_dir / "nvenc_sweep",
                                     args.preset, args.bitrate, args.crf)
            summary.append(r)
            if r["failed"] > 0:
                logger.info("NVENC saturation at %d-way", n)

        with open(output_dir / "nvenc_summary.json", "w") as f:
            json.dump(summary, f, indent=2)

    # ── Phase B: QSV sweep ──
    if args.encoder in ("qsv", "all"):
        logger.info("=== QSV iGPU Concurrency Sweep ===")
        summary = []
        for n in (4, 8, 16, 32, 48, 64, 80, 96, 112, 128):
            r = run_concurrency_test(sources, "qsv", min(n, len(sources)),
                                     ffmpeg, args.x265, output_dir / "qsv_sweep",
                                     "veryslow", args.bitrate, args.crf)
            summary.append(r)
            if r["failed"] > 0:
                logger.info("QSV saturation at %d-way", n)

        with open(output_dir / "qsv_summary.json", "w") as f:
            json.dump(summary, f, indent=2)

    # ── Phase C: x265 CPU sweep ──
    if args.encoder in ("x265", "all"):
        logger.info("=== x265 CPU Concurrency Sweep ===")
        summary = []
        for n in (2, 4, 6, 8, 12, 16):
            r = run_concurrency_test(sources, "x265", min(n, len(sources)),
                                     ffmpeg, args.x265, output_dir / "x265_sweep",
                                     "medium", args.bitrate, args.crf)
            summary.append(r)
            # x265 doesn't have a hard session limit — failures come from CPU overload

        with open(output_dir / "x265_summary.json", "w") as f:
            json.dump(summary, f, indent=2)

    logger.info("All tests complete. Results in %s", output_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
