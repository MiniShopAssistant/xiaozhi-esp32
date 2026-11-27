#ifndef BANBANHMI_ES3C28P_VI_LV_FONT_H
#define BANBANHMI_ES3C28P_VI_LV_FONT_H

#include <lvgl.h>

// 4-bit anti-aliased Vietnamese bitmap fonts wrapped as LVGL lv_font_t instances
// (backed by fonts/font_vi.h: Arial Bold 38 px, 20 px, and 72 px digits).
extern const lv_font_t kViFont38;
extern const lv_font_t kViFont20;
extern const lv_font_t kViFontDigits72;

#endif  // BANBANHMI_ES3C28P_VI_LV_FONT_H
