#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="${MFX50RT_SDK_VERSION:-0.6.0-b570}"
ARCH="${MFX50RT_SDK_ARCH:-linux-x86_64}"
PKG_NAME="mfx50rt-b570-sdk-${VERSION}-${ARCH}"

BUILD_DIR="${MFX50RT_COMBINATION_BUILD_DIR:-${ROOT_DIR}/build_hybridtsrq}"
WORK_DIR="${MFX50RT_PACKAGE_WORK_DIR:-${ROOT_DIR}/build/package_b570}"
STAGE_DIR="${WORK_DIR}/${PKG_NAME}"
DIST_DIR="${MFX50RT_DIST_DIR:-${ROOT_DIR}/dist}"
TARBALL="${DIST_DIR}/${PKG_NAME}.tar.gz"

VPL_TOOLS_ROOT="${MFX50RT_LIBVPL_TOOLS_ROOT:-/home/admi/libvpl-tools-1.5.0}"
VPL_BUILD_DIR="${MFX50RT_LIBVPL_BUILD_DIR:-${VPL_TOOLS_ROOT}/build-codex-hybrid}"
SAMPLE_MULTI_TRANSCODE="${MFX50RT_SAMPLE_MULTI_TRANSCODE:-${VPL_BUILD_DIR}/sample_multi_transcode}"

copy_if_exists() {
    local src="$1"
    local dst="$2"
    if [[ -e "${src}" ]]; then
        mkdir -p "$(dirname "${dst}")"
        cp -a "${src}" "${dst}"
    fi
}

require_file() {
    local path="$1"
    if [[ ! -e "${path}" ]]; then
        echo "required file missing: ${path}" >&2
        exit 1
    fi
}

mkdir -p "${DIST_DIR}"
rm -rf "${WORK_DIR}" "${TARBALL}" "${TARBALL}.sha256"
mkdir -p "${STAGE_DIR}/"{bin,docs,examples/b570_fastpath,examples/simple_transcode,examples/split_codec,include,lib,validation}

if [[ "${MFX50RT_PACKAGE_SKIP_BUILD:-0}" != "1" ]]; then
    if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
        cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release -DMFX50RT_BUILD_FFMPEG_DEMOS=OFF
    fi
    cmake --build "${BUILD_DIR}" --parallel --target \
        mfx50rt mfx50_realtime mfx50_encoder \
        mfx50_policy policy_decision_demo \
        mfx50rt_api_probe mfx50rt_stream_probe query_capabilities \
        simple_transcode async_45_routes bench_real_45_files \
        mfx50_encoder_minimal_demo mfx50_split_file_transcode
fi

if [[ "${MFX50RT_PACKAGE_SKIP_BUILD:-0}" != "1" && ! -x "${SAMPLE_MULTI_TRANSCODE}" && -f "${VPL_BUILD_DIR}/CMakeCache.txt" ]]; then
    cmake --build "${VPL_BUILD_DIR}" --parallel --target sample_multi_transcode
fi
require_file "${SAMPLE_MULTI_TRANSCODE}"

cp -a "${ROOT_DIR}/mfx50_realtime.h" "${STAGE_DIR}/include/"
cp -a "${ROOT_DIR}/mfx50_api.h" "${STAGE_DIR}/include/"
cp -a "${ROOT_DIR}/mfx50_device.h" "${STAGE_DIR}/include/"
cp -a "${ROOT_DIR}/mfx50_surface.h" "${STAGE_DIR}/include/"
cp -a "${ROOT_DIR}/mfx50_decoder.h" "${STAGE_DIR}/include/"
cp -a "${ROOT_DIR}/mfx50_encoder.h" "${STAGE_DIR}/include/"
cp -a "${ROOT_DIR}/mfx50_transcoder.h" "${STAGE_DIR}/include/"
cp -a "${ROOT_DIR}/include/mfx50rt.h" "${STAGE_DIR}/include/"
cp -a "${ROOT_DIR}/include/mfx50_realtime_algo.h" "${STAGE_DIR}/include/"
cp -a "${ROOT_DIR}/include/mfx50_types.h" "${STAGE_DIR}/include/"
cp -a "${ROOT_DIR}/include/mfx50_policy.h" "${STAGE_DIR}/include/"

