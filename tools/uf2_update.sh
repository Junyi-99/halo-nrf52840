#!/bin/bash
# Update a running halo-nrf52840 board that has the Adafruit UF2 bootloader
# (XIAO nRF52840, nice!nano, Feather): serial DFU command, wait for the drive,
# copy the UF2. Usage: tools/uf2_update.sh [port] [uf2]   (needs sudo for mount)
set -e
PORT=${1:-/dev/ttyACM0}
UF2=${2:-$(dirname "$0")/../build/xiao_ble/zephyr/zephyr.uf2}
python3 -c "
import serial, time
p = serial.Serial('$PORT', 115200, timeout=1); p.dtr = p.rts = False
p.write(b'DFU\n'); time.sleep(0.2)
" 2>/dev/null || true
DEV=
for i in $(seq 1 40); do
  DEV=$(lsblk -rno NAME,TRAN,TYPE | awk '$2=="usb" && $3=="disk" {print "/dev/"$1; exit}')
  [ -n "$DEV" ] && break
  python3 -c 'import time; time.sleep(0.5)'
done
[ -n "$DEV" ] || { echo "no UF2 drive appeared"; exit 1; }
MP=$(lsblk -rno MOUNTPOINT "$DEV")
if [ -z "$MP" ]; then sudo mkdir -p /mnt/uf2 && sudo mount -o uid=$(id -u) "$DEV" /mnt/uf2 && MP=/mnt/uf2; fi
cp "$UF2" "$MP/" && sync
python3 -c 'import time; time.sleep(5)'
sudo umount "$MP" 2>/dev/null || true
for i in $(seq 1 30); do
  lsusb 2>/dev/null | grep -q halo-nrf52840 && [ -e "$PORT" ] && break
  python3 -c 'import time; time.sleep(0.5)'
done
echo "updated; port $PORT"
