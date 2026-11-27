#include "connectivity.h"

#include "application.h"
#include "board.h"
#include "settings.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <memory>
#include <string>

namespace {

constexpr char kTag[] = "BanBanhMiLink";
constexpr int kProbeTimeoutMs = 2000;
constexpr int kIdleProbeIntervalMs = 4000;
constexpr int kRetryAfterFailureMs = 1000;
constexpr int kOfflineProbeIntervalMs = 5000;

std::atomic<bool> g_offline{false};
std::function<void(bool)> g_on_change;

std::string ProbeUrl() {
    Settings settings("wifi", false);
    std::string url = settings.GetString("ota_url");
    if (url.empty()) {
        url = CONFIG_OTA_URL;
    }
    return url;
}

// One GET to the OTA URL. Never logs the URL or the body.
bool ProbeServer() {
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        return false;
    }
    auto http = network->CreateHttp(0);
    http->SetTimeout(kProbeTimeoutMs);
    if (!http->Open("GET", ProbeUrl())) {
        return false;
    }
    auto status = http->GetStatusCode();
    const bool ok = status && *status == 200;
    http->Close();
    return ok;
}

void SetOffline(bool offline) {
    if (g_offline.exchange(offline) == offline) {
        return;
    }
    ESP_LOGW(kTag, "server %s", offline ? "unreachable" : "reachable again");
    if (g_on_change) {
        g_on_change(offline);
    }
}

void MonitorTask(void*) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(g_offline.load() ? kOfflineProbeIntervalMs : kIdleProbeIntervalMs));

        // Only judge while idle: a conversation, Wi-Fi setup or an upgrade has its own traffic.
        if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
            continue;
        }

        bool ok = ProbeServer();
        if (!ok && !g_offline.load()) {
            vTaskDelay(pdMS_TO_TICKS(kRetryAfterFailureMs));
            ok = ProbeServer();
        }
        SetOffline(!ok);
    }
}

}  // namespace

bool IsServerOffline() { return g_offline.load(); }

void StartConnectivityMonitor(std::function<void(bool offline)> on_change) {
    static bool started = false;
    if (started) {
        return;
    }
    started = true;
    g_on_change = std::move(on_change);
    xTaskCreate(MonitorTask, "link_monitor", 6144, nullptr, 3, nullptr);
}
