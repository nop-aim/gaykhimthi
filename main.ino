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
#define PIN_SCL 6
#define PIN_I2S_BCK 7
#define PIN_I2S_WS 8
#define PIN_I2S_DATA 9
#define PIN_BUTTON 6 

HardwareSerial SIM(2); 
HardwareSerial GPS_Serial(1);
TinyGPSPlus gps;
I2SStream i2s;
SineWaveGenerator<int16_t> sineWave;
GeneratedSoundStream<int16_t> sound(sineWave);

bool hasObstacle_AI = false;
bool hasObstacle_US = false;
String currentGPS = "0,0";

const int filterSamples = 5;
int distanceHistory[filterSamples];
int historyIndex = 0;

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
  Serial.println("\n--- KHOI DONG HE THONG GAY THONG MINH ---");

  // 1. Kiểm tra I2C & Siêu âm
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(100000); 
  Wire.setTimeOut(50); 
  Wire.beginTransmission(0x70);
  if (Wire.endTransmission() == 0) Serial.println("[ OK ] Sieu am san sang.");
  else Serial.println("[ FAIL ] Loi ket noi Sieu am!");

  // 2. Kiểm tra SIM
  SIM.begin(115200, SERIAL_8N1, 4, 1);
  SIM.println("AT"); 
  unsigned long startSIM = millis();
  bool simOk = false;
  while (millis() - startSIM < 1500) {
    if (SIM.find("OK")) { simOk = true; break; }
  }
  if (simOk) Serial.println("[ OK ] Module SIM phan hoi.");
  else Serial.println("[ FAIL ] Module SIM khong phan hoi!");

  // 3. Khởi tạo GPS
  GPS_Serial.begin(9600, SERIAL_8N1, 3, 2);
  Serial.println("[ INFO ] GPS Serial khoi tao.");

  // 4. Khởi tạo Camera
  camera_config_t config;
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

  esp_err_t err = esp_camera_init(&config); // Sửa lỗi khai báo err
  if (err == ESP_OK) {
    sensor_t * s = esp_camera_sensor_get();
    s->set_hmirror(s, 1);
    s->set_vflip(s, 1);
    Serial.println("[ OK ] Camera san sang.");
  } else {
    Serial.printf("[ FAIL ] Camera THAT BAI! Code: 0x%x\n", err);
  }

  // 5. Kết nối WiFi (Sửa lỗi thiếu lệnh begin và vòng lặp đợi)
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  WiFi.begin(ssid, password);
  Serial.print("Dang ket noi WiFi");
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 20) { // Đợi tối đa 10 giây
    vTaskDelay(500 / portTICK_PERIOD_MS);
    Serial.print(".");
    retry++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[ OK ] WiFi IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n[ FAIL ] WiFi khong the ket noi!");
  }

  // 6. Khởi tạo Loa
  auto info = i2s.defaultConfig();
  info.sample_rate = 44100;
  info.pin_bck = PIN_I2S_BCK;
  info.pin_ws = PIN_I2S_WS;
  info.pin_data = PIN_I2S_DATA;
  i2s.begin(info);
  Serial.println("[ OK ] Am thanh I2S san sang.");

  Serial.println("--- Bat dau cac Task ---");
  xTaskCreatePinnedToCore(TaskCamera, "TaskCam", 10240, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(TaskSensors, "TaskSens", 10240, NULL, 1, NULL, 1);
}

// --- CORE 0: AI VISION ---
void TaskCamera(void * p) {
  for(;;) {
    if (WiFi.status() == WL_CONNECTED) {
      camera_fb_t * fb = esp_camera_fb_get();
      if (fb) {
        HTTPClient http;
        http.begin(serverUrl);
        int httpCode = http.POST(fb->buf, fb->len);
        
        if (httpCode == 200) {
          String rawResponse = http.getString(); // Lấy toàn bộ chuỗi (có thể bị rác)
          
          // KỸ THUẬT TÁCH NỘI DUNG ĐẶC BIỆT
          int startIdx = rawResponse.indexOf("[[");
          int endIdx = rawResponse.indexOf("]]");

          if (startIdx != -1 && endIdx != -1) {
            // Lấy nội dung ở giữa [[ và ]]
            String cleanRes = rawResponse.substring(startIdx + 2, endIdx);
            cleanRes.trim(); // Xóa nốt khoảng trắng nếu còn

            if (cleanRes != "CLEAR" && cleanRes.length() > 0) {
              hasObstacle_AI = true;
              Serial.flush(); 
              Serial.println(""); // Ép xuống dòng mới hoàn toàn
              Serial.print(">>> AI XAC NHAN: "); 
              Serial.println(cleanRes); 
              Serial.println(""); // Kết thúc bằng một dòng trống để dễ nhìn 
            }
          }
        }
        http.end();
        esp_camera_fb_return(fb);
      }
    }
    vTaskDelay(200 / portTICK_PERIOD_MS); 
  }
}
// --- CORE 1: SENSORS & FEEDBACK ---
void TaskSensors(void * p) {
  for(;;) {
    // 1. Siêu âm
    Wire.beginTransmission(0x70);
    Wire.write(0x51);
    Wire.endTransmission();
    vTaskDelay(100 / portTICK_PERIOD_MS);

    Wire.requestFrom(0x70, 2);
    if (Wire.available() >= 2) {
      int rawDist = (Wire.read() << 8) | Wire.read();
      if (rawDist > 0 && rawDist < 4000) {
        distanceHistory[historyIndex] = rawDist;
        historyIndex = (historyIndex + 1) % filterSamples;
        long sum = 0;
        for(int i=0; i<filterSamples; i++) sum += distanceHistory[i];
        int avgDist = sum / filterSamples;
        if (avgDist < 50) hasObstacle_US = true;
      }
    }

    // 2. GPS
    while (GPS_Serial.available() > 0) {
      if (gps.encode(GPS_Serial.read()) && gps.location.isValid()) {
        currentGPS = String(gps.location.lat(), 6) + "," + String(gps.location.lng(), 6);
      }
    }

    // 3. SOS Button
    if (digitalRead(PIN_BUTTON) == LOW) {
      static unsigned long lastSms = 0;
      if (millis() - lastSms > 15000) {
        sendSMS("0344196807", "SOS! Vi tri: " + currentGPS);
        lastSms = millis();
      }
    }

    // 4. Âm thanh
    if (hasObstacle_US) {
      playAlert(2500, 200); 
      hasObstacle_US = false;
      hasObstacle_AI = false; 
    } 
    else if (hasObstacle_AI) {
      playAlert(800, 500);
      hasObstacle_AI = false;
    }
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

void playAlert(int freq, int duration) {
  sineWave.begin(2, 44100, freq); 
  for(int i=0; i < (duration/2); i++) {
    StreamCopy copier(i2s, sound);
    copier.copy();
  }
}

void sendSMS(String phone, String msg) {
  SIM.println("AT+CMGF=1"); 
  vTaskDelay(100 / portTICK_PERIOD_MS);
  SIM.printf("AT+CMGS=\"%s\"\r\n", phone.c_str());
  vTaskDelay(100 / portTICK_PERIOD_MS);
  SIM.print(msg);
  vTaskDelay(100 / portTICK_PERIOD_MS);
  SIM.write(26);
}

void loop() {}