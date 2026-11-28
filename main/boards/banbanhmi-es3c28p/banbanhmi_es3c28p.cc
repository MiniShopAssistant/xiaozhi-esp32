#include "application.h"
#include "button.h"
#include "codecs/es8311_audio_codec.h"
#include "config.h"
#include "connectivity.h"
#include "device_state.h"
#include "device_tools.h"
#include "display/lcd_display.h"
#include "led/led.h"
#include "vi_lv_font.h"
#include "wifi_board.h"

#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_lcd_ili9341.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_touch_ft5x06.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <esp_timer.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define TAG "BanBanhMiEs3c28p"

static constexpr float kMicGainDb = 42.0f;
static constexpr int kSpeakerVolume = 100;

// Latency probe for `pnpm device:latency` (spec 007 T070, research R11). One line when the finger is lifted
// and one when the first frame of the spoken answer is written to the speaker; the server joins them
// with its own `device_turn` log. Only counts turns answered with TTS (state "speaking").
static std::atomic<int> g_turn{0};
static std::atomic<bool> g_wait_first_audio{false};

static void NoteRelease() {
    const int turn = ++g_turn;
    ESP_LOGI(TAG, "VOICE_METRIC turn=%d event=release esp_timer_us=%lld", turn,
             static_cast<long long>(esp_timer_get_time()));
    g_wait_first_audio = true;
}

static void NoteOutput() {
    if (!g_wait_first_audio.load()) {
        return;
    }
    if (Application::GetInstance().GetDeviceState() != kDeviceStateSpeaking) {
        return;
    }
    if (g_wait_first_audio.exchange(false)) {
        ESP_LOGI(TAG, "VOICE_METRIC turn=%d event=first_audio esp_timer_us=%lld", g_turn.load(),
                 static_cast<long long>(esp_timer_get_time()));
    }
}

// ILI9341V vendor-specific initialization sequence (from quandong 2.8" SPI board)
static const ili9341_lcd_init_cmd_t kVendorSpecificInit[] = {
    {0xCF, (uint8_t[]){0x00, 0xC1, 0x30}, 3, 0},
    {0xED, (uint8_t[]){0x64, 0x03, 0x12, 0x81}, 4, 0},
    {0xE8, (uint8_t[]){0x85, 0x00, 0x78}, 3, 0},
    {0xCB, (uint8_t[]){0x39, 0x2C, 0x00, 0x34, 0x02}, 5, 0},
    {0xF7, (uint8_t[]){0x20}, 1, 0},
    {0xEA, (uint8_t[]){0x00, 0x00}, 2, 0},
    {0xC0, (uint8_t[]){0x13}, 1, 0},
    {0xC1, (uint8_t[]){0x13}, 1, 0},
    {0xC5, (uint8_t[]){0x22, 0x35}, 2, 0},
    {0xC7, (uint8_t[]){0xBD}, 1, 0},
    {0, (uint8_t[]){0}, 0x11, 0},
    {0x36, (uint8_t[]){0x08}, 1, 0},
    {0xB6, (uint8_t[]){0x0A, 0xA2}, 2, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0xF6, (uint8_t[]){0x01, 0x30}, 2, 0},
    {0xB1, (uint8_t[]){0x00, 0x1B}, 2, 0},
    {0xF2, (uint8_t[]){0x00}, 1, 0},
    {0x26, (uint8_t[]){0x01}, 1, 0},
    {0xE0,
     (uint8_t[]){0x0F, 0x35, 0x31, 0x0B, 0x0E, 0x06, 0x49, 0xA7, 0x33, 0x07, 0x0F, 0x03, 0x0C,
                 0x0A, 0x00},
     15, 0},
    {0xE1,
     (uint8_t[]){0x00, 0x0A, 0x0F, 0x04, 0x11, 0x08, 0x36, 0x58, 0x4D, 0x07, 0x10, 0x0C, 0x32,
                 0x34, 0x0F},
     15, 0},
    {0, (uint8_t[]){0}, 0x11, 0},
    {0, (uint8_t[]){0}, 0x29, 0},
};

// ES8311 codec for this board with two changes, both kept inside the board directory:
//  - the first 100 ms after the input is switched on are muted: the ADC start-up produces a large
//    low-frequency step (measured: -25 dBFS, 94% of the energy under 100 Hz, peak -8 dBFS) that the
//    STT model "recognised" as a word when nobody was speaking;
//  - the microphone gain is raised to 42 dB (default 30 dB): speech was only about 10 dB above the
//    noise floor at 30 cm.
class BanBanhMiCodec : public Es8311AudioCodec {
public:
    using Es8311AudioCodec::Es8311AudioCodec;

    void EnableInput(bool enable) override {
        if (enable && !input_enabled()) {
            blank_until_us_ = esp_timer_get_time() + kInputBlankUs;
        }
        Es8311AudioCodec::EnableInput(enable);
    }

    void OutputData(std::vector<int16_t>& data) override {
        NoteOutput();
        Es8311AudioCodec::OutputData(data);
    }

    bool InputData(std::vector<int16_t>& data) override {
        const bool ok = AudioCodec::InputData(data);
        if (ok && esp_timer_get_time() < blank_until_us_) {
            std::fill(data.begin(), data.end(), 0);
        }
        return ok;
    }

private:
    static constexpr int64_t kInputBlankUs = 100 * 1000;
    int64_t blank_until_us_ = 0;
};

class Es3c28pDisplay : public SpiLcdDisplay {
private:
    // Landscape 320x240 conversation layout constants:
    // top bar (28) + reply area (84) + heard area (84) + PTT bar (44) = 240 px.
    static constexpr int kTopBarHeight = 28;
    static constexpr int kPttHeight = 44;
    static constexpr int kBodyHeight = DISPLAY_HEIGHT - kTopBarHeight - kPttHeight;  // 168 px

