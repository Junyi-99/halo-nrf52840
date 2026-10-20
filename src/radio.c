/* nRF52840 RADIO driver for the ScreenBar Halo air interface.
 *
 * The lamp (Holtek BC5602) talks 125 kbit/s GFSK with +-160 kHz deviation on
 * 2405 MHz. The nRF52840 RADIO has no 125 kbit/s (nor 250 kbit/s) proprietary
 * mode, so the frame is sent through the 1 Mbit/s modulator with every air
 * symbol repeated 8 times: one 0x00 or 0xFF payload byte per 125 kbit/s bit.
 * Nrf_1Mbit uses +-170 kHz deviation, close enough to the lamp's 160 kHz.
 * Whitening and the hardware CRC are off; the frame CRC is in the payload.
 *
 * On-air stream (1 Mbit/s bytes):
 *   hw preamble (1 byte, alternating)      ignored by the lamp
 *   hw address  FF 00 FF 00 FF             symbols 1 0 1 0 1: extends the preamble
 *   payload     81 bytes of 0x00/0xFF      the 81-bit Halo frame
 *
 * The hardware address is sent base first (register LSByte first), prefix last;
 * this is the same mapping Bluetooth LE uses (BASE0 = AA << 8, PREFIX0 = AA >> 24).
 *
 * ACK: after END the radio flips to RX with a 3-symbol hardware address (see
 * rx_pattern below) that fires on the lamp's ramp-up carrier, preamble or ACK
 * body, then captures HALO_RX_BYTES of 0x00/0xFF bytes. decode_ack() votes them
 * into symbols at all 8 phases and aligns them against the expected preamble +
 * 57-bit ACK. RSSI is sampled until the address matches so an unmatched reply
 * still shows up as energy in DIAG.
 */
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <nrfx.h>

#include "halo_frame.h"
#include "radio.h"

#define HALO_FREQUENCY      5u		/* 2400 + 5 = 2405 MHz */
#define SYMBOL_US           8u		/* one 125 kbit/s bit = 8 us = 8 bits at 1 Mbit/s */
#define RX_HW_SYMBOLS       3u		/* hardware address: 3 oversampled symbols */
#define RSSI_TRACE_STEP_US  16u
#define RX_WINDOW_US        1600u	/* lamp answers ~184 us after our packet, ACK lasts ~520 us */
#define TX_TIMEOUT_US       3000u
#define RETRY_DELAY_MS      100
#define HFXO_TIMEOUT_US     100000u

#define SYM(bit) ((uint8_t)((bit) ? 0xffu : 0x00u))

static halo_phy_t current_phy = HALO_PHY_NRF1M;
static uint8_t tx_buf[HALO_PACKET_BITS];
static uint8_t rx_buf[HALO_RX_BYTES];
static halo_diag_t diag;

/* GAIN 0..10 -> dBm. Index 8 (the ESP tool's default) is +4 dBm. */
static const int gain_dbm[11] = {-40, -20, -16, -12, -8, -4, 0, 2, 4, 6, 8};
static const uint32_t gain_reg[11] = {
	RADIO_TXPOWER_TXPOWER_Neg40dBm, RADIO_TXPOWER_TXPOWER_Neg20dBm,
	RADIO_TXPOWER_TXPOWER_Neg16dBm, RADIO_TXPOWER_TXPOWER_Neg12dBm,
	RADIO_TXPOWER_TXPOWER_Neg8dBm,  RADIO_TXPOWER_TXPOWER_Neg4dBm,
	RADIO_TXPOWER_TXPOWER_0dBm,     RADIO_TXPOWER_TXPOWER_Pos2dBm,
	RADIO_TXPOWER_TXPOWER_Pos4dBm,  RADIO_TXPOWER_TXPOWER_Pos6dBm,
	RADIO_TXPOWER_TXPOWER_Pos8dBm,
};

/* TIMER1 as a free-running 1 MHz microsecond clock. */
static void timer_init(void)
{
	NRF_TIMER1->TASKS_STOP = 1;
	NRF_TIMER1->MODE = TIMER_MODE_MODE_Timer;
	NRF_TIMER1->BITMODE = TIMER_BITMODE_BITMODE_32Bit;
	NRF_TIMER1->PRESCALER = 4;	/* 16 MHz / 2^4 */
	NRF_TIMER1->TASKS_CLEAR = 1;
	NRF_TIMER1->TASKS_START = 1;
}

