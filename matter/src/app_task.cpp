#include "app_task.h"

#include "app/matter_init.h"
#include "app/task_executor.h"
#include "board/board.h"
#include "clusters/identify.h"
#include "halo_radio_ts.h"

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/persistence/AttributePersistenceProviderInstance.h>
#include <app/persistence/DefaultAttributePersistenceProvider.h>
#include <app/persistence/DeferredAttributePersistenceProvider.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace ::chip;
using namespace ::chip::app;
using namespace ::chip::DeviceLayer;

namespace
{
constexpr EndpointId kFront = AppTask::kFrontEndpointId;
constexpr EndpointId kBack = AppTask::kBackEndpointId;
constexpr uint16_t kMiredsCold = 154;	/* 6500 K */
constexpr uint16_t kMiredsWarm = 370;	/* 2700 K */
constexpr uint8_t kLevelMinCode = 0x4c;	/* lamp brightness code range, see USB firmware */
constexpr uint8_t kLevelMaxCode = 0xfe;

auto sIdentifyDone = []() { Nrf::PostTask([] { Nrf::GetBoard().GetLED(Nrf::DeviceLeds::LED2).Set(false); }); };
Nrf::Matter::IdentifyCluster sIdentifyFront(kFront, true, sIdentifyDone);
Nrf::Matter::IdentifyCluster sIdentifyBack(kBack, true, sIdentifyDone);

/* Level and colour temperature are written often while a slider moves; persist them lazily. */
DeferredAttribute gDeferred[] = {
	DeferredAttribute(ConcreteAttributePath(kFront, Clusters::LevelControl::Id,
						Clusters::LevelControl::Attributes::CurrentLevel::Id)),
	DeferredAttribute(ConcreteAttributePath(kFront, Clusters::ColorControl::Id,
						Clusters::ColorControl::Attributes::ColorTemperatureMireds::Id)),
	DeferredAttribute(ConcreteAttributePath(kBack, Clusters::LevelControl::Id,
						Clusters::LevelControl::Attributes::CurrentLevel::Id)),
	DeferredAttribute(ConcreteAttributePath(kBack, Clusters::ColorControl::Id,
						Clusters::ColorControl::Attributes::ColorTemperatureMireds::Id)),
};
DefaultAttributePersistenceProvider gSimpleAttributePersistence;
DeferredAttributePersistenceProvider gDeferredAttributePersister(gSimpleAttributePersistence,
								 Span<DeferredAttribute>(gDeferred, 4),
								 System::Clock::Milliseconds32(5000));

/* Lamp state as the Matter side wants it, and what still has to go out. */
struct LampState {
	bool frontOn;
	bool backOn;
	uint8_t levelPct;	/* 1..100, shared by both lamps */
	uint8_t tempPct;	/* 0 coldest .. 100 warmest, shared */
};
LampState sState = { false, false, 50, 50 };
uint8_t sLevelRaw = 254;	/* last Level Control value seen, to mirror it to the other endpoint */
uint16_t sMiredsRaw = 250;
bool sMirroring;		/* set while writing the mirrored attribute, so the callback ignores it */
bool sDirtyPower;
bool sDirtyTemp;
bool sReady;	/* attribute writes before the server is up are restores, not commands */
unsigned sPid;

K_SEM_DEFINE(sSyncSem, 0, 1);

/* Status LED (XIAO: LED1 red, LED2 green, LED3 blue):
 *   blue blinking slowly   waiting to be paired (BLE advertising)
 *   blue blinking fast     a phone is connected, commissioning in progress
 *   green for 5 s          joined the Thread network / provisioned
 *   green / red flash      a frame was ACKed / not ACKed by the lamp
 */
void GreenOff(k_timer *)
{
	Nrf::PostTask([] { Nrf::GetBoard().GetLED(Nrf::DeviceLeds::LED2).Set(false); });
}
K_TIMER_DEFINE(sGreenOffTimer, GreenOff, nullptr);

void StatusLedHandler()
{
	auto &board = Nrf::GetBoard();
	auto &red = board.GetLED(Nrf::DeviceLeds::LED1);
	auto &green = board.GetLED(Nrf::DeviceLeds::LED2);
	auto &blue = board.GetLED(Nrf::DeviceLeds::LED3);

	red.Set(false);
	blue.Set(false);
	switch (board.GetDeviceState()) {
	case Nrf::DeviceState::DeviceDisconnected:
	case Nrf::DeviceState::DeviceAdvertisingBLE:
		blue.Blink(300, 300);
		break;
	case Nrf::DeviceState::DeviceConnectedBLE:
		blue.Blink(80, 80);
		break;
	case Nrf::DeviceState::DeviceProvisioned:
		green.Set(true);
		k_timer_start(&sGreenOffTimer, K_SECONDS(5), K_NO_WAIT);
		break;
	}
}

void FlashResult(bool ack)
{
	auto &led = Nrf::GetBoard().GetLED(ack ? Nrf::DeviceLeds::LED2 : Nrf::DeviceLeds::LED1);

	led.Set(true);
	k_msleep(120);
	led.Set(false);
}
K_THREAD_STACK_DEFINE(sHaloStack, 2048);
struct k_thread sHaloThread;

uint8_t LevelToPercent(uint8_t level)
{
	if (level == 0) {
		return 1;
	}
	return static_cast<uint8_t>(1 + (static_cast<unsigned>(level) - 1) * 99 / 253);
}

uint8_t MiredsToPercent(uint16_t mireds)
{
	if (mireds <= kMiredsCold) {
		return 0;
	}
	if (mireds >= kMiredsWarm) {
		return 100;
	}
	return static_cast<uint8_t>((mireds - kMiredsCold) * 100u / (kMiredsWarm - kMiredsCold));
}

uint8_t PercentToLevelCode(uint8_t pct)
{
	if (pct == 0) {
		return kLevelMinCode;
	}
	return static_cast<uint8_t>(kLevelMinCode + ((pct - 1) * (kLevelMaxCode - kLevelMinCode) + 49) / 99);
}

void RequestSync(bool power, bool temp)
{
	if (!sReady) {
		return;
	}
	unsigned key = irq_lock();

	sDirtyPower = sDirtyPower || power;
	sDirtyTemp = sDirtyTemp || temp;
	irq_unlock(key);
	k_sem_give(&sSyncSem);
}

AppTask::LastFrame sLast;

void SendFrame(const char *what, uint8_t flags, uint8_t value)
{
	halo_ts_result_t r = halo_ts_send(CONFIG_HALO_LAMP_ADDRESS, flags, value, sPid, CONFIG_HALO_TX_GAIN,
					  CONFIG_HALO_ATTEMPTS);

	sPid = (sPid + 1) % 4;
	sLast.what = what;
	sLast.flags = flags;
	sLast.value = value;
	sLast.result = r;
	sLast.count++;
	FlashResult(r.ack);
	LOG_INF("Halo %s %02X%02X: %s after %u attempt(s), rssi %d dBm%s [started %u tx %u rxto %u blocked %u nosig %u err %d txend %u us radiosig %u]",
		what, flags, value, r.ack ? "ACK" : "no ACK", r.attempts, r.rssi_dbm,
		r.energy && !r.ack ? " (reply heard)" : "", r.started, r.tx_done, r.rx_timeout, r.blocked,
		r.no_signal, r.request_err, r.tx_end_us, r.radio_signals);
}

void HaloThread(void *, void *, void *)
{
	if (halo_ts_init()) {
		LOG_ERR("MPSL timeslot session could not be opened");
	}
	for (;;) {
		k_sem_take(&sSyncSem, K_FOREVER);
		/* Coalesce slider moves: let the attribute settle for a moment. */
		k_msleep(80);

		unsigned key = irq_lock();
		LampState s = sState;
		bool power = sDirtyPower, temp = sDirtyTemp;

		sDirtyPower = false;
		/* keep a pending temperature until a lamp is on */
		sDirtyTemp = (s.frontOn || s.backOn) ? false : sDirtyTemp;
		irq_unlock(key);

		/* Every frame is an absolute state: which lamps, and POWER. All off is
		 * sent as an ordinary frame with POWER clear, like the remote does.
		 */
		bool on = s.frontOn || s.backOn;
		uint8_t lamps = on ? ((s.frontOn ? HALO_FLAG_FRONT : 0) | (s.backOn ? HALO_FLAG_BACK : 0))
				   : (HALO_FLAG_FRONT | HALO_FLAG_BACK);

		if (power) {
			uint8_t flags = lamps | HALO_FLAG_BRIGHTNESS | (on ? HALO_FLAG_POWER : 0);

			SendFrame("brightness", flags, on ? PercentToLevelCode(s.levelPct) : kLevelMinCode);
		}
		if (temp && on) {
			SendFrame("temperature", lamps | HALO_FLAG_TEMPERATURE | HALO_FLAG_POWER, s.tempPct);
		}
	}
}
} /* namespace */

