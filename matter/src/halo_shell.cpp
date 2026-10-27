/* "halo" shell commands: drive the lamp directly, bypassing Matter, to check the
 * radio path (timeslots, ACK) from the USB console.
 */
#include "app_task.h"
#include "ac_link.h"
#include "crash_box.h"
#include "env_sensor.h"
#include "halo_radio_ts.h"

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/shell/shell.h>

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app/server/Server.h>
#include <platform/CHIPDeviceLayer.h>

#include <stdlib.h>
#include <string.h>

static int set_lamp(const struct shell *sh, chip::EndpointId endpoint, size_t argc, char **argv)
{
	if (argc != 2 || (strcmp(argv[1], "on") && strcmp(argv[1], "off"))) {
		shell_print(sh, "usage: halo %s on|off", endpoint == AppTask::kBackEndpointId ? "back" : "front");
		return -EINVAL;
	}
	AppTask::Instance().SetOnOff(endpoint, !strcmp(argv[1], "on"));
	return 0;
}

static int cmd_front(const struct shell *sh, size_t argc, char **argv)
{
	return set_lamp(sh, AppTask::kFrontEndpointId, argc, argv);
}

static int cmd_back(const struct shell *sh, size_t argc, char **argv)
{
	return set_lamp(sh, AppTask::kBackEndpointId, argc, argv);
}

static int cmd_on(const struct shell *sh, size_t, char **)
{
	AppTask::Instance().SetOnOff(AppTask::kFrontEndpointId, true);
	AppTask::Instance().SetOnOff(AppTask::kBackEndpointId, true);
	shell_print(sh, "both lamps on requested");
	return 0;
}

static int cmd_off(const struct shell *sh, size_t, char **)
{
	AppTask::Instance().SetOnOff(AppTask::kFrontEndpointId, false);
	AppTask::Instance().SetOnOff(AppTask::kBackEndpointId, false);
	shell_print(sh, "both lamps off requested");
	return 0;
}

static int cmd_level(const struct shell *sh, size_t argc, char **argv)
{
	if (argc != 2) {
		shell_print(sh, "usage: halo level <1..254>");
		return -EINVAL;
	}
	AppTask::Instance().SetLevel(AppTask::kFrontEndpointId, static_cast<uint8_t>(atoi(argv[1])));
	return 0;
}

static int cmd_temp(const struct shell *sh, size_t argc, char **argv)
{
	if (argc != 2) {
		shell_print(sh, "usage: halo temp <mireds 154..370>");
		return -EINVAL;
	}
	AppTask::Instance().SetMireds(AppTask::kFrontEndpointId, static_cast<uint16_t>(atoi(argv[1])));
	return 0;
}

static int cmd_last(const struct shell *sh, size_t, char **)
{
	const AppTask::LastFrame &l = AppTask::GetLastFrame();

	if (!l.count) {
		shell_print(sh, "no frame sent yet");
		return 0;
	}
	const halo_ts_result_t &r = l.result;

	shell_print(sh, "#%u %s %02X%02X: %s after %u attempt(s), rssi %d dBm%s", l.count, l.what, l.flags,
		    l.value, r.ack ? "ACK" : "no ACK", r.attempts, r.rssi_dbm,
		    r.energy && !r.ack ? " (reply heard)" : "");
	shell_print(sh, "started %u tx %u rxto %u blocked %u nosig %u err %d txend %u us radiosig %u",
		    r.started, r.tx_done, r.rx_timeout, r.blocked, r.no_signal, r.request_err, r.tx_end_us,
		    r.radio_signals);
	return 0;
}

static int cmd_crash(const struct shell *sh, size_t, char **)
{
	if (crash_box.magic != CRASH_BOX_MAGIC) {
		shell_print(sh, "no crash recorded since the last clear");
		return 0;
	}
	crash_box.mpsl_file[sizeof(crash_box.mpsl_file) - 1] = 0;
	shell_print(sh, "fatal error: reason %u pc 0x%08x lr 0x%08x thread 0x%08x MPSL assert %s:%u",
		    crash_box.reason, crash_box.pc, crash_box.lr, crash_box.thread, crash_box.mpsl_file,
		    crash_box.mpsl_line);
	shell_print(sh, "trace entries %u", crash_box.trace_count);
	for (unsigned i = 0; i < crash_box.trace_count && i < 24; i++) {
		shell_print(sh, "trace 0x%08x", crash_box.trace[i]);
	}
	return 0;
}

static int cmd_env(const struct shell *sh, size_t, char **)
{
	int16_t t;
	uint16_t h;

	uint32_t lux;

	if (!EnvSensor::Latest(t, h)) {
		shell_print(sh, "no AHT20 reading yet (sensor missing or not wired to D4/D5)");
	} else {
		shell_print(sh, "temperature %d.%02d C, humidity %u.%02u %%RH", t / 100, abs(t) % 100, h / 100,
			    h % 100);
	}
	int16_t hPa;

	if (!EnvSensor::LatestPressure(hPa)) {
		shell_print(sh, "no BMP280 reading yet (sensor missing or not wired to D4/D5)");
	} else {
		shell_print(sh, "pressure %d hPa", hPa);
	}
	if (!EnvSensor::LatestLux(lux)) {
		shell_print(sh, "no OPT3001 reading yet (sensor missing or not wired to D2/D3)");
	} else {
		shell_print(sh, "illuminance %u.%02u lux", lux / 100, lux % 100);
	}
	return 0;
}