static inline uint32_t now_us(void)
{
	NRF_TIMER1->TASKS_CAPTURE[0] = 1;
	return NRF_TIMER1->CC[0];
}

static bool hfxo_running(void)
{
	return (NRF_CLOCK->HFCLKSTAT & (CLOCK_HFCLKSTAT_SRC_Msk | CLOCK_HFCLKSTAT_STATE_Msk)) ==
	       ((CLOCK_HFCLKSTAT_SRC_Xtal << CLOCK_HFCLKSTAT_SRC_Pos) |
		(CLOCK_HFCLKSTAT_STATE_Running << CLOCK_HFCLKSTAT_STATE_Pos));
}

void halo_radio_init(void)
{
	/* The RADIO needs the 32 MHz crystal, not the RC oscillator. Keep it on. */
	static const struct device *clock = DEVICE_DT_GET_ONE(nordic_nrf_clock);

	(void)clock_control_on(clock, CLOCK_CONTROL_NRF_SUBSYS_HF);
	timer_init();
	memset(&diag, 0, sizeof(diag));
}

void halo_radio_set_phy(halo_phy_t phy)
{
	current_phy = phy;
}

halo_phy_t halo_radio_get_phy(void)
{
	return current_phy;
}

static int halo_radio_gain_dbm(unsigned gain)
{
	return gain_dbm[gain > 10 ? 10 : gain];
}

static void radio_reset_common(unsigned gain)
{
	NRF_RADIO->POWER = 0;
	NRF_RADIO->POWER = 1;
	NRF_RADIO->MODE = (current_phy == HALO_PHY_BLE1M ? RADIO_MODE_MODE_Ble_1Mbit
							 : RADIO_MODE_MODE_Nrf_1Mbit)
			  << RADIO_MODE_MODE_Pos;
	/* Fast ramp-up (40 us instead of 140 us: the ACK arrives ~184 us after our
	 * packet), DTX = center (unmodulated carrier outside the packet).
	 */
	NRF_RADIO->MODECNF0 = (RADIO_MODECNF0_RU_Fast << RADIO_MODECNF0_RU_Pos) |
			      (RADIO_MODECNF0_DTX_Center << RADIO_MODECNF0_DTX_Pos);
	NRF_RADIO->TXPOWER = gain_reg[gain > 10 ? 10 : gain] << RADIO_TXPOWER_TXPOWER_Pos;
	NRF_RADIO->FREQUENCY = HALO_FREQUENCY;
	/* No S0 / LENGTH / S1 fields, 8-bit preamble. */
	NRF_RADIO->PCNF0 = RADIO_PCNF0_PLEN_8bit << RADIO_PCNF0_PLEN_Pos;
	NRF_RADIO->CRCCNF = RADIO_CRCCNF_LEN_Disabled << RADIO_CRCCNF_LEN_Pos;
	NRF_RADIO->TIFS = 0;
	NRF_RADIO->SHORTS = 0;
	NRF_RADIO->INTENCLR = 0xffffffffu;
}

static uint32_t pcnf1(unsigned payload_bytes, unsigned balen)
{
	return ((uint32_t)payload_bytes << RADIO_PCNF1_MAXLEN_Pos) |
	       ((uint32_t)payload_bytes << RADIO_PCNF1_STATLEN_Pos) |
	       ((uint32_t)balen << RADIO_PCNF1_BALEN_Pos) |
	       (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos) |
	       (RADIO_PCNF1_WHITEEN_Disabled << RADIO_PCNF1_WHITEEN_Pos);
}

static void clear_events(void)
{
	NRF_RADIO->EVENTS_READY = 0;
	NRF_RADIO->EVENTS_ADDRESS = 0;
	NRF_RADIO->EVENTS_PAYLOAD = 0;
	NRF_RADIO->EVENTS_END = 0;
	NRF_RADIO->EVENTS_DISABLED = 0;
	NRF_RADIO->EVENTS_RSSIEND = 0;
}

