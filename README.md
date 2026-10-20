# halo-nrf52840

Experimental: drive a first-generation BenQ ScreenBar Halo from an nRF52840's own radio.
Companion to [halo-esp32s3](https://github.com/Junyi-99/halo-esp32s3); same serial protocol, same host tool.

**Status: working.** Tested 2026-10-21 against a ScreenBar Halo on an nRF52840 dongle and on a
Seeed XIAO nRF52840: the lamp answers every command (50/50 ACK bursts seen) and the firmware decodes
and CRC-verifies the ACK on 65..87% of single attempts; with the tool's default 3 attempts that is
effectively always. A wrong address gets nothing but noise floor. Brightness, lamp selection and
colour temperature were all confirmed by eye.

## The problem

The lamp (Holtek BC5602) listens on 2405 MHz for **125 kbit/s GFSK, ±160 kHz deviation**, Enhanced
ShockBurst framing. The nRF52840 RADIO offers Nrf_1Mbit, Nrf_2Mbit, Ble_1Mbit, Ble_2Mbit, the coded
BLE PHYs and 802.15.4. The 250 kbit/s proprietary mode that older nRF52s had was removed, and there
is no 125 kbit/s plain GFSK at all. The register map simply has no way to ask for it.

## The trick

Send the frame through the 1 Mbit/s modulator with every air bit repeated 8 times: one payload byte
of `0x00` or `0xFF` per 125 kbit/s bit. The lamp sees 8 µs symbols, which is what it expects.

- `Nrf_1Mbit` deviates ±170 kHz, close enough to the lamp's ±160 kHz. (`Ble_1Mbit` is ±250 kHz and
  is available as a fallback through `PHY BLE1M`.)
- Edges are sharper than a BT = 0.5 Gaussian at 125 kbit/s would make them (1 µs instead of ~8 µs);
  the lamp's ~445 kHz channel filter rounds them off. Slightly wider spectrum, no decoding problem.
- Whitening and the hardware CRC are off, S0/LENGTH/S1 are zero length, static payload length 81.
  The Halo CRC16 is computed in software and lives inside the payload like any other bit.
- The hardware preamble (1 byte, alternating at 1 Mbit/s) and the 5-byte hardware address
  `FF 00 FF 00 FF` (symbols `1 0 1 0 1`) come out before the payload and just extend the lamp's
  `0x55` preamble.

On air, after the nRF ramp-up:

```
hw preamble   hw address          payload (81 bytes, 1 symbol each)
AA            FF 00 FF 00 FF      55-preamble, address, PCF, flags, value, CRC16
8 us          40 us               648 us
```

### ACK reception

The lamp answers with an empty ACK at the same 125 kbit/s: its carrier shows up ~150 µs after our
packet ends and the ACK body starts at ~258 µs (jitter ±3 µs). The firmware flips to RX with fast
ramp-up (ready after ~88 µs) and receives the ACK the same way backwards, at 1 Mbit/s.

What the 1 Mbit/s demodulator makes of 8 µs symbols, from raw captures: 0 symbols come out as clean
runs of zeros; 1 symbols lose a bit at each end and sometimes shrink to a single one, or vanish (the
decision threshold sits off-centre, presumably a carrier offset the receiver cannot estimate without
a proper 1 Mbit/s preamble). The receiver design follows from that:

- The hardware address (BALEN = 2, three oversampled symbols) is `0 0 0`: 24 zeros, which the
  lamp's ramp-up carrier, the preamble/address boundary or any `0 0 0` in the body satisfies. It
  typically fires 160..200 µs after our packet. `1 0 0` is enabled as logical address 1 as a
  fallback. Patterns needing clean ones (`1 1 0`) matched only half the time, and `0 0 1` fired
  spuriously on the unmodulated carrier after the ACK.
- 96 bytes (768 µs) are captured so the whole ACK is inside wherever the match fired.
- Decoding: for each of the 8 symbol phases, the middle 6 bits of every 8-bit window make one
  symbol, and any one in the window makes it a 1. The symbols are aligned against the expected
  sequence (preamble 0x55 + 57-bit body) at every shift that keeps the CRC inside the capture.
- Accepted as `ACK` when the overlap (≥ 24 bits) matches exactly, or when the whole body is present,
  every expected 0 was received as 0, and at most 6 expected ones were squashed (the only error the
  demodulator makes). A different ACK or noise cannot pass: all ~35 zero positions and the rest of
  the ones still have to match.

