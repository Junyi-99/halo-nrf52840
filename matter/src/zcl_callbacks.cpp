#include "app_task.h"

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app-common/zap-generated/cluster-objects.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/ConcreteAttributePath.h>
#include <lib/support/logging/CHIPLogging.h>

#include <string.h>

using namespace ::chip;
using namespace ::chip::app::Clusters;

void MatterPostAttributeChangeCallback(const chip::app::ConcreteAttributePath &attributePath, uint8_t type,
				       uint16_t size, uint8_t *value)
{
	ClusterId clusterId = attributePath.mClusterId;
	AttributeId attributeId = attributePath.mAttributeId;

	if (clusterId == OnOff::Id && attributeId == OnOff::Attributes::OnOff::Id) {
		AppTask::Instance().SetOnOff(*value != 0);
	} else if (clusterId == LevelControl::Id && attributeId == LevelControl::Attributes::CurrentLevel::Id) {
		AppTask::Instance().SetLevel(*value);
	} else if (clusterId == ColorControl::Id &&
		   attributeId == ColorControl::Attributes::ColorTemperatureMireds::Id) {
		uint16_t mireds;

		memcpy(&mireds, value, sizeof(mireds));
		AppTask::Instance().SetMireds(mireds);
	}
}

/* Runs once the data model is up: cache what was persisted, do not transmit. */
void emberAfOnOffClusterInitCallback(EndpointId endpoint)
{
	bool on = false;
	uint8_t level = 254;
	uint16_t mireds = 250;

	OnOff::Attributes::OnOff::Get(endpoint, &on);
	LevelControl::Attributes::CurrentLevel::TypeInfo::Type currentLevel;
	if (LevelControl::Attributes::CurrentLevel::Get(endpoint, currentLevel) ==
	    Protocols::InteractionModel::Status::Success) {
		level = currentLevel.ValueOr(254);
	}
	ColorControl::Attributes::ColorTemperatureMireds::Get(endpoint, &mireds);
	AppTask::Instance().SeedState(on, level, mireds);
}