static void radio_disable_sync(void)
{
	NRF_RADIO->SHORTS = 0;
	NRF_RADIO->EVENTS_DISABLED = 0;
	NRF_RADIO->TASKS_DISABLE = 1;
	uint32_t t0 = now_us();

	while (!NRF_RADIO->EVENTS_DISABLED && now_us() - t0 < 1000u) {
	}
}

/* Transmit tx_buf. Returns the END timestamp, or 0 on timeout. */
static uint32_t transmit(unsigned gain)
{
	radio_reset_common(gain);
	NRF_RADIO->PCNF1 = pcnf1(HALO_PACKET_BITS, 4);
	NRF_RADIO->BASE0 = 0x00ff00ffu;		/* on air: FF 00 FF 00 */
	NRF_RADIO->PREFIX0 = 0xffu;		/* then FF */
	NRF_RADIO->TXADDRESS = 0;
	NRF_RADIO->PACKETPTR = (uint32_t)tx_buf;
	NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk;
	clear_events();

	uint32_t t0 = now_us();

	NRF_RADIO->TASKS_TXEN = 1;
	uint32_t t_end = 0;

	while (now_us() - t0 < TX_TIMEOUT_US) {
		if (NRF_RADIO->EVENTS_END) {
			t_end = now_us();
			break;
		}
	}
	diag.tx_us = t_end ? t_end - t0 : 0;
	if (!t_end) {
		radio_disable_sync();
		return 0;
	}
	while (!NRF_RADIO->EVENTS_DISABLED && now_us() - t_end < 1000u) {
	}
	return t_end;
}

/* Hardware address patterns. Captures show the 1 Mbit/s demodulator decodes the
 * lamp's 0 symbols as solid runs of zeros but shortens and sometimes corrupts
 * the 1 symbols (its decision threshold sits off-centre: carrier offset). So
 * logical address 0 is three 0 symbols (24 zeros), which the lamp's unmodulated
 * ramp-up carrier, the preamble/address boundary or any 0 0 0 in the body
 * satisfies; logical address 1 (1 0 0) is a fallback that needs a two-symbol
 * run of ones in front of it. The capture is long enough to hold the whole ACK
 * from wherever the correlator fired; decode_ack() aligns it.
 */
static const uint8_t rx_pattern[2][RX_HW_SYMBOLS] = {{0, 0, 0}, {1, 0, 0}};

/* Decode the capture. Bit i of byte k was on air at 8k+i. For each of the 8
 * symbol phases the middle 6 bits of every 8-bit window are voted into a symbol.
 * Captures show 0 symbols come out as clean zeros while 1 symbols shrink to as
 * little as one bit (the demodulator's threshold sits off-centre), so any one
 * in the window makes it a 1. The result is aligned against the expected
 * sequence (8-bit preamble 0x55 + 57-bit ACK body) at every shift that keeps
 * the end of the body, i.e. the CRC, inside the capture. Accepted when the
 * overlap is exact, or when the whole body is present, every expected 0 was
 * received as 0 and at most MAX_SQUASHED expected ones came back as 0 (the
 * only error the demodulator makes). All ~35 zero positions plus the
 * remaining ones must still match, so a different ACK or noise cannot pass.
 */
#define MIN_VERIFY_BITS 24u
#define ACK_SEQ_BITS    (8u + HALO_ACK_BITS)
#define MAX_SQUASHED    6u

