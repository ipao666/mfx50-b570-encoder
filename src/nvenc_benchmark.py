#!/usr/bin/env python3
"""NVENC concurrency benchmark — finds max stable encode sessions with optimal power efficiency.

Features:
- Sweep mode: auto-discovers saturation point and optimal concurrency
- pynvml-based GPU monitoring (nvidia-smi fallback) including power draw
- YAML/JSON config file support
- Skips already-prepared sample (--force-regenerate to override)
- Separate presets for sample normalization vs benchmark encodes
"""

from __future__ import annotations

import argparse
import atexit
import csv
import json
import logging
import shutil
import signal
import subprocess
import sys
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any

# ── Module-level constants ─────────────────────────────────
DEFAULT_WIDTH = 1920
DEFAULT_HEIGHT = 1080
DEFAULT_FPS = 30
DEFAULT_DURATION = 10
DEFAULT_BENCH_PRESET = "p1"
DEFAULT_SAMPLE_PRESET = "p4"  # higher quality for faithful source sample
DEFAULT_RATE_CONTROL = "vbr"
DEFAULT_BITRATE = "2500k"

SUMMARY_FIELDS = [
    "concurrency", "started", "completed", "failed",
    "total_elapsed_s", "aggregate_fps", "realtime_factor",
    "avg_gpu_util", "avg_enc_util", "avg_power_w",
    "fps_per_watt",
]

GPU_LOG_HEADER = [
    "timestamp", "concurrency",
    "gpu_util", "enc_util", "dec_util",
    "mem_used_mb", "mem_total_mb", "power_w",
]

FAILED_PATTERNS = (
    "Conversion failed",
    "No capable devices found",
    "OpenEncodeSessionEx failed",
)

logger = logging.getLogger("nvenc_bench")

# ── pynvml loader ──────────────────────────────────────────
_pynvml = None

def _get_pynvml():
    global _pynvml
    if _pynvml is not None:
        return _pynvml
    try:
        import pynvml
        pynvml.nvmlInit()
        _pynvml = pynvml
        logger.info("GPU monitoring: pynvml (direct API)")
    except Exception:
        _pynvml = False
        logger.info("GPU monitoring: nvidia-smi fallback")
    return _pynvml


# ── Data types ─────────────────────────────────────────────
@dataclass
class GpuStats:
    gpu_util: int = 0
    enc_util: int = 0
    dec_util: int = 0
    mem_used_mb: int = 0
    mem_total_mb: int = 0
    power_w: float | None = None


@dataclass
class EncodeJob:
    process: subprocess.Popen[bytes]
    log_path: Path
    index: int


# ── Tool resolution ────────────────────────────────────────
def resolve_tool(tool: str) -> str:
    path = Path(tool)
    if path.exists():
        return str(path.resolve())
    found = shutil.which(tool)
    if found:
        return found
    raise FileNotFoundError(f"Tool not found: {tool}")


# ── GPU monitoring ─────────────────────────────────────────
def read_gpu_stats(gpu_index: int = 0) -> GpuStats | None:
    """Read GPU stats via pynvml (preferred) or nvidia-smi fallback."""
    pynvml = _get_pynvml()
    if pynvml:
        return _read_gpu_stats_pynvml(pynvml, gpu_index)
    return _read_gpu_stats_nvidia_smi(gpu_index)


def _read_gpu_stats_pynvml(pynvml, gpu_index: int) -> GpuStats | None:
    try:
        handle = pynvml.nvmlDeviceGetHandleByIndex(gpu_index)
        util = pynvml.nvmlDeviceGetUtilizationRates(handle)
        mem = pynvml.nvmlDeviceGetMemoryInfo(handle)
        power = None
        try:
            power_mw = pynvml.nvmlDeviceGetPowerUsage(handle)
            power = round(power_mw / 1000.0, 1)
        except pynvml.NVMLError:
            pass
        return GpuStats(
            gpu_util=util.gpu,
            enc_util=util.encoder,
            dec_util=util.decoder,
            mem_used_mb=mem.used // (1024 * 1024),
            mem_total_mb=mem.total // (1024 * 1024),
            power_w=power,
        )
    except pynvml.NVMLError:
        return None


