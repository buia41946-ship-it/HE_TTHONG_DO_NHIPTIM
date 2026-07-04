#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "MAX30105.h"
#include "heartRate.h"
#include <WiFi.h>
#include <WebServer.h>

const char* ssid = "VIETTEL";       
const char* password = "12345678";     

WebServer server(80); 

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

MAX30105 particleSensor;

// cập nhật OLED mỗi 250ms
unsigned long lastOLED = 0;
const int OLED_INTERVAL = 250;

long lastBeat = 0;
int stableBPM = 0;
int stableSpO2 = 0;
bool isFirstBeat = true; // Cờ chặn lỗi sai số thời gian ở nhịp đập đầu tiên

//giá trị min max để tính biên độ AC
long irMin = 999999, irMax = 0;
long redMin = 999999, redMax = 0;

// lấy mẫu 5 lần để tính trung bình, giảm nhiễu
#define N 5 
int bpmBuffer[N];
int spo2Buffer[N];
int idx = 0, countData = 0;

// khử nhiễu chuyển động
#define HIST_SIZE 25
long irHistory[HIST_SIZE];
long redHistory[HIST_SIZE];
int hIdx = 0;

// hàm lọc dữ liệu nhịp tim và SpO2 để giảm nhiễu
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

// hàm kiểm tra trạng thái nhịp tim và SpO2 để đưa ra cảnh báo
String getStatus(int bpm, int spo2) {
  if (spo2 > 0 && spo2 < 90) return "LOW OXYGEN";
  if (bpm > 0 && bpm < 50) return "LOW BPM";
  if (bpm > 120) return "HIGH BPM";
  return "NORMAL";
}

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

// HÀM XỬ LÝ WEB SERVER
void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<title>HUST Health Monitor</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  
  // Dùng link CDN bản ổn định nhất của Chart.js để tránh lỗi
  html += "<script src='https://cdnjs.cloudflare.com/ajax/libs/Chart.js/3.9.1/chart.min.js'></script>";
  
  html += "<style>";
  html += "body{font-family:sans-serif; text-align:center; padding-top:20px; background-color:#f4f4f9; color:#333;} ";
  html += ".container{display:flex; flex-wrap:wrap; justify-content:center; max-width:800px; margin:0 auto;} ";
  html += ".card{background:white; border-radius:10px; padding:15px; margin:10px; width:140px; box-shadow:0 4px 8px rgba(0,0,0,0.05);} ";
  html += ".value{font-size:32px; font-weight:bold; color:#d9534f;} ";
  html += ".chart-container{background:white; border-radius:10px; padding:20px; margin:10px; width:90%; max-width:700px; box-shadow:0 4px 8px rgba(0,0,0,0.05);} ";
  html += "#status{margin-top:10px; font-weight:bold; color:#555; font-size:16px;}";
  html += "</style></head><body>";
  
  html += "<h2>Hệ Thống Theo Dõi Sức Khỏe</h2>";
  
  html += "<div class='container'>";
  html += "  <div class='card'><h3>BPM</h3><div class='value' id='bpm'>--</div></div>";
  html += "  <div class='card'><h3>SpO2</h3><div class='value' id='spo2'>--</div><span style='font-size:18px; font-weight:bold;'>%</span></div>";
  html += "</div>";
  
  html += "<div class='container'>";
  html += "  <div class='chart-container'><canvas id='realtimeChart'></canvas></div>";
  html += "</div>";
  
  html += "<div id='status'>Đang kết nối thiết bị...</div>";
  
  html += "<script>";
  // Khởi tạo biến biểu đồ nhưng dùng block try...catch để bắt lỗi
  html += "let healthChart = null;";
  html += "try {";
  html += "  const ctx = document.getElementById('realtimeChart').getContext('2d');";
  html += "  healthChart = new Chart(ctx, {";
  html += "    type: 'line',";
  html += "    data: {";
  html += "      labels: [],";
  html += "      datasets: [";
  html += "        { label: 'Nhịp tim (BPM)', data: [], borderColor: '#d9534f', backgroundColor: 'rgba(217,83,79,0.1)', tension: 0.3, yAxisID: 'yBpm', borderWidth: 2 },";
  html += "        { label: 'SpO2 (%)', data: [], borderColor: '#0275d8', backgroundColor: 'rgba(2,117,216,0.1)', tension: 0.3, yAxisID: 'ySpo2', borderWidth: 2 }";
  html += "      ]";
  html += "    },";
  html += "    options: {";
  html += "      responsive: true,";
  html += "      scales: {";
  html += "        yBpm: { type: 'linear', position: 'left', min: 40, max: 140 },";
  html += "        ySpo2: { type: 'linear', position: 'right', min: 80, max: 100, grid: { drawOnChartArea: false } }";
  html += "      },";
  html += "      animation: { duration: 0 }";
  html += "    }";
  html += "  });";
  html += "} catch (err) { console.error('Không thể vẽ biểu đồ:', err); }";
  
  // Vòng lặp cập nhật số liệu - Tách biệt độc lập với biểu đồ
  html += "setInterval(function(){";
  html += "  fetch('/data').then(response => response.json()).then(data => {";
  
  // 1. Cập nhật con số hiển thị
  html += "    document.getElementById('bpm').innerHTML = data.bpm > 0 ? data.bpm : '--';";
  html += "    document.getElementById('spo2').innerHTML = data.spo2 > 0 ? data.spo2 : '--';";
  
  // 2. Cập nhật trạng thái bằng chữ
  html += "    let statusText = 'Đang đo...';";
  html += "    if(data.bpm == 0) statusText = 'Vui lòng đặt ngón tay vào cảm biến';";
  html += "    else if(data.spo2 < 90) statusText = 'CẢNH BÁO: Oxy Thấp';";
  html += "    else statusText = 'Trạng thái: BÌNH THƯỜNG';";
  html += "    document.getElementById('status').innerHTML = statusText;";
  
  // 3. Nếu biểu đồ đã khởi tạo thành công mới cập nhật đường vẽ
  html += "    if(healthChart) {";
  html += "      let timeNow = new Date().toLocaleTimeString([], {hour: '2-digit', minute:'2-digit', second:'2-digit'});";
  html += "      if(healthChart.data.labels.length >= 20) {";
  html += "        healthChart.data.labels.shift();";
  html += "        healthChart.data.datasets[0].data.shift();";
  html += "        healthChart.data.datasets[1].data.shift();";
  html += "      }";
  html += "      healthChart.data.labels.push(timeNow);";
  html += "      healthChart.data.datasets[0].data.push(data.bpm > 0 ? data.bpm : null);";
  html += "      healthChart.data.datasets[1].data.push(data.spo2 > 0 ? data.spo2 : null);";
  html += "      healthChart.update();";
  html += "    }";
  
  html += "  }).catch(err => console.error('Lỗi khi lấy dữ liệu từ ESP32:', err));";
  html += "}, 1000);";
  html += "</script></body></html>";
  
  server.send(200, "text/html", html);
}

