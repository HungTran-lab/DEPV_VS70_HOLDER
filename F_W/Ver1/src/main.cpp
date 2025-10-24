#include <Arduino.h>
#include <Bounce2.h>
#include <Ticker.h>
#include <Adafruit_NeoPixel.h>

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ==== Pin định nghĩa ====
#define START_PIN   39
#define STOP_PIN    36

#define ADC1_PIN    25
#define ADC2_PIN    33

#define SENSOR1_PIN 34
#define SENSOR2_PIN 35
#define SENSOR3_PIN 26   // tránh trùng GPIO17

#define SOL1_PIN    4
#define SOL2_PIN    5
#define SOL3_PIN    15
#define SOL4_PIN    17

#define LED_GREEN   14
#define LED_RED     27
#define BUZZER_PIN  18

#define WS2812_PIN  13
#define WS2812_NUM  1   // chỉ 1 LED


// ==== Ngưỡng ADC ====
const int ADC1_MIN = 2400;   // chỉnh theo thực tế 2500
const int ADC1_MAX = 2600;
const int ADC2_MIN = 3900;   //4095
const int ADC2_MAX = 4100;

// ==== OLED ====
#define SDA_PIN  14
#define SCL_PIN  27
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ==== Biến ====
Bounce debStart = Bounce();
Bounce debStop  = Bounce();

int solenoidStep = 0;
bool systemRunning = false;
bool buzzerActive = false;

// ==== Prototype ====
void showSolenoidStep(int step);
void solenoidSequence();
void stopBuzzer();
void doCheck();
void resetSystem();

// ==== Khai báo Ticker ====

int solenoidDelay[4] = {200, 400, 600, 800}; // ms cho solenoid 1→4

Ticker solenoidTimer(solenoidSequence, 1000, 0, MILLIS);
Ticker buzzerTimer(stopBuzzer, 2000, 1, MILLIS);
Ticker checkTimer(doCheck, 2000, 1, MILLIS);    // chờ 2s sau sol4 ON rồi mới đo
Ticker resetTimer(resetSystem, 2000, 1, MILLIS); // reset sau khi báo kết quả 2s

// ==== WS2812 ====
Adafruit_NeoPixel pixel(WS2812_NUM, WS2812_PIN, NEO_GRB + NEO_KHZ800);

// ==== Hàm quy đổi ADC ====
int convertADC(int value, int minVal, int maxVal) 
{
  if (value >= minVal && value <= maxVal)
   return 1;
   return 0;
} 

void setPixelColor(uint8_t r, uint8_t g, uint8_t b) {
  pixel.setPixelColor(0, pixel.Color(r, g, b));
  pixel.show();
}
// ==== Hàm update OLED ====
void showStatus(const char* line1, const char* line2) 
{
  display.clearDisplay();
  display.setTextSize(2); 
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 8);
  display.println(line1);

  display.setCursor(0, 45);
  display.println(line2);

  display.display();
}
// ==== Hàm tắt buzzer sau 2s ====
void stopBuzzer() {
  digitalWrite(BUZZER_PIN, LOW);
  buzzerActive = false;
  // Serial.println("Buzzer OFF");
}

// ==== Hàm kiểm tra sensor + ADC (sau 2s delay) ====
void doCheck() 
{
  int adc1_raw = analogRead(ADC1_PIN);
  int adc2_raw = analogRead(ADC2_PIN);

  int adc1 = convertADC(adc1_raw, ADC1_MIN, ADC1_MAX);
  int adc2 = convertADC(adc2_raw, ADC2_MIN, ADC2_MAX);

  int s1 = digitalRead(SENSOR1_PIN);
  int s2 = digitalRead(SENSOR2_PIN);
  int s3 = digitalRead(SENSOR3_PIN);

  // Gửi data 1 lần
  // Serial.printf("data: %d,%d,%d,%d,%d\n", s1, s2, s3, adc1, adc2);
  //showStatus(s1, s2, s3, adc1, adc2);

  // Kiểm tra OK/NG
// Gửi data mới: 0 = OK, 1 = NG
int d1 = (s1 == 1) ? 0 : 1;
int d2 = (s2 == 1) ? 0 : 1;
int d3 = (s3 == 1) ? 0 : 1;
int d4 = (adc1 == 1) ? 0 : 1;
int d5 = (adc2 == 1) ? 0 : 1;

bool allOK = (d1 == 0 && d2 == 0 && d3 == 0 && d4 == 0 && d5 == 0);

  if (allOK) 
  {
    digitalWrite(LED_GREEN, HIGH);
    digitalWrite(LED_RED, LOW);
    setPixelColor(0, 255, 0);   // xanh
    showStatus("Check Done", "PASS");
    Serial.printf("OK: data=%d,%d,%d,%d,%d\n", d1, d2, d3, d4, d5);
  }
   else 
   {
    digitalWrite(LED_GREEN, LOW);
    digitalWrite(LED_RED, HIGH); setPixelColor(255, 0, 0);   // đỏ
    showStatus("Check Done", "FAIL");
    Serial.printf("NG: data=%d,%d,%d,%d,%d\n", d1, d2, d3, d4, d5);
    if (!buzzerActive) {
      digitalWrite(BUZZER_PIN, HIGH);
      buzzerActive = true;
      buzzerTimer.start(); // tự tắt sau 2s
      // Serial.println("Buzzer ON");
    }
  }

  // Sau 2s reset hệ thống
  resetTimer.start();
}