cp -a "${BUILD_DIR}"/libmfx50rt.so* "${STAGE_DIR}/lib/"
cp -a "${BUILD_DIR}"/libmfx50_realtime.so* "${STAGE_DIR}/lib/"
cp -a "${BUILD_DIR}"/libmfx50_encoder.so* "${STAGE_DIR}/lib/"
cp -a "${BUILD_DIR}"/libmfx50_policy.so* "${STAGE_DIR}/lib/"

for exe in mfx50rt_api_probe mfx50rt_stream_probe query_capabilities simple_transcode async_45_routes bench_real_45_files policy_decision_demo mfx50_encoder_minimal_demo mfx50_split_file_transcode; do
    copy_if_exists "${BUILD_DIR}/${exe}" "${STAGE_DIR}/bin/${exe}"
done
cp -a "${SAMPLE_MULTI_TRANSCODE}" "${STAGE_DIR}/bin/sample_multi_transcode_b570"

cp -a "${ROOT_DIR}/examples/b570_fastpath/." "${STAGE_DIR}/examples/b570_fastpath/"
chmod +x "${STAGE_DIR}/examples/b570_fastpath/"*.sh
copy_if_exists "${ROOT_DIR}/examples/simple_transcode.c" "${STAGE_DIR}/examples/simple_transcode/simple_transcode.c"
copy_if_exists "${ROOT_DIR}/tools/mfx50_split_file_transcode.cpp" "${STAGE_DIR}/examples/split_codec/mfx50_split_file_transcode.cpp"

copy_if_exists "${ROOT_DIR}/docs/B570_BINARY_SDK_README.md" "${STAGE_DIR}/docs/B570_BINARY_SDK_README.md"
copy_if_exists "${ROOT_DIR}/docs/ENGINEER_HANDOFF.md" "${STAGE_DIR}/docs/ENGINEER_HANDOFF.md"
copy_if_exists "${ROOT_DIR}/docs/B570_API_REFERENCE.md" "${STAGE_DIR}/docs/B570_API_REFERENCE.md"
copy_if_exists "${ROOT_DIR}/docs/B570_VIDEO_COMPRESSION_FLOW.md" "${STAGE_DIR}/docs/B570_VIDEO_COMPRESSION_FLOW.md"
copy_if_exists "${ROOT_DIR}/docs/MFX50_SPLIT_CODEC_API.md" "${STAGE_DIR}/docs/MFX50_SPLIT_CODEC_API.md"
copy_if_exists "${ROOT_DIR}/docs/MFX50_CODEC_FUNCTION_REFERENCE.md" "${STAGE_DIR}/docs/MFX50_CODEC_FUNCTION_REFERENCE.md"
copy_if_exists "${ROOT_DIR}/docs/MFX50_SPLIT_CODEC_CHANGE_SUMMARY_20260603.md" "${STAGE_DIR}/docs/MFX50_SPLIT_CODEC_CHANGE_SUMMARY_20260603.md"
copy_if_exists "${ROOT_DIR}/docs/MFX50_SPLIT_DECODER_FIX_20260604.md" "${STAGE_DIR}/docs/MFX50_SPLIT_DECODER_FIX_20260604.md"
copy_if_exists "${ROOT_DIR}/docs/MFX50_DEVICE_SESSION_FIX_20260605.md" "${STAGE_DIR}/docs/MFX50_DEVICE_SESSION_FIX_20260605.md"
copy_if_exists "${ROOT_DIR}/docs/MFX50_ENCODER_RESOLUTION_BUFFER_FIX_20260608.md" "${STAGE_DIR}/docs/MFX50_ENCODER_RESOLUTION_BUFFER_FIX_20260608.md"
copy_if_exists "${ROOT_DIR}/docs/绿屏抽帧修复说明_20260608.md" "${STAGE_DIR}/docs/绿屏抽帧修复说明_20260608.md"
copy_if_exists "${ROOT_DIR}/docs/MFX50_REALTIME_FLUSH_FIX_20260609.md" "${STAGE_DIR}/docs/MFX50_REALTIME_FLUSH_FIX_20260609.md"
copy_if_exists "${ROOT_DIR}/docs/MFX50_ENCODER_API_AND_DEMO.md" "${STAGE_DIR}/docs/MFX50_TRANSCODER_API_AND_DEMO.md"
copy_if_exists "${ROOT_DIR}/docs/B570_FILE_STRUCTURE.md" "${STAGE_DIR}/docs/B570_FILE_STRUCTURE.md"
copy_if_exists "${ROOT_DIR}/docs/B570_CURRENT_LIMITS.md" "${STAGE_DIR}/docs/B570_CURRENT_LIMITS.md"
copy_if_exists "${ROOT_DIR}/docs/B570_OPTIMIZATION_DECISION.md" "${STAGE_DIR}/docs/B570_OPTIMIZATION_DECISION.md"
copy_if_exists "${ROOT_DIR}/docs/POLICY_SDK_README.md" "${STAGE_DIR}/docs/POLICY_SDK_README.md"
copy_if_exists "${ROOT_DIR}/docs/POLICY_SDK_API.md" "${STAGE_DIR}/docs/POLICY_SDK_API.md"
copy_if_exists "${ROOT_DIR}/docs/B570_POLICY_MAPPING.md" "${STAGE_DIR}/docs/B570_POLICY_MAPPING.md"

