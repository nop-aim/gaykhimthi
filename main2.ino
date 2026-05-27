#include "esp_camera.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <TinyGPS++.h>
#include "AudioTools.h"

// ════════════════════════════════════════════════════════════════
//  CẤU HÌNH
// ════════════════════════════════════════════════════════════════
const char* serverUrl = "http://192.168.1.135:5000/upload";
const char* gpsUrl    = "http://192.168.1.135:5000/gps";
const char* ssid      = "TMaii";
const char* password  = "16071779@";
const char* SOS_PHONE = "0397969813";

#define PIN_SDA        5
#define PIN_SCL        6
#define PIN_I2S_BCK    7
#define PIN_I2S_WS     8
#define PIN_I2S_DATA   9
#define PIN_BUTTON    44
#define HTTP_TIMEOUT_MS 3000    // [FIX 1] Timeout cho mọi request HTTP

// ════════════════════════════════════════════════════════════════
//  CAMERA PINS (XIAO ESP32-S3 SENSE)
// ════════════════════════════════════════════════════════════════
#define PWDN_GPIO_NUM  -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  10
#define SIOD_GPIO_NUM  40
#define SIOC_GPIO_NUM  39
#define Y9_GPIO_NUM    48
#define Y8_GPIO_NUM    11
#define Y7_GPIO_NUM    12
#define Y6_GPIO_NUM    14
#define Y5_GPIO_NUM    16
#define Y4_GPIO_NUM    18
#define Y3_GPIO_NUM    17
#define Y2_GPIO_NUM    15
#define VSYNC_GPIO_NUM 38
#define HREF_GPIO_NUM  47
#define PCLK_GPIO_NUM  13

// ════════════════════════════════════════════════════════════════
//  OBJECTS
// ════════════════════════════════════════════════════════════════
HardwareSerial SIM(2);
HardwareSerial GPS_Serial(1);
TinyGPSPlus gps;
I2SStream i2s;
SineWaveGenerator<int16_t> sineWave;
GeneratedSoundStream<int16_t> sound(sineWave);

// [FIX 3] Mutex bảo vệ biến dùng chung giữa 2 core
SemaphoreHandle_t obstacleMutex;
volatile bool hasObstacle_AI = false;
volatile bool hasObstacle_US = false;
String ai_warning_msg = "";
String currentGPS     = "0,0";

// Bộ lọc siêu âm
const int filterSamples = 5;
int distanceHistory[filterSamples] = {0};
int historyIndex = 0;

// Timers
unsigned long lastGPSSend  = 0;
unsigned long lastSosTap   = 0;   // [FIX 4] debounce SOS

// ════════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n=== KHOI DONG GAY THONG MINH (XIAO S3) ===");

  // [FIX 3] Tạo mutex trước mọi task
  obstacleMutex = xSemaphoreCreateMutex();

  // 1. I2C
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(100000);

  // 2. SIM A7680C
  SIM.begin(115200, SERIAL_8N1, 4, 1);
  delay(2000);
  sendAT("AT");
  sendAT("AT+CPIN?");
  sendAT("AT+CSQ");
  sendAT("AT+CNMP=38");
  sendAT("AT+AUTOCONN=1");
  delay(3000);

  // 3. GPS
  GPS_Serial.begin(9600, SERIAL_8N1, 3, 2);
  Serial.println("Dang cho GPS fix...");
  unsigned long start = millis();
  while (millis() - start < 20000) {
    while (GPS_Serial.available()) gps.encode(GPS_Serial.read());
    if (gps.location.isValid() && gps.satellites.value() >= 4) {
      Serial.printf("\n[ OK ] GPS: %.6f, %.6f (%d sats)\n",
                    gps.location.lat(), gps.location.lng(), gps.satellites.value());
      break;
    }
    Serial.print(".");
    delay(500);
  }

  // 4. Camera
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM; config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM; config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM; config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM; config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk    = XCLK_GPIO_NUM;  config.pin_pclk  = PCLK_GPIO_NUM;
  config.pin_vsync   = VSYNC_GPIO_NUM; config.pin_href  = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM; config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn  = PWDN_GPIO_NUM;    config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size   = FRAMESIZE_QVGA;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode    = CAMERA_GRAB_LATEST;
  config.fb_count     = 2;
  config.jpeg_quality = 12;

  esp_err_t err = esp_camera_init(&config);
  if (err == ESP_OK) {
    sensor_t* s = esp_camera_sensor_get();
    s->set_hmirror(s, 1);
    s->set_vflip(s, 0);
    Serial.println("[ OK ] Camera san sang.");
  } else {
    Serial.printf("[ FAIL ] Camera loi: 0x%x\n", err);
  }

  // 5. WiFi
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  connectWiFi();

  // 6. Loa I2S
  auto info = i2s.defaultConfig();
  info.sample_rate = 44100;
  info.pin_bck     = PIN_I2S_BCK;
  info.pin_ws      = PIN_I2S_WS;
  info.pin_data    = PIN_I2S_DATA;
  i2s.begin(info);

  // Tạo tasks — Stack size tăng thêm do thêm xử lý
  xTaskCreatePinnedToCore(TaskCamera,  "TaskCam",  16000, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(TaskSensors, "TaskSens", 10000, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(TaskGPS,     "TaskGPS",   8000, NULL, 1, NULL, 1);
}

// ════════════════════════════════════════════════════════════════
//  [FIX 5] Tự động kết nối lại WiFi
// ════════════════════════════════════════════════════════════════
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.begin(ssid, password);
  Serial.print("Connecting WiFi");
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 20) {
    delay(500);
    Serial.print(".");
    retry++;
  }
  if (WiFi.status() == WL_CONNECTED)
    Serial.println("\n[ OK ] IP: " + WiFi.localIP().toString());
  else
    Serial.println("\n[ WARN ] WiFi timeout, se thu lai sau.");
}

