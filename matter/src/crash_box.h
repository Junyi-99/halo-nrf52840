/* Crash black box kept in RAM across a reboot. */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct crash_record {
	uint32_t magic;
	uint32_t reason;
	uint32_t pc;
	uint32_t lr;
	uint32_t thread;
	char mpsl_file[12];
	uint32_t mpsl_line;
};

extern struct crash_record crash_box;
#define CRASH_BOX_MAGIC 0xC5A5A0A0u

#ifdef __cplusplus
}
#endif
