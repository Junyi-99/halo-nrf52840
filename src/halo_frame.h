/* BenQ ScreenBar Halo (1st gen) air frame: bit layout and CRC16.
 * Plain C, no SoC dependencies. Same layout as halo-esp32s3/main/halo_synth.h.
 *
 * Command (81 bits, MSB first): 8-bit preamble 0x55, 32-bit address, 6-bit
 * length (=2), 2-bit PID, NO_ACK (=0), 2 payload bytes, CRC16 (poly 0x1021,
 * init 0xFFFF) over everything after the preamble.
 *
 * ACK (57 bits after the lamp's own preamble): 32-bit address, 6-bit length 0,
 * the same PID, NO_ACK = 1, CRC16 over those 41 bits.
 */
#pragma once
#include <stdint.h>

#define HALO_PACKET_BITS   81u
#define HALO_ACK_BITS      57u
#define HALO_ACK_BODY_BITS 41u

static inline uint16_t halo_crc16(const uint8_t *bits, unsigned count)
{
	uint16_t crc = 0xffff;

	for (unsigned j = 0; j < count; j++) {
		unsigned mix = (crc >> 15) ^ bits[j];

		crc = (uint16_t)(crc << 1);
		if (mix) {
			crc ^= 0x1021;
		}
	}
	return crc;
}

static inline void halo_append_bits(uint8_t *bits, unsigned *n, uint32_t value, unsigned width)
{
	for (unsigned i = width; i; i--) {
		bits[(*n)++] = (value >> (i - 1)) & 1;
	}
}

/* Write the 81-bit command MSB first into bits[]; returns its CRC16. */
static inline uint16_t halo_packet(uint32_t address, uint8_t flags, uint8_t value, unsigned pid,
				   uint8_t bits[HALO_PACKET_BITS])
{
	unsigned n = 0;

	halo_append_bits(bits, &n, 0x55, 8);
	halo_append_bits(bits, &n, address, 32);
	halo_append_bits(bits, &n, 2, 6);	/* payload length */
	halo_append_bits(bits, &n, pid, 2);
	halo_append_bits(bits, &n, 0, 1);	/* NO_ACK = 0: we want an ACK */
	halo_append_bits(bits, &n, flags, 8);
	halo_append_bits(bits, &n, value, 8);
	uint16_t crc = halo_crc16(bits + 8, n - 8);

	halo_append_bits(bits, &n, crc, 16);
	return crc;
}

/* Build the 57-bit pattern an ACK for (address, pid) must contain; returns its CRC16. */
static inline uint16_t halo_ack_pattern(uint32_t address, unsigned pid, uint8_t bits[HALO_ACK_BITS])
{
	unsigned n = 0;

	halo_append_bits(bits, &n, address, 32);
	halo_append_bits(bits, &n, 0, 6);	/* payload length 0 */
	halo_append_bits(bits, &n, pid, 2);
	halo_append_bits(bits, &n, 1, 1);	/* NO_ACK */
	uint16_t crc = halo_crc16(bits, HALO_ACK_BODY_BITS);

	halo_append_bits(bits, &n, crc, 16);
	return crc;
}