RSSI is sampled every 16 µs through the listen window (`DIAG rssi16us=`), so a reply that was not
decoded still shows as a burst of energy ~20 dB above the noise floor.

## Build

Zephyr v4.4.2 is installed under `~/zephyrproject` (narrow checkout: zephyr, cmsis, hal_nordic)
with the Zephyr SDK 1.0.1 ARM toolchain in `~/zephyr-sdk-1.0.1` (this machine is aarch64).

```sh
./build.sh xiao_ble                         # Seeed XIAO nRF52840 (UF2, app at 0x27000)
./build.sh promicro_nrf52840/nrf52840/uf2   # Pro Micro / SuperMini / nice!nano clones (UF2, app at 0x26000)
./build.sh nrf52840dongle/nrf52840          # Nordic PCA10059 dongle (Nordic DFU)
./build.sh nrf52840dk/nrf52840              # Nordic DK (J-Link)
./build.sh adafruit_feather_nrf52840        # Feather (UF2 variant)
```

Outputs land in `build/<board>/zephyr/` as `zephyr.uf2` / `zephyr.hex`.

## Flash

### The dongle used here (no reset button, was running OpenSK)

Done once over J-Link (SWDIO, SWCLK, GND, VTref): full erase, then
`bootloader/open_bootloader_gpregret_mbr_pca10059.hex`, which is Nordic's PCA10059 open USB
bootloader from nRF5 SDK 17.1.0 rebuilt with `NRF_BL_DFU_ENTER_METHOD_GPREGRET 1` (the prebuilt one
only enters DFU from the reset pin) plus the MBR, UICR bootloader address 0xE0000:

```sh
nrfutil device program --firmware bootloader/open_bootloader_gpregret_mbr_pca10059.hex --traits jlink --options chip_erase_mode=ERASE_ALL
nrfutil device reset --traits jlink
```

From then on everything is over USB. The firmware's `DFU` command writes GPREGRET = 0xB1 and
reboots into the bootloader (`Open DFU Bootloader`, 1915:521f); `tools/dfu_update.sh` does the
whole update: `DFU` on the serial port, wait for the bootloader, `nrfutil device program --firmware
build/halo_dongle.zip --traits nordicDfu`, wait for the app to come back. The zip comes from
`tools/make_dfu_zip.py` (Nordic's `nrf5sdk-tools` is not available for aarch64 Linux; the script
writes the same unsigned init packet `pkg generate --hw-version 52 --sd-req 0x00` would).
`nrfutil` here is Nordic's binary in `~/.local/bin` with the `device` command installed;
`/etc/udev/rules.d/99-halo-nrf52840.rules` makes both USB identities world-accessible.

### XIAO nRF52840 (Adafruit UF2 bootloader)

Nothing to prepare: double-tap RST, copy `build/xiao_ble/zephyr/zephyr.uf2` onto the `XIAO-SENSE`
drive. Afterwards the firmware's `DFU` command reboots into the drive (GPREGRET = 0x57) and
`tools/uf2_update.sh` does DFU + mount + copy in one go. `bootloader/xiao_nrf52840_ble_bootloader-*.hex`
is the official Adafruit bootloader in case a board has lost it (flash over SWD, then UF2 as usual).

### Pro Micro / SuperMini nRF52840 (nice!nano clones, UF2)

Same as the XIAO with the `promicro_nrf52840/nrf52840/uf2` build. One check first: the app address
depends on the SoftDevice the bootloader carries. Open the drive's `INFO_UF2.TXT`: `S140 6.1.1`
(nice!nano layout) wants the Pro Micro build (0x26000); `S140 7.3.0` wants the XIAO build (0x27000,
only the LED pin differs). Flashing the wrong one overwrites part of the SoftDevice and the board
just drops back into the bootloader; re-copy the right UF2.

### Other boards

- **Feather (UF2):** same as the XIAO.
- **Stock dongle with reset button:** reset into DFU, then the `nordicDfu` program step above.
- **DK:** `west flash -d build/nrf52840dk_nrf52840` with J-Link, or `-r pyocd`.
- **Any board over SWD, no bootloader:** `nrf52840dongle/nrf52840/bare` (code at 0x0).

The board enumerates as a USB CDC ACM port (product string `halo-nrf52840`).

## Use

Same protocol as halo-esp32s3, so its tool works unchanged (a copy is in `tools/`):

