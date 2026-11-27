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
python scripts/build.py banbanhmi-es3c28p --language vi-VN --wake-word wn9_hilili_tts
```

Lệnh trên bật wake word AFE `"Hi,Lily"` (`wn9_hilili_tts`), gửi dữ liệu âm thanh wake word (`CONFIG_SEND_WAKE_WORD_DATA=y`), tắt AEC (chế độ nghe `auto`) và tắt nhận diện wake word khi đang nghe (`CONFIG_WAKE_WORD_DETECTION_IN_LISTENING=n`). Khi vào trạng thái nghe rảnh tay, màn hình hiển thị tín hiệu trực quan (không phát âm thanh để tránh mic thu lại) và vẫn giữ vùng **"Giữ để nói"** hoạt động bình thường.

Để bật chế độ kiểm tra cảm ứng (ghi log tọa độ và sự kiện nhấn/thả qua serial), bật `CONFIG_BANBANHMI_TOUCH_TEST` trong `idf.py menuconfig` tại mục **Xiaozhi Assistant → BanBanhMi ES3C28P**.

## Công cụ MCP trên thiết bị

- `self.audio.play_phrase`: Phát âm thanh thông báo tiếng Việt ngắn được nhúng sẵn theo tham số chuỗi `id`. Các giá trị `id` hợp lệ:
  - `listening`
  - `not_heard`
  - `offline`
  - `quota`
  - `too_long`
  - `not_ready`
  - `tts_fail`
- `self.screen.show_text`: Hiển thị văn bản UTF-8 (tối đa 200 byte) ở vùng tin nhắn trên màn hình theo tham số chuỗi `text` (tối đa 4 dòng, tràn sẽ rút gọn bằng dấu `...`; truyền chuỗi rỗng để xóa).
- `self.screen.show_status`: Cập nhật dòng trạng thái phiên nghe trên màn hình theo hai tham số `state` (`listening` | `processing` | `awaiting_ok` | `closed`) và `text` (UTF-8, tối đa 40 ký tự / 160 byte). Giá trị `state = "closed"` trả dòng trạng thái về chữ chờ mặc định.
- `self.session.close`: Đưa thiết bị từ trạng thái đang nghe/đang nói về trạng thái nghỉ (`idle`) mà không đóng kênh WebSocket, tự động tắt xử lý giọng nói và bật lại nhận diện wake word.