/* "halo i2c": scan both buses (bus 0 = D2/D3, bus 1 = D4/D5) for responding addresses. */
static int cmd_i2c(const struct shell *sh, size_t, char **)
{
	static const struct device *const buses[] = {DEVICE_DT_GET(DT_NODELABEL(i2c0)), DEVICE_DT_GET(DT_NODELABEL(i2c1))};
	static const char *const pins[] = {"D2/D3", "D4/D5"};

	for (int b = 0; b < 2; b++) {
		int found = 0;

		if (!device_is_ready(buses[b])) {
			shell_print(sh, "i2c%d (%s): not ready", b, pins[b]);
			continue;
		}
		shell_fprintf(sh, SHELL_NORMAL, "i2c%d (%s):", b, pins[b]);
		for (uint16_t a = 0x08; a < 0x78; a++) {
			if (i2c_write(buses[b], NULL, 0, a) == 0) {
				shell_fprintf(sh, SHELL_NORMAL, " 0x%02X", a);
				found++;
			}
		}
		shell_print(sh, "%s", found ? "" : " no devices");
	}
	return 0;
}

/* "halo matter": what the Matter data model currently holds for the sensor endpoints. */
static int cmd_matter(const struct shell *sh, size_t, char **)
{
	using namespace chip::app::Clusters;
	chip::app::DataModel::Nullable<int16_t> t, p;
	chip::app::DataModel::Nullable<uint16_t> h, l;
	chip::Protocols::InteractionModel::Status st[4];

	chip::DeviceLayer::PlatformMgr().LockChipStack();
	st[0] = TemperatureMeasurement::Attributes::MeasuredValue::Get(EnvSensor::kTemperatureEndpointId, t);
	st[1] = RelativeHumidityMeasurement::Attributes::MeasuredValue::Get(EnvSensor::kHumidityEndpointId, h);
	st[2] = IlluminanceMeasurement::Attributes::MeasuredValue::Get(EnvSensor::kIlluminanceEndpointId, l);
	st[3] = PressureMeasurement::Attributes::MeasuredValue::Get(EnvSensor::kPressureEndpointId, p);
	chip::DeviceLayer::PlatformMgr().UnlockChipStack();

	shell_print(sh, "ep3 temperature %d (0.01 C)  status %d", t.ValueOr(0), static_cast<int>(st[0]));
	shell_print(sh, "ep4 humidity    %u (0.01 %%)  status %d", h.ValueOr(0), static_cast<int>(st[1]));
	shell_print(sh, "ep5 illuminance %u (10000*log10(lux)+1)  status %d", l.ValueOr(0), static_cast<int>(st[2]));
	shell_print(sh, "ep6 pressure    %d (hPa)  status %d", p.ValueOr(0), static_cast<int>(st[3]));
	return 0;
}

/* "halo ac <text command>": send a command to the AC-IR blaster over BLE, e.g. "halo ac set on 25 auto". */
static int cmd_ac(const struct shell *sh, size_t argc, char **argv)
{
	char line[48] = "";

	if (argc < 2) {
		shell_print(sh, "usage: halo ac set on|off <16..30> <0..4|auto>   (also: power, temp, fan, mode)");
		return -EINVAL;
	}
	for (size_t i = 1; i < argc; i++) {
		strncat(line, argv[i], sizeof(line) - strlen(line) - 1);
		if (i + 1 < argc) {
			strncat(line, " ", sizeof(line) - strlen(line) - 1);
		}
	}
	shell_print(sh, "sending \"%s\" to AC-IR ...", line);
	AcLink::Result r = AcLink::Send(line);

	shell_print(sh, "result: %s (detail %d)", AcLink::ResultName(r), AcLink::LastError());
	return 0;
}

static int cmd_factoryreset(const struct shell *sh, size_t, char **)
{
	shell_print(sh, "factory reset: forgetting the Matter fabric and Thread network, rebooting");
	chip::DeviceLayer::PlatformMgr().ScheduleWork(
		[](intptr_t) { chip::Server::GetInstance().ScheduleFactoryReset(); }, 0);
	return 0;
}

static int cmd_dfu(const struct shell *sh, size_t, char **)
{
	shell_print(sh, "rebooting into the UF2 bootloader");
	k_msleep(50);
	halo_ts_reboot_to_bootloader();
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(halo_cmds, SHELL_CMD(on, NULL, "Both lamps on", cmd_on),
			       SHELL_CMD(front, NULL, "Front lamp on|off", cmd_front),
			       SHELL_CMD(back, NULL, "Back lamp on|off", cmd_back),
			       SHELL_CMD(last, NULL, "Result of the last frame sent", cmd_last),
			       SHELL_CMD(crash, NULL, "Crash black box from the previous boot", cmd_crash),
			       SHELL_CMD(i2c, NULL, "Scan the two I2C buses", cmd_i2c),
			       SHELL_CMD(ac, NULL, "Send a text command to the AC-IR blaster over BLE", cmd_ac),
			       SHELL_CMD(matter, NULL, "Sensor attribute values in the Matter data model", cmd_matter),
			       SHELL_CMD(env, NULL, "Latest AHT20, BMP280 and OPT3001 readings", cmd_env),
			       SHELL_CMD(factoryreset, NULL, "Matter factory reset (unpair) and reboot", cmd_factoryreset),
			       SHELL_CMD(dfu, NULL, "Reboot into the UF2 bootloader", cmd_dfu),
			       SHELL_CMD(off, NULL, "Both lamps off", cmd_off),
			       SHELL_CMD(level, NULL, "Brightness 1..254", cmd_level),
			       SHELL_CMD(temp, NULL, "Colour temperature in mireds", cmd_temp),
			       SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(halo, &halo_cmds, "ScreenBar Halo direct control", NULL);
