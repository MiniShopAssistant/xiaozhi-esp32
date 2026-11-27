#ifndef BANBANHMI_ES3C28P_CONNECTIVITY_H
#define BANBANHMI_ES3C28P_CONNECTIVITY_H

#include <functional>

// Background check that the voice server answers (spec 007 R4, contracts/device-protocol.md 1b).
// While the device is idle it calls GET <OTA URL> every 4 s (timeout 2 s). A failure is retried
// after 1 s and two failures in a row mean "offline". While offline it keeps trying every 5 s and
// clears the state on the first answer. Wi-Fi loss shows up the same way as a stopped server.
// `on_change(offline)` runs on the monitor task, only when the state flips.
void StartConnectivityMonitor(std::function<void(bool offline)> on_change);

bool IsServerOffline();

#endif  // BANBANHMI_ES3C28P_CONNECTIVITY_H
