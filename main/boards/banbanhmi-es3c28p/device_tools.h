#ifndef BANBANHMI_ES3C28P_DEVICE_TOOLS_H
#define BANBANHMI_ES3C28P_DEVICE_TOOLS_H

class Display;

// Play one of the embedded phrases (listening, not_heard, offline, quota, too_long, not_ready, tts_fail).
// Returns false for an unknown id. Safe to call from any task.
bool PlayBoardPhrase(const char* id);

void RegisterBanBanhMiDeviceTools(Display* display);

#endif  // BANBANHMI_ES3C28P_DEVICE_TOOLS_H