    static constexpr int kTopBarPadX = 8;
    static constexpr int kTopIconSize = 20;
    static constexpr int kTopBarGap = 6;
    static constexpr int kStateLabelX = kTopBarPadX + kTopIconSize + kTopBarGap;  // 34 px
    static constexpr int kOnlineDotSize = 10;
    static constexpr int kOnlineDotMarginRight = 8;
    static constexpr int kStateLabelMaxWidth =
        DISPLAY_WIDTH - kStateLabelX - kOnlineDotMarginRight - kOnlineDotSize - kTopBarGap;  // 262 px
    static constexpr int kStateLabelMaxHeight = 24;  // 1 line of kViFont20 (line_height = 24)
    static constexpr int kCountdownUnitOffsetRight =
        kOnlineDotMarginRight + kOnlineDotSize + kTopBarGap;  // 24 px
    static constexpr int kCountdownDigitWidth = 28;
    static constexpr int kCountdownDigitOffsetRight =
        kCountdownUnitOffsetRight + 44;  // leaves 4 px gap before "giây" (~40 px wide)

    static constexpr int kBodyPadY = 2;
    static constexpr int kBubbleGapY = 4;
    static constexpr int kBubbleHeight =
        (kBodyHeight - 2 * kBodyPadY - kBubbleGapY) / 2;  // 80 px (each area = 84 px incl. margins)
    static constexpr int kBubbleMarginX = 8;
    static constexpr int kBubbleWidth = 272;
    static constexpr int kBubbleRadius = 10;
    static constexpr int kBubbleStripeWidth = 4;
    static constexpr int kBubbleBorderWidth = 1;
    static constexpr int kBubblePadX = 8;
    static constexpr int kReplyLabelWidth =
        kBubbleWidth - kBubbleStripeWidth - 2 * kBubblePadX;  // 252 px
    static constexpr int kHeardLabelWidth =
        kBubbleWidth - 2 * kBubbleBorderWidth - 2 * kBubblePadX;  // 254 px
    static constexpr int kBubbleTextMaxHeight = 3 * 24;           // 3 lines of kViFont20

    static constexpr uint32_t kCountdownIntervalMs = 1000;
    static constexpr int kCountdownUrgentThreshold = 3;

    // Theme colours per state: bg = body, sep = lighter reply bubble fill, bar = darker heard
    // bubble and top bar fill, accent = reply left stripe and dim placeholder text.
    struct StatePalette {
        uint32_t bg;
        uint32_t bar;
        uint32_t sep;
        uint32_t accent;
    };

    static constexpr StatePalette kPaletteClosed = {0x263238, 0x1C252A, 0x37474F, 0xB0BEC5};
    static constexpr StatePalette kPaletteListening = {0x1B5E20, 0x124116, 0x2E7D32, 0xA5D6A7};
    static constexpr StatePalette kPaletteProcessing = {0xE65100, 0xBF360C, 0xF57C00, 0xFFE0B2};
    static constexpr StatePalette kPaletteAwaitingOk = {0x0D47A1, 0x082E6B, 0x1976D2, 0xBBDEFB};
    static constexpr StatePalette kPaletteOffline = {0x546E7A, 0x455A64, 0x607D8B, 0xCFD8DC};

    static constexpr uint32_t kColorWhite = 0xFFFFFF;
    static constexpr uint32_t kColorUrgentYellow = 0xFFF59D;
    static constexpr uint32_t kColorOnlineDot = 0x66BB6A;
    static constexpr uint32_t kColorOfflineDot = 0xEF5350;

    static constexpr uint32_t kPttIdleColorHex = 0x1E88E5;
    static constexpr uint32_t kPttActiveColorHex = 0xD32F2F;
    static constexpr uint32_t kPttListeningColorHex = 0x2E7D32;
    static constexpr uint32_t kPttOfflineColorHex = 0x546E7A;

    static constexpr const char* kReplyPlaceholder = "Máy sẽ trả lời ở đây";
    static constexpr const char* kHeardPlaceholder = "Bạn nói, tôi nghe";

public:
    enum class StatusState {
        kClosed,
        kListening,
        kProcessing,
        kAwaitingOk,
    };

