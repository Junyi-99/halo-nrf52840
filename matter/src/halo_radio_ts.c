/* Halo air interface inside an MPSL timeslot.
 *
 * Same oversampled 1 Mbit/s TX and ACK RX as ../../src/radio.c, with these
 * differences forced by sharing the radio:
 *  - the RADIO is configured register by register (no POWER reset), left
 *    disabled with interrupts off when the timeslot ends;
 *  - MPSL_TIMER0 (running at 1 MHz from the timeslot start) is the clock;
 *  - everything happens in the timeslot callback at interrupt priority 0, so
 *    no kernel calls there: the result is handed back through a volatile flag
 *    the requesting thread polls.
 */
#include <string.h>

#include <zephyr/kernel.h>
#include <mpsl_timeslot.h>
#include <mpsl.h>
#include <nrfx.h>

#include "halo_frame.h"
#include "halo_radio_ts.h"

#define HALO_FREQUENCY   5u		/* 2405 MHz */
#define SYMBOL_US        8u
#define RX_HW_SYMBOLS    3u
#define RX_BYTES         96u
#define RX_WINDOW_US     1250u		/* ACK body ends ~715 us after our END */
#define TX_TIMEOUT_US    1500u
#define TIMESLOT_US      4000u		/* ramp 40 + TX 737 + RX 1250 + margins */
#define RETRY_DELAY_MS   100
#define RSSI_ENERGY_DBM  (-85)
#define MIN_VERIFY_BITS  24u
#define ACK_SEQ_BITS     (8u + HALO_ACK_BITS)
#define MAX_SQUASHED     6u

#define SYM(bit) ((uint8_t)((bit) ? 0xffu : 0x00u))

static const uint32_t gain_reg[11] = {
	RADIO_TXPOWER_TXPOWER_Neg40dBm, RADIO_TXPOWER_TXPOWER_Neg20dBm,
	RADIO_TXPOWER_TXPOWER_Neg16dBm, RADIO_TXPOWER_TXPOWER_Neg12dBm,
	RADIO_TXPOWER_TXPOWER_Neg8dBm,  RADIO_TXPOWER_TXPOWER_Neg4dBm,
	RADIO_TXPOWER_TXPOWER_0dBm,     RADIO_TXPOWER_TXPOWER_Pos2dBm,
	RADIO_TXPOWER_TXPOWER_Pos4dBm,  RADIO_TXPOWER_TXPOWER_Pos6dBm,
	RADIO_TXPOWER_TXPOWER_Pos8dBm,
};
static const uint8_t rx_pattern[2][RX_HW_SYMBOLS] = {{0, 0, 0}, {1, 0, 0}};

static mpsl_timeslot_session_id_t session_id;
static bool session_open;
static mpsl_timeslot_signal_return_param_t ret_param;
static const mpsl_timeslot_request_t request_earliest = {
	.request_type = MPSL_TIMESLOT_REQ_TYPE_EARLIEST,
	.params.earliest.hfclk = MPSL_TIMESLOT_HFCLK_CFG_XTAL_GUARANTEED,
	.params.earliest.priority = MPSL_TIMESLOT_PRIORITY_HIGH,
	.params.earliest.length_us = TIMESLOT_US,
	.params.earliest.timeout_us = 200000,
};

/* Shared between the thread and the timeslot callback. */
static uint8_t tx_buf[HALO_PACKET_BITS];
static uint8_t rx_buf[RX_BYTES];
static uint8_t expected[HALO_ACK_BITS];
static uint32_t slot_gain;
static volatile bool slot_done;
static volatile int slot_rssi;
static volatile bool slot_started, slot_tx_done, slot_timeout, slot_blocked;
static volatile uint32_t slot_tx_end_us;
static volatile unsigned slot_radio_signals;
static uint32_t phase_deadline_us;

#define TICK_US 20u

/* TIMER0 belongs to the application during the timeslot. Run it ourselves as a
 * 1 MHz free-running clock rather than assuming MPSL started it.
 */
