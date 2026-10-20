/* Serial front end: same line protocol as halo-esp32s3 so tools/halo_control.py
 * works unchanged. Nothing transmits unless the host sends a HALO command.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/sys/reboot.h>
#include <nrfx.h>

#include "halo_frame.h"
#include "radio.h"

#define CMD_LINE_MAX 128

#if defined(CONFIG_BOARD_XIAO_BLE) || defined(CONFIG_BOARD_NICE_NANO) || \
	defined(CONFIG_BOARD_ADAFRUIT_FEATHER_NRF52840) || defined(CONFIG_BOARD_PROMICRO_NRF52840)
#define DFU_MAGIC 0x57		/* Adafruit nRF52 bootloader: DFU_MAGIC_UF2_RESET */
#else
#define DFU_MAGIC 0xb1		/* Nordic open bootloader: BOOTLOADER_DFU_START */
#endif

/* Payload byte 0 (flags) of a control packet. */
#define FLAG_POWER      0x80
#define FLAG_FRONT      0x40
#define FLAG_BACK       0x01
#define FLAG_BRIGHTNESS 0x04	/* selector: this packet carries a brightness value */

/* Payload byte 1: raw brightness code. 1..100 % maps linearly onto 0x4C..0xFE. */
#define LEVEL_MIN 0x4c
#define LEVEL_MAX 0xfe

#if DT_NODE_HAS_STATUS(DT_ALIAS(led0), okay)
static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
#define HAVE_LED 1
#else
#define HAVE_LED 0
#endif

static const struct device *console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

static const char HELP_TEXT[] =
	"BenQ ScreenBar Halo (1st generation) controller - nRF52840\n"
	"Firmware: halo-nrf52840 v0.2 (experimental)\n"
	"Radio: 2405 MHz, 125 kbit/s GFSK synthesized as 8x oversampled 1 Mbit/s symbols.\n"
	"Commands (newline terminated); press Enter to show this help again.\n"
	"HELP | help | ?       Print this help; no RF transmission.\n"
	"HALO?                Query protocol capabilities.\n"
	"HALO ADDR MODE LEVEL PID GAIN ATTEMPTS\n"
	"  ADDR: 8 hexadecimal digits, on-air address byte order.\n"
	"  MODE: 0=off, 1=front, 2=back, 3=both.\n"
	"  LEVEL: 0..100; 0=off, 1..100 maps to raw 4C..FE.\n"
	"  PID: 0..3; advance modulo 4 for each new command.\n"
	"  GAIN: 0..10 -> -40 -20 -16 -12 -8 -4 0 +2 +4 +6 +8 dBm.\n"
	"  ATTEMPTS: 1..5 maximum tries. Stop on matching CRC-valid ACK.\n"
	"  Example: HALO 16224CB1 2 50 0 8 3\n"
	"  Reply: HALODONE status attempts payload crc16 synth_us total_us turnaround_us ack_address_us ack_crc\n"
	"DIAG                 Details of the last attempt (RSSI, raw ACK bytes).\n"
	"PHY NRF1M|BLE1M      Modulator: Nrf_1Mbit (+-170 kHz, default) or Ble_1Mbit (+-250 kHz).\n"
	"DFU                  Reboot into the USB bootloader (UF2 drive or Nordic DFU).\n"
	"LED R G B            Board LED on if any value is non-zero.\n"
	"LED?                 Query LED type.\n"
	"SYNC nonce           Echo synchronization marker.\n"
	"RELEASE              Acknowledge host cleanup.\n"
	"Each HALO command is bounded. No autonomous loop, no BLE.\n"
	"READY\n";

static void reply(const char *text)
{
	printk("%s", text);
}

static void replyf(const char *format, ...)
{
	char line[256];
	va_list args;

	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	reply(line);
}

static void handle_led(const char *line)
{
	unsigned r, g, b;
	char extra;

	if (sscanf(line, "LED %u %u %u %c", &r, &g, &b, &extra) != 3 || r > 255 || g > 255 ||
	    b > 255) {
		reply("ERR led_args\n");
		return;
	}
#if HAVE_LED
	gpio_pin_set_dt(&led0, (r | g | b) != 0);
	replyf("OK LED %u %u %u\n", r, g, b);
#else
	reply("ERR led_init\n");
#endif
}

static void handle_halo(const char *args)
{
	unsigned addr, mode, level, pid, gain, max_attempts;
	char extra;
	int fields = sscanf(args, "%x %u %u %u %u %u %c", &addr, &mode, &level, &pid, &gain,
			    &max_attempts, &extra);

	if (fields != 6 || mode > 3 || level > 100 || pid > 3 || gain > 10 || max_attempts < 1 ||
	    max_attempts > 5) {
		reply("ERR halo_args\n");
		return;
	}

	/* Mode 0 keeps the front-lamp bits of an ordinary command but with POWER clear. */
	uint8_t lamps = mode == 2 ? FLAG_BACK : mode == 3 ? (FLAG_FRONT | FLAG_BACK) : FLAG_FRONT;
	bool on = mode != 0 && level != 0;
	uint8_t flags = lamps | FLAG_BRIGHTNESS | (on ? FLAG_POWER : 0);
	uint8_t value = level ? (uint8_t)(LEVEL_MIN + ((level - 1) * (LEVEL_MAX - LEVEL_MIN) + 49) / 99)
			      : LEVEL_MIN;

	int64_t start = k_uptime_ticks();
	uint16_t crc;
	halo_result_t r = halo_radio_send(addr, flags, value, pid, gain, max_attempts, &crc);
	uint64_t total_us = k_ticks_to_us_floor64(k_uptime_ticks() - start);
	const char *status = r.ack ? "ACK" : r.rx_error ? "RX_ERROR" : "NO_ACK";

	/* synth_us is 0: the frame is a byte table, there is no IQ synthesis here. */
	replyf("HALODONE %s %u %02X%02X %04X 0 %llu %u %u %04X\n", status, r.attempts,
	       (unsigned)flags, (unsigned)value, (unsigned)crc, (unsigned long long)total_us,
	       r.turnaround_us, r.ack_address_us, (unsigned)r.ack_crc);
}

