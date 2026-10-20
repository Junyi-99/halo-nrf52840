/* halo-matter application: Matter colour-temperature light endpoint whose state
 * is pushed to a BenQ ScreenBar Halo over the private 2.4 GHz protocol.
 */
#pragma once

#include "board/board.h"

#include <platform/CHIPDeviceLayer.h>

class AppTask {
public:
	static AppTask &Instance()
	{
		static AppTask sAppTask;
		return sAppTask;
	};

	CHIP_ERROR StartApp();

	/* Called from Matter attribute callbacks (Matter thread). */
	void SetOnOff(bool on);
	void SetLevel(uint8_t level);		/* Level Control 1..254 */
	void SetMireds(uint16_t mireds);	/* Color Control, 154 (6500 K) .. 370 (2700 K) */

	/* Seed the cached state from persisted attributes at boot, without transmitting. */
	void SeedState(bool on, uint8_t level, uint16_t mireds);

private:
	CHIP_ERROR Init();
	static void ButtonEventHandler(Nrf::ButtonState state, Nrf::ButtonMask hasChanged);
};
