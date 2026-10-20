#include "app_task.h"
#include "crash_box.h"

#include <zephyr/arch/cpu.h>
#include <cmsis_core.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/fatal.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

LOG_MODULE_REGISTER(app, CONFIG_CHIP_APP_LOG_LEVEL);

/* A fatal error at high interrupt priority cannot reach the USB console, so
 * its reason and faulting address are kept in RAM across the reboot and printed
 * on the next boot (see crash_box.c for the MPSL assert hook).
 */
extern "C" void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	crash_box.magic = CRASH_BOX_MAGIC;
	crash_box.reason = reason;
	crash_box.pc = esf ? esf->basic.pc : 0;
	crash_box.lr = esf ? esf->basic.lr : 0;
	crash_box.thread = (uint32_t)k_current_get();
	/* Poor man's backtrace: scan the stack above the exception frame for
	 * anything that looks like a Thumb code address in flash.
	 */
	crash_box.trace_count = 0;
	{
		/* The esf we get is a copy on the interrupt stack; walk the thread's
		 * own stack from its current PSP to the end of its stack region.
		 */
		struct k_thread *t = k_current_get();
		uint32_t sp = __get_PSP();
		uint32_t end = t->stack_info.start + t->stack_info.size;

		if (sp < t->stack_info.start || sp >= end) {
			sp = t->stack_info.start;
		}
		for (uint32_t a = sp & ~3u; a + 4 <= end && crash_box.trace_count < 24; a += 4) {
			uint32_t w = *reinterpret_cast<const uint32_t *>(a);

			if (w >= 0x27000u && w < 0x100000u && (w & 1u)) {
				crash_box.trace[crash_box.trace_count++] = w;
			}
		}
	}
	sys_reboot(SYS_REBOOT_COLD);
}

int main()
{
	const struct device *console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

	/* The console is USB CDC: give a host a few seconds to open the port. After
	 * a crash wait much longer, so a boot loop leaves time to read "halo crash".
	 */
	int wait_ticks = crash_box.magic == CRASH_BOX_MAGIC ? 600 : 80;

	for (int i = 0; i < wait_ticks; i++) {
		uint32_t dtr = 0;

		if (uart_line_ctrl_get(console, UART_LINE_CTRL_DTR, &dtr) == 0 && dtr) {
			break;
		}
		k_msleep(100);
	}

	/* A recorded crash stays in crash_box for the "halo crash" shell command;
	 * a clean power-up leaves random RAM, which the magic value filters out.
	 * After a crash hold the stack for 30 s so a boot loop can be inspected.
	 */
	if (crash_box.magic != CRASH_BOX_MAGIC) {
		crash_box.mpsl_file[0] = 0;
		crash_box.mpsl_line = 0;
	} else {
		k_msleep(30000);
	}

	CHIP_ERROR err = AppTask::Instance().StartApp();

	LOG_ERR("Exited with code %" CHIP_ERROR_FORMAT, err.Format());
	return err == CHIP_NO_ERROR ? EXIT_SUCCESS : EXIT_FAILURE;
}
