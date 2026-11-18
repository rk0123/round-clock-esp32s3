#!/usr/bin/env bash
set -euo pipefail

firmware_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${ROUND_CLOCK_BUILD_DIR:-$firmware_dir/.build}"
cli=("${ARDUINO_CLI:-arduino-cli}")
if [[ -n "${ROUND_CLOCK_ARDUINO_CONFIG:-}" ]]; then
  cli+=(--config-file "$ROUND_CLOCK_ARDUINO_CONFIG")
fi

board='esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,CDCOnBoot=cdc,CPUFreq=240,UploadSpeed=921600,PartitionScheme=app5M_fat7M_16MB'
export TZ=Asia/Shanghai
"${cli[@]}" compile --fqbn "$board" --build-path "$build_dir" "$firmware_dir" "$@"
