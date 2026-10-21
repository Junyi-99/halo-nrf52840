# halo-nrf52840 details

How the nRF52840 fakes the ScreenBar Halo's 125 kbit/s protocol, what was measured, and how the
Matter over Thread firmware shares the radio. Companion to the [README](../README.md).

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

## USB firmware: host tool and serial protocol

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
DFU             reboot into the board's USB bootloader
```

`gain` maps to TX power: 0..10 → -40 -20 -16 -12 -8 -4 0 +2 +4 +6 +8 dBm. The tool's default 8 is
+4 dBm; the measurements above were at 10 (+8 dBm) with the lamp a few metres away (ACK at -74 dBm).

The Matter firmware has a USB shell of its own: `halo on|off`, `halo front|back on|off`,
`halo level N`, `halo temp MIREDS`, `halo last` (result and timeslot counters of the last frame),
`halo crash` (crash black box), `halo factoryreset` (unpair), `halo dfu` (reboot into the bootloader).

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
  154..370 mireds), regenerated with ZAP. Endpoint 1 is the front lamp, endpoint 2 the back lamp;
  each has its own ZAP endpoint type on purpose: with one shared type ZAP sizes the per-cluster
  state tables by type, the second endpoint then has no Level Control state and the stack aborts
  during initialisation.
- `matter/src/app_task.cpp`: Matter attribute changes → lamp state (two on/off flags, shared
  brightness and colour temperature mirrored to the other endpoint) → sync thread → frames whose
  lamp bits follow which endpoints are on.
- `matter/src/halo_radio_ts.c`: the timeslot radio driver.
- Size-trimmed build: no logging, no Matter/OpenThread shells, LTO; 636 kB of the XIAO's 788 kB
  partition. A minimal Zephyr shell stays on USB CDC: `halo on|off|level N|temp MIREDS` drive the lamp
  directly, `halo last` shows the last frame's result and timeslot counters, `halo crash` the crash
  black box (fatal reason, pc, a stack scan, MPSL assert location), `halo front|back on|off` switch
  one lamp, `halo factoryreset` unpairs (forgets the Matter fabric and Thread network), `halo dfu`
  reboots into the board's USB bootloader. Status LED on the XIAO: blue blinking slowly = waiting to
  be paired, blue fast = commissioning in progress, green for 5 s = joined; after each command a
  green flash means the lamp ACKed, red means it did not. `prj_debug.conf` (`-DFILE_SUFFIX=debug`)
  is the same firmware with logging for diagnosis. Pairing uses the Matter test setup
  (passcode 20202021, discriminator 0xF00: manual code 34970112332; uncertified test vendor 0xFFF1,
  which Apple Home accepts after a warning).
- Boards: `xiao_ble` (UF2, 32 kB storage split 28 kB settings + 4 kB factory-data node) and
  `nrf52840dongle/nrf52840` for a dongle carrying Nordic's open bootloader (`matter/boards/*.overlay` has the
  Makerdiary MDK USB Dongle's LED/button pins); updates through `halo dfu` then UF2 copy or
  `nrfutil device program --traits nordicDfu`.
