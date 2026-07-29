#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 ]]; then
    cat >&2 <<'USAGE'
usage: run_45_from_h264_dir.sh <h264_dir> <output_dir> [max_frames] [route_count]

Runs the B570 oneVPL multi-session path on H.264 AnnexB elementary streams
and writes HEVC elementary streams plus the generated .par file.

Default route count is 45 to reproduce the B570 validation. Override it with
the fourth argument or ROUTE_COUNT.

Environment overrides:
  SAMPLE_MULTI_TRANSCODE     Path to sample_multi_transcode_b570.
  DEVICE                     oneVPL device selector, default /dev/dri/renderD129.
  ROUTE_COUNT                Number of input streams, default 45.
  QPI QPP QPB                CQP values, defaults 36 38 48.
  GOP_SIZE DIST USAGE        Defaults 300 4 veryfast.
  ROI_ONLY                   Default 1.
  ROI_ANALYZE_INTERVAL       Default 30.
  ROI_DELTA_QP               Default -4.
  PLATE_ROI_DELTA_QP         Default -12.
  PLATE_ROI_MARGIN           Default 24.
  ENABLE_TRAFFIC_ROI         Default 1.
  STATIC_SKIP                Default 0.
USAGE
    exit 2
fi

INPUT_DIR="$1"
OUT_DIR="$2"
MAX_FRAMES="${3:-${TRANSCODE_FRAMES:-}}"
ROUTE_COUNT="${4:-${ROUTE_COUNT:-45}}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDK_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
source "${SCRIPT_DIR}/b570_policy_env.sh"

SAMPLE_MULTI_TRANSCODE="${SAMPLE_MULTI_TRANSCODE:-${SDK_ROOT}/bin/sample_multi_transcode_b570}"
DEVICE="${DEVICE:-/dev/dri/renderD129}"
QPI="${QPI:-36}"
QPP="${QPP:-38}"
QPB="${QPB:-48}"
GOP_SIZE="${GOP_SIZE:-300}"
DIST="${DIST:-4}"
USAGE="${USAGE:-veryfast}"
VEHICLE_ROI_MARGIN="${VEHICLE_ROI_MARGIN:-8}"
TEXT_ROI_DELTA_QP="${TEXT_ROI_DELTA_QP:--4}"
TEXT_ROI_MARGIN="${TEXT_ROI_MARGIN:-4}"
ENABLE_TRAFFIC_ROI="${ENABLE_TRAFFIC_ROI:-1}"
STATIC_SKIP="${STATIC_SKIP:-0}"

if [[ ! -x "${SAMPLE_MULTI_TRANSCODE}" ]]; then
    echo "sample_multi_transcode not executable: ${SAMPLE_MULTI_TRANSCODE}" >&2
    exit 1
fi
if [[ ! -d "${INPUT_DIR}" ]]; then
    echo "input directory not found: ${INPUT_DIR}" >&2
    exit 1
fi

mkdir -p "${OUT_DIR}/h265" "${OUT_DIR}/logs"
PAR_FILE="${OUT_DIR}/b570_${ROUTE_COUNT}_routes.par"

mapfile -t INPUTS < <(find "${INPUT_DIR}" -maxdepth 1 -type f \( -iname '*.h264' -o -iname '*.264' \) | sort | head -n "${ROUTE_COUNT}")
if [[ "${#INPUTS[@]}" -ne "${ROUTE_COUNT}" ]]; then
    echo "need exactly ${ROUTE_COUNT} input streams, found ${#INPUTS[@]} in ${INPUT_DIR}" >&2
    exit 1
fi

: >"${PAR_FILE}"
idx=0
for input in "${INPUTS[@]}"; do
    id="$(printf 'ch%03d' "${idx}")"
    out="${OUT_DIR}/h265/${id}.h265"
    frame_arg=""
    if [[ -n "${MAX_FRAMES}" ]]; then
        frame_arg=" -n ${MAX_FRAMES}"
    fi
    printf -- "-hw -device %s -i::h264 %s -o::h265 %s%s -u %s -async 2 -MemType::video -gpucopy::on -cqp -qpi %s -qpp %s -qpb %s -gop_size %s -dist %s -override_encoder_framerate 30 -AdaptiveI:on -extmbqp\n" \
        "${DEVICE}" "${input}" "${out}" "${frame_arg}" "${USAGE}" "${QPI}" "${QPP}" "${QPB}" "${GOP_SIZE}" "${DIST}" \
        | sed 's/ -extmbqp$/ -ScenarioInfo 6 -ContentInfo 3 -AdaptiveCQM:on -extmbqp/' >>"${PAR_FILE}"
    idx=$((idx + 1))
done

export MFX50RT_SAMPLE_HYBRIDTSRQ="${ENABLE_TRAFFIC_ROI}"
export MFX50RT_SAMPLE_VEHICLE_ROI_MARGIN="${VEHICLE_ROI_MARGIN}"
export MFX50RT_SAMPLE_TEXT_ROI_DELTA_QP="${TEXT_ROI_DELTA_QP}"
export MFX50RT_SAMPLE_TEXT_ROI_MARGIN="${TEXT_ROI_MARGIN}"
export MFX50RT_SAMPLE_STATIC_SKIP="${STATIC_SKIP}"
mfx50_b570_export_policy_env

export LD_LIBRARY_PATH="${SDK_ROOT}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
"${SAMPLE_MULTI_TRANSCODE}" -par "${PAR_FILE}" 2>&1 | tee "${OUT_DIR}/logs/transcode.log"
