/* "halo" shell commands: drive the lamp directly, bypassing Matter, to check the
 * radio path (timeslots, ACK) from the USB console.
 */
#include "app_task.h"
#include "halo_radio_ts.h"

#include <zephyr/shell/shell.h>

#include <stdlib.h>

static int cmd_on(const struct shell *sh, size_t, char **)
{
	AppTask::Instance().SetOnOff(true);
	shell_print(sh, "on requested");
	return 0;
}

static int cmd_off(const struct shell *sh, size_t, char **)
{
	AppTask::Instance().SetOnOff(false);
	shell_print(sh, "off requested");
	return 0;
}

static int cmd_level(const struct shell *sh, size_t argc, char **argv)
{
	if (argc != 2) {
		shell_print(sh, "usage: halo level <1..254>");
		return -EINVAL;
	}
	AppTask::Instance().SetLevel(static_cast<uint8_t>(atoi(argv[1])));
	return 0;
}

static int cmd_temp(const struct shell *sh, size_t argc, char **argv)
{
	if (argc != 2) {
		shell_print(sh, "usage: halo temp <mireds 154..370>");
		return -EINVAL;
	}
	AppTask::Instance().SetMireds(static_cast<uint16_t>(atoi(argv[1])));
	return 0;
}

static int cmd_dfu(const struct shell *sh, size_t, char **)
{
	shell_print(sh, "rebooting into the UF2 bootloader");
	k_msleep(50);
	halo_ts_reboot_to_bootloader();
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(halo_cmds, SHELL_CMD(on, NULL, "Lamp on", cmd_on),
			       SHELL_CMD(dfu, NULL, "Reboot into the UF2 bootloader", cmd_dfu),
			       SHELL_CMD(off, NULL, "Lamp off", cmd_off),
			       SHELL_CMD(level, NULL, "Brightness 1..254", cmd_level),
			       SHELL_CMD(temp, NULL, "Colour temperature in mireds", cmd_temp),
			       SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(halo, &halo_cmds, "ScreenBar Halo direct control", NULL);
