#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "MAX30105.h"
#include "heartRate.h"

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ================= SENSOR =================
MAX30105 particleSensor;

// ================= TIMER OLED =================
unsigned long lastOLED = 0;
const int OLED_INTERVAL = 250;

// ================= VARIABLES =================
long lastBeat = 0;
int stableBPM = 0;
int stableSpO2 = 0;
bool isFirstBeat = true; // Cờ chặn lỗi sai số thời gian ở nhịp đập đầu tiên

// ================= MIN MAX =================
long irMin = 999999, irMax = 0;
long redMin = 999999, redMax = 0;

// ================= FILTER =================
#define N 5 
int bpmBuffer[N];
int spo2Buffer[N];
int idx = 0, countData = 0;

// ================= STABILITY GATE (CỔNG LỌC CHUYỂN ĐỘNG) =================
#define HIST_SIZE 25
long irHistory[HIST_SIZE];
long redHistory[HIST_SIZE];
int hIdx = 0;

int smoothData(int bpmVal, int spo2Val) {
  bpmBuffer[idx] = bpmVal;
  spo2Buffer[idx] = spo2Val;

  idx++;
  if (idx >= N) idx = 0;

  if (countData < N) countData++;

  int sumBpm = 0, sumSpo2 = 0;

  for (int i = 0; i < countData; i++) {
    sumBpm += bpmBuffer[i];
    sumSpo2 += spo2Buffer[i];
  }

  stableBPM = sumBpm / countData;
  stableSpO2 = sumSpo2 / countData;

  return stableBPM;
}

// ================= STATUS =================
String getStatus(int bpm, int spo2) {
  if (spo2 > 0 && spo2 < 90) return "LOW OXYGEN";
  if (bpm > 0 && bpm < 50) return "LOW BPM";
  if (bpm > 120) return "HIGH BPM";
  return "NORMAL";
}

// ================= OLED =================
void showOLED(int bpm, int spo2, String status) {
  display.clearDisplay();

  display.setTextSize(1);
  display.setTextColor(WHITE);

  display.setCursor(0, 0);
  display.print("BPM: ");
  display.print(bpm);

  display.setCursor(0, 25);
  display.print("SpO2: ");
  display.print(spo2);
  display.print("%");

  display.setCursor(0, 50);
  display.print("Status: ");
  display.print(status);

  display.display();
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED FAIL");
    while (1);
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 20);
  display.print("INITIALIZING...");
  display.display();

  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("MAX30102 FAIL");
    while (1);
  }

  particleSensor.setup();
  particleSensor.setPulseAmplitudeRed(0x3F);
  particleSensor.setPulseAmplitudeIR(0x3F);
  particleSensor.setPulseAmplitudeGreen(0);

  // Khởi tạo sạch bộ đệm lịch sử
  for(int i = 0; i < HIST_SIZE; i++) {
      irHistory[i] = 0;
      redHistory[i] = 0;
  }
}

// ================= LOOP =================
void loop() {
  long currentIR = particleSensor.getIR();
  long currentRed = particleSensor.getRed();

  // Đẩy dữ liệu mới vào mảng lịch sử (Ring Buffer)
  irHistory[hIdx] = currentIR;
  redHistory[hIdx] = currentRed;

  // Lấy dữ liệu cũ nhất cách đây 25 mẫu (tương đương độ trễ ~60ms)
  int oldestIdx = (hIdx + 1) % HIST_SIZE;
  long oldestIR = irHistory[oldestIdx];
  long oldestRed = redHistory[oldestIdx];
  hIdx = oldestIdx; 

  // Tính độ sai lệch tín hiệu để phát hiện cử động ngón tay
  long diff = abs(currentIR - oldestIR);

  // ================= CỔNG ĐÓNG (KHI KHÔNG CÓ TAY HOẶC ĐANG CỬ ĐỘNG) =================
  if (currentIR < 50000 || diff > 2500) {
    
    stableBPM = 0;
    stableSpO2 = 0;
    countData = 0;
    idx = 0;
    for (int i = 0; i < N; i++) {
      bpmBuffer[i] = 0;
      spo2Buffer[i] = 0;
    }

    irMin = 999999; irMax = 0;
    redMin = 999999; redMax = 0;
    isFirstBeat = true; // Khôi phục trạng thái nhịp đầu tiên

    if (millis() - lastOLED > OLED_INTERVAL) {
      display.clearDisplay();
      display.setTextSize(1);
      display.setCursor(0, 20);
      
      // Phân biệt hiển thị: Không có tay vs Tay đang nhúc nhích
      if (currentIR < 50000) {
        display.print("PLACE FINGER");
      } else {
        display.print("HOLD STILL...");
      }
      
      display.display();
      lastOLED = millis();
    }

  } else {
    // ================= CỔNG MỞ (NGÓN TAY ĐÃ ĐẶT YÊN TĨNH) =================
    
    // Thu thập biên độ AC bằng dữ liệu an toàn (oldest)
    if (oldestIR > irMax) irMax = oldestIR;
    if (oldestIR < irMin) irMin = oldestIR;
    if (oldestRed > redMax) redMax = oldestRed;
    if (oldestRed < redMin) redMin = oldestRed;

    // Chỉ đẩy dữ liệu an toàn (chậm 60ms) vào thuật toán nhịp tim
    if (checkForBeat(oldestIR)) {
      
      if (isFirstBeat) {
        // Nhịp đầu tiên chỉ dùng để làm mốc tính thời gian cho nhịp thứ 2
        isFirstBeat = false;
        lastBeat = millis();
      } else {
        long now = millis();
        long delta = now - lastBeat;
        lastBeat = now;

        if (delta > 270 && delta < 1500) {
          int currentBpm = 60000 / delta;

          float irAC = irMax - irMin;
          float redAC = redMax - redMin;

          float irDC = (irMax + irMin) / 2.0;
          float redDC = (redMax + redMin) / 2.0;

          int currentSpO2 = 0;

          if (irDC > 0 && redDC > 0 && irAC > 0) {
            float R = (redAC / (float)redDC) / (irAC / (float)irDC);
            currentSpO2 = 110 - (25 * R);

            if (currentSpO2 > 100) currentSpO2 = 100;
            if (currentSpO2 < 0) currentSpO2 = 0;
          }

          if (currentSpO2 > 70 && currentBpm > 40 && currentBpm < 200) {
            smoothData(currentBpm, currentSpO2);
          }

          Serial.print("BPM: ");
          Serial.print(stableBPM);
          Serial.print(" | SpO2: ");
          Serial.println(stableSpO2);
        }
      }

      // Reset min/max để chuẩn bị cho nhịp kế tiếp
      irMin = 999999; irMax = 0;
      redMin = 999999; redMax = 0;
    }

    // ================= HIỂN THỊ OLED =================
    if (millis() - lastOLED > OLED_INTERVAL) {
      String status = (stableBPM > 0) ? getStatus(stableBPM, stableSpO2) : "CALCULATING...";
      showOLED(stableBPM, stableSpO2, status);
      lastOLED = millis();
    }
  }
}
