#!/usr/bin/env python3
"""Mixed concurrency test: NVENC 8 + QSV 8 + x265 sweep, all simultaneous.

Requirement: encode time < video duration (300s) for real-time feasibility.
"""
from __future__ import annotations

import argparse
import json
import logging
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

logger = logging.getLogger("mixed_test")

FFMPEG_DEFAULT = r"C:\tmp\ffmpeg\ffmpeg-8.1.1-essentials_build\bin\ffmpeg.exe"

@dataclass
class Job:
    encoder: str
    source: Path
    output: Path
    log: Path
    index: int

@dataclass
class Result:
    encoder: str
    index: int
    encode_time_s: float
    returncode: int
    file_size: int


def run_mixed_test(
    sources: list[Path],
    ffmpeg: str,
    output_dir: Path,
    nvenc_n: int = 8,
    qsv_n: int = 8,
    x265_n: int = 4,
    duration: int = 300,
) -> dict:
    """Run NVENC + QSV + x265 simultaneously."""
    out = output_dir / f"mixed_nv{nvenc_n}_qsv{qsv_n}_x265{x265_n}"
    out.mkdir(parents=True, exist_ok=True)

    jobs: list[Job] = []
    idx = 0

    # NVENC jobs
    for i in range(nvenc_n):
        jobs.append(Job("nvenc", sources[idx], out / f"nv_{i:03d}.hevc",
                        out / f"nv_{i:03d}.log", idx))
        idx += 1

    # QSV jobs
    for i in range(qsv_n):
        jobs.append(Job("qsv", sources[idx], out / f"qsv_{i:03d}.hevc",
                        out / f"qsv_{i:03d}.log", idx))
        idx += 1

    # x265 jobs
    for i in range(x265_n):
        jobs.append(Job("x265", sources[idx], out / f"x265_{i:03d}.hevc",
                        out / f"x265_{i:03d}.log", idx))
        idx += 1

    logger.info("Launching %d total jobs (NVENC=%d QSV=%d x265=%d)...",
                len(jobs), nvenc_n, qsv_n, x265_n)

    processes: dict[subprocess.Popen, Job] = {}
    for job in jobs:
        job.output.parent.mkdir(parents=True, exist_ok=True)
        log_f = open(job.log, "wb")

        if job.encoder == "nvenc":
            cmd = [
                ffmpeg, "-y", "-hide_banner", "-i", str(job.source),
                "-t", str(duration), "-an", "-c:v", "hevc_nvenc",
                "-preset", "p1", "-rc", "vbr", "-b:v", "2500k",
                "-f", "hevc", str(job.output),
            ]
            proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=log_f)
        elif job.encoder == "qsv":
            cmd = [
                ffmpeg, "-y", "-hide_banner", "-i", str(job.source),
                "-t", str(duration), "-an", "-c:v", "hevc_qsv",
                "-preset", "veryfast", "-b:v", "2500k",
                "-f", "hevc", str(job.output),
            ]
            proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=log_f)
        else:  # x265 via FFmpeg libx265 (no pipe needed)
            cmd = [
                ffmpeg, "-y", "-hide_banner", "-i", str(job.source),
                "-t", str(duration), "-an", "-c:v", "libx265",
                "-preset", "ultrafast", "-crf", "28",
                "-x265-params", "tune=ssim:fps=25:no-progress=1",
                "-f", "hevc", str(job.output),
            ]
            proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=log_f)

        processes[proc] = job
        log_f.close()

    # Wait for all with per-job timing
    results: list[Result] = []
    started = time.perf_counter()

    for proc, job in processes.items():
        t0 = time.perf_counter()
        try:
            proc.wait(timeout=max(duration * 3, 900))
        except subprocess.TimeoutExpired:
            logger.warning("%s job %d hung, killing", job.encoder, job.index)
            proc.kill()
            proc.wait()
        t1 = time.perf_counter()

        sz = job.output.stat().st_size if job.output.exists() else 0
        results.append(Result(
            encoder=job.encoder, index=job.index,
            encode_time_s=round(t1 - t0, 1),
            returncode=proc.returncode,
            file_size=sz,
        ))

    total_elapsed = time.perf_counter() - started

    # Summarize
    summary = {
        "nvenc_n": nvenc_n, "qsv_n": qsv_n, "x265_n": x265_n,
        "total_elapsed_s": round(total_elapsed, 1),
        "nvenc": _summarize_encoder(results, "nvenc"),
        "qsv": _summarize_encoder(results, "qsv"),
        "x265": _summarize_encoder(results, "x265"),
    }

    return summary


