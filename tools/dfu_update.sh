#!/bin/bash
# Update a running halo-nrf52840 dongle over USB: serial DFU command, then Nordic DFU.
# Usage: tools/dfu_update.sh [port] [zip]
set -e
PORT=${1:-/dev/ttyACM0}
ZIP=${2:-$(dirname "$0")/../build/halo_dongle.zip}
python3 -c "
import serial, time
p = serial.Serial('$PORT', 115200, timeout=1); p.dtr = p.rts = False
p.write(b'DFU\n'); time.sleep(0.2)
" 2>/dev/null || true
for i in $(seq 1 30); do
  if nrfutil device list 2>/dev/null | grep -q nordicDfu; then break; fi
  python3 -c 'import time; time.sleep(0.5)'
done
nrfutil device program --firmware "$ZIP" --traits nordicDfu
for i in $(seq 1 30); do
  lsusb 2>/dev/null | grep -q halo-nrf52840 && [ -e "$PORT" ] && break
  python3 -c 'import time; time.sleep(0.5)'
done
echo "updated; port $PORT"