// ==== Hàm reset ====
void resetSystem() 
{
  digitalWrite(SOL1_PIN, LOW);
  digitalWrite(SOL2_PIN, LOW);
  digitalWrite(SOL3_PIN, LOW);
  digitalWrite(SOL4_PIN, LOW);
  digitalWrite(LED_GREEN, LOW);
  digitalWrite(LED_RED, LOW);
  digitalWrite(BUZZER_PIN, LOW);    
  // setPixelColor(0, 0, 0); // tắt WS2812
  showStatus("Idle", "WAIT START");

  solenoidStep = 0;
  systemRunning = false;
  buzzerActive = false;

  solenoidTimer.stop();
  buzzerTimer.stop();
  checkTimer.stop();
  resetTimer.stop();

  // Serial.println("System reset");
}

// ==== Hàm bật solenoid tuần tự ====
void solenoidSequence() 
{
    char buf[32];
  sprintf(buf, "Soli %d", solenoidStep);
  showStatus(buf, "RUN...");
  //-------------------------
  solenoidStep++;
  switch (solenoidStep) 
  {
    case 1: digitalWrite(SOL1_PIN, HIGH); break;
    case 2: digitalWrite(SOL2_PIN, HIGH); break;
    case 3: digitalWrite(SOL3_PIN, HIGH); break;
    case 4:
      digitalWrite(SOL4_PIN, HIGH);
      solenoidTimer.stop();        // dừng chuỗi solenoid
      setPixelColor(0, 0, 255); // Blue trong lúc chờ ADC ổn định
      checkTimer.start();          // chờ 2s rồi mới đo
      // Serial.println("Solenoid4 ON -> wait 2s for ADC stable");
      break;
  }
  if (solenoidStep < 4) 
  {
    solenoidTimer.stop();
    solenoidTimer.interval(solenoidDelay[solenoidStep]); // ms cho lần kế tiếp
    solenoidTimer.start();
  }


  // Serial.printf("Solenoid step %d ON\n", solenoidStep);
  // showSolenoidStep(solenoidStep);  
}

void setup() 
{
  Serial.begin(115200);

  // Input
  pinMode(START_PIN, INPUT_PULLUP);
  pinMode(STOP_PIN, INPUT_PULLUP);
  pinMode(SENSOR1_PIN, INPUT);
  pinMode(SENSOR2_PIN, INPUT);
  pinMode(SENSOR3_PIN, INPUT);

  // Output
  pinMode(SOL1_PIN, OUTPUT);
  pinMode(SOL2_PIN, OUTPUT);
  pinMode(SOL3_PIN, OUTPUT);
  pinMode(SOL4_PIN, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_RED, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  Wire.begin(SDA_PIN, SCL_PIN);

  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) 
  {
    Serial.println("SSD1306 allocation failed");
    for(;;);
  }
  display.clearDisplay();
  display.display();
  showStatus("Idle", "WAIT START");

  resetSystem();

  // Setup Bounce2
  debStart.attach(START_PIN);
  debStart.interval(25);
  debStop.attach(STOP_PIN);
  debStop.interval(25);
}

void loop() 
{
  debStart.update();
  debStop.update();

  // update ticker
  solenoidTimer.update();
  buzzerTimer.update();
  checkTimer.update();
  resetTimer.update();

  // STOP button
  if (debStop.fell()) 
  {
    setPixelColor(0, 0, 0); // tắt WS2812 khi STOP
    resetSystem();
  }

  // START button
  if (!systemRunning && debStart.fell()) 
  {
    setPixelColor(0, 0, 0); // tắt WS2812 cho chu kỳ mới
    systemRunning = true;
    solenoidStep = 0;
    solenoidTimer.start();   // bắt đầu chuỗi solenoid
    // Serial.println("System started");
  }
}


