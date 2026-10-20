/* halo-matter application: Matter colour-temperature light endpoint whose state
 * is pushed to a BenQ ScreenBar Halo over the private 2.4 GHz protocol.
 */
#pragma once

#include "halo_radio_ts.h"

#include <platform/CHIPDeviceLayer.h>

class AppTask {
public:
	static AppTask &Instance()
	{
		static AppTask sAppTask;
		return sAppTask;
	};

	CHIP_ERROR StartApp();

	/* Endpoint 1 is the front lamp, endpoint 2 the back lamp. Brightness and colour
	 * temperature are shared by the lamp, so a change on one endpoint is mirrored
	 * to the other.
	 */
	static constexpr chip::EndpointId kFrontEndpointId = 1;
	static constexpr chip::EndpointId kBackEndpointId = 2;

	/* Called from Matter attribute callbacks (Matter thread). */
	void SetOnOff(chip::EndpointId endpoint, bool on);
	void SetLevel(chip::EndpointId endpoint, uint8_t level);		/* Level Control 1..254 */
	void SetMireds(chip::EndpointId endpoint, uint16_t mireds);	/* 154 (6500 K) .. 370 (2700 K) */

	/* Seed the cached state from persisted attributes at boot, without transmitting. */
	void SeedState(chip::EndpointId endpoint, bool on, uint8_t level, uint16_t mireds);

	/* Result of the most recent frame, for the "halo last" shell command. */
	struct LastFrame {
		const char *what;
		uint8_t flags;
		uint8_t value;
		halo_ts_result_t result;
		unsigned count;
	};
	static const LastFrame &GetLastFrame();

private:
	CHIP_ERROR Init();
};
