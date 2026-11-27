#include "device_tools.h"

#include "application.h"
#include "assets/lang_config.h"
#include "display/display.h"
#include "mcp_server.h"

#include <esp_log.h>
#include <esp_timer.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace {

constexpr char kTag[] = "BanBanhMiTools";
constexpr size_t kMaxScreenTextBytes = 200;

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

void RegisterBanBanhMiDeviceTools(Display* display) {
    auto& mcp_server = McpServer::GetInstance();

    mcp_server.AddTool(
        "self.audio.play_phrase",
        "Play a short embedded Vietnamese audio phrase by id. "
        "Allowed ids: listening, not_heard, offline, quota, too_long, not_ready, tts_fail.",
        PropertyList({Property("id", kPropertyTypeString)}),
        [](const PropertyList& properties) -> ToolResult {
            const auto& id = properties["id"].value<std::string>();
            const PhraseEntry* entry = FindPhrase(id);
            if (entry == nullptr) {
                return std::unexpected("unknown phrase id");
            }

            ESP_LOGI(kTag, "play_phrase id=%s t=%lld ms", entry->id,
                     static_cast<long long>(esp_timer_get_time() / 1000));

            const std::string_view sound = entry->sound;
            Application::GetInstance().Schedule(
                [sound]() { Application::GetInstance().PlaySound(sound); });
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
}