def _read_gpu_stats_nvidia_smi(gpu_index: int) -> GpuStats | None:
    nvidia_smi = shutil.which("nvidia-smi")
    if not nvidia_smi:
        return None
    result = subprocess.run(
        [
            nvidia_smi,
            "--query-gpu=utilization.gpu,utilization.encoder,utilization.decoder,memory.used,memory.total,power.draw",
            "--format=csv,noheader,nounits",
            f"--id={gpu_index}",
        ],
        capture_output=True, text=True, errors="replace", check=False,
    )
    if result.returncode != 0 or not result.stdout.strip():
        return None
    parts = [p.strip() for p in result.stdout.splitlines()[0].split(",")]
    power = None
    try:
        if len(parts) >= 6 and parts[5]:
            power = float(parts[5])
    except ValueError:
        pass
    return GpuStats(
        gpu_util=int(parts[0]) if parts[0] else 0,
        enc_util=int(parts[1]) if parts[1] else 0,
        dec_util=int(parts[2]) if parts[2] else 0,
        mem_used_mb=int(parts[3]) if parts[3] else 0,
        mem_total_mb=int(parts[4]) if parts[4] else 0,
        power_w=power,
    )


def _pynvml_shutdown() -> None:
    pynvml = _get_pynvml()
    if pynvml:
        try:
            pynvml.nvmlShutdown()
        except Exception:
            pass


# ── Config file loading ────────────────────────────────────
def _load_config_file(path: Path) -> dict[str, Any]:
    """Load config from YAML or JSON file. Returns empty dict if not found or unreadable."""
    if not path.exists():
        return {}

    raw = path.read_text(encoding="utf-8")
    suffix = path.suffix.lower()

    if suffix in (".yaml", ".yml"):
        try:
            import yaml
            return yaml.safe_load(raw) or {}
        except ImportError:
            logger.warning("PyYAML not installed, trying JSON parse for %s", path)
            return json.loads(raw)
    elif suffix == ".json":
        return json.loads(raw)
    elif suffix == ".toml":
        try:
            import tomllib
        except ImportError:
            try:
                import tomli as tomllib
            except ImportError:
                logger.warning("toml/tomli not available, cannot parse %s", path)
                return {}
        with path.open("rb") as f:
            return tomllib.load(f)

    logger.warning("Unrecognized config format: %s", path)
    return {}


def _cli_to_config_key(cli_key: str) -> str:
    """Map argparse dest name to config file key."""
    return cli_key.replace("-", "_")


def merge_config_with_args(
    config: dict[str, Any],
    parsed: argparse.Namespace,
    arg_keys: list[str],
) -> None:
    """Apply config values as defaults — only where CLI arg was not explicitly set."""
    for key in arg_keys:
        config_key = _cli_to_config_key(key)
        if config_key in config:
            if getattr(parsed, key) == _ARG_DEFAULTS.get(key):
                setattr(parsed, key, config[config_key])


# ── FFmpeg helpers ─────────────────────────────────────────
def has_nvenc_encoder(ffmpeg: str) -> bool:
    result = subprocess.run(
        [ffmpeg, "-hide_banner", "-encoders"],
        capture_output=True, text=True, errors="replace", check=False,
    )
    return "hevc_nvenc" in (result.stdout + result.stderr)


def log_has_failure(log_path: Path) -> bool:
    if not log_path.exists():
        return True
    text = log_path.read_text(encoding="utf-8", errors="replace")
    return any(pattern in text for pattern in FAILED_PATTERNS)


def detect_gpu_capabilities() -> dict[str, Any]:
    """Auto-detect GPU encoder capabilities."""
    capabilities = {
        "nvenc_available": False,
        "qsv_available": False,
        "gpu_name": "unknown",
        "vram_mb": 0,
    }
    nvidia_smi = shutil.which("nvidia-smi")
    if nvidia_smi:
        result = subprocess.run(
            [nvidia_smi, "--query-gpu=name,memory.total", "--format=csv,noheader,nounits"],
            capture_output=True, text=True, errors="replace", check=False,
        )
        if result.returncode == 0 and result.stdout.strip():
            parts = [p.strip() for p in result.stdout.splitlines()[0].split(",")]
            capabilities["gpu_name"] = parts[0] if parts else "unknown"
            capabilities["vram_mb"] = int(parts[1]) if len(parts) > 1 else 0
    # Check FFmpeg encoder availability using existing ffmpeg resolution
    ffmpeg_path = shutil.which("ffmpeg") or "ffmpeg"
    try:
        result = subprocess.run(
            [ffmpeg_path, "-hide_banner", "-encoders"],
            capture_output=True, text=True, errors="replace", check=False,
        )
        encoders = result.stdout + result.stderr
        capabilities["nvenc_available"] = "hevc_nvenc" in encoders
        capabilities["qsv_available"] = "hevc_qsv" in encoders
    except FileNotFoundError:
        logger.warning("FFmpeg not found, cannot detect encoder capabilities.")
    return capabilities


