#include "esp_camera.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <TinyGPS++.h>
#include "AudioTools.h"

// --- CẤU HÌNH HỆ THỐNG ---
const char* serverUrl = "http://192.168.100.130:5000/upload";
const char* ssid = "Minion";
const char* password = "20042007";

#define PIN_SDA 5
#define PIN_SCL 4     // Đổi sang chân 4 để tránh trùng GPIO 6
#define PIN_I2S_BCK 7
#define PIN_I2S_WS 8
#define PIN_I2S_DATA 9
#define PIN_BUTTON 44  // Nút nhấn nối chân số 7 (GPIO 7)

HardwareSerial SIM(2); 
HardwareSerial GPS_Serial(1);
TinyGPSPlus gps;
I2SStream i2s;
SineWaveGenerator<int16_t> sineWave;
GeneratedSoundStream<int16_t> sound(sineWave);

bool hasObstacle_AI = false;
bool hasObstacle_US = false;
String currentGPS = "0,0";

// --- CẤU HÌNH CAMERA XIAO S3 ---
#define PWDN_GPIO_NUM  -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM   10
#define SIOD_GPIO_NUM   40
#define SIOC_GPIO_NUM   39
#define Y9_GPIO_NUM     48
#define Y8_GPIO_NUM     11
#define Y7_GPIO_NUM     12
#define Y6_GPIO_NUM     14
#define Y5_GPIO_NUM     16
#define Y4_GPIO_NUM     18
#define Y3_GPIO_NUM     17
#define Y2_GPIO_NUM     15
#define VSYNC_GPIO_NUM 38
#define HREF_GPIO_NUM   47
#define PCLK_GPIO_NUM   13

void setup() {
  Serial.begin(115200);
  vTaskDelay(2000 / portTICK_PERIOD_MS);
  Serial.println("\n--- [START] KHOI DONG HE THONG GAY THONG MINH ---");

  // 1. I2C & Siêu âm
  Wire.begin(PIN_SDA, PIN_SCL);
  Serial.println("[LOG] Dang kiem tra cam bien sieu am...");
  Wire.beginTransmission(0x70);
  if (Wire.endTransmission() == 0) Serial.println("[ OK ] Sieu am tim thay tai dia chi 0x70.");
  else Serial.println("[ FAIL ] Khong tim thay Sieu am!");

  // 2. Module SIM (Fix chập chờn)
  Serial.println("[LOG] Dang khoi tao Module SIM (A7680C)...");
  SIM.begin(115200, SERIAL_8N1, 1, 2); // Thử đổi chân RX/TX nếu cần
  SIM.println("AT");
  vTaskDelay(500 / portTICK_PERIOD_MS);
  if (SIM.available()) {
    String res = SIM.readString();
    Serial.println("[ OK ] SIM phan hoi: " + res);
  } else {
    Serial.println("[ FAIL ] Module SIM im lang. Kiem tra nguon 2A!");
  }

  // 3. GPS & Camera
  GPS_Serial.begin(9600, SERIAL_8N1, 43, 44); // Chân RX/TX GPS cho XIAO S3
  
  camera_config_t config;
  // ... (giữ nguyên cấu hình camera của bạn) ...
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM; config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM; config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM; config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM; config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM; config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM; config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM; config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM; config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size = FRAMESIZE_QVGA;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.fb_count = 2;
  config.jpeg_quality = 12;

  esp_err_t err = esp_camera_init(&config);
  if (err == ESP_OK) {
    sensor_t * s = esp_camera_sensor_get();
    // 0: Tắt, 1: Bật
    s->set_hmirror(s, 0); // Lật ảnh theo chiều ngang
    s->set_vflip(s, 1);
    Serial.println("[ OK ] Camera da san sang.");}
  else Serial.printf("[ FAIL ] Camera loi: 0x%x\n", err);

  // 4. WiFi
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  WiFi.begin(ssid, password);
  Serial.print("[LOG] Dang ket noi WiFi: " + String(ssid));
  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(500 / portTICK_PERIOD_MS);
    Serial.print(".");
  }
  Serial.println("\n[ OK ] Da ket noi WiFi. IP: " + WiFi.localIP().toString());

  // 5. Loa I2S
  auto info = i2s.defaultConfig();
  info.sample_rate = 44100;
  info.pin_bck = PIN_I2S_BCK;
  info.pin_ws = PIN_I2S_WS;
  info.pin_data = PIN_I2S_DATA;
  i2s.begin(info);

  xTaskCreatePinnedToCore(TaskCamera, "TaskCam", 10240, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(TaskSensors, "TaskSens", 10240, NULL, 1, NULL, 1);
}