void handleData() {
  String json = "{\"bpm\":" + String(stableBPM) + ",\"spo2\":" + String(stableSpO2) + "}";
  server.send(200, "application/json", json);
}
// =====================================================================

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

  // ================= THÊM MỚI: KẾT NỐI WIFI TRONG SETUP =================
  display.clearDisplay();
  display.setCursor(0, 20);
  display.print("CONNECTING WIFI...");
  display.display();
  
  Serial.print("Đang kết nối WiFi...");
  WiFi.begin(ssid, password);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("\nKết nối WiFi thành công!");
  Serial.print("ĐỊA CHỈ IP ĐỂ XEM TRÊN WEB: ");
  Serial.println(WiFi.localIP()); 

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.begin();
}

void loop() {
  //DUY TRÌ WEB SERVER 
  server.handleClient(); // Chạy liên tục để lắng nghe truy cập từ web

  long currentIR = particleSensor.getIR();
  long currentRed = particleSensor.getRed();

  // Đẩy dữ liệu mới vào mảng lịch sử 
  irHistory[hIdx] = currentIR;
  redHistory[hIdx] = currentRed;

  // Lấy dữ liệu cũ nhất cách đây 25 mẫu (tương đương độ trễ ~60ms)
  int oldestIdx = (hIdx + 1) % HIST_SIZE;
  long oldestIR = irHistory[oldestIdx];
  long oldestRed = redHistory[oldestIdx];
  hIdx = oldestIdx; 

  // Tính độ sai lệch tín hiệu để phát hiện cử động ngón tay
  long diff = abs(currentIR - oldestIR);

  // khử nhiễu chuyển động (KHI KHÔNG CÓ TAY HOẶC ĐANG CỬ ĐỘNG)
  if (currentIR < 50000 || diff > 5000) {
    
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

  } else {      // (NGÓN TAY ĐÃ ĐẶT YÊN TĨNH) 
    
    // Thu thập biên độ AC bằng dữ liệu an toàn 
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

    //  hiển thị OLED khi đang đo nhịp tim
    if (millis() - lastOLED > OLED_INTERVAL) {
      String status = (stableBPM > 0) ? getStatus(stableBPM, stableSpO2) : "CALCULATING...";
      showOLED(stableBPM, stableSpO2, status);
      lastOLED = millis();
    }
  }
}