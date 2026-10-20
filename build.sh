#!/bin/bash
# Usage: ./build.sh <board> [extra west args]
# Boards: xiao_ble  promicro_nrf52840/nrf52840/uf2  nrf52840dongle/nrf52840  nrf52840dongle/nrf52840/bare  nrf52840dk/nrf52840  adafruit_feather_nrf52840
set -e
BOARD=${1:?board}
shift
cd "$(dirname "$0")"
WS=${ZEPHYR_WORKSPACE:-$HOME/zephyrproject}
source "$WS/.venv/bin/activate"
export ZEPHYR_BASE="$WS/zephyr"
export ZEPHYR_SDK_INSTALL_DIR=${ZEPHYR_SDK_INSTALL_DIR:-$HOME/zephyr-sdk-1.0.1}
west build -p auto -b "$BOARD" -d "build/${BOARD//\//_}" . "$@"