const AppTask::LastFrame &AppTask::GetLastFrame()
{
	return sLast;
}

/* Write a shared attribute to the other endpoint without triggering a new sync.
 * Only once the server is up: during cluster initialisation each endpoint
 * restores its own persisted value, and writing into a cluster that has not been
 * initialised yet faults.
 */
template <typename Setter> void Mirror(EndpointId from, Setter set)
{
	if (!sReady) {
		return;
	}
	EndpointId other = from == kFront ? kBack : kFront;

	sMirroring = true;
	set(other);
	sMirroring = false;
}

void AppTask::SeedState(EndpointId endpoint, bool on, uint8_t level, uint16_t mireds)
{
	unsigned key = irq_lock();

	if (endpoint == kBack) {
		sState.backOn = on;
	} else {
		sState.frontOn = on;
	}
	sState.levelPct = LevelToPercent(level);
	sState.tempPct = MiredsToPercent(mireds);
	sLevelRaw = level;
	sMiredsRaw = mireds;
	irq_unlock(key);
}

void AppTask::SetOnOff(EndpointId endpoint, bool on)
{
	unsigned key = irq_lock();
	bool wasOn = sState.frontOn || sState.backOn;

	if (endpoint == kBack) {
		sState.backOn = on;
	} else {
		sState.frontOn = on;
	}
	bool isOn = sState.frontOn || sState.backOn;
	irq_unlock(key);
	RequestSync(true, !wasOn && isOn);	/* re-apply a pending temperature when switching on */
}

