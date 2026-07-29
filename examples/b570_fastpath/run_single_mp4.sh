#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 ]]; then
    cat >&2 <<'USAGE'
usage: run_single_mp4.sh <input.mp4> <output.mp4> [max_frames]

Compresses one H.264 MP4 file to one HEVC MP4 file through the same B570
oneVPL settings used by the multi-route example.
USAGE
    exit 2
fi

INPUT_MP4="$1"
OUTPUT_MP4="$2"
MAX_FRAMES="${3:-${TRANSCODE_FRAMES:-}}"

SDK_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "${SDK_ROOT}/examples/b570_fastpath/b570_policy_env.sh"
SAMPLE_MULTI_TRANSCODE="${SAMPLE_MULTI_TRANSCODE:-${SDK_ROOT}/bin/sample_multi_transcode_b570}"
FFMPEG="${FFMPEG:-ffmpeg}"
FFPROBE="${FFPROBE:-ffprobe}"
DEVICE="${DEVICE:-/dev/dri/renderD129}"
QPI="${QPI:-36}"
QPP="${QPP:-38}"
QPB="${QPB:-48}"
GOP_SIZE="${GOP_SIZE:-300}"
DIST="${DIST:-4}"
USAGE="${USAGE:-veryfast}"
ROI_ONLY="${ROI_ONLY:-1}"
ROI_ANALYZE_INTERVAL="${ROI_ANALYZE_INTERVAL:-30}"
ROI_DELTA_QP="${ROI_DELTA_QP:--4}"
VEHICLE_ROI_MARGIN="${VEHICLE_ROI_MARGIN:-8}"
PLATE_ROI_DELTA_QP="${PLATE_ROI_DELTA_QP:--12}"
PLATE_ROI_MARGIN="${PLATE_ROI_MARGIN:-24}"
TEXT_ROI_DELTA_QP="${TEXT_ROI_DELTA_QP:--4}"
TEXT_ROI_MARGIN="${TEXT_ROI_MARGIN:-4}"
ENABLE_TRAFFIC_ROI="${ENABLE_TRAFFIC_ROI:-1}"
OUTPUT_FPS="${OUTPUT_FPS:-30}"

if [[ ! -x "${SAMPLE_MULTI_TRANSCODE}" ]]; then
    echo "sample_multi_transcode not executable: ${SAMPLE_MULTI_TRANSCODE}" >&2
    exit 1
fi

codec="$("${FFPROBE}" -v error -select_streams v:0 -show_entries stream=codec_name -of csv=p=0 "${INPUT_MP4}" | head -1)"
if [[ "${codec}" != "h264" ]]; then
    echo "unsupported codec: ${codec}; this example expects H.264 MP4 input" >&2
    exit 1
fi

TMP="$(mktemp -d /tmp/b570_single.XXXXXX)"
trap 'rm -rf "${TMP}"' EXIT
mkdir -p "${TMP}/logs"

frames=()
frame_arg=""
if [[ -n "${MAX_FRAMES}" ]]; then
    frames=(-frames:v "${MAX_FRAMES}")
    frame_arg=" -n ${MAX_FRAMES}"
fi

"${FFMPEG}" -hide_banner -loglevel error -y -i "${INPUT_MP4}" \
    -map 0:v:0 -an -sn -dn -c:v copy -bsf:v h264_mp4toannexb \
    "${frames[@]}" -f h264 "${TMP}/input.h264"

PAR_FILE="${TMP}/single.par"
printf -- "-hw -device %s -i::h264 %s -o::h265 %s%s -u %s -async 2 -MemType::video -gpucopy::on -cqp -qpi %s -qpp %s -qpb %s -gop_size %s -dist %s -override_encoder_framerate 30 -AdaptiveI:on -extmbqp\n" \
    "${DEVICE}" "${TMP}/input.h264" "${TMP}/output.h265" "${frame_arg}" "${USAGE}" "${QPI}" "${QPP}" "${QPB}" "${GOP_SIZE}" "${DIST}" >"${PAR_FILE}"

export MFX50RT_SAMPLE_HYBRIDTSRQ="${ENABLE_TRAFFIC_ROI}"
export MFX50RT_SAMPLE_HYBRIDTSRQ_ROI_ONLY="${ROI_ONLY}"
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
"${SAMPLE_MULTI_TRANSCODE}" -par "${PAR_FILE}" 2>&1 | tee "${TMP}/logs/transcode.log"
"${FFMPEG}" -hide_banner -loglevel error -y -f hevc -r "${OUTPUT_FPS}" -i "${TMP}/output.h265" \
    -c:v copy -tag:v hvc1 -movflags +faststart "${OUTPUT_MP4}"

echo "output: ${OUTPUT_MP4}"
