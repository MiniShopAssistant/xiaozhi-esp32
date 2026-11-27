# BanBanhMi ES3C28P (LCDWIKI 2.8" ESP32-S3)

Board tùy chỉnh cho mạch **LCDWIKI ES3C28P** (ESP32-S3 N16R8):
- Màn hình SPI **ILI9341V** 2.8" ngang 320x240 (`DISPLAY_SWAP_XY=true`, `DISPLAY_MIRROR_X=true`, `DISPLAY_MIRROR_Y=false`)
- Cảm ứng điện dung **FT6336G** (chung bus I2C với codec, ánh xạ tọa độ khớp hướng màn hình ngang)
- Audio codec **ES8311** + mạch khuếch đại loa **FM8002E**
- Màn hình trạng thái rảnh tay gồm thanh trên 24 px (`BanBanhMi` + chấm trạng thái kết nối), vùng giữa chia hai cột (cột trái 212 px: biểu tượng + tên trạng thái 38 px + chữ `vừa nghe` và tối đa 2 dòng nội dung; cột phải 108 px: số đếm ngược 72 px + `giây` khi ở `awaiting_ok` hoặc gợi ý ngắn ở các trạng thái khác), và thanh dưới 44 px là nút cảm ứng **"Giữ để nói"** (FR-009 dự phòng). Nút **BOOT** vẫn dùng làm nút nhấn-giữ dự phòng (và nhấn khi khởi động để vào chế độ cấu hình Wi-Fi).

## Trạng thái màn hình

Mỗi trạng thái kết hợp đồng thời **màu nền, biểu tượng và chữ tiếng Việt** (không dùng màu làm tín hiệu duy nhất):

| Trạng thái (`state`) | Màu nền | Chữ trạng thái | Cột phải |
| --- | --- | --- | --- |
| `closed` | Xám đen (`#263238`) | `Đã đóng` / `Gọi tên để bắt đầu` | `Gọi Hi Lily` |
| `listening` | Xanh lá (`#1B5E20`) | `Đang nghe` + `vừa nghe` | `Nói đơn` |
| `processing` | Cam (`#E65100`) | `Đang xử lý` + `vừa nghe` | `...` |
| `awaiting_ok` | Xanh dương (`#0D47A1`) | `Chờ ok` / `Nói ok để chốt đơn` | Số đếm ngược (`countdown_s`) + `giây` |
| Mất kết nối (offline) | Xám (`#546E7A`) | `Mất kết nối, đang thử lại` | `...` |

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
- `self.screen.show_status`: Cập nhật màn hình trạng thái phiên nghe theo các tham số `state` (`listening` | `processing` | `awaiting_ok` | `closed`), `text` (UTF-8, tối đa 40 ký tự / 160 byte, hiển thị ở vùng `vừa nghe`), và `countdown_s` (số nguyên tuỳ chọn `0..30`, mặc định `0`). Khi `state = "awaiting_ok"` và `countdown_s > 0`, board chạy bộ đếm ngược cục bộ mỗi 1 giây ở cột phải; mọi lệnh `show_status` mới hoặc thay đổi trạng thái sẽ huỷ bộ đếm.
- `self.session.close`: Đưa thiết bị từ trạng thái đang nghe/đang nói về trạng thái nghỉ (`idle`) mà không đóng kênh WebSocket, tự động tắt xử lý giọng nói và bật lại nhận diện wake word.