// ════════════════════════════════════════════════════════════════
//  CORE 0: CAMERA & UPLOAD
// ════════════════════════════════════════════════════════════════
void TaskCamera(void* p) {
  for (;;) {
    // [FIX 5] Kiểm tra và reconnect WiFi nếu cần
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("!!! WiFi mat ket noi — dang thu lai...");
      connectWiFi();
      vTaskDelay(2000 / portTICK_PERIOD_MS);
      continue;
    }

    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("!!! Camera Capture Failed");
      vTaskDelay(100 / portTICK_PERIOD_MS);
      continue;
    }

    HTTPClient http;
    http.begin(serverUrl);
    http.addHeader("Content-Type", "image/jpeg");
    http.setTimeout(HTTP_TIMEOUT_MS);   // [FIX 1]

    int httpCode = http.POST(fb->buf, fb->len);

    if (httpCode == 200) {
      String response = http.getString();
      int s = response.indexOf("[[");
      int e = response.indexOf("]]");
      if (s != -1 && e != -1) {
        String cleanRes = response.substring(s + 2, e);
        cleanRes.trim();

        // [FIX 3] Dùng mutex khi ghi biến dùng chung
        if (xSemaphoreTake(obstacleMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
          if (cleanRes != "CLEAR") {
            ai_warning_msg  = cleanRes;
            hasObstacle_AI  = true;
          } else {
            hasObstacle_AI  = false;
          }
          xSemaphoreGive(obstacleMutex);
        }
      }
    } else if (httpCode < 0) {
      Serial.printf("!!! HTTP loi: %d\n", httpCode);
    }

    http.end();
    esp_camera_fb_return(fb);
    vTaskDelay(300 / portTICK_PERIOD_MS);
  }
}

// ════════════════════════════════════════════════════════════════
//  [FIX 2] CORE 1 - TASK GPS RIÊNG (không block siêu âm)
// ════════════════════════════════════════════════════════════════
void TaskGPS(void* p) {
  for (;;) {
    while (GPS_Serial.available() > 0) {
      if (gps.encode(GPS_Serial.read()) && gps.location.isValid()) {
        currentGPS = String(gps.location.lat(), 6) + "," + String(gps.location.lng(), 6);
      }
    }

    // Gửi GPS mỗi 5 giây
    if (millis() - lastGPSSend > 5000 && gps.location.isValid()
        && WiFi.status() == WL_CONNECTED) {
      HTTPClient httpGPS;
      httpGPS.begin(gpsUrl);
      httpGPS.addHeader("Content-Type", "application/json");
      httpGPS.setTimeout(HTTP_TIMEOUT_MS);  // [FIX 1]
      String payload = "{\"lat\":" + String(gps.location.lat(), 6) +
                       ",\"lng\":" + String(gps.location.lng(), 6) + "}";
      httpGPS.POST(payload);
      httpGPS.end();
      lastGPSSend = millis();
    }

    vTaskDelay(200 / portTICK_PERIOD_MS);
  }
}