copy_if_exists "/home/admi/codex_eval/_b570_local45_current/results.csv" "${STAGE_DIR}/validation/results_size_first_45x1000.csv"
copy_if_exists "/home/admi/codex_eval/_b570_local45_yguard_q36/results.csv" "${STAGE_DIR}/validation/results_y_guard_45x1000.csv"
copy_if_exists "/home/admi/codex_eval/_b570_local45_full_q41/results.csv" "${STAGE_DIR}/validation/results_size_first_45_full.csv"
copy_if_exists "/home/admi/codex_eval/_b570_daynight_static_45x1000/results_h264ref.csv" "${STAGE_DIR}/validation/results_quality_daynight_45x1000.csv"
copy_if_exists "/home/admi/112/112_B570/onevpl_results/b570-linux-x2-20路/metrics_500f_summary.txt" "${STAGE_DIR}/validation/old_onevpl_20route_summary.txt"

python3 - "${STAGE_DIR}/validation" >"${STAGE_DIR}/validation/VALIDATION_SUMMARY.md" <<'PY'
import csv
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
print("# Validation Summary\n")
for csv_path in sorted(root.glob("results_*.csv")):
    rows = list(csv.DictReader(csv_path.open(encoding="utf-8")))
    print(f"## {csv_path.name}\n")
    print(f"routes: {len(rows)}\n")
    for key in ["compression_pct", "session_fps", "ssim_all", "ssim_y", "ssim_u", "ssim_v"]:
        vals = []
        for row in rows:
            try:
                if row.get(key) not in (None, ""):
                    vals.append(float(row[key]))
            except ValueError:
                pass
        if vals:
            avg = sum(vals) / len(vals)
            print(f"- {key}: avg={avg:.6f}, min={min(vals):.6f}, max={max(vals):.6f}")
    if rows and "ssim_y" in rows[0]:
        low = sorted(
            (row for row in rows if row.get("ssim_y")),
            key=lambda row: float(row["ssim_y"]),
        )[:5]
        if low:
            print("\nlowest Y SSIM:")
            for row in low:
                print(f"- {row.get('id','')}: compression={row.get('compression_pct','')}, "
                      f"ssim_y={row.get('ssim_y','')}, ssim_all={row.get('ssim_all','')}")
    print()
PY

