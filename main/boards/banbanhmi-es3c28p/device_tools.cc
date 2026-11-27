#include "device_tools.h"

#include "application.h"
#include "assets/lang_config.h"
#include "device_state.h"
#include "display/display.h"
#include "mcp_server.h"

#include <esp_log.h>
#include <esp_timer.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr char kTag[] = "BanBanhMiTools";
constexpr size_t kMaxScreenTextBytes = 200;
constexpr size_t kMaxStatusTextChars = 40;
constexpr size_t kMaxStatusTextBytes = 160;
constexpr int kMaxCountdownSec = 30;

bool IsAllowedStatusState(std::string_view state) {
    return state == "listening" || state == "processing" || state == "awaiting_ok" ||
           state == "closed";
}

size_t CountUtf8Chars(std::string_view text) {
    size_t count = 0;
    for (unsigned char byte : text) {
        if ((byte & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

struct PhraseEntry {
    const char* id;
    std::string_view sound;
};

const PhraseEntry* FindPhrase(std::string_view id) {
    static const PhraseEntry kPhrases[] = {
        {"listening", Lang::Sounds::OGG_PHRASE_LISTENING},
        {"not_heard", Lang::Sounds::OGG_PHRASE_NOT_HEARD},
        {"offline", Lang::Sounds::OGG_PHRASE_OFFLINE},
        {"quota", Lang::Sounds::OGG_PHRASE_QUOTA},
        {"too_long", Lang::Sounds::OGG_PHRASE_TOO_LONG},
        {"not_ready", Lang::Sounds::OGG_PHRASE_NOT_READY},
        {"tts_fail", Lang::Sounds::OGG_PHRASE_TTS_FAIL},
    };
    for (const auto& entry : kPhrases) {
        if (id == entry.id) {
            return &entry;
        }
    }
    return nullptr;
}

}  // namespace

bool PlayBoardPhrase(const char* id) {
    const PhraseEntry* entry = FindPhrase(id);
    if (entry == nullptr) {
        return false;
    }
    ESP_LOGI(kTag, "play_phrase id=%s t=%lld ms", entry->id,
             static_cast<long long>(esp_timer_get_time() / 1000));
    const std::string_view sound = entry->sound;
    Application::GetInstance().Schedule([sound]() { Application::GetInstance().PlaySound(sound); });
    return true;
}

void RegisterBanBanhMiDeviceTools(Display* display, StatusCallback on_status) {
    auto& mcp_server = McpServer::GetInstance();

    mcp_server.AddTool(
        "self.audio.play_phrase",
        "Play a short embedded Vietnamese audio phrase by id. "
        "Allowed ids: listening, not_heard, offline, quota, too_long, not_ready, tts_fail.",
        PropertyList({Property("id", kPropertyTypeString)}),
        [](const PropertyList& properties) -> ToolResult {
            const auto& id = properties["id"].value<std::string>();
            if (!PlayBoardPhrase(id.c_str())) {
                return std::unexpected("unknown phrase id");
            }
            return true;
        });

    mcp_server.AddTool(
        "self.screen.show_text",
        "Display a short UTF-8 message (up to 200 bytes) in the screen chat area. "
        "Pass an empty string to clear the message.",
        PropertyList({Property("text", kPropertyTypeString).SetMaxLength(kMaxScreenTextBytes)}),
        [display](const PropertyList& properties) -> ToolResult {
            const auto& text = properties["text"].value<std::string>();
            if (text.size() > kMaxScreenTextBytes) {
                return std::unexpected("text exceeds 200 bytes");
            }
            if (display == nullptr) {
                return std::unexpected("display not available");
            }

            ESP_LOGI(kTag, "show_text len=%u t=%lld ms", static_cast<unsigned>(text.size()),
                     static_cast<long long>(esp_timer_get_time() / 1000));

            std::string message = text;
            Application::GetInstance().Schedule(
                [display, message = std::move(message)]() {
                    display->SetChatMessage("assistant", message.c_str());
                });
            return true;
        });

    mcp_server.AddTool(
        "self.screen.show_status",
        "Update the landscape status screen for a voice listening session. "
        "Allowed state values: listening, processing, awaiting_ok, closed. "
        "text must be UTF-8, up to 40 characters (160 bytes), shown in the heard-text area. "
        "Optional countdown_s (0..30, default 0) runs a local 1 s countdown on screen when "
        "state is 'awaiting_ok'; any new show_status call or state change cancels the timer.",
        PropertyList({Property("state", kPropertyTypeString),
                      Property("text", kPropertyTypeString, std::string(""))
                          .SetMaxLength(kMaxStatusTextBytes),
                      Property("countdown_s", kPropertyTypeInteger, 0, 0, kMaxCountdownSec)}),
        [display, on_status = std::move(on_status)](const PropertyList& properties) -> ToolResult {
            const auto& state = properties["state"].value<std::string>();
            if (!IsAllowedStatusState(state)) {
                return std::unexpected("invalid status state");
            }
            const auto& text = properties["text"].value<std::string>();
            if (text.size() > kMaxStatusTextBytes) {
                return std::unexpected("text exceeds 160 bytes");
            }
            if (CountUtf8Chars(text) > kMaxStatusTextChars) {
                return std::unexpected("text exceeds 40 characters");
            }
            const int countdown_s = properties["countdown_s"].value<int>();
            if (countdown_s < 0 || countdown_s > kMaxCountdownSec) {
                return std::unexpected("countdown_s out of range (0..30)");
            }
            if (display == nullptr && !on_status) {
                return std::unexpected("display not available");
            }

            ESP_LOGI(kTag, "show_status state=%s len=%u countdown_s=%d t=%lld ms", state.c_str(),
                     static_cast<unsigned>(text.size()), countdown_s,
                     static_cast<long long>(esp_timer_get_time() / 1000));

            std::string status_state = state;
            std::string status_text = text;
            Application::GetInstance().Schedule(
                [display, on_status, status_state = std::move(status_state),
                 status_text = std::move(status_text), countdown_s]() {
                    if (on_status) {
                        on_status(status_state, status_text, countdown_s);
                    } else if (display != nullptr) {
                        if (status_state == "closed") {
                            display->SetStatus(Lang::Strings::STANDBY);
                        } else {
                            display->SetStatus(status_text.c_str());
                        }
                    }
                });
            return true;
        });

    mcp_server.AddTool(
        "self.session.close",
        "End the active voice listening session and return the device to idle state "
        "without closing the audio channel.",
        PropertyList(),
        [](const PropertyList&) -> ToolResult {
            ESP_LOGI(kTag, "session_close t=%lld ms",
                     static_cast<long long>(esp_timer_get_time() / 1000));
            Application::GetInstance().Schedule([]() {
                auto& app = Application::GetInstance();
                const DeviceState state = app.GetDeviceState();
                if (state == kDeviceStateListening || state == kDeviceStateSpeaking) {
                    app.SetDeviceState(kDeviceStateIdle);
                }
            });
            return true;
        });
}