static void timer_start(void)
{
	MPSL_TIMER0->TASKS_STOP = 1;
	MPSL_TIMER0->INTENCLR = 0xffffffffu;
	MPSL_TIMER0->SHORTS = 0;
	MPSL_TIMER0->MODE = TIMER_MODE_MODE_Timer;
	MPSL_TIMER0->BITMODE = TIMER_BITMODE_BITMODE_32Bit;
	MPSL_TIMER0->PRESCALER = 4;
	MPSL_TIMER0->TASKS_CLEAR = 1;
	MPSL_TIMER0->TASKS_START = 1;
}

static inline uint32_t now_us(void)
{
	MPSL_TIMER0->TASKS_CAPTURE[2] = 1;
	return MPSL_TIMER0->CC[2];
}



static void radio_configure(void)
{
	NRF_RADIO->SHORTS = 0;
	NRF_RADIO->INTENCLR = 0xffffffffu;
	NRF_RADIO->MODE = RADIO_MODE_MODE_Nrf_1Mbit << RADIO_MODE_MODE_Pos;
	NRF_RADIO->MODECNF0 = (RADIO_MODECNF0_RU_Fast << RADIO_MODECNF0_RU_Pos) |
			      (RADIO_MODECNF0_DTX_Center << RADIO_MODECNF0_DTX_Pos);
	NRF_RADIO->TXPOWER = gain_reg[slot_gain] << RADIO_TXPOWER_TXPOWER_Pos;
	NRF_RADIO->FREQUENCY = HALO_FREQUENCY;
	NRF_RADIO->PCNF0 = RADIO_PCNF0_PLEN_8bit << RADIO_PCNF0_PLEN_Pos;
	NRF_RADIO->CRCCNF = RADIO_CRCCNF_LEN_Disabled << RADIO_CRCCNF_LEN_Pos;
	NRF_RADIO->TIFS = 0;
	NRF_RADIO->DATAWHITEIV = 0x40;
	NRF_RADIO->CCACTRL = 0;
	NRF_RADIO->DACNF = 0;
	NRF_RADIO->SFD = 0xa7;
	NRF_RADIO->EDCNT = 0;
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

/* ---- Timeslot state machine (all at MPSL callback priority) ----------------
 * START: TX. RADIO signal: END of TX -> switch to RX; END of RX -> done.
 * TIMER0 signal: TX or RX window timeout -> done without ACK. The callback never
 * blocks for more than a few microseconds; the ACK is decoded in the thread.
 */
enum slot_state { SLOT_IDLE, SLOT_TX, SLOT_RX, SLOT_DONE };
static volatile enum slot_state slot_state;
static volatile bool slot_rx_ended;

/* The radio is polled from a TIMER0 tick: RADIO interrupts are not reliably
 * delivered to the timeslot callback next to the 802.15.4 driver, TIMER0 ones are.
 */
static void tick_arm(void)
{
	MPSL_TIMER0->EVENTS_COMPARE[0] = 0;
	MPSL_TIMER0->CC[0] = now_us() + TICK_US;
	MPSL_TIMER0->INTENSET = TIMER_INTENSET_COMPARE0_Msk;
}

static void phase_begin(uint32_t duration_us)
{
	phase_deadline_us = now_us() + duration_us;
	tick_arm();
}

static void start_tx(void)
{
	NRF_RADIO->POWER = 0;
	NRF_RADIO->POWER = 1;
	radio_configure();
	NRF_RADIO->PCNF1 = pcnf1(HALO_PACKET_BITS, 4);
	NRF_RADIO->BASE0 = 0x00ff00ffu;
	NRF_RADIO->PREFIX0 = 0xffu;
	NRF_RADIO->TXADDRESS = 0;
	NRF_RADIO->PACKETPTR = (uint32_t)tx_buf;
	NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk;
	clear_events();
	NRF_RADIO->INTENSET = RADIO_INTENSET_DISABLED_Msk;
	NRF_RADIO->TASKS_TXEN = 1;
	slot_state = SLOT_TX;
	phase_begin(TX_TIMEOUT_US);
}

static void start_rx(void)
{
	radio_configure();
	NRF_RADIO->PCNF1 = pcnf1(RX_BYTES, 2);
	NRF_RADIO->BASE0 = ((uint32_t)SYM(rx_pattern[0][0]) << 16) | ((uint32_t)SYM(rx_pattern[0][1]) << 24);
	NRF_RADIO->BASE1 = ((uint32_t)SYM(rx_pattern[1][0]) << 16) | ((uint32_t)SYM(rx_pattern[1][1]) << 24);
	NRF_RADIO->PREFIX0 = (uint32_t)SYM(rx_pattern[0][2]) | ((uint32_t)SYM(rx_pattern[1][2]) << 8);
	NRF_RADIO->RXADDRESSES = 0x3u;
	NRF_RADIO->PACKETPTR = (uint32_t)rx_buf;
	NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk |
			    RADIO_SHORTS_ADDRESS_RSSISTART_Msk;
	memset(rx_buf, 0xa5, sizeof(rx_buf));
	clear_events();
	NRF_RADIO->INTENSET = RADIO_INTENSET_DISABLED_Msk;
	NRF_RADIO->TASKS_RXEN = 1;
	slot_state = SLOT_RX;
	phase_begin(RX_WINDOW_US);
}

static void finish_slot(bool rx_ended)
{
	NRF_RADIO->INTENCLR = 0xffffffffu;
	NRF_RADIO->SHORTS = 0;
	MPSL_TIMER0->INTENCLR = 0xffffffffu;
	if (rx_ended) {
		slot_rssi = -(int)(NRF_RADIO->RSSISAMPLE & 0x7f);
	}
	/* Leave the RADIO disabled for MPSL. END_DISABLE already did it after a
	 * packet; after a timeout force it and wait the few microseconds it takes.
	 */
	NRF_RADIO->EVENTS_DISABLED = 0;
	NRF_RADIO->TASKS_DISABLE = 1;
	for (unsigned n = 0; !NRF_RADIO->EVENTS_DISABLED && n < 2000u; n++) {
	}
	NRF_RADIO->EVENTS_DISABLED = 0;
	MPSL_TIMER0->TASKS_STOP = 1;
	slot_rx_ended = rx_ended;
	slot_state = SLOT_DONE;
	slot_done = true;
}

static bool decode_ack(unsigned nbytes)
{
	static uint8_t seq[ACK_SEQ_BITS];
	static uint8_t symbols[8][RX_BYTES];
	unsigned nsym = 0;

	for (unsigned i = 0; i < 8; i++) {
		seq[i] = i & 1;
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

	unsigned best_errors = HALO_ACK_BITS, best_spurious = HALO_ACK_BITS, best_overlap = 0;

	for (int d = -(int)(ACK_SEQ_BITS - MIN_VERIFY_BITS); d + (int)ACK_SEQ_BITS <= (int)nsym; d++) {
		unsigned first = d < 0 ? (unsigned)(-d) : 0;
		unsigned overlap = ACK_SEQ_BITS - first;

		for (unsigned phase = 0; phase < 8; phase++) {
			unsigned errors = 0, spurious = 0;

			for (unsigned i = first; i < ACK_SEQ_BITS && errors <= best_errors; i++) {
				uint8_t got = symbols[phase][(unsigned)((int)i + d)];

				errors += got != seq[i];
				spurious += got && !seq[i];
			}
			if (errors < best_errors || (errors == best_errors && overlap > best_overlap)) {
				best_errors = errors;
				best_spurious = spurious;
				best_overlap = overlap;
			}
		}
	}
	if (best_overlap < MIN_VERIFY_BITS) {
		return false;
	}
	return best_errors == 0 ||
	       (best_overlap >= HALO_ACK_BITS && best_spurious == 0 && best_errors <= MAX_SQUASHED);
}

static mpsl_timeslot_signal_return_param_t *timeslot_callback(mpsl_timeslot_session_id_t id,
							       uint32_t signal)
{
	(void)id;
	ret_param.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_NONE;

	switch (signal) {
	case MPSL_TIMESLOT_SIGNAL_START:
		timer_start();
		slot_rssi = -128;
		slot_started = true;
		start_tx();
		break;

	case MPSL_TIMESLOT_SIGNAL_RADIO:
		slot_radio_signals++;
		/* fall through: handled by the same poll */
	case MPSL_TIMESLOT_SIGNAL_TIMER0:
		MPSL_TIMER0->EVENTS_COMPARE[0] = 0;
		if (slot_state == SLOT_TX) {
			if (NRF_RADIO->EVENTS_DISABLED) {
				NRF_RADIO->EVENTS_DISABLED = 0;
				slot_tx_done = true;
				slot_tx_end_us = now_us();
				start_rx();
			} else if ((int32_t)(now_us() - phase_deadline_us) >= 0) {
				slot_timeout = true;
				finish_slot(false);
				ret_param.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_END;
			} else {
				tick_arm();
			}
		} else if (slot_state == SLOT_RX) {
			if (NRF_RADIO->EVENTS_DISABLED) {
				NRF_RADIO->EVENTS_DISABLED = 0;
				finish_slot(true);
				ret_param.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_END;
			} else if ((int32_t)(now_us() - phase_deadline_us) >= 0) {
				slot_timeout = true;
				finish_slot(false);
				ret_param.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_END;
			} else {
				tick_arm();
			}
		}
		break;

	case MPSL_TIMESLOT_SIGNAL_CANCELLED:
	case MPSL_TIMESLOT_SIGNAL_BLOCKED:
		slot_blocked = true;
		slot_state = SLOT_DONE;
		slot_done = true;
		break;

	case MPSL_TIMESLOT_SIGNAL_OVERSTAYED:
		/* Should not happen: every phase is bounded by TIMER0. */
		slot_state = SLOT_DONE;
		slot_done = true;
		break;

	default:
		break;
	}
	return &ret_param;
}

int halo_ts_init(void)
{
	if (session_open) {
		return 0;
	}
	int32_t err = mpsl_timeslot_session_open(timeslot_callback, &session_id);

	if (err) {
		return (int)err;
	}
	session_open = true;
	return 0;
}

void halo_ts_reboot_to_bootloader(void)
{
	NRF_POWER->GPREGRET = 0x57;
	NVIC_SystemReset();
}

halo_ts_result_t halo_ts_send(uint32_t address, uint8_t flags, uint8_t value, unsigned pid,
			      unsigned gain, unsigned attempts)
{
	halo_ts_result_t r = {0};
	uint8_t bits[HALO_PACKET_BITS];

	if (!session_open && halo_ts_init()) {
		return r;
	}
	halo_packet(address, flags, value, pid, bits);
	for (unsigned i = 0; i < HALO_PACKET_BITS; i++) {
		tx_buf[i] = SYM(bits[i]);
	}
	halo_ack_pattern(address, pid, expected);
	slot_gain = gain > 10 ? 10 : gain;
	r.rssi_dbm = -128;

	for (unsigned attempt = 0; attempt < attempts; attempt++) {
		slot_done = false;
		slot_rx_ended = false;
		slot_started = slot_tx_done = slot_timeout = slot_blocked = false;
		slot_radio_signals = 0;
		slot_state = SLOT_IDLE;
		int32_t err = mpsl_timeslot_request(session_id, &request_earliest);

		if (err) {
			r.request_err = (int)err;
			k_msleep(RETRY_DELAY_MS);
			continue;
		}
		r.attempts++;
		for (unsigned i = 0; i < 100 && !slot_done; i++) {
			k_msleep(5);
		}
		r.started += slot_started;
		r.tx_done += slot_tx_done;
		r.rx_timeout += slot_timeout && slot_tx_done;
		r.blocked += slot_blocked;
		r.no_signal += !slot_done;
		r.radio_signals += slot_radio_signals;
		if (slot_tx_done) {
			r.tx_end_us = slot_tx_end_us;
		}
		if (slot_rssi > r.rssi_dbm) {
			r.rssi_dbm = slot_rssi;
		}
		if (slot_rx_ended) {
			r.energy = true;
			if (decode_ack(RX_BYTES)) {
				r.ack = true;
				break;
			}
		}
		if (attempt + 1 < attempts) {
			k_msleep(RETRY_DELAY_MS);
		}
	}
	return r;
}
