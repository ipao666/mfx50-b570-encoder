#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 ]]; then
    cat >&2 <<'USAGE'
usage: run_45_daynight_from_mp4_dir.sh <mp4_dir> <output_dir> [max_frames] [route_count]

Demuxes H.264 MP4 files, assigns a day or night CQP profile from
the timestamp in each filename, runs the B570 multi-route example, and muxes
HEVC outputs back to MP4.

Default route count is 45. Override it with the fourth argument or ROUTE_COUNT.

Defaults:
  day   06:00-18:59 -> QPI/QPP/QPB 36/38/44 for SSIM guard
  night 19:00-05:59 -> QPI/QPP/QPB 47/49/51 for high compression

Environment overrides:
  DAY_START_HOUR DAY_END_HOUR
  DAY_QPI DAY_QPP DAY_QPB
  NIGHT_QPI NIGHT_QPP NIGHT_QPB
  SAMPLE_MULTI_TRANSCODE DEVICE OUTPUT_FPS
USAGE
    exit 2
fi

INPUT_DIR="$1"
OUT_DIR="$2"
MAX_FRAMES="${3:-${TRANSCODE_FRAMES:-}}"
ROUTE_COUNT="${4:-${ROUTE_COUNT:-45}}"

SDK_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "${SDK_ROOT}/examples/b570_fastpath/b570_policy_env.sh"
SAMPLE_MULTI_TRANSCODE="${SAMPLE_MULTI_TRANSCODE:-${SDK_ROOT}/bin/sample_multi_transcode_b570}"
FFMPEG="${FFMPEG:-ffmpeg}"
FFPROBE="${FFPROBE:-ffprobe}"
DEVICE="${DEVICE:-/dev/dri/renderD129}"
OUTPUT_FPS="${OUTPUT_FPS:-30}"

DAY_START_HOUR="${DAY_START_HOUR:-6}"
DAY_END_HOUR="${DAY_END_HOUR:-19}"
DAY_QPI="${DAY_QPI:-36}"
DAY_QPP="${DAY_QPP:-38}"
DAY_QPB="${DAY_QPB:-44}"
NIGHT_QPI="${NIGHT_QPI:-47}"
NIGHT_QPP="${NIGHT_QPP:-49}"
NIGHT_QPB="${NIGHT_QPB:-51}"

USAGE_PRESET="${USAGE:-veryfast}"
GOP_SIZE="${GOP_SIZE:-300}"
DIST="${DIST:-4}"
ROI_ANALYZE_INTERVAL="${ROI_ANALYZE_INTERVAL:-30}"
ROI_DELTA_QP="${ROI_DELTA_QP:--4}"
VEHICLE_ROI_MARGIN="${VEHICLE_ROI_MARGIN:-8}"
PLATE_ROI_DELTA_QP="${PLATE_ROI_DELTA_QP:--12}"
PLATE_ROI_MARGIN="${PLATE_ROI_MARGIN:-24}"
TEXT_ROI_DELTA_QP="${TEXT_ROI_DELTA_QP:--4}"
TEXT_ROI_MARGIN="${TEXT_ROI_MARGIN:-4}"

if [[ ! -x "${SAMPLE_MULTI_TRANSCODE}" ]]; then
    echo "sample_multi_transcode not executable: ${SAMPLE_MULTI_TRANSCODE}" >&2
    exit 1
fi

mkdir -p "${OUT_DIR}/h264" "${OUT_DIR}/h265" "${OUT_DIR}/mp4" "${OUT_DIR}/logs"
PAR_FILE="${OUT_DIR}/b570_daynight_45.par"
PLAN_FILE="${OUT_DIR}/daynight_plan.csv"

mapfile -t MP4S < <(find "${INPUT_DIR}" -maxdepth 1 -type f \( -iname '*.mp4' -o -iname '*.m4v' \) | sort | head -n "${ROUTE_COUNT}")
if [[ "${#MP4S[@]}" -ne "${ROUTE_COUNT}" ]]; then
    echo "need exactly ${ROUTE_COUNT} MP4 inputs, found ${#MP4S[@]} in ${INPUT_DIR}" >&2
    exit 1
fi

hour_from_name() {
    python3 - "$1" <<'PY'
import re, sys
name = sys.argv[1]
matches = re.findall(r"20[0-9]{6}(\d{2})(\d{2})(\d{2})", name)
if not matches:
    print(12)
else:
    print(int(matches[0][0]))
PY
}

printf 'id,profile,hour,qpi,qpp,qpb,source\n' >"${PLAN_FILE}"
: >"${PAR_FILE}"

