/* AHT20 on the XIAO's D4/D5 I2C pins, reported as Matter Temperature (endpoint 3)
 * and Relative Humidity (endpoint 4) measurements; OPT3001 on D2/D3 as Illuminance
 * (endpoint 5); BMP280 on D4/D5 as Pressure (endpoint 6).
 */
#pragma once

#include <lib/core/DataModelTypes.h>

namespace EnvSensor {
constexpr chip::EndpointId kTemperatureEndpointId = 3;
constexpr chip::EndpointId kHumidityEndpointId = 4;
constexpr chip::EndpointId kIlluminanceEndpointId = 5;
constexpr chip::EndpointId kPressureEndpointId = 6;

/* Latest reading in 0.01 degC / 0.01 %RH; false until the first successful sample. */
bool Latest(int16_t &centiDegC, uint16_t &centiPercent);

/* Latest OPT3001 reading in 0.01 lux; false until the first successful sample. */
bool LatestLux(uint32_t &centiLux);

/* Latest BMP280 reading in 0.1 kPa (1 hPa); false until the first successful sample. */
bool LatestPressure(int16_t &hPa);

/* Start periodic sampling; call once the Matter stack is initialised. */
void Start();
}
