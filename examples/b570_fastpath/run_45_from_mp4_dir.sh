#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 ]]; then
    cat >&2 <<'USAGE'
usage: run_45_from_mp4_dir.sh <mp4_dir> <output_dir> [max_frames] [route_count]

Demuxes H.264 MP4 files into AnnexB H.264, runs the B570 multi-route example,
and muxes HEVC elementary stream outputs back to MP4.

Default route count is 45. Override it with the fourth argument or ROUTE_COUNT.
USAGE
    exit 2
fi

INPUT_DIR="$1"
OUT_DIR="$2"
MAX_FRAMES="${3:-${TRANSCODE_FRAMES:-}}"
ROUTE_COUNT="${4:-${ROUTE_COUNT:-45}}"
FFMPEG="${FFMPEG:-ffmpeg}"
FFPROBE="${FFPROBE:-ffprobe}"
RUN_H264_SCRIPT="${RUN_H264_SCRIPT:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/run_45_from_h264_dir.sh}"
OUTPUT_FPS="${OUTPUT_FPS:-30}"

mkdir -p "${OUT_DIR}/h264" "${OUT_DIR}/mp4" "${OUT_DIR}/logs"

mapfile -t MP4S < <(find "${INPUT_DIR}" -maxdepth 1 -type f \( -iname '*.mp4' -o -iname '*.m4v' \) | sort | head -n "${ROUTE_COUNT}")
if [[ "${#MP4S[@]}" -ne "${ROUTE_COUNT}" ]]; then
    echo "need exactly ${ROUTE_COUNT} MP4 inputs, found ${#MP4S[@]} in ${INPUT_DIR}" >&2
    exit 1
fi

idx=0
for src in "${MP4S[@]}"; do
    id="$(printf 'ch%03d' "${idx}")"
    codec="$("${FFPROBE}" -v error -select_streams v:0 -show_entries stream=codec_name -of csv=p=0 "${src}" | head -1)"
    if [[ "${codec}" != "h264" ]]; then
        echo "unsupported codec for ${src}: ${codec}; this example expects H.264 MP4 input" >&2
        exit 1
    fi
    frames=()
    if [[ -n "${MAX_FRAMES}" ]]; then
        frames=(-frames:v "${MAX_FRAMES}")
    fi
    "${FFMPEG}" -hide_banner -loglevel error -y -i "${src}" \
        -map 0:v:0 -an -sn -dn -c:v copy -bsf:v h264_mp4toannexb \
        "${frames[@]}" -f h264 "${OUT_DIR}/h264/${id}.h264"
    idx=$((idx + 1))
done

ROUTE_COUNT="${ROUTE_COUNT}" "${RUN_H264_SCRIPT}" "${OUT_DIR}/h264" "${OUT_DIR}" "${MAX_FRAMES}" "${ROUTE_COUNT}"

idx=0
for _src in "${MP4S[@]}"; do
    id="$(printf 'ch%03d' "${idx}")"
    "${FFMPEG}" -hide_banner -loglevel error -y -f hevc -r "${OUTPUT_FPS}" -i "${OUT_DIR}/h265/${id}.h265" \
        -c:v copy -tag:v hvc1 -movflags +faststart "${OUT_DIR}/mp4/${id}.mp4"
    idx=$((idx + 1))
done

echo "mp4 outputs: ${OUT_DIR}/mp4"