static void handle_diag(void)
{
	const halo_diag_t *d = halo_radio_diag();

	replyf("DIAG phy=%s tx=%ddBm tx_us=%u rx_ready=%u rx_end=%u rx_match=%u address_us=%u bytes=%u\n",
	       d->phy == HALO_PHY_BLE1M ? "BLE1M" : "NRF1M", d->tx_dbm, d->tx_us, d->rx_ready,
	       d->rx_end, d->rx_match, d->rx_address_us, d->rx_bytes);
	replyf("DIAG rssi_max=%ddBm at %uus rssi_ack=%ddBm mismatches=%u spurious=%u offset=%d phase=%u verified=%u\n",
	       d->rssi_max_dbm, d->rssi_max_us, d->rssi_ack_dbm, d->mismatches, d->spurious, d->best_offset,
	       d->best_phase, d->verified_bits);
	reply("DIAG rssi16us=");
	for (unsigned i = 0; i < HALO_RSSI_TRACE; i++) {
		replyf("%d ", d->rssi_trace[i]);
	}
	reply("\n");
	reply("DIAG raw=");
	for (unsigned i = 0; i < d->rx_bytes && i < HALO_RX_BYTES; i++) {
		replyf("%02X", d->raw[i]);
	}
	reply("\n");
}

static void handle(const char *line)
{
	if (!strcmp(line, "HELP") || !strcmp(line, "help") || !strcmp(line, "?")) {
		reply(HELP_TEXT);
	} else if (!strncmp(line, "SYNC ", 5)) {
		reply(line);
		reply("\n");
	} else if (!strcmp(line, "RELEASE")) {
		reply("OK\n");
	} else if (!strcmp(line, "LED?")) {
		reply(HAVE_LED ? "LED board led0 GPIO\n" : "LED none\n");
	} else if (!strncmp(line, "LED ", 4)) {
		handle_led(line);
	} else if (!strcmp(line, "HALO?")) {
		reply("HALO 3 ACK MAX_ATTEMPTS\n");
	} else if (!strncmp(line, "HALO ", 5)) {
		handle_halo(line + 5);
	} else if (!strcmp(line, "DFU")) {
		/* Reboot into the board's USB bootloader through GPREGRET: Adafruit UF2
		 * bootloader (XIAO, nice!nano, Feather) wants 0x57 and then shows a drive;
		 * Nordic's open bootloader wants BOOTLOADER_DFU_START = 0xB1.
		 */
		reply("OK DFU rebooting\n");
		k_msleep(50);
		NRF_POWER->GPREGRET = DFU_MAGIC;
		sys_reboot(SYS_REBOOT_COLD);
	} else if (!strcmp(line, "DIAG")) {
		handle_diag();
	} else if (!strcmp(line, "PHY NRF1M")) {
		halo_radio_set_phy(HALO_PHY_NRF1M);
		reply("OK PHY NRF1M\n");
	} else if (!strcmp(line, "PHY BLE1M")) {
		halo_radio_set_phy(HALO_PHY_BLE1M);
		reply("OK PHY BLE1M\n");
	} else if (!strcmp(line, "PHY?")) {
		replyf("PHY %s\n", halo_radio_get_phy() == HALO_PHY_BLE1M ? "BLE1M" : "NRF1M");
	} else {
		reply("ERR command - type HELP or press Enter for instructions\n");
	}
}

int main(void)
{
#if HAVE_LED
	if (gpio_is_ready_dt(&led0)) {
		gpio_pin_configure_dt(&led0, GPIO_OUTPUT_INACTIVE);
	}
#endif
	if (usb_enable(NULL) != 0) {
		return 0;
	}
	halo_radio_init();

	char line[CMD_LINE_MAX];
	unsigned used = 0;
	bool overflow = false;

	for (;;) {
		unsigned char c;

		if (uart_poll_in(console, &c) != 0) {
			k_msleep(1);
			continue;
		}
		if (c == '\r') {
			continue;
		}
		if (c != '\n') {
			if (used + 1 < sizeof(line)) {
				line[used++] = (char)c;
			} else {
				overflow = true;
			}
			continue;
		}
		if (overflow) {
			reply("ERR line_too_long\n");
		} else if (used) {
			line[used] = 0;
			handle(line);
		} else {
			reply(HELP_TEXT);
		}
		used = 0;
		overflow = false;
	}
	return 0;
}