// ════════════════════════════════════════════════════════════════
//  CORE 1: CẢM BIẾN SIÊU ÂM & ÂM THANH
// ════════════════════════════════════════════════════════════════
void TaskSensors(void* p) {
  for (;;) {
    // ── Siêu âm I2C ──────────────────────────────────────────────
    Wire.beginTransmission(0x70);
    Wire.write(0x51);
    Wire.endTransmission();
    vTaskDelay(70 / portTICK_PERIOD_MS);

    Wire.requestFrom(0x70, 2);
    if (Wire.available() >= 2) {
      int rawDist = (Wire.read() << 8) | Wire.read();
      if (rawDist > 0 && rawDist < 4000) { // lấy khoảng từ 0-4m
        distanceHistory[historyIndex] = rawDist;
        historyIndex = (historyIndex + 1) % filterSamples;
        long sum = 0;
        for (int i = 0; i < filterSamples; i++) sum += distanceHistory[i];
        if ((sum / filterSamples) < 120) { //120 là 2m
          // [FIX 3] mutex khi ghi
          if (xSemaphoreTake(obstacleMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            hasObstacle_US = true;
            xSemaphoreGive(obstacleMutex);
          }
        }
      }
    }

    // ── [FIX 4] Nút SOS có debounce 3 giây ──────────────────────
    if (digitalRead(PIN_BUTTON) == LOW) {
      unsigned long now = millis();
      if (now - lastSosTap > 3000) {
        lastSosTap = now;
        sendSMS(SOS_PHONE, "SOS! Vi tri: https://maps.google.com/?q=" + currentGPS);
        playAlert(1500, 1000);
      }
    }

    // ── Ưu tiên cảnh báo âm thanh ────────────────────────────────
    bool doUS = false, doAI = false;
    String warnCopy;

    // [FIX 3] Đọc cờ trong mutex, sau đó xử lý ngoài
    if (xSemaphoreTake(obstacleMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      if (hasObstacle_US) { doUS = true; hasObstacle_US = false; }
      else if (hasObstacle_AI) { doAI = true; warnCopy = ai_warning_msg; hasObstacle_AI = false; }
      xSemaphoreGive(obstacleMutex);
    }

    // [FIX 6] playAlert chạy trong scope nhỏ, không lock mutex
    if (doUS) {
      Serial.println(">>> CANH BAO: SIEU AM!");
      playAlert(2000, 150);
    } else if (doAI) {
      Serial.println(">>> AI WARNING: " + warnCopy);
      playAlert(1500, 400);
    }

    vTaskDelay(30 / portTICK_PERIOD_MS);
  }
}

// ════════════════════════════════════════════════════════════════
//  TIỆN ÍCH
// ════════════════════════════════════════════════════════════════

// [FIX 6] playAlert dùng timeout thay vì while(busy) để không block lâu
void playAlert(int freq, int duration) {
  sineWave.begin(2, 44100, freq);   // Tăng volume nhẹ (5→8)
  unsigned long stop = millis() + duration;
  while (millis() < stop) {
    StreamCopy copier(i2s, sound);
    copier.copy();
    // Nhường CPU trong 1 tick để OS có thể xử lý task khác nếu cần
    taskYIELD();
  }
  i2s.flush();
}

// ── Hàm phụ: đọc response từ SIM trong timeout ───────────────
String readSIM(unsigned long timeoutMs) {
  String resp = "";
  unsigned long t = millis();
  while (millis() - t < timeoutMs) {
    while (SIM.available()) {
      char c = SIM.read();
      resp += c;
    }
    // Thoát sớm nếu đã có OK, ERROR hoặc dấu >
    if (resp.indexOf("OK") != -1  ||
        resp.indexOf("ERROR") != -1 ||
        resp.indexOf(">") != -1) break;
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
  return resp;
}

// ── Gửi SMS ──────────────────────────────────────────────────
void sendSMS(String phone, String msg) {
  Serial.println(">>> GUI SMS: " + phone);

  // Lỗi 1 cũ: delay 100ms không đủ cho CMGF
  SIM.println("AT+CMGF=1");
  String r1 = readSIM(1000);
  if (r1.indexOf("OK") == -1) {
    Serial.println("!!! CMGF that bai: " + r1);
    return;  // Không gửi nếu chế độ text chưa được bật
  }

  // Lỗi 2 cũ: delay 100ms không đủ + không đọc prompt
  SIM.printf("AT+CMGS=\"%s\"\r\n", phone.c_str());

  // Phải chờ dấu ">" — SIM module mới sẵn sàng nhận nội dung
  String r2 = readSIM(5000);  // A7680C có thể mất ~2-3 giây
  if (r2.indexOf(">") == -1) {
    Serial.println("!!! Khong nhan duoc prompt '>': " + r2);
    return;
  }

  // Lỗi 3 cũ: delay 100ms giữa print và CTRL+Z không đủ
  SIM.print(msg);
  vTaskDelay(200 / portTICK_PERIOD_MS);  // Đợi SIM nhận xong nội dung
  SIM.write(26);  // CTRL+Z = ký tự ASCII 26, ra lệnh gửi

  // Chờ kết quả — CMGS: +CMGS: <mr> rồi OK
  String r3 = readSIM(10000);
  if (r3.indexOf("+CMGS:") != -1) {
    Serial.println(">>> SMS THANH CONG!");
  } else {
    Serial.println("!!! SMS THAT BAI: " + r3);
  }
}

// ── sendAT cũng cần dùng vTaskDelay thay vì delay() ─────────
// (delay() blocking trong task FreeRTOS gây watchdog reset!)
void sendAT(String cmd) {
  Serial.println(">>> " + cmd);
  SIM.println(cmd);
  vTaskDelay(1000 / portTICK_PERIOD_MS);  // Sửa: delay → vTaskDelay
  while (SIM.available()) Serial.write(SIM.read());
}

void loop() {
  // [FIX 5] Watchdog nhẹ: kiểm tra WiFi mỗi 30 giây
  static unsigned long lastWiFiCheck = 0;
  if (millis() - lastWiFiCheck > 30000) {
    lastWiFiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) connectWiFi();
  }
  delay(1000);
}
