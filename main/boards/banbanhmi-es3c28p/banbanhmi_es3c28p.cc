#include "application.h"
#include "button.h"
#include "codecs/es8311_audio_codec.h"
#include "config.h"
#include "display/lcd_display.h"
#include "wifi_board.h"

#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_lcd_ili9341.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_touch_ft5x06.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <esp_timer.h>

#define TAG "BanBanhMiEs3c28p"

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

class Es3c28pDisplay : public SpiLcdDisplay {
public:
    Es3c28pDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                   int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                   bool swap_xy)
        : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y,
                        swap_xy) {}

    void SetupUI() override {
        SpiLcdDisplay::SetupUI();

        DisplayLockGuard lock(this);
        lv_obj_t* screen = lv_screen_active();

        // Keep the emoji/status and the chat message bar in the top half, above the
        // hold-to-talk area (the parent layout anchors them to the screen centre/bottom).
        if (emoji_box_ != nullptr) {
            lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, -kPttHeight / 2);
        }
        AnchorChatBar();

        // Hold-to-talk area covering the bottom half of the portrait screen
        ptt_btn_ = lv_obj_create(screen);
        lv_obj_set_size(ptt_btn_, DISPLAY_WIDTH, kPttHeight);
        lv_obj_align(ptt_btn_, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_radius(ptt_btn_, 0, 0);
        lv_obj_set_style_border_width(ptt_btn_, 0, 0);
        lv_obj_set_style_bg_color(ptt_btn_, lv_color_hex(kPttIdleColorHex), 0);
        lv_obj_set_style_bg_opa(ptt_btn_, LV_OPA_COVER, 0);
        lv_obj_set_scrollbar_mode(ptt_btn_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(ptt_btn_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(ptt_btn_, LV_OBJ_FLAG_CLICKABLE);

        ptt_label_ = lv_label_create(ptt_btn_);
        lv_label_set_text(ptt_label_, "Giữ để nói");
        lv_obj_set_style_text_color(ptt_label_, lv_color_white(), 0);
        lv_obj_center(ptt_label_);

        lv_obj_add_event_cb(ptt_btn_, PttEventCb, LV_EVENT_PRESSED, this);
        lv_obj_add_event_cb(ptt_btn_, PttEventCb, LV_EVENT_RELEASED, this);
        lv_obj_add_event_cb(ptt_btn_, PttEventCb, LV_EVENT_PRESS_LOST, this);
        lv_obj_move_foreground(ptt_btn_);

#ifdef CONFIG_BANBANHMI_TOUCH_TEST
        // Bubble press events from upper-half containers so taps anywhere on screen are logged
        if (container_ != nullptr) {
            lv_obj_add_flag(container_, LV_OBJ_FLAG_EVENT_BUBBLE);
        }
        if (top_bar_ != nullptr) {
            lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_EVENT_BUBBLE);
        }
        if (status_bar_ != nullptr) {
            lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_EVENT_BUBBLE);
        }
        if (content_ != nullptr) {
            lv_obj_add_flag(content_, LV_OBJ_FLAG_EVENT_BUBBLE);
        }
        if (bottom_bar_ != nullptr) {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_EVENT_BUBBLE);
        }
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

    void SetChatMessage(const char* role, const char* content) override {
        SpiLcdDisplay::SetChatMessage(role, content);
        // The parent re-aligns the multiline chat bar to the screen bottom on every update.
        DisplayLockGuard lock(this);
        AnchorChatBar();
    }

private:
    static constexpr int kPttHeight = DISPLAY_HEIGHT / 2;
    static constexpr uint32_t kPttIdleColorHex = 0x1E88E5;

    // Caller must hold the display lock.
    void AnchorChatBar() {
        if (bottom_bar_ != nullptr) {
            lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, -kPttHeight);
        }
    }
    static constexpr uint32_t kPttActiveColorHex = 0xD32F2F;

    lv_obj_t* ptt_btn_ = nullptr;
    lv_obj_t* ptt_label_ = nullptr;

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
            lv_obj_set_style_bg_color(self->ptt_btn_, lv_color_hex(kPttActiveColorHex), 0);
            lv_label_set_text(self->ptt_label_, "Đang nghe…");
            Application::GetInstance().StartListening();
        } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
            lv_obj_set_style_bg_color(self->ptt_btn_, lv_color_hex(kPttIdleColorHex), 0);
            lv_label_set_text(self->ptt_label_, "Giữ để nói");
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

class BanBanhMiEs3c28pBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    Button boot_button_;
    LcdDisplay* display_ = nullptr;

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

    // Drive GPIO1 LOW to enable the on-board FM8002E speaker amplifier
    void InitializeAudioPaEnable() {
        gpio_config_t io_conf = {};
        io_conf.pin_bit_mask = (1ULL << AUDIO_PA_ENABLE_PIN);
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&io_conf);
        gpio_set_level(AUDIO_PA_ENABLE_PIN, 0);
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
        // Portrait orientation matching the panel; axis flags to be verified on hardware
        esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_WIDTH,
            .y_max = DISPLAY_HEIGHT,
            .rst_gpio_num = TOUCH_RST_PIN,
            .int_gpio_num = TOUCH_INT_PIN,
            .levels =
                {
                    .reset = 0,
                    .interrupt = 0,
                },
            .flags =
                {
                    .swap_xy = 0,
                    .mirror_x = 0,
                    .mirror_y = 0,
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
            app.StartListening();
        });
        boot_button_.OnPressUp([this]() {
            auto& app = Application::GetInstance();
            auto state = app.GetDeviceState();
            if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
                return;
            }
            app.StopListening();
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
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(
            i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, true, true);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override { return display_; }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }
};

DECLARE_BOARD(BanBanhMiEs3c28pBoard);
