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
	uint32_t trace[24];	/* code addresses found on the faulting thread's stack */
	uint32_t trace_count;
};

extern struct crash_record crash_box;
#define CRASH_BOX_MAGIC 0xC5A5A0A0u

#ifdef __cplusplus
}
#endif