void AppTask::SetLevel(EndpointId endpoint, uint8_t level)
{
	if (sMirroring) {
		return;
	}
	uint8_t pct = LevelToPercent(level);
	unsigned key = irq_lock();
	bool on = sState.frontOn || sState.backOn;
	bool changed = sLevelRaw != level;

	sState.levelPct = pct;
	sLevelRaw = level;
	irq_unlock(key);
	if (changed) {
		Mirror(endpoint, [level](EndpointId other) {
			Clusters::LevelControl::Attributes::CurrentLevel::Set(other, level);
		});
	}
	if (on) {
		RequestSync(true, false);
	}
}

void AppTask::SetMireds(EndpointId endpoint, uint16_t mireds)
{
	if (sMirroring) {
		return;
	}
	uint8_t pct = MiredsToPercent(mireds);
	unsigned key = irq_lock();
	bool changed = sMiredsRaw != mireds;

	sState.tempPct = pct;
	sMiredsRaw = mireds;
	irq_unlock(key);
	if (changed) {
		Mirror(endpoint, [mireds](EndpointId other) {
			Clusters::ColorControl::Attributes::ColorTemperatureMireds::Set(other, mireds);
		});
	}
	RequestSync(false, true);
}

CHIP_ERROR AppTask::Init()
{
	ReturnErrorOnFailure(Nrf::Matter::PrepareServer(Nrf::Matter::InitData{ .mPostServerInitClbk = []() {
		app::SetAttributePersistenceProvider(&gDeferredAttributePersister);
		gSimpleAttributePersistence.Init(Nrf::Matter::GetPersistentStorageDelegate());
		return CHIP_NO_ERROR;
	} }));

	if (!Nrf::GetBoard().Init(nullptr, StatusLedHandler)) {
		LOG_WRN("Board LEDs/buttons initialization failed (no buttons on this board is fine)");
	}

	ReturnErrorOnFailure(Nrf::Matter::RegisterEventHandler(Nrf::Board::DefaultMatterEventHandler, 0));
	ReturnErrorOnFailure(sIdentifyFront.Init());
	ReturnErrorOnFailure(sIdentifyBack.Init());

	sPid = sys_rand32_get() & 3;
	k_thread_create(&sHaloThread, sHaloStack, K_THREAD_STACK_SIZEOF(sHaloStack), HaloThread, nullptr,
			nullptr, nullptr, K_PRIO_PREEMPT(10), 0, K_NO_WAIT);
	k_thread_name_set(&sHaloThread, "halo");

	CHIP_ERROR err = Nrf::Matter::StartServer();

	sReady = (err == CHIP_NO_ERROR);
	return err;
}

CHIP_ERROR AppTask::StartApp()
{
	ReturnErrorOnFailure(Init());

	while (true) {
		Nrf::DispatchNextTask();
	}

	return CHIP_NO_ERROR;
}