void TaskCamera(void * p) {
  // Khai báo http bên ngoài vòng lặp để tái sử dụng nếu cần, hoặc quản lý tốt hơn
  for(;;) {
    if (WiFi.status() == WL_CONNECTED) {
      camera_fb_t * fb = esp_camera_fb_get();
      if (fb) {
        HTTPClient http;
        http.begin(serverUrl);
        http.setTimeout(1500); // TỐI ƯU 1: Chỉ đợi Server tối đa 1.5 giây
        http.addHeader("Content-Type", "image/jpeg");
        http.addHeader("GPS-Coord", currentGPS);

        int httpCode = http.POST(fb->buf, fb->len);
        
        if (httpCode == HTTP_CODE_OK) { // HTTP 200
          String rawResponse = http.getString();
          
          int startIdx = rawResponse.indexOf("[[");
          int endIdx = rawResponse.indexOf("]]");

          if (startIdx != -1 && endIdx != -1) {
            String cleanRes = rawResponse.substring(startIdx + 2, endIdx);
            cleanRes.trim(); // TỐI ƯU 2: Xóa sạch khoảng trắng/xuống dòng thừa

            // TỐI ƯU 3: Kiểm tra điều kiện chặt chẽ hơn
            if (cleanRes != "CLEAR" && cleanRes.length() > 0) {
              hasObstacle_AI = true;
              Serial.print("[AI] "); 
              Serial.println(cleanRes);
            } else {
              hasObstacle_AI = false; 
            }
          }
        } else {
          // In lỗi chi tiết hơn để debug
          Serial.printf("[HTTP] Loi: %s\n", http.errorToString(httpCode).c_str());
          hasObstacle_AI = false;
        }
        
        http.end(); // Bắt buộc phải có để giải phóng bộ nhớ
        esp_camera_fb_return(fb);
      }
    } else {
      Serial.println("[WIFI] Mat ket noi, dang doi...");
      vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
    
    // TỐI ƯU 4: Điều chỉnh tốc độ gửi ảnh
    // 200ms - 500ms là khoảng đẹp để không làm treo Server mà vẫn đủ nhanh
    vTaskDelay(200 / portTICK_PERIOD_MS); 
  }
}

void TaskSensors(void * p) {
  for(;;) {
    // 1. Quét nút nhấn SOS (GPIO 7)
    if (digitalRead(PIN_BUTTON) == LOW) {
      vTaskDelay(50 / portTICK_PERIOD_MS); // Chống rung
      if (digitalRead(PIN_BUTTON) == LOW) {
        Serial.println("[SOS] NUT NHAN DA BAM! Dang kich hoat khan cap...");
        sendSMS("0344196807", "SOS! Toi can giup do. Vi tri: https://www.google.com/maps?q=" + currentGPS);
        playAlert(1500, 1000); // Kêu còi báo động tại chỗ
        while(digitalRead(PIN_BUTTON) == LOW) vTaskDelay(10 / portTICK_PERIOD_MS); // Đợi thả nút
      }
    }

    // 2. GPS
    while (GPS_Serial.available() > 0) {
      if (gps.encode(GPS_Serial.read()) && gps.location.isValid()) {
        currentGPS = String(gps.location.lat(), 6) + "," + String(gps.location.lng(), 6);
      }
    }

    // 3. Phản hồi âm thanh
    if (hasObstacle_AI) {
      playAlert(800, 400); 
      hasObstacle_AI = false;
    }
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

void sendSMS(String phone, String msg) {
  Serial.println("[SIM] Dang gui tin nhắn den: " + phone);
  SIM.println("AT+CMGF=1"); 
  vTaskDelay(200 / portTICK_PERIOD_MS);
  SIM.print("AT+CMGS=\"");
  SIM.print(phone);
  SIM.println("\"");
  vTaskDelay(200 / portTICK_PERIOD_MS);
  SIM.print(msg);
  vTaskDelay(200 / portTICK_PERIOD_MS);
  SIM.write(26); // Gửi mã Ctrl+Z
  Serial.println("[SIM] Lenh gui SMS da phat di.");
}

void playAlert(int freq, int duration) {
  sineWave.begin(25000, 44100, freq); 
  for(int i=0; i < (duration/2); i++) {
    StreamCopy copier(i2s, sound);
    copier.copy();
  }
  Serial.println("[AUDIO] Da phat canh bao tan so: " + String(freq));
}

void loop() {}