def estimate_max_concurrency(vram_mb: int) -> int:
    """Estimate safe max concurrency for auto-sweep based on VRAM."""
    if vram_mb >= 16000:
        return 64
    elif vram_mb >= 8000:
        return 48
    elif vram_mb >= 4000:
        return 24
    return 12


def auto_sweep(ffmpeg: str, sample_path: Path, out_dir: Path, args: argparse.Namespace) -> int:
    """Auto mode: detect hardware, sweep concurrency, print recommendation."""
    caps = detect_gpu_capabilities()
    logger.info("GPU: %s (%d MB VRAM)", caps["gpu_name"], caps["vram_mb"])
    logger.info("NVENC: %s, QSV: %s",
                "available" if caps["nvenc_available"] else "unavailable",
                "available" if caps["qsv_available"] else "unavailable")

    max_n = estimate_max_concurrency(caps["vram_mb"])
    logger.info("Auto-sweeping: 4 -> %d-way (step=4)", max_n)

    results = run_sweep(
        ffmpeg, sample_path, out_dir,
        min_concurrency=4, max_concurrency=max_n, step=4,
        duration=args.duration, fps=args.fps, preset=args.preset,
        rate_control=args.rate_control, bitrate=args.bitrate,
        gpu_index=args.gpu_index,
    )

    saturated = next((r for r in results if r["failed"] > 0 or r.get("avg_enc_util", 0) > 95), None)
    max_stable = saturated["concurrency"] - 4 if saturated else max_n
    best_eff = max(results, key=lambda r: r.get("fps_per_watt") or 0)

    print(f"\nAuto Mode — Recommended: {max_stable}-way (max stable), {best_eff['concurrency']}-way (best fps/watt)")
    return 0

# ── Sample preparation ─────────────────────────────────────
def prepare_sample(
    ffmpeg: str,
    input_path: Path,
    sample_path: Path,
    duration: int,
    width: int,
    height: int,
    fps: int,
    sample_preset: str,
    rate_control: str,
    bitrate: str,
) -> None:
    sample_path.parent.mkdir(parents=True, exist_ok=True)
    vf = (
        f"scale={width}:{height}:force_original_aspect_ratio=decrease,"
        f"pad={width}:{height}:(ow-iw)/2:(oh-ih)/2,"
        f"fps={fps}"
    )
    logger.info("Encoding sample: %s → %s (preset=%s)", input_path.name, sample_path, sample_preset)
    subprocess.run(
        [
            ffmpeg, "-hide_banner", "-y",
            "-stream_loop", "-1", "-i", str(input_path),
            "-t", str(duration), "-an",
            "-vf", vf,
            "-c:v", "h264_nvenc",
            "-preset", sample_preset,
            "-rc", rate_control,
            "-b:v", bitrate,
            str(sample_path),
        ],
        check=True,
    )


