#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="${MFX50RT_SDK_VERSION:-0.5.0-alpha1}"
ARCH="${MFX50RT_SDK_ARCH:-linux-x86_64}"
PKG_NAME="mfx50rt-sdk-${VERSION}-${ARCH}"

WORK_DIR="${MFX50RT_PACKAGE_WORK_DIR:-${ROOT_DIR}/build/package}"
BUILD_DIR="${WORK_DIR}/build"
INSTALL_DIR="${WORK_DIR}/install"
STAGE_DIR="${WORK_DIR}/${PKG_NAME}"
DIST_DIR="${MFX50RT_DIST_DIR:-${ROOT_DIR}/dist}"
TARBALL="${DIST_DIR}/${PKG_NAME}.tar.gz"
SMOKE_INPUT="${MFX50RT_SMOKE_INPUT:-/home/admi/下载/mfx50_dll_src/tmp/ch000_120.h264}"
BUILD_FFMPEG_DEMOS="${MFX50RT_BUILD_FFMPEG_DEMOS:-OFF}"

run_logged() {
    local log_file="$1"
    shift
    {
        printf 'command:'
        printf ' %q' "$@"
        printf '\n'
        "$@"
    } >"${log_file}" 2>&1
}

mkdir -p "${DIST_DIR}"
rm -rf "${WORK_DIR}" "${TARBALL}"
mkdir -p "${BUILD_DIR}" "${INSTALL_DIR}" "${STAGE_DIR}"

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}" \
    -DMFX50RT_BUILD_FFMPEG_DEMOS="${BUILD_FFMPEG_DEMOS}"
cmake --build "${BUILD_DIR}" --parallel
cmake --install "${BUILD_DIR}"

mkdir -p \
    "${STAGE_DIR}/include" \
    "${STAGE_DIR}/lib" \
    "${STAGE_DIR}/bin" \
    "${STAGE_DIR}/demo" \
    "${STAGE_DIR}/docs" \
    "${STAGE_DIR}/python" \
    "${STAGE_DIR}/packaging" \
    "${STAGE_DIR}/validation"

cp -a "${INSTALL_DIR}/include/." "${STAGE_DIR}/include/"
cp -a "${INSTALL_DIR}/lib/." "${STAGE_DIR}/lib/"
cp -a "${INSTALL_DIR}/bin/." "${STAGE_DIR}/bin/"
cp "${ROOT_DIR}/demo_mfx50_realtime_file.cpp" "${STAGE_DIR}/demo/"
if [[ -d "${ROOT_DIR}/demo" ]]; then
    cp -a "${ROOT_DIR}/demo/." "${STAGE_DIR}/demo/"
fi
cp -a "${ROOT_DIR}/docs/." "${STAGE_DIR}/docs/"
find "${ROOT_DIR}/python" -maxdepth 1 -type f -name '*.py' -exec cp {} "${STAGE_DIR}/python/" \;
cp -a "${ROOT_DIR}/packaging/." "${STAGE_DIR}/packaging/"

LD_PATH="${STAGE_DIR}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
run_logged "${STAGE_DIR}/validation/api_probe.log" \
    env LD_LIBRARY_PATH="${LD_PATH}" "${STAGE_DIR}/bin/mfx50rt_api_probe"

cat >"${WORK_DIR}/minimal_c_demo.c" <<'EOF'
#include "mfx50_realtime.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    MFX50RT_Config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = sizeof(cfg);
    int rc = MFX50RT_DefaultConfig(&cfg);
    if (rc != MFX50_OK) {
        return 1;
    }
    printf("version=%s\n", MFX50RT_GetVersion());
    printf("abi=%d\n", MFX50RT_GetAbiVersion());
    printf("config_size=%u\n", cfg.struct_size);
    return cfg.abi_version == MFX50RT_API_VERSION ? 0 : 2;
}
EOF
run_logged "${STAGE_DIR}/validation/minimal_c_demo_build.log" \
    cc "${WORK_DIR}/minimal_c_demo.c" \
    -I"${STAGE_DIR}/include" \
    -L"${STAGE_DIR}/lib" \
    -Wl,-rpath,"${STAGE_DIR}/lib" \
    -lmfx50_realtime \
    -o "${WORK_DIR}/minimal_c_demo"
run_logged "${STAGE_DIR}/validation/minimal_c_demo_run.log" \
    env LD_LIBRARY_PATH="${LD_PATH}" "${WORK_DIR}/minimal_c_demo"

if [[ -f "${SMOKE_INPUT}" && -x "${STAGE_DIR}/bin/mfx50rt_stream_probe" ]]; then
    run_logged "${STAGE_DIR}/validation/stream_probe_poll.log" \
        env LD_LIBRARY_PATH="${LD_PATH}" "${STAGE_DIR}/bin/mfx50rt_stream_probe" \
        --input "${SMOKE_INPUT}" \
        --input-format annexb \
        --output "${WORK_DIR}/smoke_poll.h265" \
        --output-mode poll \
        --profile quality_90_near \
        --strict-realtime \
        --max-frames 120
    run_logged "${STAGE_DIR}/validation/stream_probe_callback.log" \
        env LD_LIBRARY_PATH="${LD_PATH}" "${STAGE_DIR}/bin/mfx50rt_stream_probe" \
        --input "${SMOKE_INPUT}" \
        --input-format annexb \
        --output "${WORK_DIR}/smoke_callback.h265" \
        --output-mode callback \
        --profile quality_90_near \
        --strict-realtime \
        --max-frames 120 \
        --drain-via-flush
else
    {
        printf 'stream_probe smoke skipped\n'
        printf 'reason: input not found or stream probe missing\n'
        printf 'input: %s\n' "${SMOKE_INPUT}"
    } >"${STAGE_DIR}/validation/stream_probe_skipped.log"
fi

(
    cd "${STAGE_DIR}"
    find . \( -type f -o -type l \) | sed 's#^\./##' | sort > MANIFEST.txt
    find . -type f ! -name sha256sums.txt -print0 | sort -z | xargs -0 sha256sum > sha256sums.txt
)

tar -C "${WORK_DIR}" -czf "${TARBALL}" "${PKG_NAME}"
sha256sum "${TARBALL}" >"${TARBALL}.sha256"

printf 'package=%s\n' "${TARBALL}"
printf 'sha256=%s.sha256\n' "${TARBALL}"
printf 'stage=%s\n' "${STAGE_DIR}"
