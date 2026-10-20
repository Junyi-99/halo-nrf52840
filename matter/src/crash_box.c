#include "crash_box.h"

#include <string.h>

#include <zephyr/kernel.h>

__noinit struct crash_record crash_box;

/* MPSL assert hook (CONFIG_MPSL_ASSERT_HANDLER): record where, then oops like
 * the default handler so the fatal error handler reboots.
 */
void mpsl_assert_handle(char *file, uint32_t line)
{
	memset(crash_box.mpsl_file, 0, sizeof(crash_box.mpsl_file));
	strncpy(crash_box.mpsl_file, file ? file : "?", sizeof(crash_box.mpsl_file) - 1);
	crash_box.mpsl_line = line;
	k_oops();
}
