#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
	HALO_PHY_NRF1M = 0,	/* Nrf_1Mbit: +-170 kHz deviation (closest to the lamp's 160 kHz) */
	HALO_PHY_BLE1M = 1,	/* Ble_1Mbit: +-250 kHz deviation, fallback experiment */
} halo_phy_t;

typedef struct {
	unsigned attempts;	/* transmissions actually made */
	bool ack;		/* exact, CRC-valid ACK received */
	bool rx_error;		/* radio did not reach RX in time */
	uint32_t turnaround_us;	/* end of our packet to RX ready */
	uint32_t ack_address_us;/* ACK address field start, relative to end of our packet */
	uint16_t ack_crc;	/* CRC16 of the accepted ACK */
} halo_result_t;

/* Diagnostics of the last attempt, for the DIAG command. */
#define HALO_RX_BYTES 96u
#define HALO_RSSI_TRACE 48u

typedef struct {
	halo_phy_t phy;
	int tx_dbm;
	uint32_t tx_us;		/* TXEN to END */
	bool rx_ready;
	bool rx_end;		/* hardware address matched and payload captured */
	unsigned rx_match;	/* RXMATCH logical address when rx_end */
	uint32_t rx_address_us;	/* ADDRESS event, relative to end of our packet */
	unsigned rx_bytes;	/* payload bytes captured */
	unsigned best_phase;	/* symbol phase (0..7) of the best decode */
	unsigned verified_bits;	/* ACK bits compared at the best alignment */
	int8_t rssi_trace[HALO_RSSI_TRACE];	/* dBm every 16 us from RX ready */
	int rssi_max_dbm;	/* strongest RSSI sample seen in the listen window */
	uint32_t rssi_max_us;	/* when it was seen, relative to end of our packet */
	int rssi_ack_dbm;	/* RSSI sampled at the address match (rx_end only) */
	unsigned mismatches;	/* bit errors against the expected ACK at the best offset */
	unsigned spurious;	/* of those, expected 0 received as 1 */
	int best_offset;	/* capture symbol index of the expected preamble start */
	uint8_t raw[HALO_RX_BYTES];
} halo_diag_t;

void halo_radio_init(void);
void halo_radio_set_phy(halo_phy_t phy);
halo_phy_t halo_radio_get_phy(void);
int halo_radio_gain_dbm(unsigned gain);

/* Send one command with up to max_attempts tries, 100 ms apart, stopping at the
 * first verified ACK. Returns the result and fills *crc with the command CRC16.
 */
halo_result_t halo_radio_send(uint32_t address, uint8_t flags, uint8_t value, unsigned pid,
			      unsigned gain, unsigned max_attempts, uint16_t *crc);

const halo_diag_t *halo_radio_diag(void);
