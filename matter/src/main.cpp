#include "app_task.h"
#include "crash_box.h"

#include <zephyr/arch/cpu.h>
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

	if (crash_box.magic == CRASH_BOX_MAGIC) {
		crash_box.mpsl_file[sizeof(crash_box.mpsl_file) - 1] = 0;
		LOG_ERR("Previous boot ended in a fatal error: reason %u pc 0x%08x lr 0x%08x thread 0x%08x MPSL assert %s:%u",
			crash_box.reason, crash_box.pc, crash_box.lr, crash_box.thread, crash_box.mpsl_file,
			crash_box.mpsl_line);
		crash_box.magic = 0;
		crash_box.mpsl_file[0] = 0;
		crash_box.mpsl_line = 0;
	}

	CHIP_ERROR err = AppTask::Instance().StartApp();

	LOG_ERR("Exited with code %" CHIP_ERROR_FORMAT, err.Format());
	return err == CHIP_NO_ERROR ? EXIT_SUCCESS : EXIT_FAILURE;
}