{
    echo "# B570 Validation Environment"
    echo
    echo "## uname"
    uname -a || true
    echo
    echo "## os-release"
    cat /etc/os-release 2>/dev/null || true
    echo
    echo "## dri"
    ls -l /dev/dri 2>/dev/null || true
    echo
    echo "## oneVPL libraries"
    ldconfig -p 2>/dev/null | grep -E 'libvpl|libmfx-gen' || true
    echo
    echo "## pci display devices"
    lspci 2>/dev/null | grep -Ei 'vga|display|3d|intel|arc' || true
    echo
    echo "## vainfo renderD129"
    vainfo --display drm --device /dev/dri/renderD129 2>&1 | head -160 || true
} >"${STAGE_DIR}/validation/ENVIRONMENT.md"

cat >"${WORK_DIR}/minimal_realtime_demo.c" <<'EOF'
#include "mfx50_realtime.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    MFX50RT_Config cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (MFX50RT_DefaultConfig(&cfg) != MFX50_OK) return 1;
    cfg.device_selector = "intel:dGPU:0";
    cfg.route_count = 45;
    cfg.async_mode = 1;
    cfg.profile = MFX50_PROFILE_COMPRESS_90_PROBE_D;
    cfg.gop = 60;
    cfg.gop_ref_dist = 4;
    cfg.target_usage = 7;
    cfg.qpi = 41;
    cfg.qpp = 43;
    cfg.qpb = 51;
    printf("version=%s abi=%d routes=%d qpi/qpp/qpb=%d/%d/%d\n",
           MFX50RT_GetVersion(), MFX50RT_GetAbiVersion(),
           cfg.route_count, cfg.qpi, cfg.qpp, cfg.qpb);
    return cfg.abi_version == MFX50RT_API_VERSION ? 0 : 2;
}
EOF

cc "${WORK_DIR}/minimal_realtime_demo.c" \
    -I"${STAGE_DIR}/include" -L"${STAGE_DIR}/lib" \
    -Wl,-rpath,'$ORIGIN/../lib' -lmfx50_realtime \
    -o "${STAGE_DIR}/bin/minimal_realtime_demo"

LD_LIBRARY_PATH="${STAGE_DIR}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" \
    "${STAGE_DIR}/bin/minimal_realtime_demo" >"${STAGE_DIR}/validation/minimal_realtime_demo.log"

if [[ -x "${STAGE_DIR}/bin/mfx50rt_api_probe" ]]; then
    LD_LIBRARY_PATH="${STAGE_DIR}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" \
        "${STAGE_DIR}/bin/mfx50rt_api_probe" >"${STAGE_DIR}/validation/api_probe.log"
fi

cat >"${STAGE_DIR}/README.md" <<'EOF'
# MFX50RT B570 完整 SDK

请先阅读：

```text
docs/ENGINEER_HANDOFF.md
```

本包包含公开头文件、动态库、B570 example 二进制程序、B570 运行示例脚本和简要验证记录。
本包不包含算法源码、评估脚本、测试视频或临时探针输出。

已经验证过的 B570 全流程入口是：

```text
bin/sample_multi_transcode_b570
examples/b570_fastpath/*.sh
```

本包也包含 split decoder/encoder API 修复和验证 demo：

```text
docs/MFX50_SPLIT_DECODER_FIX_20260604.md
docs/MFX50_DEVICE_SESSION_FIX_20260605.md
docs/MFX50_SPLIT_CODEC_API.md
docs/MFX50_CODEC_FUNCTION_REFERENCE.md
bin/simple_transcode
examples/simple_transcode/simple_transcode.c
bin/mfx50_split_file_transcode
examples/split_codec/mfx50_split_file_transcode.cpp
```

其中 `mfx50_split_file_transcode` 用于验证：

```text
H.264/H.265 Annex-B packet -> MFX50 Decoder -> NV12 oneVPL GPU surface -> MFX50 Encoder -> HEVC packet
```

跨设备算法决策库是：

```text
include/mfx50_policy.h
include/mfx50_types.h
lib/libmfx50_policy.so
```
EOF

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
