/* Matter Thermostat (endpoint 7, cooling only) and Fan (endpoint 8) -> AC-IR over BLE.
 * Attribute writes from a controller update a shadow of the A/C state; after a short pause the
 * whole state is sent once as "set <on|off> <temp> <fan>".
 */
#pragma once

#include <app/ConcreteAttributePath.h>

namespace AcControl {
constexpr chip::EndpointId kThermostatEndpointId = 7;
constexpr chip::EndpointId kFanEndpointId = 8;

/* Called from MatterPostAttributeChangeCallback (Matter thread); returns immediately. */
void OnAttributeChanged(const chip::app::ConcreteAttributePath &path, uint16_t size, uint8_t *value);

/* Start the worker; call once at boot. */
void Start();
}
