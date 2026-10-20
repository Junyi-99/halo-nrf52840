#include "app_task.h"

#include <zephyr/arch/cpu.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/fatal.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

LOG_MODULE_REGISTER(app, CONFIG_CHIP_APP_LOG_LEVEL);

/* Crash black box: a fatal error at high interrupt priority cannot reach the USB
 * console, so its reason and faulting address are kept in RAM across the reboot
 * and printed on the next boot.
 */
struct crash_record {
	uint32_t magic;
	uint32_t reason;
	uint32_t pc;
	uint32_t lr;
	uint32_t thread;
};
static __noinit struct crash_record crash;

extern "C" void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	crash.magic = 0xC5A5A0A0u;
	crash.reason = reason;
	crash.pc = esf ? esf->basic.pc : 0;
	crash.lr = esf ? esf->basic.lr : 0;
	crash.thread = (uint32_t)k_current_get();
	sys_reboot(SYS_REBOOT_COLD);
}

int main()
{
	const struct device *console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

	/* The console is USB CDC: give a host a few seconds to open the port. */
	for (int i = 0; i < 80; i++) {
		uint32_t dtr = 0;

		if (uart_line_ctrl_get(console, UART_LINE_CTRL_DTR, &dtr) == 0 && dtr) {
			break;
		}
		k_msleep(100);
	}

	if (crash.magic == 0xC5A5A0A0u) {
		LOG_ERR("Previous boot ended in a fatal error: reason %u pc 0x%08x lr 0x%08x thread 0x%08x",
			crash.reason, crash.pc, crash.lr, crash.thread);
		crash.magic = 0;
	}

	CHIP_ERROR err = AppTask::Instance().StartApp();

	LOG_ERR("Exited with code %" CHIP_ERROR_FORMAT, err.Format());
	return err == CHIP_NO_ERROR ? EXIT_SUCCESS : EXIT_FAILURE;
}