# ── Concurrency run ────────────────────────────────────────
def run_concurrency_case(
    ffmpeg: str,
    sample_path: Path,
    out_dir: Path,
    concurrency: int,
    duration: int,
    fps: int,
    preset: str,
    rate_control: str,
    bitrate: str,
    gpu_index: int = 0,
) -> dict[str, Any]:
    case_dir = out_dir / f"{concurrency}way"
    case_dir.mkdir(parents=True, exist_ok=True)
    gpu_log_path = out_dir / "gpu_log.csv"

    jobs: list[EncodeJob] = []
    started_at = time.perf_counter()
    gpu_samples: list[GpuStats] = []

    logger.info("Starting %d concurrent HEVC NVENC encodes...", concurrency)

    # Launch all encodes
    for index in range(concurrency):
        output_path = case_dir / f"out_{index:03d}.hevc"
        log_path = case_dir / f"out_{index:03d}.log"
        log_file = log_path.open("wb")
        args = [
            ffmpeg, "-hide_banner", "-y",
            "-stream_loop", "-1", "-i", str(sample_path),
            "-t", str(duration), "-an",
            "-c:v", "hevc_nvenc",
            "-preset", preset,
            "-rc", rate_control,
            "-b:v", bitrate,
            "-f", "hevc",
            str(output_path),
        ]
        process = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=log_file)
        _register_process(process)
        jobs.append(EncodeJob(process=process, log_path=log_path, index=index))
        log_file.close()  # Popen dup'd the fd, we can close ours

    # Poll GPU while encodes run
    while any(j.process.poll() is None for j in jobs):
        stats = read_gpu_stats(gpu_index)
        if stats:
            gpu_samples.append(stats)
            with gpu_log_path.open("a", newline="", encoding="utf-8") as f:
                writer = csv.writer(f)
                writer.writerow([
                    datetime.now().isoformat(timespec="seconds"), concurrency,
                    stats.gpu_util, stats.enc_util, stats.dec_util,
                    stats.mem_used_mb, stats.mem_total_mb, stats.power_w or "",
                ])
        time.sleep(1)

    # Cleanup — wait for all encodes with timeout
    for job in jobs:
        try:
            job.process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            logger.warning("Encode %d hung, killing", job.index)
            job.process.kill()
            job.process.wait()
        finally:
            _deregister_process(job.process)

    elapsed = time.perf_counter() - started_at

    # Tally results
    completed = 0
    for job in jobs:
        output_path = case_dir / f"out_{job.index:03d}.hevc"
        has_output = output_path.exists() and output_path.stat().st_size > 0
        if has_output and job.process.returncode == 0 and not log_has_failure(job.log_path):
            completed += 1

    failed = concurrency - completed
    aggregate_fps = round((completed * duration * fps) / max(elapsed, 0.001), 2)
    realtime_factor = round(aggregate_fps / (concurrency * fps), 3) if concurrency > 0 else 0

    # Aggregate GPU stats
    avg_gpu = round(sum(s.gpu_util for s in gpu_samples) / max(len(gpu_samples), 1), 1)
    avg_enc = round(sum(s.enc_util for s in gpu_samples) / max(len(gpu_samples), 1), 1)
    power_samples = [s.power_w for s in gpu_samples if s.power_w is not None]
    avg_power = round(sum(power_samples) / max(len(power_samples), 1), 1) if power_samples else None
    fps_per_watt = round(aggregate_fps / avg_power, 2) if avg_power and avg_power > 0 else None

    logger.info(
        "%d-way: completed=%d failed=%d elapsed=%.1fs fps=%.1f "
        "enc_util=%.1f%% power=%sW fps/w=%s",
        concurrency, completed, failed, elapsed, aggregate_fps,
        avg_enc, avg_power, fps_per_watt,
    )

    return {
        "concurrency": concurrency,
        "started": concurrency,
        "completed": completed,
        "failed": failed,
        "total_elapsed_s": round(elapsed, 2),
        "aggregate_fps": aggregate_fps,
        "realtime_factor": realtime_factor,
        "avg_gpu_util": avg_gpu,
        "avg_enc_util": avg_enc,
        "avg_power_w": avg_power,
        "fps_per_watt": fps_per_watt,
    }


# ── Sweep mode ─────────────────────────────────────────────
def run_sweep(
    ffmpeg: str,
    sample_path: Path,
    out_dir: Path,
    min_concurrency: int,
    max_concurrency: int,
    step: int,
    duration: int,
    fps: int,
    preset: str,
    rate_control: str,
    bitrate: str,
    gpu_index: int,
) -> list[dict[str, Any]]:
    """Sweep concurrency levels and find saturation / optimal points."""
    results: list[dict[str, Any]] = []
    saturation = None

    for n in range(min_concurrency, max_concurrency + 1, step):
        logger.info("── Sweep: %d-way (%d–%d, step=%d) ──", n, min_concurrency, max_concurrency, step)
        row = run_concurrency_case(
            ffmpeg, sample_path, out_dir, n, duration, fps,
            preset, rate_control, bitrate, gpu_index,
        )
        results.append(row)
        append_summary(out_dir / "summary.csv", row)

        # Detect saturation: encoder > 95% or any failures
        if saturation is None:
            enc_util = row.get("avg_enc_util", 0)
            if isinstance(enc_util, (int, float)) and enc_util > 95:
                saturation = n
                logger.info("Encoder saturation at %d-way (enc_util=%.1f%%)", n, enc_util)
            if row["failed"] > 0:
                saturation = n
                logger.info("First failures at %d-way — marking saturation", n)

    return results


# ── CSV output ──

