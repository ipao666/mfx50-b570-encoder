#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 ]]; then
    cat >&2 <<'USAGE'
usage: run_45_from_manifest.sh <manifest.csv> <output_dir> [route_offset] [route_count]

Runs a B570 oneVPL multi-route batch from a manifest produced by the validation
pipeline. Default route count is 45, but the fourth argument can specify any
route count available in the manifest. The manifest must contain:

  id,input_codec,input_annexb,h265

Optional:

  RISK_CSV=/path/to/risk.csv

When RISK_CSV is provided, the script chooses the same three CQP profiles used
by the 2026-06-02 B570 v3 10-minute validation:

  base_q36b48     -> QPI/QPP/QPB 36/38/48
  day_guard_q34   -> QPI/QPP/QPB 34/36/42
  risk_q35        -> QPI/QPP/QPB 35/37/43

Environment overrides:

  SAMPLE_MULTI_TRANSCODE     default: <sdk>/bin/sample_multi_transcode_b570
  DEVICE                     default: /dev/dri/renderD129
  OUTPUT_FPS                 default: 30
  MUX_MP4                    default: 0
  FFMPEG                     default: ffmpeg
USAGE
    exit 2
fi

MANIFEST="$1"
OUT_DIR="$2"
ROUTE_OFFSET="${3:-0}"
ROUTE_COUNT="${4:-45}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDK_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
source "${SCRIPT_DIR}/b570_policy_env.sh"

SAMPLE_MULTI_TRANSCODE="${SAMPLE_MULTI_TRANSCODE:-${SDK_ROOT}/bin/sample_multi_transcode_b570}"
DEVICE="${DEVICE:-/dev/dri/renderD129}"
OUTPUT_FPS="${OUTPUT_FPS:-30}"
MUX_MP4="${MUX_MP4:-0}"
FFMPEG="${FFMPEG:-ffmpeg}"
RISK_CSV="${RISK_CSV:-}"

if [[ ! -f "${MANIFEST}" ]]; then
    echo "manifest not found: ${MANIFEST}" >&2
    exit 1
fi
if [[ ! -x "${SAMPLE_MULTI_TRANSCODE}" ]]; then
    echo "sample_multi_transcode not executable: ${SAMPLE_MULTI_TRANSCODE}" >&2
    exit 1
fi

mkdir -p "${OUT_DIR}/h265" "${OUT_DIR}/mp4" "${OUT_DIR}/logs"
PAR_FILE="${OUT_DIR}/b570_manifest_45.par"
PLAN_FILE="${OUT_DIR}/policy_plan.csv"

python3 - "$MANIFEST" "$OUT_DIR" "$PAR_FILE" "$PLAN_FILE" "$ROUTE_OFFSET" "$ROUTE_COUNT" "$DEVICE" "$RISK_CSV" <<'PY'
import csv
import sys
from pathlib import Path

manifest_path = Path(sys.argv[1])
out_dir = Path(sys.argv[2])
par_path = Path(sys.argv[3])
plan_path = Path(sys.argv[4])
route_offset = int(sys.argv[5])
route_count = int(sys.argv[6])
device = sys.argv[7]
risk_csv = Path(sys.argv[8]) if sys.argv[8] else None

risk = {}
if risk_csv and risk_csv.exists():
    with risk_csv.open(newline="") as f:
        risk = {row["id"]: row for row in csv.DictReader(f)}

def pick_profile(cid):
    row = risk.get(cid, {})
    try:
        mean_y = float(row.get("mean_y", "0"))
        edge = float(row.get("edge_density", "0"))
    except ValueError:
        mean_y, edge = 0.0, 0.0
    if 95.0 <= mean_y <= 108.0 and 0.15 <= edge <= 0.225:
        return "day_guard_q34", 34, 36, 42
    if 95.0 <= mean_y <= 112.0 and edge > 0.25:
        return "risk_q35", 35, 37, 43
    return "base_q36b48", 36, 38, 48

with manifest_path.open(newline="") as f:
    rows = list(csv.DictReader(f))

batch = rows[route_offset:route_offset + route_count]
if len(batch) != route_count:
    raise SystemExit(f"need {route_count} rows from offset {route_offset}, got {len(batch)}")

with par_path.open("w") as par, plan_path.open("w", newline="") as plan:
    writer = csv.writer(plan)
    writer.writerow(["session", "id", "input_codec", "profile", "qpi", "qpp", "qpb", "input_annexb", "output_h265"])
    for session, row in enumerate(batch):
        cid = row["id"]
        codec = row.get("input_codec", "h264").strip()
        if codec not in ("h264", "hevc", "h265"):
            raise SystemExit(f"unsupported codec for {cid}: {codec}")
        input_type = "h265" if codec in ("hevc", "h265") else "h264"
        input_path = row["input_annexb"]
        output_path = out_dir / "h265" / f"{cid}.h265"
        profile, qpi, qpp, qpb = pick_profile(cid)
        writer.writerow([session, cid, input_type, profile, qpi, qpp, qpb, input_path, output_path])
        par.write(
            f"-hw -device {device} -i::{input_type} {input_path} -o::h265 {output_path} "
            f"-u veryfast -async 2 -MemType::video -gpucopy::on "
            f"-cqp -qpi {qpi} -qpp {qpp} -qpb {qpb} "
            f"-gop_size 300 -dist 4 -override_encoder_framerate 30 "
            f"-AdaptiveI:on -ScenarioInfo 6 -ContentInfo 3 -AdaptiveCQM:on -extmbqp\n"
        )
PY

mfx50_b570_export_policy_env
export LD_LIBRARY_PATH="${SDK_ROOT}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

/usr/bin/time -f 'wall=%e' "${SAMPLE_MULTI_TRANSCODE}" -par "${PAR_FILE}" \
    > "${OUT_DIR}/logs/transcode.log" 2>&1

if [[ "${MUX_MP4}" == "1" ]]; then
    while IFS=, read -r session cid _rest; do
        [[ "${session}" == "session" ]] && continue
        "${FFMPEG}" -hide_banner -loglevel error -y -f hevc -r "${OUTPUT_FPS}" \
            -i "${OUT_DIR}/h265/${cid}.h265" -c:v copy -tag:v hvc1 \
            -movflags +faststart "${OUT_DIR}/mp4/${cid}.mp4"
    done < "${PLAN_FILE}"
fi

echo "par: ${PAR_FILE}"
echo "plan: ${PLAN_FILE}"
echo "h265 outputs: ${OUT_DIR}/h265"
