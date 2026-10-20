# halo-nrf52840

Control a first-generation BenQ ScreenBar Halo from an nRF52840's own radio, and put it in
**Apple Home** (or any Matter controller) as a Matter over Thread light. Companion to
[halo-esp32s3](https://github.com/Junyi-99/halo-esp32s3).

Two firmwares, one radio trick:

| | `matter/` — Apple Home firmware | root — USB firmware |
|---|---|---|
| What it is | Matter over Thread **Color Temperature Light**: on/off, brightness, colour temperature from the Home app, Siri, automations | USB CDC serial controller, same line protocol as halo-esp32s3, `halo_control.py` on the host |
| Needs | XIAO nRF52840 + a Thread border router (HomePod mini, Apple TV 4K, …) | any nRF52840 board on USB |
| SDK | nRF Connect SDK v3.4.1 | Zephyr v4.4.2 |
| Pairing | fixed test code **3497-011-2332**, QR below | n/a |

<p align="center"><img src="docs/matter-pairing-qr.png" width="180" alt="Matter pairing QR code"><br><code>MT:6FCJ142C00KA0648G00</code> · manual code 34970112332</p>

**Status (2026-10-21):** both firmwares verified on the lamp. The Matter firmware is commissioned in
Apple Home through a HomePod mini; on/off, brightness and colour temperature all reach the lamp with
verified ACKs, including bursts of slider moves. Apple shows an "uncertified accessory" warning once
(test vendor ID), then it behaves like any other light. The lamp only ever sends empty ACKs, so
changes made with the original remote are not reflected back.

## Apple Home quick start

1. Build (`matter/build.sh xiao_ble`, needs NCS v3.4.1 under `~/ncs`) or take a release UF2, double-tap
   RST on the XIAO and copy `zephyr.uf2` onto the `XIAO-SENSE` drive.
2. Home app → + → Add Accessory → scan the QR above (or *More options…* → enter 3497-011-2332) →
   *Add Anyway* on the uncertified warning. Commissioning goes over Bluetooth LE, then the XIAO joins
   the border router's Thread network; about a minute.
3. The lamp address is `CONFIG_HALO_LAMP_ADDRESS` in `matter/Kconfig` (default 16224CB1, change it
   for your own lamp).

Every board flashed with this firmware uses the same pairing code (Matter test discriminator 0xF00,
passcode 20202021), so pair one board at a time. Already-paired boards do not need the code again:
sharing with another app goes through the Home app's own pairing mode. Over USB the board also prints
the codes (`matter onboardingcodes ble`) and takes `halo on|off|level|temp` for direct tests and
`halo dfu` to reboot into the UF2 bootloader.

## The radio trick (both firmwares)

### The problem

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

Copy the UF2 onto the board's bootloader drive (double-tap RST on the XIAO), or program the hex over SWD with a J-Link; later updates go through the firmware's `DFU` / `halo dfu` command.

## Use

Same protocol as halo-esp32s3, so its tool works unchanged (a copy is in `tools/`):

```sh
pip install pyserial
python tools/halo_control.py --address 16224CB1 --mode back --brightness 50
python tools/halo_control.py --mode front --brightness 100 --led on          # LED in the mode colour, green 1 s on ACK
python tools/halo_control.py --mode front --brightness 100 --rgb 0,0,255     # same with a custom colour
```

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

## Matter over Thread (`matter/`)

The XIAO can also be a Matter **Color Temperature Light** on Thread, so the ScreenBar shows up in
Apple Home (or any Matter controller) with on/off, brightness and colour temperature, no other
hardware involved. This needs the nRF Connect SDK (v3.4.1 under `~/ncs`, see `matter/build.sh`),
because Matter and the MPSL radio scheduler only exist there.

How the lamp gets its frames while Thread and Bluetooth LE own the radio: each command requests a
4 ms **MPSL timeslot**; in the timeslot the firmware starts the oversampled TX, then polls the
radio from a 20 µs TIMER0 tick (RADIO interrupts are not delivered to the timeslot callback next to
the 802.15.4 driver), switches to RX for the ACK, and hands the capture back to a thread for
decoding. TIMER0 is MPSL's during the slot (it runs it at 1 MHz and times the slot end with it):
the firmware only sets a compare channel on it. Stopping or clearing it trips an MPSL assert, which
is what the first version did under a stream of slider commands. A crash black box in retained RAM
records fatal errors (and, through `CONFIG_MPSL_ASSERT_HANDLER`, MPSL's assert file and line) and
prints them on the next boot. Measured on the XIAO with Thread (router) and BLE advertising active: every frame
reaches the lamp, ACK verified on about two out of three single attempts, three attempts per
command. Commissioned into Apple Home through a HomePod mini on 2026-10-21; on/off, brightness and
colour temperature from the Home app all reached the lamp with verified ACKs.

- `matter/src/default_zap/`: data model (light bulb sample + Color Control, temperature feature,
  154..370 mireds), regenerated with ZAP.
- `matter/src/app_task.cpp`: Matter attribute changes → lamp state → sync thread → frames.
- `matter/src/halo_radio_ts.c`: the timeslot radio driver.
- Size-trimmed build: no logging, no Matter/OpenThread shells, LTO; 636 kB of the XIAO's 788 kB
  partition. A minimal Zephyr shell stays on USB CDC: `halo on|off|level N|temp MIREDS` drive the lamp
  directly, `halo last` shows the last frame's result and timeslot counters, `halo crash` the crash
  black box, `halo dfu` reboots into the board's USB bootloader. Pairing uses the Matter test setup
  (passcode 20202021, discriminator 0xF00: manual code 34970112332; uncertified test vendor 0xFFF1,
  which Apple Home accepts after a warning).
- Boards: `xiao_ble` (UF2, 32 kB storage split 28 kB settings + 4 kB factory-data node) and
  `nrf52840dongle/nrf52840` for a dongle carrying Nordic's open bootloader (`boards/*.overlay` has the
  Makerdiary MDK USB Dongle's LED/button pins); updates through `halo dfu` then UF2 copy or
  `nrfutil device program --traits nordicDfu`.

## Acknowledgements

- ScreenBar Halo protocol: [Djoko-cli/benq-screenbar-halo-matter](https://github.com/Djoko-cli/benq-screenbar-halo-matter)
- Frame layout and host tool: [halo-esp32s3](https://github.com/Junyi-99/halo-esp32s3)

## License

GPL-3.0, see `LICENSE`.
