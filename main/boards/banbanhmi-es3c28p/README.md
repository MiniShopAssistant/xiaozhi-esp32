# BanBanhMi ES3C28P (LCDWIKI 2.8" ESP32-S3)

Board tùy chỉnh cho mạch **LCDWIKI ES3C28P** (ESP32-S3 N16R8):
- Màn hình SPI **ILI9341V** 2.8" dọc 240x320
- Cảm ứng điện dung **FT6336G** (chung bus I2C với codec)
- Audio codec **ES8311** + mạch khuếch đại loa **FM8002E**
- Vùng cảm ứng **"Giữ để nói"** chiếm nửa dưới màn hình (nhấn giữ để nói, thả để dừng); nút **BOOT** dùng làm nút nhấn-giữ dự phòng (và nhấn khi khởi động để vào chế độ cấu hình Wi-Fi).

## Sơ đồ chân (GPIO)

| Chức năng | Tín hiệu | GPIO |
| --- | --- | --- |
| I2S Audio | MCLK / BCLK / WS / DIN / DOUT | 4 / 5 / 7 / 6 / 8 |
| I2C (ES8311 + FT6336G) | SCL / SDA | 15 / 16 |
| Audio PA (FM8002E) | PA_EN (mức thấp bật loa) | 1 |
| LCD SPI (ILI9341V) | SCK / MOSI / CS / DC | 12 / 11 / 10 / 46 |
| LCD Backlight | BL (mức cao bật đèn nền) | 45 |
| Touch (FT6336G, 0x38) | RST / INT | 18 / 17 |
| Nút nhấn | BOOT | 0 |

## Cách build

```bash
python scripts/build.py banbanhmi-es3c28p --language vi-VN --wake-word disabled
```

Để bật chế độ kiểm tra cảm ứng (ghi log tọa độ và sự kiện nhấn/thả qua serial), bật `CONFIG_BANBANHMI_TOUCH_TEST` trong `idf.py menuconfig` tại mục **Xiaozhi Assistant → BanBanhMi ES3C28P**.
