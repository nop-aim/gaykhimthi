# 🦯 Gậy Thông Minh – Smart Cane

> Thiết bị hỗ trợ người khiếm thị tích hợp AI nhận diện vật cản, cảnh báo âm thanh theo thời gian thực và nút SOS khẩn cấp.

---

## Mục lục

1. [Tổng quan dự án](#1-tổng-quan-dự-án)
2. [Kiến trúc hệ thống](#2-kiến-trúc-hệ-thống)
3. [Phần cứng](#3-phần-cứng)
4. [Phần mềm](#4-phần-mềm)
5. [Luồng hoạt động](#5-luồng-hoạt-động)
6. [Hướng dẫn cài đặt](#6-hướng-dẫn-cài-đặt)
7. [Cấu hình](#7-cấu-hình)
8. [Giao thức truyền thông](#8-giao-thức-truyền-thông)
9. [Xử lý lỗi và cơ chế an toàn](#9-xử-lý-lỗi-và-cơ-chế-an-toàn)
10. [Cấu trúc thư mục](#10-cấu-trúc-thư-mục)

---

## 1. Tổng quan dự án

**Gậy Thông Minh** là thiết bị hỗ trợ điều hướng cho người khiếm thị, hoạt động theo mô hình **Edge–Cloud Hybrid**:

- **Trên gậy (Edge – XIAO ESP32-S3):** chụp ảnh liên tục, đọc cảm biến siêu âm, phát âm thanh cảnh báo qua loa I2S, theo dõi GPS và gửi SMS khẩn cấp.
- **Máy tính xách tay / máy chủ (Cloud – Python/Flask):** nhận ảnh từ ESP32, chạy mô hình YOLO11 để nhận diện vật cản, trả kết quả cảnh báo bằng tiếng Việt và phát âm thanh TTS trên màn hình.

Hai tầng kết nối với nhau qua WiFi nội bộ bằng HTTP REST.

---

## 2. Kiến trúc hệ thống

```
┌──────────────────────────────────────────────────────────┐
│                  XIAO ESP32-S3 (trên gậy)                │
│                                                          │
│  ┌──────────┐   JPEG    ┌──────────────────────────────┐ │
│  │ Camera   │ ────────► │  TaskCamera (Core 0)         │ │
│  │ OV2640   │           │  POST /upload → nhận WARNING │ │
│  └──────────┘           └──────────────────────────────┘ │
│                                        │ mutex           │
│  ┌──────────┐  I2C      ┌──────────────▼──────────────┐  │
│  │ Siêu âm  │ ────────► │  TaskSensors (Core 1)       │  │
│  │ SRF02    │           │  Đọc khoảng cách + playAlert│  │
│  └──────────┘           └─────────────────────────────┘  │
│                                                          │
│  ┌──────────┐  UART     ┌──────────────────────────────┐ │
│  │  GPS     │ ────────► │  TaskGPS (Core 1)            │ │
│  │ NEO-6M   │           │  POST /gps mỗi 5 giây        │ │
│  └──────────┘           └──────────────────────────────┘ │
│                                                          │
│  ┌──────────┐  GPIO     ┌──────────────────────────────┐ │
│  │ Nút SOS  │ ────────► │  Gửi SMS + playAlert(SOS)    │ │
│  └──────────┘           └──────────────────────────────┘ │
│                                                          │
│  ┌──────────┐  UART     ┌──────────────────────────────┐ │
│  │ SIM A7680│ ◄──────── │  sendSMS() + sendAT()        │ │
│  └──────────┘           └──────────────────────────────┘ │
└──────────────────────────────────────────────────────────┘
                        │ WiFi (HTTP)
                        ▼
┌──────────────────────────────────────────────────────────┐
│              Server Python / Flask (máy tính)            │
│                                                          │
│  POST /upload ──► img_queue ──► ai_processing_thread     │
│                                  │                       │
│                                  ├─ YOLO11 inference     │
│                                  ├─ Xác định vị trí +    │
│                                  │  khoảng cách          │
│                                  ├─ gTTS → pygame TTS    │
│                                  └─ Trả [[WARNING_TEXT]] │
│                                                          │
│  POST /gps ─────────────────────► Lưu / hiển thị GPS     │
└──────────────────────────────────────────────────────────┘
```

---

## 3. Phần cứng

### 3.1 Danh sách linh kiện

| Linh kiện | Model | Giao tiếp | Vai trò |
|-----------|-------|-----------|---------|
| Vi điều khiển | Seeed XIAO ESP32-S3 Sense | — | Não xử lý trung tâm |
| Camera | OV2640 (tích hợp sẵn) | DVP parallel | Chụp ảnh QVGA JPEG |
| Cảm biến siêu âm | SRF02 (I2C, addr `0x70`) | I2C | Đo khoảng cách < 2 m |
| Module GPS | NEO-6M (hoặc tương đương) | UART1 (RX=3, TX=2) | Lấy toạ độ thời gian thực |
| Module SIM | SIM A7680C (4G LTE) | UART2 (RX=4, TX=1) | Gửi SMS SOS |
| Loa I2S | MAX98357A hoặc tương đương | I2S (BCK=7, WS=8, DATA=9) | Phát âm thanh cảnh báo |
| Nút bấm | Nút thường hở (NO) | GPIO 44 (INPUT_PULLUP) | Kích hoạt SOS |

### 3.2 Sơ đồ chân (XIAO ESP32-S3)

| GPIO | Tên chân | Kết nối |
|------|----------|---------|
| 5 | SDA | I2C SDA (cảm biến siêu âm) |
| 6 | SCL | I2C SCL (cảm biến siêu âm) |
| 7 | I2S_BCK | Loa I2S – Bit Clock |
| 8 | I2S_WS | Loa I2S – Word Select (LRCK) |
| 9 | I2S_DATA | Loa I2S – Data |
| 44 | BUTTON | Nút SOS (GND khi nhấn) |
| 1 | TX2 | SIM A7680C RX |
| 4 | RX2 | SIM A7680C TX |
| 2 | TX1 | GPS TX |
| 3 | RX1 | GPS RX |

> **Lưu ý:** Camera OV2640 sử dụng các chân DVP cố định theo cấu hình XIAO ESP32-S3 Sense (D0–D7, XCLK, PCLK, VSYNC, HREF, SIOD, SIOC) — không thay đổi được.

---

## 4. Phần mềm

### 4.1 Firmware ESP32 (`esp32_improved.ino`)

Viết bằng **Arduino framework** cho ESP32, sử dụng **FreeRTOS** để chạy đa nhiệm trên 2 nhân CPU.

| Task | Nhân | Stack | Chức năng |
|------|------|-------|-----------|
| `TaskCamera` | Core 0 | 16 KB | Chụp ảnh → POST lên server → nhận cảnh báo AI |
| `TaskSensors` | Core 1 | 10 KB | Đọc siêu âm I2C, xử lý nút SOS, phát âm thanh |
| `TaskGPS` | Core 1 | 8 KB | Đọc GPS NMEA, gửi toạ độ lên server mỗi 5 giây |

**Thư viện Arduino cần thiết:**

- `esp_camera.h` — driver camera ESP32
- `WiFi.h` + `HTTPClient.h` — kết nối mạng
- `Wire.h` — giao tiếp I2C với cảm biến siêu âm
- `TinyGPS++` — giải mã dữ liệu NMEA từ module GPS
- `AudioTools` — tạo sóng sin và xuất I2S cho loa

### 4.2 Server AI (`serve_2.py`)

Viết bằng **Python 3**, chạy trên máy tính trong cùng mạng WiFi.

**Thư viện Python cần thiết:**

- `flask` — HTTP server nhận ảnh từ ESP32
- `ultralytics` — chạy mô hình YOLO11
- `opencv-python` (`cv2`) — giải mã JPEG, hiển thị kết quả
- `numpy` — xử lý mảng ảnh
- `gTTS` — chuyển text thành giọng nói tiếng Việt
- `pygame` — phát âm thanh TTS
- `threading`, `queue` — xử lý bất đồng bộ

---

## 5. Luồng hoạt động

### 5.1 Nhận diện vật cản bằng AI (Camera + YOLO)

```
ESP32                              Server (Flask + YOLO)
  │                                        │
  ├─ Chụp ảnh QVGA JPEG (300ms/lần)        │
  ├─ POST /upload (raw bytes)  ──────────► │
  │                                        ├─ Giải mã JPEG → frame
  │                                        ├─ YOLO11 inference (conf=0.5)
  │                                        ├─ Lọc class: người, xe, ghế, bàn...
  │                                        ├─ Tính vị trí (trái/trước/phải)
  │                                        ├─ Tính khoảng cách (% diện tích bbox)
  │                                        ├─ Tạo chuỗi: "Có ô tô phía trước, rất gần"
  │                                        ├─ gTTS phát tiếng (nếu cảnh báo mới)
  │ ◄────────────────── [[WARNING_TEXT]]   │
  ├─ Cập nhật `hasObstacle_AI`             │
  ├─ Phát beep I2S (1500Hz, 400ms)         │
```

**Logic xác định khoảng cách** (dựa vào tỉ lệ diện tích bounding box / tổng diện tích frame):

| Tỉ lệ diện tích | Cảnh báo |
|-----------------|----------|
| > 35%           | "rất gần"|
| 12% – 35% | "đang đến gần" |
| < 12% | "ở xa" |

**Các lớp vật thể được nhận diện:**

| ID COCO | Tên tiếng Anh | Tên tiếng Việt|
|---------|--------------|----------------|
| 0 | person | người |
| 1 | bicycle | xe đạp |
| 2 | car | ô tô |
| 3 | motorcycle | xe máy |
| 5 | bus | xe buýt |
| 7 | truck | xe tải |
| 56 | chair | ghế |
| 60 | dining table | bàn |

### 5.2 Cảm biến siêu âm (khoảng cách gần)

- Đọc liên tục qua I2C (địa chỉ `0x70`)
- Áp dụng bộ lọc trung bình trượt 5 mẫu để giảm nhiễu
- Nếu khoảng cách trung bình < **120 cm**: kích hoạt cờ `hasObstacle_US`
- Phát âm thanh beep khẩn (2000 Hz, 150 ms) — **ưu tiên cao hơn cảnh báo AI**

### 5.3 SOS khẩn cấp

1. Người dùng nhấn nút GPIO 44
2. Debounce 3 giây (tránh gửi nhiều lần)
3. Gửi SMS đến số `SOS_PHONE` qua SIM A7680C:
   ```
   SOS! Vi tri: https://maps.google.com/?q=<lat>,<lng>
   ```
4. Phát âm thanh cảnh báo (1500 Hz, 1000 ms)

### 5.4 GPS tracking

- Task `TaskGPS` chạy riêng trên Core 1, không block cảm biến siêu âm
- Giải mã dữ liệu NMEA từ module GPS qua UART1
- Cần tối thiểu **4 vệ tinh** để coi là tín hiệu hợp lệ
- Gửi toạ độ JSON lên `POST /gps` mỗi **5 giây**:
  ```json
  {"lat": 10.762622, "lng": 106.660172}
  ```

---

## 6. Hướng dẫn cài đặt

### 6.1 Cài đặt Server (máy tính)

```bash
# 1. Tạo môi trường ảo Python
python -m venv venv
source venv/bin/activate       # Linux/macOS
venv\Scripts\activate          # Windows

# 2. Cài đặt thư viện
pip install flask ultralytics opencv-python gTTS pygame numpy

# 3. Tải mô hình YOLO11s (tự động tải khi chạy lần đầu)
#    Hoặc tải thủ công: https://github.com/ultralytics/assets/releases
#    Đặt file yolo11s.pt cùng thư mục với serve_2.py

# 4. Chạy server
python serve_2.py
# Server sẽ lắng nghe tại http://0.0.0.0:5000
```

### 6.2 Nạp firmware ESP32

**Yêu cầu:**
- Arduino IDE 2.x hoặc VS Code + PlatformIO
- Board: `Seeed XIAO ESP32S3` (cài qua Board Manager)

**Thư viện cần cài (Library Manager):**
- `TinyGPS++` by Mikal Hart
- `arduino-audio-tools` by pschatzmann

**Bước nạp:**
1. Mở `esp32_improved.ino` trong Arduino IDE
2. Cập nhật cấu hình mạng (xem mục 7)
3. Chọn **Board:** `XIAO_ESP32S3` và **Port** tương ứng
4. Nhấn **Upload**

---

## 7. Cấu hình

Tất cả cấu hình nằm ở đầu file `esp32_improved.ino`:

```cpp
// ── Mạng & Server ────────────────────────────────────────
const char* serverUrl = "http://192.168.1.135:5000/upload"; // IP máy chủ
const char* gpsUrl    = "http://192.168.1.135:5000/gps";    // Endpoint GPS
const char* ssid      = "Tên_WiFi";                         // SSID WiFi
const char* password  = "Mật_khẩu_WiFi";                   // Mật khẩu WiFi

// ── SOS ──────────────────────────────────────────────────
const char* SOS_PHONE = "09xxxxxxxx";                       // Số điện thoại nhận SMS

// ── Ngưỡng cảm biến siêu âm ──────────────────────────────
// Trong TaskSensors: (sum / filterSamples) < 120
// 120 tương đương ~1.2 m (đơn vị cảm biến SRF02: cm)
// Thay đổi giá trị này để điều chỉnh độ nhạy

// ── Tần suất chụp ảnh ────────────────────────────────────
vTaskDelay(300 / portTICK_PERIOD_MS); // 300ms ≈ ~3 fps
```

**Tìm IP máy chủ** (máy chạy `serve_2.py`):
```bash
# Linux/macOS
ip addr show | grep "inet "

# Windows
ipconfig
```

---

## 8. Giao thức truyền thông

### POST `/upload` — Gửi ảnh từ ESP32

| Thuộc tính | Giá trị |
|------------|---------|
| Method | POST |
| Content-Type | `image/jpeg` |
| Body | Raw JPEG bytes |
| Response | `[[CLEAR]]` hoặc `[[Có ô tô phía trước, rất gần]]` |

### POST `/gps` — Gửi toạ độ GPS

| Thuộc tính | Giá trị |
|------------|---------|
| Method | POST |
| Content-Type | `application/json` |
| Body | `{"lat": 10.762622, "lng": 106.660172}` |

---

## 9. Xử lý lỗi và cơ chế an toàn

| Vấn đề | Giải pháp đã triển khai |
|--------|------------------------|
| WiFi mất kết nối | `connectWiFi()` tự động thử lại; watchdog kiểm tra mỗi 30 giây trong `loop()` |
| HTTP timeout | Mọi request đều có timeout 3 giây (`HTTP_TIMEOUT_MS`) |
| Race condition giữa 2 core | FreeRTOS mutex (`obstacleMutex`) bảo vệ biến dùng chung |
| GPS block siêu âm | `TaskGPS` chạy tách biệt trên Core 1 |
| Nút SOS nhấn nhiều lần | Debounce phần mềm 3 giây (`lastSosTap`) |
| Loa block CPU | `playAlert()` dùng timeout + `taskYIELD()` thay vì busy-wait |
| FreeRTOS watchdog reset | Thay `delay()` bằng `vTaskDelay()` trong tất cả task |
| SIM không nhận lệnh | `readSIM()` chờ prompt `>` trước khi gửi nội dung SMS |
| Bộ lọc nhiễu siêu âm | Trung bình trượt 5 mẫu (`filterSamples = 5`) |
| TTS phát liên tục | Cooldown 3 giây (`last_speak_time`) giữa các lần phát |
| Hàng đợi ảnh tràn | `img_queue` kích thước 1, tự xoá frame cũ khi đầy |

---

## 10. Cấu trúc thư mục

```
smart-cane/
├── esp32_improved.ino   # Firmware XIAO ESP32-S3
├── serve_2.py           # Server AI Flask + YOLO11
├── yolo11s.pt           # Mô hình YOLO11s (tải riêng)
└── README.md            # Tài liệu này
```

---

## Tác giả & Giấy phép

Dự án được phát triển nhằm mục đích hỗ trợ người khiếm thị. Mã nguồn mở để học tập và cải tiến.