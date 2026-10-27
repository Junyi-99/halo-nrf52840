#include "ac_control.h"
#include "ac_link.h"

#include <app-common/zap-generated/cluster-objects.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>

#include <zephyr/kernel.h>

#include <stdio.h>
#include <string.h>

using namespace ::chip;
using namespace ::chip::app::Clusters;

namespace {
constexpr uint32_t kDebounceMs = 700;
constexpr int kRetries = 2;

/* Shadow of the A/C state. Panasonic fan values: 1 low, 2 medium, 3 high, 7 auto. */
bool sOn = false;
int sTempC = 25;
int sFan = 7;

K_THREAD_STACK_DEFINE(sStack, 2048);
struct k_work_q sQueue;
struct k_work_delayable sWork;

void SendWork(struct k_work *)
{
	char cmd[40];

	snprintf(cmd, sizeof(cmd), "set %s %d %d", sOn ? "on" : "off", sTempC, sFan);
	for (int i = 0; i <= kRetries; i++) {
		if (AcLink::Send(cmd) == AcLink::kOk) {
			return;
		}
		k_msleep(500);
	}
}

void Schedule()
{
	k_work_reschedule_for_queue(&sQueue, &sWork, K_MSEC(kDebounceMs));
}
} /* namespace */

void AcControl::OnAttributeChanged(const app::ConcreteAttributePath &path, uint16_t size, uint8_t *value)
{
	if (path.mEndpointId == kThermostatEndpointId && path.mClusterId == Thermostat::Id) {
		if (path.mAttributeId == Thermostat::Attributes::SystemMode::Id) {
			/* 0 Off, 3 Cool; anything else is not offered (ControlSequence: cooling only) */
			sOn = *value == static_cast<uint8_t>(Thermostat::SystemModeEnum::kCool);
			Schedule();
		} else if (path.mAttributeId == Thermostat::Attributes::OccupiedCoolingSetpoint::Id && size == 2) {
			int16_t centi;

			memcpy(&centi, value, sizeof(centi));
			int t = (centi + 50) / 100;

			sTempC = t < 16 ? 16 : t > 30 ? 30 : t;
			Schedule();
		}
	} else if (path.mEndpointId == kFanEndpointId && path.mClusterId == FanControl::Id &&
		   path.mAttributeId == FanControl::Attributes::FanMode::Id) {
		switch (static_cast<FanControl::FanModeEnum>(*value)) {
		case FanControl::FanModeEnum::kLow: sFan = 1; break;
		case FanControl::FanModeEnum::kMedium: sFan = 2; break;
		case FanControl::FanModeEnum::kHigh: sFan = 3; break;
		case FanControl::FanModeEnum::kAuto: sFan = 7; break;
		default: return;     /* Off / On: no meaning for the A/C fan */
		}
		Schedule();
	}
}

void AcControl::Start()
{
	k_work_queue_init(&sQueue);
	k_work_queue_start(&sQueue, sStack, K_THREAD_STACK_SIZEOF(sStack), K_PRIO_PREEMPT(12), nullptr);
	k_work_init_delayable(&sWork, SendWork);
}
