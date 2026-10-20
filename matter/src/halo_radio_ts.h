/* ScreenBar Halo transmitter running inside MPSL timeslots, so it can share the
 * nRF52840 radio with Thread and Bluetooth LE.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Payload byte 0 flags, same as the USB firmware. */
#define HALO_FLAG_POWER       0x80
#define HALO_FLAG_FRONT       0x40
#define HALO_FLAG_BACK        0x01
#define HALO_FLAG_BRIGHTNESS  0x04
#define HALO_FLAG_TEMPERATURE 0x02

typedef struct {
	unsigned attempts;
	bool ack;		/* verified ACK */
	bool energy;		/* a packet was captured in the ACK window */
	int rssi_dbm;
	/* diagnostics, accumulated over the attempts */
	unsigned started;	/* timeslots that actually started */
	unsigned tx_done;	/* transmissions that completed */
	unsigned rx_timeout;	/* receive windows that expired */
	unsigned blocked;	/* requests blocked or cancelled by MPSL */
	unsigned no_signal;	/* requests that never produced any signal */
	int request_err;	/* last mpsl_timeslot_request() error */
	unsigned radio_signals;	/* RADIO signals the callback received (expected 0 next to 802.15.4) */
	uint32_t tx_end_us;	/* last TX end time within the slot */
} halo_ts_result_t;

/* Reboot into the board's USB bootloader (Adafruit UF2 or Nordic DFU). */
void halo_ts_reboot_to_bootloader(void);

/* Open the MPSL timeslot session. Call once from a thread. */
int halo_ts_init(void);

/* Send one frame with up to `attempts` tries 100 ms apart, stopping at a verified
 * ACK. Blocks the calling thread for up to attempts * ~110 ms. Thread context only.
 */
halo_ts_result_t halo_ts_send(uint32_t address, uint8_t flags, uint8_t value, unsigned pid,
			      unsigned gain, unsigned attempts);

#ifdef __cplusplus
}
#endif
