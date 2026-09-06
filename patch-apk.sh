#!/usr/bin/env bash
set -euo pipefail
shopt -s nullglob

APKTOOL_VERSION="3.0.3"
BUILD_TOOLS_VERSION="37.0.0"

PATCHES_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

usage() { echo "Usage: ${0##*/} <path/to/bh.apk>" >&2; exit 1; }

[[ $# -eq 1 ]] || usage
[[ -f "$1" ]] || { echo "Error: '$1' is not a file." >&2; exit 1; }
APK="$(realpath -- "$1")"

for cmd in java git curl; do
  command -v "$cmd" >/dev/null 2>&1 || { echo "Error: $cmd not found in PATH." >&2; exit 1; }
done

BUILD_DIR="$(mktemp -d -t blockheads-XXXXXXXX)"
cleanup() { rm -rf -- "${BUILD_DIR}"; }
trap cleanup EXIT

export ANDROID_HOME="${BUILD_DIR}/android-sdk"
BUILD_TOOLS="${ANDROID_HOME}/build-tools/${BUILD_TOOLS_VERSION}"
SECURITY_DIR="${BUILD_DIR}/build/target/product/security"

cd "${BUILD_DIR}"

# Download dependencies
echo "Downloading dependencies..."
curl -fsSL -o apktool.jar \
  "https://github.com/iBotPeaches/Apktool/releases/download/v${APKTOOL_VERSION}/apktool_${APKTOOL_VERSION}.jar"
curl -fsSL -o android "https://dl.google.com/android/cli/latest/linux_x86_64/android"
chmod +x android
./android sdk install "build-tools;${BUILD_TOOLS_VERSION}"
git clone --filter=blob:none --sparse --depth 1 --single-branch -b main \
  https://android.googlesource.com/platform/build build
git -C build sparse-checkout set target/product/security


# Decompile, patch and recompile APK
echo "" && echo "Patching APK..."
cp -- "${APK}" bh.apk
java -jar apktool.jar d ./bh.apk -o bh -p "${BUILD_DIR}/framework"
(
  cd bh
  git apply --verbose "${PATCHES_DIR}/all-in-one.patch"
  libs=( "${PATCHES_DIR}"/*/libs/armeabi-v7a/* )
  if (( ${#libs[@]} )); then
    mkdir -p lib/armeabi-v7a
    cp -- "${libs[@]}" lib/armeabi-v7a/
  else
    echo "Warning: no native libs found under ${PATCHES_DIR}/*/libs/armeabi-v7a" >&2
  fi
)
java -jar apktool.jar b bh -o unaligned-bh.apk -p "${BUILD_DIR}/framework"

# Align and sign APK
echo "" && echo "Signing APK..."
"${BUILD_TOOLS}/zipalign" -p -f 4 unaligned-bh.apk aligned-bh.apk
OUT="${PATCHES_DIR}/signed-patched-bh.apk"
"${BUILD_TOOLS}/apksigner" sign \
  --key "${SECURITY_DIR}/testkey.pk8" \
  --cert "${SECURITY_DIR}/testkey.x509.pem" \
  --out "${OUT}" \
  aligned-bh.apk

"${BUILD_TOOLS}/apksigner" verify --print-certs "${OUT}"
echo "" && echo "Done! Patched APK available at ${OUT}"