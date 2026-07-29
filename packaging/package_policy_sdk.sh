#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-$ROOT_DIR/build-policy-sdk}"
OUT_DIR="${2:-$ROOT_DIR/dist/mfx50_policy_sdk_1.0.0_linux_x86_64}"

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR/include" "$OUT_DIR/lib" "$OUT_DIR/examples" "$OUT_DIR/docs"

cp "$ROOT_DIR/include/mfx50_types.h" "$OUT_DIR/include/"
cp "$ROOT_DIR/include/mfx50_policy.h" "$OUT_DIR/include/"
cp "$BUILD_DIR"/libmfx50_policy.so* "$OUT_DIR/lib/"
cp "$ROOT_DIR/examples/policy_decision_demo.c" "$OUT_DIR/examples/"
cp "$ROOT_DIR/docs/POLICY_SDK_README.md" "$OUT_DIR/docs/"
cp "$ROOT_DIR/docs/POLICY_SDK_API.md" "$OUT_DIR/docs/"
cp "$ROOT_DIR/docs/B570_POLICY_MAPPING.md" "$OUT_DIR/docs/"
cp "$ROOT_DIR/docs/POLICY_SDK_FILE_STRUCTURE.md" "$OUT_DIR/docs/"

(
    cd "$(dirname "$OUT_DIR")"
    tar -czf "$(basename "$OUT_DIR").tar.gz" "$(basename "$OUT_DIR")"
    sha256sum "$(basename "$OUT_DIR").tar.gz" > "$(basename "$OUT_DIR").tar.gz.sha256"
)

echo "$OUT_DIR"
echo "$OUT_DIR.tar.gz"
