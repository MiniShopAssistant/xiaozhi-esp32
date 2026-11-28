# BanBanhMi ES3C28P (LCDWIKI 2.8" ESP32-S3)

Board tùy chỉnh cho mạch **LCDWIKI ES3C28P** (ESP32-S3 N16R8):
- Màn hình SPI **ILI9341V** 2.8" ngang 320x240 (`DISPLAY_SWAP_XY=true`, `DISPLAY_MIRROR_X=true`, `DISPLAY_MIRROR_Y=false`)
- Cảm ứng điện dung **FT6336G** (chung bus I2C với codec, ánh xạ tọa độ khớp hướng màn hình ngang)
- Audio codec **ES8311** + mạch khuếch đại loa **FM8002E**
- Màn hình hội thoại ngang 320x240 gồm:
  - Thanh trạng thái trên cùng 28 px: biểu tượng trạng thái (20 px) + tên trạng thái (20 px đậm) + bộ đếm ngược (`<số> giây`, tô vàng khi `<= 3 giây`) khi ở `awaiting_ok` + chấm trạng thái kết nối bên phải.
  - Vùng hội thoại giữa 168 px chia hai khung bong bóng (mỗi vùng 84 px, bong bóng rộng 272 px, bo góc 10 px, tối đa 3 dòng `kViFont20`, tràn rút gọn `...`):
    - Bong bóng trên (căn trái, nền sáng hơn, vạch nhấn 4 px bên trái): câu trả lời gần nhất của máy (từ `tts sentence_start` / `self.screen.show_text`); khi trống hiện chữ mờ `"Máy sẽ trả lời ở đây"`. Giữ nguyên sau khi đọc xong, chỉ xoá khi có câu nghe mới từ người bán hoặc khi phiên đóng (`closed`).
    - Bong bóng dưới (căn phải, nền đậm hơn): câu vừa nghe của người bán (từ `self.screen.show_status`); khi trống hoặc nhận chuỗi mặc định (`"Đang nghe…"`, `"Đang xử lý…"`, `"Chờ xác nhận…"`, `"Đã đóng phiên"`) hiện chữ mờ `"Bạn nói, tôi nghe"`; khi ở `processing` tự thêm `"..."` ở cuối.
  - Thanh dưới 44 px là nút cảm ứng **"Giữ để nói"** (FR-009 dự phòng). Nút **BOOT** vẫn dùng làm nút nhấn-giữ dự phòng (và nhấn khi khởi động để vào chế độ cấu hình Wi-Fi).

## Trạng thái màn hình

Mỗi trạng thái kết hợp đồng thời **màu nền, biểu tượng và chữ tiếng Việt**; hai bong bóng hội thoại phân biệt bằng **vị trí trái/phải, độ đậm nền và vạch nhấn 4 px** (không dùng màu làm tín hiệu duy nhất):

| Trạng thái (`state`) | Màu nền | Thanh trên (biểu tượng + chữ) | Bộ đếm ngược |
| --- | --- | --- | --- |
| `closed` | Xám đen (`#263238`) | Vòng rỗng + `Đã đóng` | Ẩn (xoá cả 2 bong bóng) |
| `listening` | Xanh lá (`#1B5E20`) | Chấm sáng trong hào quang + `Đang nghe` | Ẩn |
| `processing` | Cam (`#E65100`) | Ba chấm ngang + `Đang xử lý` | Ẩn (thêm `...` sau câu nghe) |
| `awaiting_ok` | Xanh dương (`#0D47A1`) | Vòng tròn có chấm giữa + `Chờ ok` | `<countdown_s> giây` ở thanh trên |
| Mất kết nối (offline) | Xám (`#546E7A`) | Vòng `!` + `Mất kết nối, đang thử lại` | Ẩn |

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
- `self.screen.show_text`: Hiển thị văn bản UTF-8 (tối đa 200 byte) ở bong bóng trả lời trên cùng theo tham số chuỗi `text` (tối đa 3 dòng `kViFont20`, tràn sẽ rút gọn bằng dấu `...`; truyền chuỗi rỗng để xóa).
- `self.screen.show_status`: Cập nhật màn hình trạng thái phiên nghe theo các tham số `state` (`listening` | `processing` | `awaiting_ok` | `closed`), `text` (UTF-8, tối đa 40 ký tự / 160 byte, hiển thị ở bong bóng câu người bán bên dưới), và `countdown_s` (số nguyên tuỳ chọn `0..30`, mặc định `0`). Khi `state = "awaiting_ok"` và `countdown_s > 0`, board chạy bộ đếm ngược cục bộ mỗi 1 giây trên thanh trên cùng; mọi lệnh `show_status` mới hoặc thay đổi trạng thái sẽ huỷ bộ đếm.
- `self.session.close`: Đưa thiết bị từ trạng thái đang nghe/đang nói về trạng thái nghỉ (`idle`) mà không đóng kênh WebSocket, tự động tắt xử lý giọng nói và bật lại nhận diện wake word.
