#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_4
#define AUDIO_I2S_GPIO_WS   GPIO_NUM_7
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_5
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_6
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_8

#define AUDIO_CODEC_I2C_SCL_PIN GPIO_NUM_15
#define AUDIO_CODEC_I2C_SDA_PIN GPIO_NUM_16
#define AUDIO_CODEC_ES8311_ADDR ES8311_CODEC_DEFAULT_ADDR

// On-board FM8002E speaker amplifier: LOW = on, HIGH = off. The codec driver switches it with the
// output (pa_inverted = true), so the speaker is silent while only the microphone is in use.
// Holding it LOW all the time made every codec re-initialisation pop through the speaker, and the
// microphone next to it recorded the pop (peak 0 dBFS), which the STT model then "recognised".
#define AUDIO_CODEC_PA_PIN GPIO_NUM_1
#define AUDIO_PA_ENABLE_PIN AUDIO_CODEC_PA_PIN

#define BUILTIN_LED_GPIO        GPIO_NUM_NC
#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_NC
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_NC

// ILI9341V SPI LCD (reset shares chip EN, MISO unused)
#define DISPLAY_SPI_SCK_PIN  GPIO_NUM_12
#define DISPLAY_SPI_MOSI_PIN GPIO_NUM_11
#define DISPLAY_DC_PIN       GPIO_NUM_46
#define DISPLAY_SPI_CS_PIN   GPIO_NUM_10

// Landscape 320x240 (ILI9341 rotated 90 deg: swap_xy=true, mirror_x=true, mirror_y=false).
// If the image appears rotated 180 deg on hardware, flip to MIRROR_X=false, MIRROR_Y=true.
// Touch coordinates in banbanhmi_es3c28p.cc derive their axis flags from these macros.
#define DISPLAY_WIDTH    320
#define DISPLAY_HEIGHT   240
#define DISPLAY_MIRROR_X true
#define DISPLAY_MIRROR_Y false
#define DISPLAY_SWAP_XY  true

#define DISPLAY_OFFSET_X 0
#define DISPLAY_OFFSET_Y 0

#define DISPLAY_BACKLIGHT_PIN           GPIO_NUM_45
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false

// FT6336G capacitive touch (shares I2C bus with ES8311, address 0x38)
#define TOUCH_RST_PIN GPIO_NUM_18
#define TOUCH_INT_PIN GPIO_NUM_17

#endif  // _BOARD_CONFIG_H_