static bool decode_ack(const uint8_t *expected, unsigned nbytes)
{
	static uint8_t seq[ACK_SEQ_BITS];
	static uint8_t symbols[8][HALO_RX_BYTES];
	unsigned nsym = 0;

	for (unsigned i = 0; i < 8; i++) {
		seq[i] = i & 1;		/* 0 1 0 1 0 1 0 1 */
	}
	memcpy(seq + 8, expected, HALO_ACK_BITS);

	for (unsigned phase = 0; phase < 8; phase++) {
		unsigned n = 0;

		for (unsigned k = 0; (k + 1) * 8 + phase <= nbytes * 8; k++) {
			unsigned ones = 0;

			for (unsigned i = 1; i < 7; i++) {
				unsigned bit = k * 8 + phase + i;

				ones += (rx_buf[bit / 8] >> (bit % 8)) & 1;
			}
			symbols[phase][n++] = ones >= 1;
		}
		nsym = n;
	}

	diag.mismatches = HALO_ACK_BITS;
	diag.spurious = HALO_ACK_BITS;
	diag.best_offset = 0;
	diag.best_phase = 0;
	diag.verified_bits = 0;

	/* shift d: expected index i sits at symbol index i + d. */
	for (int d = -(int)(ACK_SEQ_BITS - MIN_VERIFY_BITS); d + (int)ACK_SEQ_BITS <= (int)nsym; d++) {
		unsigned first = d < 0 ? (unsigned)(-d) : 0;
		unsigned overlap = ACK_SEQ_BITS - first;

		if (overlap < MIN_VERIFY_BITS) {
			continue;
		}
		for (unsigned phase = 0; phase < 8; phase++) {
			unsigned errors = 0, spurious = 0;

			for (unsigned i = first; i < ACK_SEQ_BITS && errors <= diag.mismatches; i++) {
				uint8_t got = symbols[phase][(unsigned)((int)i + d)];

				errors += got != seq[i];
				spurious += got && !seq[i];
			}
			if (errors < diag.mismatches ||
			    (errors == diag.mismatches && overlap > diag.verified_bits)) {
				diag.mismatches = errors;
				diag.spurious = spurious;
				diag.best_offset = d;
				diag.best_phase = phase;
				diag.verified_bits = overlap;
			}
		}
	}
	if (diag.verified_bits < MIN_VERIFY_BITS) {
		return false;
	}
	return diag.mismatches == 0 ||
	       (diag.verified_bits >= HALO_ACK_BITS && diag.spurious == 0 &&
		diag.mismatches <= MAX_SQUASHED);
}

