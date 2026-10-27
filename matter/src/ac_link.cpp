#include "ac_link.h"

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>

#include <errno.h>
#include <string.h>

namespace {
/* Public BLE address of the C3 ("AC-IR"). */
constexpr uint8_t kAddr[6] = {0x06, 0x6E, 0xB1, 0xD5, 0x96, 0x9C};   /* 9C:96:D5:B1:6E:06, little endian */
#define CMD_UUID BT_UUID_128_ENCODE(0x6d0a0002, 0x7a43, 0x4c55, 0x8a1e, 0x0a5c1e5f1a01)
struct bt_uuid_128 sCmdUuid = BT_UUID_INIT_128(CMD_UUID);

K_MUTEX_DEFINE(sBusy);
K_SEM_DEFINE(sConnected, 0, 1);
K_SEM_DEFINE(sDiscovered, 0, 1);
K_SEM_DEFINE(sDisconnected, 0, 1);

struct bt_conn *sConn;
uint16_t sCmdHandle;
bool sFound;
int sLastErr;
struct bt_gatt_discover_params sDiscover;

void OnConnected(struct bt_conn *conn, uint8_t err)
{
	if (conn != sConn) {
		return;
	}
	sLastErr = err;
	k_sem_give(&sConnected);
}

void OnDisconnected(struct bt_conn *conn, uint8_t)
{
	if (conn != sConn) {
		return;
	}
	k_sem_give(&sDisconnected);
}

BT_CONN_CB_DEFINE(sConnCb) = {
	.connected = OnConnected,
	.disconnected = OnDisconnected,
};

uint8_t OnDiscover(struct bt_conn *, const struct bt_gatt_attr *attr, struct bt_gatt_discover_params *)
{
	if (!attr) {
		k_sem_give(&sDiscovered);
		return BT_GATT_ITER_STOP;
	}
	sCmdHandle = reinterpret_cast<const struct bt_gatt_chrc *>(attr->user_data)->value_handle;
	sFound = true;
	k_sem_give(&sDiscovered);
	return BT_GATT_ITER_STOP;
}
} /* namespace */

AcLink::Result AcLink::Send(const char *command)
{
	if (k_mutex_lock(&sBusy, K_NO_WAIT) != 0) {
		return kBusy;
	}
	Result result = kOk;
	bt_addr_le_t addr;

	addr.type = BT_ADDR_LE_PUBLIC;
	memcpy(addr.a.val, kAddr, sizeof(kAddr));
	k_sem_reset(&sConnected);
	k_sem_reset(&sDiscovered);
	k_sem_reset(&sDisconnected);
	sFound = false;

	sLastErr = 0;
	int ret = bt_conn_le_create(&addr, BT_CONN_LE_CREATE_CONN, BT_LE_CONN_PARAM_DEFAULT, &sConn);

	if (ret != 0) {
		sLastErr = ret;
		sConn = nullptr;
		k_mutex_unlock(&sBusy);
		return kConnectTimeout;
	}
	if (k_sem_take(&sConnected, K_SECONDS(5)) != 0 || sLastErr != 0) {
		if (sConn) {
			bt_conn_disconnect(sConn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
			bt_conn_unref(sConn);
			sConn = nullptr;
		}
		k_mutex_unlock(&sBusy);
		return kConnectTimeout;
	}

	sDiscover = {};
	sDiscover.uuid = &sCmdUuid.uuid;
	sDiscover.func = OnDiscover;
	sDiscover.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
	sDiscover.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
	sDiscover.type = BT_GATT_DISCOVER_CHARACTERISTIC;

	int dret = bt_gatt_discover(sConn, &sDiscover);

	if (dret != 0) {
		sLastErr = dret;                       /* the discover call itself failed */
		result = kDiscoverFailed;
	} else if (k_sem_take(&sDiscovered, K_SECONDS(3)) != 0) {
		sLastErr = -ETIMEDOUT;                 /* no reply from the C3 */
		result = kDiscoverFailed;
	} else if (!sFound) {
		sLastErr = -ENOENT;                    /* discovery finished, UUID not in its database */
		result = kDiscoverFailed;
	} else if (bt_gatt_write_without_response(sConn, sCmdHandle, command, strlen(command), false) != 0) {
		result = kWriteFailed;
	} else {
		k_msleep(150);   /* let the packet go out before tearing the link down */
	}

	bt_conn_disconnect(sConn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	k_sem_take(&sDisconnected, K_SECONDS(2));
	bt_conn_unref(sConn);
	sConn = nullptr;
	k_mutex_unlock(&sBusy);
	return result;
}

int AcLink::LastError()
{
	return sLastErr;
}

const char *AcLink::ResultName(Result r)
{
	switch (r) {
	case kOk: return "ok";
	case kBusy: return "busy";
	case kConnectTimeout: return "connect timeout (is AC-IR powered and in range?)";
	case kDiscoverFailed: return "command characteristic not found";
	case kWriteFailed: return "write failed";
	}
	return "?";
}
