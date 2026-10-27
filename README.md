# halo-nrf52840

Control a first-generation BenQ ScreenBar Halo from an nRF52840 (no external RF module required).

<p align="center"><img src="docs/apple-home-two-lights.jpeg" width="260" alt="The ScreenBar Halo in Apple Home as Mainlight and Backlight">&nbsp;&nbsp;<img src="docs/xiao-nrf52840.jpeg" width="300" alt="Seeed XIAO nRF52840"></p>

<p align="center"><img src="docs/matter-pairing-qr.png" width="180" alt="Matter pairing QR code"><br><code>MT:6FCJ142C00KA0648G00</code> · manual code 34970112332</p>

## Prerequisites

- A Seeed **XIAO nRF52840** (other nRF52840 boards work too, see the build targets below).
- A Matter controller with a Thread border router: for Apple Home a HomePod mini or Apple TV 4K.
- Your ScreenBar Halo's **pairing address**, 4 bytes in on-air order (for example `16224CB1`). This
  firmware does not discover it; halo-esp32s3 and
  [benq-screenbar-halo-matter](https://github.com/Djoko-cli/benq-screenbar-halo-matter) describe
  how to capture it from the original remote.
- Toolchains: a [Zephyr](https://docs.zephyrproject.org/latest/develop/getting_started/index.html)
  v4.4 workspace with the Zephyr SDK for the USB firmware; an
  [nRF Connect SDK](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/installation.html) v3.4.1
  workspace for the Matter firmware. The build scripts look for them in `~/zephyrproject` and `~/ncs`
  (override with `ZEPHYR_WORKSPACE`, `ZEPHYR_SDK_INSTALL_DIR`, `NCS_WORKSPACE`).

Put your lamp's address into the firmware before building: `CONFIG_HALO_LAMP_ADDRESS` in
`matter/Kconfig` (default `0x16224CB1`), or pass it on the command line,
`matter/build.sh xiao_ble -- -Dmatter_CONFIG_HALO_LAMP_ADDRESS=0x16224CB1`. The USB firmware takes the
address with every command instead, so it needs no change.

## Build

```sh
./build.sh xiao_ble                         # Seeed XIAO nRF52840 (UF2, app at 0x27000)
./build.sh promicro_nrf52840/nrf52840/uf2   # Pro Micro / SuperMini / nice!nano clones (UF2, app at 0x26000)
./build.sh nrf52840dongle/nrf52840          # Nordic PCA10059 dongle (Nordic DFU)
./build.sh nrf52840dk/nrf52840              # Nordic DK (J-Link)
./build.sh adafruit_feather_nrf52840        # Feather (UF2 variant)
```

Outputs land in `build/<board>/zephyr/` as `zephyr.uf2` / `zephyr.hex`. The Matter firmware builds
with `matter/build.sh xiao_ble` into `matter/build/xiao_ble/matter/zephyr/`.

## Flash

UF2 boards (XIAO): double-tap RST, the board shows up as a USB drive, copy the image onto it. The
board reboots into the new firmware as soon as the copy completes; later updates skip the button by
sending `halo dfu` (Matter firmware) or `DFU` (USB firmware) on the serial port first.

```sh
# Linux: find the drive (label XIAO-SENSE), mount, copy, unmount
lsblk -o NAME,LABEL,TRAN | grep XIAO
sudo mount -o uid=$(id -u) /dev/sda /mnt/uf2        # /dev/sda as reported by lsblk
cp matter/build/xiao_ble/matter/zephyr/zephyr.uf2 /mnt/uf2/ && sync
sudo umount /mnt/uf2
# macOS / Windows: drag zephyr.uf2 onto the XIAO-SENSE drive
```

`tools/uf2_update.sh [port] [uf2]` does the whole cycle for the Matter firmware. Boards with Nordic's
DFU bootloader take `nrfutil device program --firmware <zip> --traits nordicDfu` (zip from
`tools/make_dfu_zip.py`, cycle in `tools/dfu_update.sh`); any board can also be programmed from the
hex over SWD with a J-Link.

## Use

Pairing with Apple Home (any Matter controller works the same way):

1. Build and flash the Matter firmware. The blue LED starts blinking slowly: the board is waiting to
   be paired.
2. Home app → **+** → **Add Accessory** → scan the QR code above (or *More options…* → enter
   **3497-011-2332**) → *Add Anyway* on the "uncertified accessory" warning. Commissioning goes over
   Bluetooth LE (blue LED blinks fast), then the board joins the border router's Thread network (green
   LED for 5 s). About a minute in total.
3. Two lights appear, "Light" (front lamp) and "Light 2" (back lamp); rename them in the Home app.
   Each switches on its own; brightness and colour temperature are shared by the lamp, so they stay
   in sync between the two.

Board LED:

| LED | Meaning |
|---|---|
| blue, slow blink | waiting to be paired (Bluetooth LE advertising) |
| blue, fast blink | a phone is connected, commissioning in progress |
| green for 5 s | joined the Thread network, paired |
| green flash after a command | the lamp acknowledged the frame |
| red flash after a command | no acknowledgement (frames are retried three times) |

Notes: every board flashed with this firmware uses the same pairing code (Matter test
discriminator 0xF00, passcode 20202021), so pair one board at a time. Sharing an already-paired
board with another app goes through the Home app's own pairing mode. The lamp only ever sends empty
acknowledgements, so changes made with the original remote are not reflected in the Home app.

## Extras: sensors and an air conditioner relay

The Matter firmware also exposes, on the XIAO nRF52840 (Sense):

| Matter endpoint | Source |
|---|---|
| 3 temperature, 4 humidity | AHT20 on I2C `i2c1` (D4 = SDA, D5 = SCL) |
| 5 illuminance | OPT3001 on I2C `i2c0` (D2 = SDA, D3 = SCL) |
| 6 pressure | BMP280 on `i2c1` (Apple Home does not show pressure sensors) |
| 7 thermostat (cooling only), 8 fan | forwarded over BLE to an ESP32-C3 infrared blaster |

The thermostat and fan do not talk to the air conditioner directly: a change in the Home app is
debounced (0.7 s) and sent once as `set on|off <temp> <fan>` to the BLE peripheral `AC-IR` from
[esp32c3-ac-ir](https://github.com/Junyi-99/esp32c3-ac-ir), which turns it into an infrared frame.
The XIAO connects on demand and disconnects after each command. The C3's Bluetooth address is a
constant in `matter/src/ac_link.cpp`; change it for your own board.

USB serial shell helpers: `halo env` (sensor readings), `halo matter` (attribute values in the data
model), `halo i2c` (scan both buses), `halo ac set on 25 auto` (send a command to AC-IR).

After changing `matter/src/default_zap/halo_light.zap`, regenerate **both** `zap-generated/` and
`halo_light.matter` (the codegen for the static cluster tables is built from the `.matter` file);
a stale `.matter` makes the firmware abort at boot.

## Details

How the radio trick works (oversampled 1 Mbit/s GFSK, ACK decoding), the measurements, the Matter
over Thread design (MPSL timeslots, two endpoints), and the USB firmware's host tool and serial
protocol: [docs/DETAILS.md](docs/DETAILS.md).

## Acknowledgements

- ScreenBar Halo protocol: [Djoko-cli/benq-screenbar-halo-matter](https://github.com/Djoko-cli/benq-screenbar-halo-matter)
- Frame layout and host tool: [halo-esp32s3](https://github.com/Junyi-99/halo-esp32s3)

## License

GPL-3.0, see `LICENSE`.