def write_csv_header(path: Path, header: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as f:
        csv.writer(f).writerow(header)

def append_summary(path: Path, row: dict[str, Any]) -> None:
    with path.open("a", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=SUMMARY_FIELDS, extrasaction="ignore")
        writer.writerow(row)


def write_json_summary(path: Path, results: list[dict[str, Any]]) -> None:
    """Write results as JSON file."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as f:
        json.dump(results, f, indent=2, default=str)


# ── Process cleanup registry ───────────────────────────────
_cleanup_registry: list[subprocess.Popen[bytes]] = []

def _register_process(proc: subprocess.Popen[bytes]) -> None:
    _cleanup_registry.append(proc)


def _deregister_process(proc: subprocess.Popen[bytes]) -> None:
    try:
        _cleanup_registry.remove(proc)
    except ValueError:
        pass


def _terminate_all() -> None:
    for proc in _cleanup_registry:
        if proc.poll() is None:
            try:
                proc.terminate()
            except Exception:
                pass
    for proc in _cleanup_registry:
        try:
            proc.wait(timeout=5)
        except (subprocess.TimeoutExpired, Exception):
            try:
                proc.kill()
            except Exception:
                pass
    _cleanup_registry.clear()


def _signal_handler(sig: int, _frame: Any) -> None:
    logger.warning("Signal %d received, cleaning up child processes...", sig)
    _terminate_all()
    _pynvml_shutdown()
    sys.exit(128 + sig)


atexit.register(_terminate_all)


# ── CLI defaults (manual, so we can merge config without parse_known_args) ──
_ARG_DEFAULTS: dict[str, object] = {
    "ffmpeg": "ffmpeg",
    "input": None,
    "out_dir": str(Path(__file__).resolve().parent.parent / "output"),
    "min_concurrency": None,
    "max_concurrency": None,
    "step": 4,
    "duration": DEFAULT_DURATION,
    "width": DEFAULT_WIDTH,
    "height": DEFAULT_HEIGHT,
    "fps": DEFAULT_FPS,
    "preset": DEFAULT_BENCH_PRESET,
    "sample_preset": DEFAULT_SAMPLE_PRESET,
    "rate_control": DEFAULT_RATE_CONTROL,
    "bitrate": DEFAULT_BITRATE,
    "gpu_index": 0,
}

_CONFIG_KEYS = list(_ARG_DEFAULTS.keys())


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="NVENC concurrency benchmark — find max stable encodes with power efficiency."
    )
    parser.add_argument("--ffmpeg", default=_ARG_DEFAULTS["ffmpeg"], help="Path to ffmpeg binary.")
    parser.add_argument("--input", default=None, help="Input video path (required).")
    parser.add_argument("--out-dir", default=_ARG_DEFAULTS["out_dir"],
                        help="Output directory for logs and encoded streams.")
    parser.add_argument("--config", type=Path, default=None,
                        help="YAML/JSON/TOML config file. CLI args override config values.")

    # Concurrency sweep
    parser.add_argument("--min-concurrency", type=int, default=None,
                        help="Sweep start concurrency (required).")
    parser.add_argument("--max-concurrency", type=int, default=None,
                        help="Sweep end concurrency, inclusive (required).")
    parser.add_argument("--step", type=int, default=_ARG_DEFAULTS["step"],
                        help="Sweep step size (default: 4).")

    # Video params
    parser.add_argument("--duration", type=int, default=_ARG_DEFAULTS["duration"],
                        help="Seconds per encode (default: 10).")
    parser.add_argument("--width", type=int, default=_ARG_DEFAULTS["width"])
    parser.add_argument("--height", type=int, default=_ARG_DEFAULTS["height"])
    parser.add_argument("--fps", type=int, default=_ARG_DEFAULTS["fps"])

    # Encode params
    parser.add_argument("--preset", default=_ARG_DEFAULTS["preset"],
                        help="NVENC preset for benchmark encodes (default: p1).")
    parser.add_argument("--sample-preset", default=_ARG_DEFAULTS["sample_preset"],
                        help="NVENC preset for sample normalization (default: p4, higher quality).")
    parser.add_argument("--rate-control", default=_ARG_DEFAULTS["rate_control"])
    parser.add_argument("--bitrate", default=_ARG_DEFAULTS["bitrate"])

    # Misc
    parser.add_argument("--auto", action="store_true",
                        help="Auto-detect hardware and find optimal concurrency.")
    parser.add_argument("--output-format", choices=["csv", "json", "both"], default="csv",
                        help="Summary output format (default: csv).")
    parser.add_argument("--gpu-index", type=int, default=_ARG_DEFAULTS["gpu_index"],
                        help="GPU index for monitoring (default: 0).")
    parser.add_argument("--force-regenerate", action="store_true",
                        help="Re-encode the normalized sample even if it already exists.")
    parser.add_argument("--verbose", "-v", action="store_true",
                        help="Enable debug logging.")

    parsed = parser.parse_args(argv)

    # Load config file if specified — config values override defaults
    if parsed.config:
        config = _load_config_file(parsed.config)
        if config:
            merge_config_with_args(config, parsed, _CONFIG_KEYS)

    return parsed


# ── Main ───────────────────────────────────────────────────
def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s [%(levelname)s] %(message)s",
        datefmt="%H:%M:%S",
    )

    ffmpeg = resolve_tool(args.ffmpeg)
    out_dir = Path(args.out_dir)
    sample_path = out_dir / "sample_1080p30_10s.mp4"
    summary_path = out_dir / "summary.csv"
    gpu_log_path = out_dir / "gpu_log.csv"

    # ── Validation ──
    if not args.input:
        logger.error("--input is required. Specify an input video file.")
        return 1
    input_path = Path(args.input)
    if not input_path.exists():
        logger.error("Input video not found: %s", input_path)
        return 1
    if not has_nvenc_encoder(ffmpeg):
        logger.error("This FFmpeg build does not expose hevc_nvenc.")
        return 1

    # ── Prepare sample (skip if exists) ──
    if sample_path.exists() and sample_path.stat().st_size > 0 and not args.force_regenerate:
        logger.info("Sample already exists, skipping: %s", sample_path)
    else:
        if args.force_regenerate and sample_path.exists():
            logger.info("Force-regenerating sample: %s", sample_path)
        prepare_sample(
            ffmpeg, input_path, sample_path,
            args.duration, args.width, args.height, args.fps,
            args.sample_preset, args.rate_control, args.bitrate,
        )

    # ── Write CSV headers ──
    write_csv_header(summary_path, SUMMARY_FIELDS)
    write_csv_header(gpu_log_path, GPU_LOG_HEADER)


    # ── Auto mode ──
    if args.auto:
        return auto_sweep(ffmpeg, sample_path, out_dir, args)
    # ── Register signal handlers ──
    signal.signal(signal.SIGINT, _signal_handler)
    if hasattr(signal, "SIGTERM"):
        signal.signal(signal.SIGTERM, _signal_handler)
    if hasattr(signal, "SIGBREAK"):
        signal.signal(signal.SIGBREAK, _signal_handler)

    # ── Run ──
    results: list[dict[str, Any]] = []

    if args.min_concurrency is None or args.max_concurrency is None:
        logger.error("--min-concurrency and --max-concurrency are both required.")
        return 1

    results = run_sweep(
        ffmpeg, sample_path, out_dir,
        args.min_concurrency, args.max_concurrency, args.step,
        args.duration, args.fps, args.preset,
        args.rate_control, args.bitrate, args.gpu_index,
    )

    # ── Summary report ──
    print("\n" + "=" * 60)
    print("  NVENC Concurrency Benchmark Results")
    print("=" * 60)
    best_fps_w = None
    for r in results:
        pw = r.get("fps_per_watt")
        marker = ""
        if pw and (best_fps_w is None or pw > best_fps_w):
            best_fps_w = pw
            marker = " ← best efficiency"
        print(
            f"  {r['concurrency']:>3d}-way | "
            f"done={r['completed']}/{r['started']} "
            f"fail={r['failed']} | "
            f"fps={r['aggregate_fps']:>8.1f} | "
            f"enc={r.get('avg_enc_util', 'N/A')}% | "
            f"power={r.get('avg_power_w', 'N/A')}W | "
            f"fps/w={pw or 'N/A'}{marker}"
        )

    saturated = next((r for r in results if r["failed"] > 0 or r.get("avg_enc_util", 0) > 95), None)
    max_stable = saturated["concurrency"] - args.step if saturated else args.max_concurrency
    best_eff = max(results, key=lambda r: r.get("fps_per_watt") or 0)
    print(f"\n  Recommended: {max_stable}-way (max stable), "
          f"{best_eff['concurrency']}-way (best fps/watt)")
    print(f"\n  Summary CSV:  {summary_path}")
    print(f"  GPU log CSV:  {gpu_log_path}")
    print("=" * 60 + "\n")

    # ── JSON output ──
    if args.output_format in ("json", "both"):
        json_path = out_dir / "summary.json"
        write_json_summary(json_path, results)
        print(f"  JSON summary: {json_path}")

    _pynvml_shutdown()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
