/* BLE link to the ESP32-C3 "AC-IR" infrared blaster: connect on demand, write one text
 * command to its command characteristic, disconnect.
 */
#pragma once

namespace AcLink {
enum Result { kOk = 0, kBusy, kConnectTimeout, kDiscoverFailed, kWriteFailed };

/* Queue a text command such as "set on 25 auto". Returns once the command has been
 * written (or failed); blocks up to about 10 s, so do not call it from the Matter thread.
 */
Result Send(const char *command);
const char *ResultName(Result r);

/* Detail of the last failure: the Zephyr error from bt_conn_le_create(), or the HCI status of a failed connection. */
int LastError();
}