```sh
pip install pyserial
python tools/halo_control.py --address 16224CB1 --mode back --brightness 50
python tools/halo_control.py --mode front --brightness 100 --led on          # LED in the mode colour, green 1 s on ACK
python tools/halo_control.py --mode front --brightness 100 --rgb 0,0,255     # same with a custom colour
```

`LED R G B` drives the three dies of the XIAO's RGB LED / the dongle's LD2 with PWM (1 kHz,
gamma 2, so 1 is just visible and 255 is full); the Pro Micro's single LED follows the brightest
channel. The PWM pin maps live in `boards/*.overlay`; boards without one fall back to GPIO on/off.

Serial, newline terminated, type `HELP` for the full list:

```text
HALO <address_hex> <mode 0-3> <brightness 0-100> <pid 0-3> <gain 0-10> <attempts 1-5>
HALOTEMP <address_hex> <mode 0-3> <temperature 0-100> <pid 0-3> <gain 0-10> <attempts 1-5>
  -> HALODONE <ACK|NO_ACK|RX_ERROR> <attempts> <payload_hex> <crc16> 0 <total_us> <turnaround_us> <ack_us> <ack_crc16>
DIAG            last attempt: RSSI max/when, RSSI during the ACK, bit mismatches, raw bytes
PHY NRF1M|BLE1M modulator (default NRF1M)
```

`gain` maps to TX power: 0..10 → -40 -20 -16 -12 -8 -4 0 +2 +4 +6 +8 dBm. The tool's default 8 is
+4 dBm; the measurements above were at 10 (+8 dBm) with the lamp a few metres away (ACK at -74 dBm).

## Measured (2026-10-21, dongle, lamp address 16224CB1)

| | |
|---|---|
| TX packet, ramp-up to END | 737 µs |
| RX ready after END (fast ramp-up) | 87..89 µs |
| Lamp carrier appears / ACK body starts | ~150 µs / 255..261 µs after END |
| ACK bursts seen, real address | 30 / 30 at -73..-76 dBm |
| ACK decoded and CRC-verified, single attempt | 26 / 30 |
| Wrong address | no energy above -91 dBm, no match |
| `halo_control.py` both → off → front, 3 attempts | ACK, ACK (2nd attempt), ACK |
| XIAO nRF52840, same test | 20/20 bursts at -63 dBm, 13/20 verified, tool ACK first try |

## If it does not ACK

1. `DIAG` first. `rssi_max` around -75 dBm at 150..250 µs means the lamp answered and only
   decoding failed; -91 dBm means nothing came back: wrong address, lamp unplugged, or out of range.
2. Count the PID: the lamp drops a frame whose PID and CRC equal the previous one. The host tool
   advances it for you; manual tests must change `pid` between commands.
3. `PHY BLE1M` (±250 kHz) exists for experiments; NRF1M is the one that is known to work.

## Layout

- `src/halo_frame.h`: frame bits and CRC16, identical output to halo-esp32s3's `halo_synth.h`
  (checked bit for bit against it).
- `src/radio.c`: RADIO setup, oversampled TX, oversampled ACK RX, RSSI diagnostics. TIMER1 is used
  as a free-running microsecond clock.
- `src/main.c`: serial parser and replies, compatible with the ESP32 firmware, plus `HALOTEMP`,
  `DIAG`, `PHY`, `DFU`, PWM/GPIO LED.
- `app.overlay`: CDC ACM console; `boards/*.overlay`: per-board PWM LED pins (each includes
  `app.overlay`, since Zephyr uses only the board overlay when one exists).
- `tools/halo_control.py`: host CLI, verbatim from halo-esp32s3.
- `tools/make_dfu_zip.py`, `tools/dfu_update.sh`: Nordic USB DFU packaging and one-shot update.
- `tools/uf2_update.sh`: one-shot update for UF2 boards.
- `bootloader/`: the rebuilt PCA10059 open bootloader (+MBR) with its `sdk_config.h`, and the
  official Adafruit UF2 bootloader for the XIAO nRF52840.

Uses the legacy Zephyr USB device stack (deprecated in 4.4, still functional) because it needs one
call to bring up a CDC ACM console; the warnings in the build come from there.

## Acknowledgements

- ScreenBar Halo protocol: [Djoko-cli/benq-screenbar-halo-matter](https://github.com/Djoko-cli/benq-screenbar-halo-matter)
- Frame layout and host tool: [halo-esp32s3](https://github.com/Junyi-99/halo-esp32s3)

## License

GPL-3.0, see `LICENSE`.