idx=0
for src in "${MP4S[@]}"; do
    id="$(printf 'ch%03d' "${idx}")"
    codec="$("${FFPROBE}" -v error -select_streams v:0 -show_entries stream=codec_name -of csv=p=0 "${src}" | head -1)"
    if [[ "${codec}" != "h264" ]]; then
        echo "unsupported codec for ${src}: ${codec}; this example expects H.264 MP4 input" >&2
        exit 1
    fi

    frames=()
    frame_arg=""
    if [[ -n "${MAX_FRAMES}" ]]; then
        frames=(-frames:v "${MAX_FRAMES}")
        frame_arg=" -n ${MAX_FRAMES}"
    fi
    "${FFMPEG}" -hide_banner -loglevel error -y -i "${src}" \
        -map 0:v:0 -an -sn -dn -c:v copy -bsf:v h264_mp4toannexb \
        "${frames[@]}" -f h264 "${OUT_DIR}/h264/${id}.h264"

    hour="$(hour_from_name "$(basename "${src}")")"
    if (( hour >= DAY_START_HOUR && hour < DAY_END_HOUR )); then
        profile="day"
        qpi="${DAY_QPI}"; qpp="${DAY_QPP}"; qpb="${DAY_QPB}"
    else
        profile="night"
        qpi="${NIGHT_QPI}"; qpp="${NIGHT_QPP}"; qpb="${NIGHT_QPB}"
    fi
    printf '"%s","%s",%s,%s,%s,%s,"%s"\n' "${id}" "${profile}" "${hour}" "${qpi}" "${qpp}" "${qpb}" "${src}" >>"${PLAN_FILE}"
    printf -- "-hw -device %s -i::h264 %s -o::h265 %s%s -u %s -async 2 -MemType::video -gpucopy::on -cqp -qpi %s -qpp %s -qpb %s -gop_size %s -dist %s -override_encoder_framerate 30 -AdaptiveI:on -extmbqp\n" \
        "${DEVICE}" "${OUT_DIR}/h264/${id}.h264" "${OUT_DIR}/h265/${id}.h265" "${frame_arg}" "${USAGE_PRESET}" "${qpi}" "${qpp}" "${qpb}" "${GOP_SIZE}" "${DIST}" >>"${PAR_FILE}"
    idx=$((idx + 1))
done

export MFX50RT_SAMPLE_HYBRIDTSRQ=1
export MFX50RT_SAMPLE_HYBRIDTSRQ_ROI_ONLY=1
export MFX50RT_SAMPLE_HYBRIDTSRQ_DISABLE_FRAME_QP=1
export MFX50RT_SAMPLE_DISABLE_ROI_FROM_MBQP=1
export MFX50RT_SAMPLE_ROI_ANALYZE_INTERVAL="${ROI_ANALYZE_INTERVAL}"
export MFX50RT_SAMPLE_MBQP_REUSE_INTERVAL="${ROI_ANALYZE_INTERVAL}"
export MFX50RT_SAMPLE_ROI_DELTA_QP="${ROI_DELTA_QP}"
export MFX50RT_SAMPLE_VEHICLE_ROI_MARGIN="${VEHICLE_ROI_MARGIN}"
export MFX50RT_SAMPLE_PLATE_ROI_DELTA_QP="${PLATE_ROI_DELTA_QP}"
export MFX50RT_SAMPLE_PLATE_ROI_MARGIN="${PLATE_ROI_MARGIN}"
export MFX50RT_SAMPLE_TEXT_ROI_DELTA_QP="${TEXT_ROI_DELTA_QP}"
export MFX50RT_SAMPLE_TEXT_ROI_MARGIN="${TEXT_ROI_MARGIN}"
mfx50_b570_export_policy_env

export LD_LIBRARY_PATH="${SDK_ROOT}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
"${SAMPLE_MULTI_TRANSCODE}" -par "${PAR_FILE}" 2>&1 | tee "${OUT_DIR}/logs/transcode.log"

idx=0
for _src in "${MP4S[@]}"; do
    id="$(printf 'ch%03d' "${idx}")"
    "${FFMPEG}" -hide_banner -loglevel error -y -f hevc -r "${OUTPUT_FPS}" -i "${OUT_DIR}/h265/${id}.h265" \
        -c:v copy -tag:v hvc1 -movflags +faststart "${OUT_DIR}/mp4/${id}.mp4"
    idx=$((idx + 1))
done

echo "plan: ${PLAN_FILE}"
echo "mp4 outputs: ${OUT_DIR}/mp4"