    Es3c28pDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                   int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                   bool swap_xy)
        : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y,
                        swap_xy) {}

    ~Es3c28pDisplay() override {
        DisplayLockGuard lock(this);
        StopCountdownTimer();
    }

    void SetupUI() override {
        SpiLcdDisplay::SetupUI();

        DisplayLockGuard lock(this);
        lv_obj_t* screen = lv_screen_active();

        // Full-screen landscape conversation overlay (320x240); hidden only during Wi-Fi/OTA setup.
        status_root_ = lv_obj_create(screen);
        InitPlainBox(status_root_, DISPLAY_WIDTH, DISPLAY_HEIGHT);
        lv_obj_set_pos(status_root_, 0, 0);
        lv_obj_set_style_bg_opa(status_root_, LV_OPA_COVER, 0);

        // Top bar (28 px): [state icon 20 px] [state word 20 px] ... [countdown] [online dot]
        header_bar_ = lv_obj_create(status_root_);
        InitPlainBox(header_bar_, DISPLAY_WIDTH, kTopBarHeight);
        lv_obj_set_pos(header_bar_, 0, 0);
        lv_obj_set_style_bg_opa(header_bar_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(header_bar_, 1, 0);
        lv_obj_set_style_border_side(header_bar_, LV_BORDER_SIDE_BOTTOM, 0);

        icon_box_ = lv_obj_create(header_bar_);
        InitPlainBox(icon_box_, kTopIconSize, kTopIconSize);
        lv_obj_align(icon_box_, LV_ALIGN_LEFT_MID, kTopBarPadX, 0);
        lv_obj_set_style_bg_opa(icon_box_, LV_OPA_TRANSP, 0);
        BuildStateIcons();

        state_label_ = lv_label_create(header_bar_);
        lv_obj_set_width(state_label_, kStateLabelMaxWidth);
        lv_obj_set_style_text_font(state_label_, &kViFont20, 0);
        lv_obj_set_style_text_color(state_label_, lv_color_hex(kColorWhite), 0);
        lv_obj_set_style_text_align(state_label_, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_max_height(state_label_, kStateLabelMaxHeight, 0);
        lv_label_set_long_mode(state_label_, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(state_label_, LV_ALIGN_LEFT_MID, kStateLabelX, 0);

        countdown_unit_label_ = lv_label_create(header_bar_);
        lv_label_set_text(countdown_unit_label_, "giây");
        lv_obj_set_style_text_font(countdown_unit_label_, &kViFont20, 0);
        lv_obj_set_style_text_align(countdown_unit_label_, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(countdown_unit_label_, LV_ALIGN_RIGHT_MID, -kCountdownUnitOffsetRight, 0);

        countdown_label_ = lv_label_create(header_bar_);
        lv_obj_set_width(countdown_label_, kCountdownDigitWidth);
        lv_obj_set_style_text_font(countdown_label_, &kViFont20, 0);
        lv_obj_set_style_text_align(countdown_label_, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(countdown_label_, LV_ALIGN_RIGHT_MID, -kCountdownDigitOffsetRight, 0);

        online_dot_ = lv_obj_create(header_bar_);
        InitPlainBox(online_dot_, kOnlineDotSize, kOnlineDotSize);
        lv_obj_set_style_radius(online_dot_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(online_dot_, LV_OPA_COVER, 0);
        lv_obj_align(online_dot_, LV_ALIGN_RIGHT_MID, -kOnlineDotMarginRight, 0);

        // Conversation body (168 px): reply bubble on top (left), heard bubble on bottom (right)
        body_ = lv_obj_create(status_root_);
        InitPlainBox(body_, DISPLAY_WIDTH, kBodyHeight);
        lv_obj_set_pos(body_, 0, kTopBarHeight);
        lv_obj_set_style_bg_opa(body_, LV_OPA_COVER, 0);

        // Reply bubble (left-aligned, lighter fill, 4 px left accent stripe)
        reply_bubble_ = lv_obj_create(body_);
        InitPlainBox(reply_bubble_, kBubbleWidth, kBubbleHeight);
        lv_obj_set_pos(reply_bubble_, kBubbleMarginX, kBodyPadY);
        lv_obj_set_style_radius(reply_bubble_, kBubbleRadius, 0);
        lv_obj_set_style_bg_opa(reply_bubble_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(reply_bubble_, kBubbleStripeWidth, 0);
        lv_obj_set_style_border_side(reply_bubble_, LV_BORDER_SIDE_LEFT, 0);

        reply_label_ = lv_label_create(reply_bubble_);
        lv_obj_set_width(reply_label_, kReplyLabelWidth);
        lv_obj_set_style_text_font(reply_label_, &kViFont20, 0);
        lv_obj_set_style_text_align(reply_label_, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_max_height(reply_label_, kBubbleTextMaxHeight, 0);
        lv_label_set_long_mode(reply_label_, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(reply_label_, LV_ALIGN_LEFT_MID, kBubblePadX, 0);

        // Heard bubble (right-aligned, darker fill, subtle 1 px border)
        heard_bubble_ = lv_obj_create(body_);
        InitPlainBox(heard_bubble_, kBubbleWidth, kBubbleHeight);
        lv_obj_set_pos(heard_bubble_, DISPLAY_WIDTH - kBubbleMarginX - kBubbleWidth,
                       kBodyPadY + kBubbleHeight + kBubbleGapY);
        lv_obj_set_style_radius(heard_bubble_, kBubbleRadius, 0);
        lv_obj_set_style_bg_opa(heard_bubble_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(heard_bubble_, kBubbleBorderWidth, 0);
        lv_obj_set_style_border_side(heard_bubble_, LV_BORDER_SIDE_FULL, 0);

        heard_label_ = lv_label_create(heard_bubble_);
        lv_obj_set_width(heard_label_, kHeardLabelWidth);
        lv_obj_set_style_text_font(heard_label_, &kViFont20, 0);
        lv_obj_set_style_text_align(heard_label_, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_max_height(heard_label_, kBubbleTextMaxHeight, 0);
        lv_label_set_long_mode(heard_label_, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(heard_label_, LV_ALIGN_RIGHT_MID, -kBubblePadX, 0);

        // Bottom bar (44 px): full-width hold-to-talk button (FR-009 fallback)
        ptt_btn_ = lv_obj_create(status_root_);
        InitPlainBox(ptt_btn_, DISPLAY_WIDTH, kPttHeight);
        lv_obj_align(ptt_btn_, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(ptt_btn_, lv_color_hex(kPttIdleColorHex), 0);
        lv_obj_set_style_bg_opa(ptt_btn_, LV_OPA_COVER, 0);
        lv_obj_add_flag(ptt_btn_, LV_OBJ_FLAG_CLICKABLE);

        ptt_label_ = lv_label_create(ptt_btn_);
        lv_label_set_text(ptt_label_, "Giữ để nói");
        lv_obj_set_style_text_font(ptt_label_, &kViFont20, 0);
        lv_obj_set_style_text_color(ptt_label_, lv_color_hex(kColorWhite), 0);
        lv_obj_set_style_text_align(ptt_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(ptt_label_);

        lv_obj_add_event_cb(ptt_btn_, PttEventCb, LV_EVENT_PRESSED, this);
        lv_obj_add_event_cb(ptt_btn_, PttEventCb, LV_EVENT_RELEASED, this);
        lv_obj_add_event_cb(ptt_btn_, PttEventCb, LV_EVENT_PRESS_LOST, this);

        lv_obj_move_foreground(status_root_);
        ApplyVisualState();

#ifdef CONFIG_BANBANHMI_TOUCH_TEST
        // Bubble press events from non-PTT containers so taps anywhere on screen are logged
        lv_obj_add_flag(status_root_, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(header_bar_, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(body_, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(reply_bubble_, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(heard_bubble_, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_event_cb(screen, ScreenTouchTestCb, LV_EVENT_PRESSED, this);

        // Five target dots (4 corners 10 px from the edges + centre) on the top layer.
        // Not clickable, so they never swallow a touch. Each press logs the distance to the nearest dot.
        for (const auto& t : kTouchTargets) {
            lv_obj_t* dot = lv_obj_create(lv_layer_top());
            lv_obj_set_size(dot, 8, 8);
            lv_obj_set_pos(dot, t.x - 4, t.y - 4);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_border_width(dot, 0, 0);
            lv_obj_set_style_pad_all(dot, 0, 0);
            lv_obj_set_style_bg_color(dot, lv_color_hex(0xFFEB3B), 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        }
#endif
    }

    // Called from MCP tool `self.screen.show_status {state, text, countdown_s?}` on the main task.
    void ShowStatus(const std::string& state, const std::string& text, int countdown_s = 0) {
        DisplayLockGuard lock(this);
        StopCountdownTimer();

        StatusState mapped = StatusState::kClosed;
        if (state == "listening") {
            mapped = StatusState::kListening;
        } else if (state == "processing") {
            mapped = StatusState::kProcessing;
        } else if (state == "awaiting_ok") {
            mapped = StatusState::kAwaitingOk;
        }

        status_state_ = mapped;
        server_status_active_ = (mapped != StatusState::kClosed);

        if (mapped == StatusState::kClosed) {
            heard_text_.clear();
            reply_text_.clear();
        } else {
            const std::string next_heard =
                IsFallbackStatusText(text) ? std::string() : SanitizeForViFont(text);
            // Clear the previous reply when a new seller utterance arrives so it does not linger
            // beside a fresh request.
            const bool is_new_heard =
                !next_heard.empty() && next_heard != heard_text_ &&
                (mapped == StatusState::kListening || mapped == StatusState::kProcessing);
            if (is_new_heard) {
                reply_text_.clear();
            }
            heard_text_ = next_heard;
        }

        if (mapped == StatusState::kAwaitingOk && countdown_s > 0) {
            countdown_remaining_ = countdown_s;
            countdown_active_ = true;
            countdown_timer_ = lv_timer_create(CountdownTimerCb, kCountdownIntervalMs, this);
        } else {
            countdown_remaining_ = 0;
            countdown_active_ = false;
        }

        ApplyVisualState();
    }

    // Called from the connectivity monitor task: show or clear "lost connection, retrying".
    void SetOffline(bool offline) {
        DisplayLockGuard lock(this);
        offline_ = offline;
        if (offline_) {
            StopCountdownTimer();
            countdown_active_ = false;
            countdown_remaining_ = 0;
        }
        ApplyVisualState();
    }

    // Called on the main task via ScreenStateIndicator when the device state changes.
    // Shows the listening look when the board enters listening on its own (e.g. button press or
    // wake word) before any server `show_status` arrives; a later `show_status` overrides it.
    void OnDeviceStateChanged(DeviceState state) {
        DisplayLockGuard lock(this);
        if (status_root_ != nullptr) {
            const bool show_setup_ui = (state == kDeviceStateWifiConfiguring ||
                                        state == kDeviceStateActivating ||
                                        state == kDeviceStateUpgrading);
            if (show_setup_ui) {
                lv_obj_add_flag(status_root_, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_remove_flag(status_root_, LV_OBJ_FLAG_HIDDEN);
            }
        }

        const bool now_listening = (state == kDeviceStateListening);
        if (now_listening) {
            device_listening_ = true;
            if (!server_status_active_) {
                StopCountdownTimer();
                countdown_active_ = false;
                status_state_ = StatusState::kListening;
                ApplyVisualState();
            }
            return;
        }

        device_listening_ = false;
        if (state == kDeviceStateIdle && !server_status_active_ && !ptt_pressed_) {
            StopCountdownTimer();
            countdown_active_ = false;
            status_state_ = StatusState::kClosed;
            ApplyVisualState();
        }
    }

    // Receives the board's spoken sentence (`tts sentence_start`) and `self.screen.show_text`.
    // Keeps the text visible in the reply bubble after speaking ends until a new heard sentence
    // arrives or the session closes.
    void SetChatMessage(const char* role, const char* content) override {
        SpiLcdDisplay::SetChatMessage(role, content);
        if (role == nullptr || std::strcmp(role, "assistant") != 0) {
            return;
        }
        DisplayLockGuard lock(this);
        if (content == nullptr || content[0] == '\0') {
            reply_text_.clear();
        } else {
            reply_text_ = SanitizeForViFont(content);
        }
        ApplyVisualState();
    }

private:
    lv_obj_t* status_root_ = nullptr;
    lv_obj_t* header_bar_ = nullptr;
    lv_obj_t* online_dot_ = nullptr;
    lv_obj_t* body_ = nullptr;

    lv_obj_t* icon_box_ = nullptr;
    lv_obj_t* icon_closed_ring_ = nullptr;
    lv_obj_t* icon_listening_halo_ = nullptr;
    lv_obj_t* icon_processing_dots_[3] = {nullptr, nullptr, nullptr};
    lv_obj_t* icon_awaiting_ring_ = nullptr;
    lv_obj_t* icon_offline_ring_ = nullptr;

    lv_obj_t* state_label_ = nullptr;
    lv_obj_t* countdown_label_ = nullptr;
    lv_obj_t* countdown_unit_label_ = nullptr;

    lv_obj_t* reply_bubble_ = nullptr;
    lv_obj_t* reply_label_ = nullptr;
    lv_obj_t* heard_bubble_ = nullptr;
    lv_obj_t* heard_label_ = nullptr;

    lv_obj_t* ptt_btn_ = nullptr;
    lv_obj_t* ptt_label_ = nullptr;

    lv_timer_t* countdown_timer_ = nullptr;
    StatusState status_state_ = StatusState::kClosed;
    std::string reply_text_;
    std::string heard_text_;
    int countdown_remaining_ = 0;
    bool countdown_active_ = false;
    bool server_status_active_ = false;
    bool offline_ = false;
    bool device_listening_ = false;
    // true between an accepted press and its release: a rejected press (offline) never starts a turn
    bool ptt_pressed_ = false;

    static void InitPlainBox(lv_obj_t* obj, int w, int h) {
        lv_obj_set_size(obj, w, h);
        lv_obj_set_style_radius(obj, 0, 0);
        lv_obj_set_style_border_width(obj, 0, 0);
        lv_obj_set_style_pad_all(obj, 0, 0);
        lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    }

    // Server sends these fixed placeholder texts in `show_status` before any transcript exists.
    static bool IsFallbackStatusText(const std::string& text) {
        if (text.empty()) {
            return true;
        }
        static constexpr const char* kFallbackTexts[] = {
            "Đang nghe…",
            "Đang xử lý…",
            "Chờ xác nhận…",
            "Đã đóng phiên",
            "Đang nghe...",
            "Đang xử lý...",
            "Chờ xác nhận...",
        };
        for (const char* fallback : kFallbackTexts) {
            if (text == fallback) {
                return true;
            }
        }
        return false;
    }

    // Replaces common punctuation missing from kViFont20 (such as U+2026 ellipsis, smart quotes,
    // en/em dashes, currency symbol) and skips any remaining unsupported codepoint so LVGL never
    // draws missing-glyph boxes.
    static std::string SanitizeForViFont(const std::string& input) {
        std::string out;
        out.reserve(input.size());

        const size_t len = input.size();
        size_t i = 0;
        while (i < len) {
            const uint8_t b0 = static_cast<uint8_t>(input[i]);
            uint32_t cp = 0;
            size_t seq_len = 1;

            if (b0 < 0x80u) {
                cp = b0;
            } else if ((b0 & 0xE0u) == 0xC0u && i + 1 < len) {
                const uint8_t b1 = static_cast<uint8_t>(input[i + 1]);
                if ((b1 & 0xC0u) == 0x80u) {
                    cp = ((b0 & 0x1Fu) << 6) | (b1 & 0x3Fu);
                    seq_len = 2;
                }
            } else if ((b0 & 0xF0u) == 0xE0u && i + 2 < len) {
                const uint8_t b1 = static_cast<uint8_t>(input[i + 1]);
                const uint8_t b2 = static_cast<uint8_t>(input[i + 2]);
                if ((b1 & 0xC0u) == 0x80u && (b2 & 0xC0u) == 0x80u) {
                    cp = ((b0 & 0x0Fu) << 12) | ((b1 & 0x3Fu) << 6) | (b2 & 0x3Fu);
                    seq_len = 3;
                }
            } else if ((b0 & 0xF8u) == 0xF0u && i + 3 < len) {
                const uint8_t b1 = static_cast<uint8_t>(input[i + 1]);
                const uint8_t b2 = static_cast<uint8_t>(input[i + 2]);
                const uint8_t b3 = static_cast<uint8_t>(input[i + 3]);
                if ((b1 & 0xC0u) == 0x80u && (b2 & 0xC0u) == 0x80u && (b3 & 0xC0u) == 0x80u) {
                    cp = ((b0 & 0x07u) << 18) | ((b1 & 0x3Fu) << 12) | ((b2 & 0x3Fu) << 6) |
                         (b3 & 0x3Fu);
                    seq_len = 4;
                }
            }

            switch (cp) {
                case 0x2026u:  // horizontal ellipsis
                    out.append("...");
                    break;
                case 0x2013u:  // en dash
                case 0x2014u:  // em dash
                case 0x2212u:  // minus sign
                    out.push_back('-');
                    break;
                case 0x201Cu:  // left double quotation mark
                case 0x201Du:  // right double quotation mark
                case 0x00ABu:  // left guillemet
                case 0x00BBu:  // right guillemet
                    out.push_back('"');
                    break;
                case 0x2018u:  // left single quotation mark
                case 0x2019u:  // right single quotation mark
                    out.push_back('\'');
                    break;
                case 0x20ABu:          // dong sign -> lowercase d-stroke (U+0111, present in font)
                    out.append("\xC4\x91");
                    break;
                case 0x00D7u:  // multiplication sign
                    out.push_back('x');
                    break;
                case 0x00A0u:  // non-breaking space
                case '\t':
                case '\n':
                    out.push_back(' ');
                    break;
                case '\r':
                case 0:
                    break;
                default:
                    if (ViFont20HasGlyph(cp)) {
                        out.append(input, i, seq_len);
                    }
                    break;
            }
            i += seq_len;
        }
        return out;
    }

    // Caller must hold the display lock. All icons fit inside the 20x20 top-bar icon box.
    void BuildStateIcons() {
        // Closed: hollow ring
        icon_closed_ring_ = lv_obj_create(icon_box_);
        InitPlainBox(icon_closed_ring_, 16, 16);
        lv_obj_set_style_radius(icon_closed_ring_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(icon_closed_ring_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(icon_closed_ring_, 3, 0);
        lv_obj_set_style_border_color(icon_closed_ring_, lv_color_hex(kPaletteClosed.accent), 0);
        lv_obj_center(icon_closed_ring_);

        // Listening: filled white dot inside semi-transparent halo
        icon_listening_halo_ = lv_obj_create(icon_box_);
        InitPlainBox(icon_listening_halo_, 18, 18);
        lv_obj_set_style_radius(icon_listening_halo_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(icon_listening_halo_, lv_color_hex(kPaletteListening.accent), 0);
        lv_obj_set_style_bg_opa(icon_listening_halo_, LV_OPA_40, 0);
        lv_obj_center(icon_listening_halo_);

        lv_obj_t* listening_dot = lv_obj_create(icon_listening_halo_);
        InitPlainBox(listening_dot, 10, 10);
        lv_obj_set_style_radius(listening_dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(listening_dot, lv_color_hex(kColorWhite), 0);
        lv_obj_set_style_bg_opa(listening_dot, LV_OPA_COVER, 0);
        lv_obj_center(listening_dot);

        // Processing: three horizontal dots
        static constexpr int kDotOffsets[3] = {-7, 0, 7};
        static constexpr int kDotSizes[3] = {5, 7, 5};
        for (int i = 0; i < 3; ++i) {
            icon_processing_dots_[i] = lv_obj_create(icon_box_);
            InitPlainBox(icon_processing_dots_[i], kDotSizes[i], kDotSizes[i]);
            lv_obj_set_style_radius(icon_processing_dots_[i], LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(
                icon_processing_dots_[i],
                lv_color_hex(i == 1 ? kColorWhite : kPaletteProcessing.accent), 0);
            lv_obj_set_style_bg_opa(icon_processing_dots_[i], i == 1 ? LV_OPA_COVER : LV_OPA_70, 0);
            lv_obj_align(icon_processing_dots_[i], LV_ALIGN_CENTER, kDotOffsets[i], 0);
        }

        // Awaiting OK: white ring with inner target dot
        icon_awaiting_ring_ = lv_obj_create(icon_box_);
        InitPlainBox(icon_awaiting_ring_, 18, 18);
        lv_obj_set_style_radius(icon_awaiting_ring_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(icon_awaiting_ring_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(icon_awaiting_ring_, 2, 0);
        lv_obj_set_style_border_color(icon_awaiting_ring_, lv_color_hex(kColorWhite), 0);
        lv_obj_center(icon_awaiting_ring_);

        lv_obj_t* ok_dot = lv_obj_create(icon_awaiting_ring_);
        InitPlainBox(ok_dot, 8, 8);
        lv_obj_set_style_radius(ok_dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(ok_dot, lv_color_hex(kColorWhite), 0);
        lv_obj_set_style_bg_opa(ok_dot, LV_OPA_COVER, 0);
        lv_obj_center(ok_dot);

        // Offline: ring with exclamation mark drawn from two boxes so it fits cleanly in 18x18
        icon_offline_ring_ = lv_obj_create(icon_box_);
        InitPlainBox(icon_offline_ring_, 18, 18);
        lv_obj_set_style_radius(icon_offline_ring_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(icon_offline_ring_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(icon_offline_ring_, 2, 0);
        lv_obj_set_style_border_color(icon_offline_ring_, lv_color_hex(kPaletteOffline.accent), 0);
        lv_obj_center(icon_offline_ring_);

        lv_obj_t* bang_bar = lv_obj_create(icon_offline_ring_);
        InitPlainBox(bang_bar, 2, 6);
        lv_obj_set_style_bg_color(bang_bar, lv_color_hex(kColorWhite), 0);
        lv_obj_set_style_bg_opa(bang_bar, LV_OPA_COVER, 0);
        lv_obj_align(bang_bar, LV_ALIGN_CENTER, 0, -2);

        lv_obj_t* bang_dot = lv_obj_create(icon_offline_ring_);
        InitPlainBox(bang_dot, 2, 2);
        lv_obj_set_style_bg_color(bang_dot, lv_color_hex(kColorWhite), 0);
        lv_obj_set_style_bg_opa(bang_dot, LV_OPA_COVER, 0);
        lv_obj_align(bang_dot, LV_ALIGN_CENTER, 0, 4);
    }

    // Caller must hold the display lock.
    void StopCountdownTimer() {
        if (countdown_timer_ != nullptr) {
            lv_timer_delete(countdown_timer_);
            countdown_timer_ = nullptr;
        }
    }

    // Caller must hold the display lock.
    void SetIconVisibility(bool show_closed, bool show_listening, bool show_processing,
                           bool show_awaiting, bool show_offline) {
        auto set_vis = [](lv_obj_t* obj, bool visible) {
            if (obj == nullptr) {
                return;
            }
            if (visible) {
                lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
            }
        };
        set_vis(icon_closed_ring_, show_closed);
        set_vis(icon_listening_halo_, show_listening);
        for (lv_obj_t* dot : icon_processing_dots_) {
            set_vis(dot, show_processing);
        }
        set_vis(icon_awaiting_ring_, show_awaiting);
        set_vis(icon_offline_ring_, show_offline);
    }

    // Caller must hold the display lock.
    void SetCountdownVisible(bool visible) {
        if (countdown_label_ == nullptr || countdown_unit_label_ == nullptr) {
            return;
        }
        if (visible) {
            lv_obj_remove_flag(countdown_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(countdown_unit_label_, LV_OBJ_FLAG_HIDDEN);
            UpdateCountdownLabel();
        } else {
            lv_obj_add_flag(countdown_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(countdown_unit_label_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Caller must hold the display lock.
    void UpdateCountdownLabel() {
        if (countdown_label_ == nullptr) {
            return;
        }
        int sec = countdown_remaining_;
        if (sec < 0) {
            sec = 0;
        } else if (sec > 99) {
            sec = 99;
        }
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%d", sec);
        lv_label_set_text(countdown_label_, buf);
        lv_obj_set_style_text_color(
            countdown_label_,
            lv_color_hex(sec <= kCountdownUrgentThreshold ? kColorUrgentYellow : kColorWhite), 0);
    }

    // Caller must hold the display lock.
    void ApplyVisualState() {
        if (status_root_ == nullptr) {
            return;
        }

        // Effective visual state: offline overrides unless user is actively holding PTT;
        // holding PTT always shows listening.
        const bool show_offline = offline_ && !ptt_pressed_;
        const StatusState effective = ptt_pressed_ ? StatusState::kListening : status_state_;

        StatePalette pal = kPaletteClosed;
        if (show_offline) {
            pal = kPaletteOffline;
        } else {
            switch (effective) {
                case StatusState::kListening:
                    pal = kPaletteListening;
                    break;
                case StatusState::kProcessing:
                    pal = kPaletteProcessing;
                    break;
                case StatusState::kAwaitingOk:
                    pal = kPaletteAwaitingOk;
                    break;
                case StatusState::kClosed:
                default:
                    pal = kPaletteClosed;
                    break;
            }
        }

        // Apply palette to top bar, body, and bubbles
        lv_obj_set_style_bg_color(status_root_, lv_color_hex(pal.bg), 0);
        lv_obj_set_style_bg_color(header_bar_, lv_color_hex(pal.bar), 0);
        lv_obj_set_style_border_color(header_bar_, lv_color_hex(pal.sep), 0);
        lv_obj_set_style_text_color(countdown_unit_label_, lv_color_hex(pal.accent), 0);
        lv_obj_set_style_bg_color(online_dot_,
                                  lv_color_hex(offline_ ? kColorOfflineDot : kColorOnlineDot), 0);

        lv_obj_set_style_bg_color(body_, lv_color_hex(pal.bg), 0);
        lv_obj_set_style_bg_color(reply_bubble_, lv_color_hex(pal.sep), 0);
        lv_obj_set_style_border_color(reply_bubble_, lv_color_hex(pal.accent), 0);
        lv_obj_set_style_bg_color(heard_bubble_, lv_color_hex(pal.bar), 0);
        lv_obj_set_style_border_color(heard_bubble_, lv_color_hex(pal.sep), 0);

        // Top bar state icon, state word, and countdown
        if (show_offline) {
            SetIconVisibility(false, false, false, false, true);
            lv_label_set_text(state_label_, "Mất kết nối, đang thử lại");
            SetCountdownVisible(false);
        } else {
            switch (effective) {
                case StatusState::kListening:
                    SetIconVisibility(false, true, false, false, false);
                    lv_label_set_text(state_label_, "Đang nghe");
                    SetCountdownVisible(false);
                    break;
                case StatusState::kProcessing:
                    SetIconVisibility(false, false, true, false, false);
                    lv_label_set_text(state_label_, "Đang xử lý");
                    SetCountdownVisible(false);
                    break;
                case StatusState::kAwaitingOk:
                    SetIconVisibility(false, false, false, true, false);
                    lv_label_set_text(state_label_, "Chờ ok");
                    SetCountdownVisible(countdown_active_);
                    break;
                case StatusState::kClosed:
                default:
                    SetIconVisibility(true, false, false, false, false);
                    lv_label_set_text(state_label_, "Đã đóng");
                    SetCountdownVisible(false);
                    break;
            }
        }

        // Reply bubble (top, left): board's last spoken sentence or dim placeholder
        if (!reply_text_.empty()) {
            lv_obj_set_style_text_color(reply_label_, lv_color_hex(kColorWhite), 0);
            lv_label_set_text(reply_label_, reply_text_.c_str());
        } else {
            lv_obj_set_style_text_color(reply_label_, lv_color_hex(pal.accent), 0);
            lv_label_set_text(reply_label_, kReplyPlaceholder);
        }

        // Heard bubble (bottom, right): seller's utterance (with trailing "..." when processing)
        // or dim placeholder
        if (!heard_text_.empty()) {
            lv_obj_set_style_text_color(heard_label_, lv_color_hex(kColorWhite), 0);
            if (!show_offline && effective == StatusState::kProcessing &&
                (heard_text_.size() < 3 ||
                 heard_text_.compare(heard_text_.size() - 3, 3, "...") != 0)) {
                std::string processing_text = heard_text_ + "...";
                lv_label_set_text(heard_label_, processing_text.c_str());
            } else {
                lv_label_set_text(heard_label_, heard_text_.c_str());
            }
        } else {
            lv_obj_set_style_text_color(heard_label_, lv_color_hex(pal.accent), 0);
            lv_label_set_text(heard_label_, kHeardPlaceholder);
        }

        // Bottom PTT bar (keeps its pressed red colour)
        if (ptt_pressed_) {
            lv_obj_set_style_bg_color(ptt_btn_, lv_color_hex(kPttActiveColorHex), 0);
            lv_label_set_text(ptt_label_, "Đang nghe...");
        } else if (offline_) {
            lv_obj_set_style_bg_color(ptt_btn_, lv_color_hex(kPttOfflineColorHex), 0);
            lv_label_set_text(ptt_label_, "Mất kết nối");
        } else if (device_listening_ || effective == StatusState::kListening) {
            lv_obj_set_style_bg_color(ptt_btn_, lv_color_hex(kPttListeningColorHex), 0);
            lv_label_set_text(ptt_label_, "Giữ để nói");
        } else {
            lv_obj_set_style_bg_color(ptt_btn_, lv_color_hex(kPttIdleColorHex), 0);
            lv_label_set_text(ptt_label_, "Giữ để nói");
        }
    }

    static void CountdownTimerCb(lv_timer_t* timer) {
        auto* self = static_cast<Es3c28pDisplay*>(lv_timer_get_user_data(timer));
        if (self == nullptr) {
            return;
        }
        if (self->countdown_remaining_ > 0) {
            --self->countdown_remaining_;
        }
        self->UpdateCountdownLabel();
        if (self->countdown_remaining_ <= 0) {
            self->StopCountdownTimer();
        }
    }

    static void PttEventCb(lv_event_t* e) {
        auto* self = static_cast<Es3c28pDisplay*>(lv_event_get_user_data(e));
        lv_event_code_t code = lv_event_get_code(e);

#ifdef CONFIG_BANBANHMI_TOUCH_TEST
        lv_indev_t* indev = lv_indev_active();
        lv_point_t p = {0, 0};
        if (indev != nullptr) {
            lv_indev_get_point(indev, &p);
        }
        const char* event_name = "press_lost";
        if (code == LV_EVENT_PRESSED) {
            event_name = "press";
        } else if (code == LV_EVENT_RELEASED) {
            event_name = "release";
        }
        ESP_LOGI(TAG, "Touch PTT %s at (%d, %d) t=%lld ms", event_name, static_cast<int>(p.x),
                 static_cast<int>(p.y), static_cast<long long>(esp_timer_get_time() / 1000));
        if (code == LV_EVENT_PRESSED) {
            LogNearestTarget(p);
        }
#endif

        // LVGL callbacks already run with the display lock held; do not take DisplayLockGuard
        if (code == LV_EVENT_PRESSED) {
            if (IsServerOffline()) {
                // FR-028: no conversation while the server is unreachable, say so instead
                PlayBoardPhrase("offline");
                return;
            }
            self->ptt_pressed_ = true;
            self->StopCountdownTimer();
            self->countdown_active_ = false;
            self->ApplyVisualState();
            Application::GetInstance().StartListening();
        } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
            if (!self->ptt_pressed_) {
                return;
            }
            self->ptt_pressed_ = false;
            if (!self->server_status_active_) {
                self->status_state_ = StatusState::kClosed;
            }
            self->ApplyVisualState();
            NoteRelease();
            Application::GetInstance().StopListening();
        }
    }

#ifdef CONFIG_BANBANHMI_TOUCH_TEST
    static void ScreenTouchTestCb(lv_event_t* e) {
        (void)e;
        lv_indev_t* indev = lv_indev_active();
        lv_point_t p = {0, 0};
        if (indev != nullptr) {
            lv_indev_get_point(indev, &p);
        }
        ESP_LOGI(TAG, "Touch screen press at (%d, %d) t=%lld ms", static_cast<int>(p.x),
                 static_cast<int>(p.y), static_cast<long long>(esp_timer_get_time() / 1000));
        LogNearestTarget(p);
    }

    struct TouchTarget {
        int x;
        int y;
        const char* name;
    };
    static constexpr TouchTarget kTouchTargets[5] = {
        {10, 10, "top-left"},
        {DISPLAY_WIDTH - 10, 10, "top-right"},
        {10, DISPLAY_HEIGHT - 10, "bottom-left"},
        {DISPLAY_WIDTH - 10, DISPLAY_HEIGHT - 10, "bottom-right"},
        {DISPLAY_WIDTH / 2, DISPLAY_HEIGHT / 2, "centre"},
    };

    static void LogNearestTarget(const lv_point_t& p) {
        const TouchTarget* best = &kTouchTargets[0];
        int best_d2 = INT32_MAX;
        for (const auto& t : kTouchTargets) {
            int dx = p.x - t.x;
            int dy = p.y - t.y;
            int d2 = dx * dx + dy * dy;
            if (d2 < best_d2) {
                best_d2 = d2;
                best = &t;
            }
        }
        // Report per-axis error and the Euclidean distance (integer sqrt is enough here)
        int dx = p.x - best->x;
        int dy = p.y - best->y;
        int dist = 0;
        while ((dist + 1) * (dist + 1) <= best_d2) {
            dist++;
        }
        ESP_LOGI(TAG, "Touch target %s dx=%d dy=%d dist=%d px", best->name, dx, dy, dist);
    }
#endif
};

// This board has no physical LED; hook Application's Led::OnStateChanged() path to drive
// a silent on-screen listening cue without touching upstream core code.
class ScreenStateIndicator : public Led {
public:
    explicit ScreenStateIndicator(Es3c28pDisplay* display) : display_(display) {}

    void OnStateChanged() override {
        if (display_ == nullptr) {
            return;
        }
        display_->OnDeviceStateChanged(Application::GetInstance().GetDeviceState());
    }

private:
    Es3c28pDisplay* display_;
};

class BanBanhMiEs3c28pBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    Button boot_button_;
    bool boot_talking_ = false;
    Es3c28pDisplay* display_ = nullptr;

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags =
                {
                    .enable_internal_pullup = 1,
                },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_SPI_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_SPI_SCK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    // Start with the FM8002E amplifier OFF (GPIO1 HIGH); the codec driver turns it on with the output
    void InitializeAudioPaEnable() {
        gpio_config_t io_conf = {};
        io_conf.pin_bit_mask = (1ULL << AUDIO_PA_ENABLE_PIN);
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&io_conf);
        gpio_set_level(AUDIO_PA_ENABLE_PIN, 1);
    }

    void InitializeDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_SPI_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io));

        ESP_LOGD(TAG, "Install ILI9341 LCD driver");
        const ili9341_vendor_config_t vendor_config = {
            .init_cmds = &kVendorSpecificInit[0],
            .init_cmds_size = sizeof(kVendorSpecificInit) / sizeof(ili9341_lcd_init_cmd_t),
        };

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.flags.reset_active_high = 1;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.vendor_config = const_cast<ili9341_vendor_config_t*>(&vendor_config);
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, true);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);

        display_ = new Es3c28pDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                      DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                      DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeTouch() {
        esp_lcd_touch_handle_t tp = nullptr;
        // FT6336G reports native portrait coordinates (240x320). esp_lcd_touch applies mirror_x
        // and mirror_y BEFORE swap_xy using x_max/y_max, so x_max/y_max must be the native portrait
        // range and the flags mirror the display's ILI9341 MADCTL rotation flags.
        esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_SWAP_XY ? DISPLAY_HEIGHT : DISPLAY_WIDTH,
            .y_max = DISPLAY_SWAP_XY ? DISPLAY_WIDTH : DISPLAY_HEIGHT,
            .rst_gpio_num = TOUCH_RST_PIN,
            .int_gpio_num = TOUCH_INT_PIN,
            .levels =
                {
                    .reset = 0,
                    .interrupt = 0,
                },
            .flags =
                {
                    .swap_xy = DISPLAY_SWAP_XY ? 1u : 0u,
                    .mirror_x = DISPLAY_MIRROR_X ? 1u : 0u,
                    .mirror_y = DISPLAY_MIRROR_Y ? 1u : 0u,
                },
        };
        esp_lcd_panel_io_handle_t tp_io_handle = nullptr;
        esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
        tp_io_config.scl_speed_hz = 400 * 1000;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io_handle));
        ESP_LOGI(TAG, "Initialize touch controller FT6336G");
        ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_ft5x06(tp_io_handle, &tp_cfg, &tp));

        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lv_display_get_default(),
            .handle = tp,
        };
        lvgl_port_add_touch(&touch_cfg);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
            }
        });
        boot_button_.OnPressDown([this]() {
            auto& app = Application::GetInstance();
            auto state = app.GetDeviceState();
            if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
                return;
            }
            if (IsServerOffline()) {
                PlayBoardPhrase("offline");
                return;
            }
            boot_talking_ = true;
            app.StartListening();
        });
        boot_button_.OnPressUp([this]() {
            if (!boot_talking_) {
                return;
            }
            boot_talking_ = false;
            NoteRelease();
            Application::GetInstance().StopListening();
        });
    }

public:
    BanBanhMiEs3c28pBoard() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        InitializeSpi();
        InitializeAudioPaEnable();
        InitializeDisplay();
        InitializeTouch();
        InitializeButtons();
        GetBacklight()->SetBrightness(100);
        RegisterBanBanhMiDeviceTools(
            display_,
            [this](const std::string& state, const std::string& text, int countdown_s) {
                if (display_ != nullptr) {
                    display_->ShowStatus(state, text, countdown_s);
                }
            });
        StartConnectivityMonitor([this](bool offline) { display_->SetOffline(offline); });
    }

    virtual Led* GetLed() override {
        static ScreenStateIndicator indicator(display_);
        return &indicator;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BanBanhMiCodec audio_codec(
            i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, true, true);
        static const bool gain_set = (audio_codec.SetInputGain(kMicGainDb), true);
        (void)gain_set;
        // Speaker at full volume (stored volume in NVS would otherwise win).
        static const bool vol_set = (audio_codec.SetOutputVolume(kSpeakerVolume), true);
        (void)vol_set;
        return &audio_codec;
    }

    virtual Display* GetDisplay() override { return display_; }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }
};

DECLARE_BOARD(BanBanhMiEs3c28pBoard);