/* Listen for the ACK right after t_end. Fills diag and the result fields. */
static void receive_ack(uint32_t t_end, uint32_t address, unsigned pid, unsigned gain,
			halo_result_t *result)
{
	uint8_t expected[HALO_ACK_BITS];
	uint16_t ack_crc = halo_ack_pattern(address, pid, expected);

	diag.rx_bytes = HALO_RX_BYTES;

	radio_reset_common(gain);
	NRF_RADIO->PCNF1 = pcnf1(HALO_RX_BYTES, 2);
	/* BALEN = 2: the base is the two most significant register bytes, sent
	 * LSByte first, then the prefix (same mapping Bluetooth LE uses).
	 */
	NRF_RADIO->BASE0 = ((uint32_t)SYM(rx_pattern[0][0]) << 16) |
			   ((uint32_t)SYM(rx_pattern[0][1]) << 24);
	NRF_RADIO->BASE1 = ((uint32_t)SYM(rx_pattern[1][0]) << 16) |
			   ((uint32_t)SYM(rx_pattern[1][1]) << 24);
	NRF_RADIO->PREFIX0 = (uint32_t)SYM(rx_pattern[0][2]) | ((uint32_t)SYM(rx_pattern[1][2]) << 8);
	NRF_RADIO->RXADDRESSES = 0x3u;
	NRF_RADIO->PACKETPTR = (uint32_t)rx_buf;
	NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk |
			    RADIO_SHORTS_ADDRESS_RSSISTART_Msk;
	memset(rx_buf, 0xa5, sizeof(rx_buf));
	memset(diag.rssi_trace, 0, sizeof(diag.rssi_trace));
	clear_events();
	NRF_RADIO->TASKS_RXEN = 1;

	bool ready = false, addressed = false, ended = false, rssi_busy = false;
	int rssi_max = -128;
	uint32_t rssi_max_us = 0, t_ready = 0, t_addr = 0;
	unsigned trace_next = 0;

	while (now_us() - t_end < RX_WINDOW_US) {
		uint32_t t = now_us() - t_end;

		if (!ready && NRF_RADIO->EVENTS_READY) {
			ready = true;
			t_ready = t;
		}
		if (!addressed && NRF_RADIO->EVENTS_ADDRESS) {
			addressed = true;
			t_addr = t;
		}
		if (NRF_RADIO->EVENTS_END) {
			ended = true;
			break;
		}
		if (ready && !addressed) {
			if (!rssi_busy) {
				NRF_RADIO->EVENTS_RSSIEND = 0;
				NRF_RADIO->TASKS_RSSISTART = 1;
				rssi_busy = true;
			} else if (NRF_RADIO->EVENTS_RSSIEND) {
				int dbm = -(int)(NRF_RADIO->RSSISAMPLE & 0x7f);

				if (dbm > rssi_max) {
					rssi_max = dbm;
					rssi_max_us = t;
				}
				if (trace_next < HALO_RSSI_TRACE && t >= trace_next * RSSI_TRACE_STEP_US) {
					diag.rssi_trace[trace_next++] = (int8_t)dbm;
				}
				rssi_busy = false;
			}
		}
	}

	if (ended) {
		/* ADDRESS_RSSISTART sampled the RSSI during the ACK itself. */
		diag.rssi_ack_dbm = -(int)(NRF_RADIO->RSSISAMPLE & 0x7f);
		diag.rx_match = NRF_RADIO->RXMATCH;
		if (diag.rssi_ack_dbm > rssi_max) {
			rssi_max = diag.rssi_ack_dbm;
			rssi_max_us = t_addr;
		}
	} else {
		diag.rssi_ack_dbm = 0;
		diag.rx_match = 0;
	}
	radio_disable_sync();

	diag.rx_ready = ready;
	diag.rx_end = ended;
	diag.rx_address_us = addressed ? t_addr : 0;
	diag.rssi_max_dbm = rssi_max;
	diag.rssi_max_us = rssi_max_us;
	memcpy(diag.raw, rx_buf, sizeof(diag.raw));
	diag.mismatches = HALO_ACK_BITS;
	diag.spurious = HALO_ACK_BITS;
	diag.best_offset = -1;
	diag.best_phase = 0;
	diag.verified_bits = 0;

	result->rx_error = !ready;
	result->turnaround_us = ready ? t_ready : 0;
	result->ack = false;
	result->ack_address_us = 0;
	result->ack_crc = 0;
	if (!ended) {
		return;
	}

	if (decode_ack(expected, HALO_RX_BYTES)) {
		result->ack = true;
		result->ack_crc = ack_crc;
		/* ADDRESS fires where the capture starts; the ACK body (expected
		 * index 8) sits best_offset + 8 symbols later.
		 */
		result->ack_address_us = (uint32_t)((int)t_addr + (diag.best_offset + 8) * (int)SYMBOL_US);
	}
}

halo_result_t halo_radio_send(uint32_t address, uint8_t flags, uint8_t value, unsigned pid,
			      unsigned gain, unsigned max_attempts, uint16_t *crc)
{
	halo_result_t result = {0};
	uint8_t bits[HALO_PACKET_BITS];

	*crc = halo_packet(address, flags, value, pid, bits);
	for (unsigned i = 0; i < HALO_PACKET_BITS; i++) {
		tx_buf[i] = SYM(bits[i]);
	}

	memset(&diag, 0, sizeof(diag));
	diag.phy = current_phy;
	diag.tx_dbm = halo_radio_gain_dbm(gain);

	/* Make sure the crystal is up before touching the RADIO. */
	uint32_t t0 = now_us();

	while (!hfxo_running() && now_us() - t0 < HFXO_TIMEOUT_US) {
		k_msleep(1);
	}
	if (!hfxo_running()) {
		result.rx_error = true;
		return result;
	}

	for (unsigned attempt = 0; attempt < max_attempts; attempt++) {
		unsigned key = irq_lock();
		uint32_t t_end = transmit(gain);

		result.attempts++;
		if (!t_end) {
			irq_unlock(key);
			result.rx_error = true;
			break;
		}
		receive_ack(t_end, address, pid, gain, &result);
		irq_unlock(key);
		if (result.ack || result.rx_error) {
			break;
		}
		if (attempt + 1 < max_attempts) {
			k_msleep(RETRY_DELAY_MS);
		}
	}
	return result;
}

const halo_diag_t *halo_radio_diag(void)
{
	return &diag;
}