def _summarize_encoder(results: list[Result], encoder: str) -> dict:
    subset = [r for r in results if r.encoder == encoder]
    if not subset:
        return {}
    completed = sum(1 for r in subset if r.returncode == 0 and r.file_size > 1000)
    times = [r.encode_time_s for r in subset if r.returncode == 0]
    return {
        "total": len(subset),
        "completed": completed,
        "failed": len(subset) - completed,
        "max_time_s": max(times) if times else 0,
        "avg_time_s": round(sum(times) / len(times), 1) if times else 0,
        "all_under_300s": all(t < 300 for t in times) if times else False,
    }


def main():
    parser = argparse.ArgumentParser(description="Mixed NVENC+QSV+x265 concurrent test")
    parser.add_argument("--sources-dir", default=r"D:\video_bench_results\sources")
    parser.add_argument("--output-dir", default=r"D:\video_bench_results\output")
    parser.add_argument("--ffmpeg", default=FFMPEG_DEFAULT)
    parser.add_argument("--nvenc", type=int, default=8)
    parser.add_argument("--qsv", type=int, default=8)
    parser.add_argument("--x265-max", type=int, default=16)
    parser.add_argument("--duration", type=int, default=300)
    parser.add_argument("--verbose", "-v", action="store_true")
    args = parser.parse_args()

    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s [%(levelname)s] %(message)s", datefmt="%H:%M:%S")

    ffmpeg = args.ffmpeg
    if not Path(ffmpeg).exists():
        ffmpeg = shutil.which("ffmpeg") or ffmpeg

    sources = sorted(Path(args.sources_dir).glob("*.mp4"))
    if len(sources) < args.nvenc + args.qsv + 2:
        logger.error("Need more sources. Have %d, need %d", len(sources), args.nvenc + args.qsv + 2)
        return 1

    logger.info("Using %d validated sources", len(sources))

    all_results = []
    x265_levels = [2, 4, 6, 8, 10, 12, 14, 16]

    for x265_n in x265_levels:
        if x265_n > args.x265_max:
            break
        logger.info("=== Test: NVENC=%d + QSV=%d + x265=%d ===", args.nvenc, args.qsv, x265_n)
        summary = run_mixed_test(sources, ffmpeg, Path(args.output_dir) / "mixed",
                                 args.nvenc, args.qsv, x265_n, args.duration)
        all_results.append(summary)

        # Print per-encoder status
        for enc in ("nvenc", "qsv", "x265"):
            s = summary[enc]
            if s:
                status = "✅ ALL <300s" if s.get("all_under_300s") else "❌ EXCEEDS 300s"
                logger.info("  %s: %d/%d done, max=%.1fs, avg=%.1fs — %s",
                            enc, s["completed"], s["total"],
                            s["max_time_s"], s["avg_time_s"], status)

    # Final report
    print("\n" + "=" * 70)
    print("  MIXED CONCURRENCY TEST — Real-time Feasibility (<300s)")
    print("=" * 70)
    print(f"  {'x265':>5s}  {'NVENC':>8s}  {'QSV':>8s}  {'x265':>8s}  {'Total':>8s}  {'Verdict'}")
    print("  " + "-" * 62)

    for r in all_results:
        nv = r["nvenc"]
        qs = r["qsv"]
        x265 = r["x265"]
        nv_ok = nv.get("all_under_300s", False) if nv else False
        qs_ok = qs.get("all_under_300s", False) if qs else False
        x265_ok = x265.get("all_under_300s", False) if x265 else False
        total_ok = nv_ok and qs_ok and x265_ok

        verdict = "✅ REALTIME" if total_ok else "❌ EXCEEDS"
        print(f"  {r['x265_n']:>5d}  {nv.get('max_time_s',0):>6.1f}s  "
              f"{qs.get('max_time_s',0):>6.1f}s  {x265.get('max_time_s',0):>6.1f}s  "
              f"{r['total_elapsed_s']:>6.1f}s  {verdict}")

    # Save JSON
    json_path = Path(args.output_dir) / "mixed_summary.json"
    with open(json_path, "w") as f:
        json.dump(all_results, f, indent=2, default=str)
    print(f"\n  Full results: {json_path}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
