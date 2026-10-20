#!/bin/bash
# Build halo-matter with nRF Connect SDK. Usage: ./build.sh [board] [extra west args]
set -e
BOARD=${1:-xiao_ble}
shift || true
cd "$(dirname "$0")"
NCS=${NCS_WORKSPACE:-$HOME/ncs}
source "$NCS/.venv/bin/activate"
export ZEPHYR_BASE="$NCS/zephyr"
export ZEPHYR_SDK_INSTALL_DIR=${ZEPHYR_SDK_INSTALL_DIR:-$HOME/zephyr-sdk-1.0.1}
west build -p auto -b "$BOARD" --sysbuild -d "build/${BOARD//\//_}" . "$@"
