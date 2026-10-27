#include "env_sensor.h"

#include "app/task_executor.h"

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app-common/zap-generated/ids/Clusters.h>

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>

#include <math.h>

using namespace ::chip;
using namespace ::chip::app::Clusters;

namespace {
constexpr uint32_t kSampleIntervalMs = 10000;

const struct device *const sSensor = DEVICE_DT_GET_ONE(aosong_aht20);
const struct device *const sLight = DEVICE_DT_GET_ONE(ti_opt3001);
const struct device *const sPressure = DEVICE_DT_GET(DT_NODELABEL(bmp280));
k_work_delayable sWork;
volatile int16_t sLastTemp;
volatile uint16_t sLastHum;
volatile bool sHaveReading;
volatile uint32_t sLastCentiLux;
volatile bool sHaveLux;
volatile int16_t sLastPressure;
volatile bool sHavePressure;

void SampleWork(k_work *)
{
	struct sensor_value temp, hum;

	if (device_is_ready(sSensor) && sensor_sample_fetch(sSensor) == 0 &&
	    sensor_channel_get(sSensor, SENSOR_CHAN_AMBIENT_TEMP, &temp) == 0 &&
	    sensor_channel_get(sSensor, SENSOR_CHAN_HUMIDITY, &hum) == 0) {
		/* Matter units: 0.01 degC and 0.01 %RH. */
		int16_t centiDegC = static_cast<int16_t>(temp.val1 * 100 + temp.val2 / 10000);
		uint16_t centiPercent = static_cast<uint16_t>(hum.val1 * 100 + hum.val2 / 10000);

		sLastTemp = centiDegC;
		sLastHum = centiPercent;
		sHaveReading = true;

		Nrf::PostTask([centiDegC, centiPercent] {
			TemperatureMeasurement::Attributes::MeasuredValue::Set(EnvSensor::kTemperatureEndpointId,
									       centiDegC);
			RelativeHumidityMeasurement::Attributes::MeasuredValue::Set(
				EnvSensor::kHumidityEndpointId, centiPercent);
			Thermostat::Attributes::LocalTemperature::Set(7, centiDegC);   /* AcControl::kThermostatEndpointId */
		});
	}

	struct sensor_value lux;

	if (device_is_ready(sLight) && sensor_sample_fetch(sLight) == 0 &&
	    sensor_channel_get(sLight, SENSOR_CHAN_LIGHT, &lux) == 0) {
		uint32_t centiLux = static_cast<uint32_t>(lux.val1) * 100 + lux.val2 / 10000;
		/* Matter Illuminance: 10000 * log10(lux) + 1, where 0 means "too dark to measure". */
		uint16_t measured = 0;

		if (centiLux >= 100) {
			double v = 10000.0 * log10(centiLux / 100.0) + 1.0;

			measured = static_cast<uint16_t>(v > 65534.0 ? 65534.0 : v);
		}
		sLastCentiLux = centiLux;
		sHaveLux = true;

		Nrf::PostTask([measured] {
			IlluminanceMeasurement::Attributes::MeasuredValue::Set(EnvSensor::kIlluminanceEndpointId,
									       measured);
		});
	}

	struct sensor_value press;

	if (device_is_ready(sPressure) && sensor_sample_fetch(sPressure) == 0 &&
	    sensor_channel_get(sPressure, SENSOR_CHAN_PRESS, &press) == 0) {
		/* Zephyr reports kPa; Matter Pressure Measurement is in 0.1 kPa (= 1 hPa). */
		int16_t hPa = static_cast<int16_t>(press.val1 * 10 + press.val2 / 100000);

		sLastPressure = hPa;
		sHavePressure = true;

		Nrf::PostTask([hPa] {
			PressureMeasurement::Attributes::MeasuredValue::Set(EnvSensor::kPressureEndpointId, hPa);
		});
	}
	k_work_reschedule(&sWork, K_MSEC(kSampleIntervalMs));
}
} /* namespace */

bool EnvSensor::Latest(int16_t &centiDegC, uint16_t &centiPercent)
{
	centiDegC = sLastTemp;
	centiPercent = sLastHum;
	return sHaveReading;
}

bool EnvSensor::LatestLux(uint32_t &centiLux)
{
	centiLux = sLastCentiLux;
	return sHaveLux;
}

bool EnvSensor::LatestPressure(int16_t &hPa)
{
	hPa = sLastPressure;
	return sHavePressure;
}

void EnvSensor::Start()
{
	k_work_init_delayable(&sWork, SampleWork);
	k_work_reschedule(&sWork, K_SECONDS(2));
}
