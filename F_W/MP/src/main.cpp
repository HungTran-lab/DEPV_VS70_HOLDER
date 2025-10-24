#include <Arduino.h>
#include <Wire.h>
#include <Bounce2.h>
#include <Ticker.h>

#include "pins.h"
#include "config.h"

#if USE_OLED
  #include <Adafruit_GFX.h>
  #include <Adafruit_SSD1306.h>
  Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
  static bool g_oled_ok = false;
#endif

#if USE_ADS1115
  #include <Adafruit_ADS1X15.h>
  Adafruit_ADS1115 ads;
  static bool g_ads_ok = false;
#endif

// ---- Forward declarations (Ticker callbacks) ----
void solenoidSequence();
void stopBuzzer();
void doCheck();
void resetSystem();
void sol1OffCB();   // tắt Sol1 trễ 1s

// ---- Tickers (con trỏ; tạo bằng new trong setup) ----
Ticker *solenoidTimer = nullptr;
Ticker *buzzerTimer   = nullptr;
Ticker *checkTimer    = nullptr;
Ticker *resetTimer    = nullptr;
Ticker *sol1OffTimer  = nullptr;  // tắt Sol1 trễ

// ---- Buttons ----
Bounce2::Button btnStart, btnStop;

// ---- App state ----
enum class AppState : uint8_t { Idle=0, Active=1, Result=2 };
static AppState g_state = AppState::Idle;

static uint8_t  solenoidStep  = 0;     // 0..3
static uint32_t solMs[4]      = { SOL_MS_0, SOL_MS_1, SOL_MS_2, SOL_MS_3 };

// ---- PCF8575 shadows ----
static uint16_t u57_shadow = 0xFFFF;   // outputs: 1=release, 0=drive LOW
static uint16_t u58_inputs = 0xFFFF;

// ---- Utils ----
static inline uint32_t clampu32(uint32_t v, uint32_t lo, uint32_t hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}
static void clampSolMs() {
  for (int i = 0; i < 4; ++i) {
    solMs[i] = clampu32(solMs[i], SOL_MS_MIN, SOL_MS_MAX);
    if (i && solMs[i] < solMs[i-1]) solMs[i] = solMs[i-1];
  }
}
static inline bool inRange(int v, int lo, int hi) { return (v >= lo) && (v <= hi); }
static inline int bit_as_int(uint16_t v, uint8_t bit) { return (v >> bit) & 1; }

// ---- PCF8575 low-level ----
static bool pcf8575_write_word(uint8_t addr, uint16_t value) {
  Wire.beginTransmission(addr);
  Wire.write((uint8_t)(value & 0xFF));
  Wire.write((uint8_t)((value >> 8) & 0xFF));
  return (Wire.endTransmission() == 0);
}
static bool pcf8575_read_word(uint8_t addr, uint16_t &out) {
  if (Wire.requestFrom((int)addr, 2) != 2) return false;
  uint8_t lo = Wire.read();
  uint8_t hi = Wire.read();
  out = (uint16_t)lo | ((uint16_t)hi << 8);
  return true;
}
static inline bool logic_to_pcf_level(bool on, bool active_low) {
  // PCF: 0 = ON (kéo xuống), 1 = OFF (thả nổi)
  if (active_low) return on ? 0 : 1;
  else            return on ? 1 : 0;
}
static void u57_set_bit(uint8_t bit, bool on, bool active_low) {
  bool level = logic_to_pcf_level(on, active_low);
  if (level) u57_shadow |=  (1u << bit);
  else       u57_shadow &= ~(1u << bit);
  pcf8575_write_word(I2C_ADDR_U57, u57_shadow);
}

// ---- Output helpers ----
static void solenoids_off_only() {
  u57_set_bit(U57_SOL1_BIT, false, U57_ACTIVE_LOW_SOLENOID);
  u57_set_bit(U57_SOL2_BIT, false, U57_ACTIVE_LOW_SOLENOID);
  u57_set_bit(U57_SOL3_BIT, false, U57_ACTIVE_LOW_SOLENOID);
  u57_set_bit(U57_SOL4_BIT, false, U57_ACTIVE_LOW_SOLENOID);
}
static void leds_off_only() {
  u57_set_bit(U57_LED_OK_BIT, false, U57_ACTIVE_LOW_LED);
  u57_set_bit(U57_LED_NG_BIT, false, U57_ACTIVE_LOW_LED);
}
static void buzzer_off_only() {
#if USE_BUZZER
  u57_set_bit(U57_BUZZER_BIT, false, U57_ACTIVE_LOW_BUZZER);
#endif
}
static void u57_all_off() { solenoids_off_only(); leds_off_only(); buzzer_off_only(); }

// ---- Buzzer ----
static void buzzFor(uint16_t ms) {
#if USE_BUZZER
  u57_set_bit(U57_BUZZER_BIT, true,  U57_ACTIVE_LOW_BUZZER);
  if (buzzerTimer) { buzzerTimer->stop(); buzzerTimer->interval(ms); buzzerTimer->start(); }
#else
  (void)ms;
#endif
}
void stopBuzzer() {
  if (buzzerTimer) buzzerTimer->stop();  // one-shot
#if USE_BUZZER
  u57_set_bit(U57_BUZZER_BIT, false, U57_ACTIVE_LOW_BUZZER);
#endif
}

// ---- OLED UI ----
#if USE_OLED
static void drawUI(int v1, bool ok1, int v2, bool ok2, uint16_t sen_bits) {
  if (!g_oled_ok) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.print(g_state == AppState::Active ? "RUN" :
                (g_state == AppState::Result ? "RES" : "IDLE"));

  display.setCursor(0, 10);
  display.print("STEP:");
  display.print(g_state == AppState::Active ? (int)solenoidStep : -1);

  display.setCursor(0, 22);
  display.print("ADC1:"); display.print(v1); display.print(ok1 ? " OK":" NG");

  display.setCursor(0, 34);
  display.print("ADC2:"); display.print(v2); display.print(ok2 ? " OK":" NG");

  display.setCursor(0, 46);
  display.print("S1:"); display.print((sen_bits >> U58_SEN1_BIT) & 1);
  display.print(" S2:"); display.print((sen_bits >> U58_SEN2_BIT) & 1);
  display.print(" S3:"); display.print((sen_bits >> U58_SEN3_BIT) & 1);

  display.display();
}
#else
static void drawUI(int, bool, int, bool, uint16_t) {}
#endif

// ---- ADC ----
static int readADCstable(uint8_t ch, uint8_t samples = 8) {
  int32_t acc = 0;
#if USE_ADS1115
  if (!g_ads_ok) return -1;
  for (uint8_t i = 0; i < samples; ++i) {
    int16_t raw = ads.readADC_SingleEnded(ch); // 0..32767
    long val = (long)raw * 4095L / 32767L;
    if (val < 0) val = 0;
    if (val > 4095) val = 4095;
    acc += (int32_t)val;
  }
#else
  acc = 0;
#endif
  return (int)(acc / (int32_t)samples);
}

// ---- Sequence control ----
static void scheduleNextStep(uint8_t curStep) {
  if (!solenoidTimer) return;
  if (curStep >= 3) { solenoidTimer->stop(); return; }
  uint32_t cur = solMs[curStep];
  uint32_t nxt = solMs[curStep + 1];
  uint32_t delta = (nxt > cur) ? (nxt - cur) : 1;

  solenoidTimer->stop();        // one-shot re-arm
  solenoidTimer->interval(delta);
  solenoidTimer->start();
}

// BẬT CỘNG DỒN: step0 -> S1 ON; step1 -> S1+S2 ON; step2 -> S1+S2+S3 ON; step3 -> S1..S4 ON
static void applyStepOutputs(uint8_t step) {
  u57_set_bit(U57_SOL1_BIT, step >= 0, U57_ACTIVE_LOW_SOLENOID);
  u57_set_bit(U57_SOL2_BIT, step >= 1, U57_ACTIVE_LOW_SOLENOID);
  u57_set_bit(U57_SOL3_BIT, step >= 2, U57_ACTIVE_LOW_SOLENOID);
  u57_set_bit(U57_SOL4_BIT, step >= 3, U57_ACTIVE_LOW_SOLENOID);
}

// Tắt Sol1 trễ 1s
static inline void sol1_off_immediate() {
  u57_set_bit(U57_SOL1_BIT, false, U57_ACTIVE_LOW_SOLENOID);
}
void sol1OffCB() 
{
  if (sol1OffTimer) sol1OffTimer->stop(); // one-shot
  sol1_off_immediate();
  // Serial.println("SOL1 OFF (delayed)");
}

// OFF theo yêu cầu: tắt 2/3/4 ngay, 1s sau tắt 1
static void solenoids_off_delayed() {
  u57_set_bit(U57_SOL2_BIT, false, U57_ACTIVE_LOW_SOLENOID);
  u57_set_bit(U57_SOL3_BIT, false, U57_ACTIVE_LOW_SOLENOID);
  u57_set_bit(U57_SOL4_BIT, false, U57_ACTIVE_LOW_SOLENOID);

  if (sol1OffTimer) {
    sol1OffTimer->stop();
    sol1OffTimer->interval(SOL1_OFF_DELAY_MS);
    sol1OffTimer->start();
  } else {
    sol1_off_immediate();
  }
}

static inline void stopAllTimers() {
  if (solenoidTimer) solenoidTimer->stop();
  if (checkTimer)    checkTimer->stop();
  if (resetTimer)    resetTimer->stop();
  if (buzzerTimer)   buzzerTimer->stop();
  if (sol1OffTimer)  sol1OffTimer->stop();
}

// ---- START / STOP behavior ----
static void beginSequence() 
{
  clampSolMs();

  // Reset LED & buzzer khi nhấn START
  stopAllTimers();
  buzzer_off_only();
  leds_off_only();

  g_state = AppState::Active;
  solenoidStep = 0;

  applyStepOutputs(solenoidStep);     // Bật S1 ngay
  Serial.println("START");
  buzzFor(60);                        // beep ngắn
  scheduleNextStep(solenoidStep);     // hẹn bước tiếp
}

static void abortSequenceToIdle() {
  // Reset LED & buzzer khi nhấn STOP
  stopAllTimers();
  buzzer_off_only();
  leds_off_only();

  solenoids_off_delayed();            // S2/3/4 off ngay, S1 off trễ
  g_state = AppState::Idle;
  Serial.println("STOP");
}

// ---- PRINT result line ----
static void printResultLine(bool overall_ok, int s1, int s2, int s3, int adc1, int adc2) 
{
  Serial.printf("%sdata=%d,%d,%d,%d,%d\n",
                overall_ok ? "OK:": "NG:",
                s1, s2, s3, adc1, adc2);
}

// ---- Measurement & result ----
void doCheck() 
{
  if (checkTimer) checkTimer->stop();   // one-shot

  if (g_state != AppState::Active && g_state != AppState::Result) return;

  // Sensors
  uint16_t sen;
  if (pcf8575_read_word(I2C_ADDR_U58, sen)) u58_inputs = sen;
  int s1 = bit_as_int(u58_inputs, U58_SEN1_BIT);
  int s2 = bit_as_int(u58_inputs, U58_SEN2_BIT);
  int s3 = bit_as_int(u58_inputs, U58_SEN3_BIT);

  // ADC
  int v1 = readADCstable(0, 8);
  int v2 = readADCstable(1, 8);

  bool ok1 = (v1 >= 0) && inRange(v1, ADC1_MIN - ADC_HYS, ADC1_MAX + ADC_HYS);
  bool ok2 = (v2 >= 0) && inRange(v2, ADC2_MIN - ADC_HYS, ADC2_MAX + ADC_HYS);

  // Điều kiện OK: s1=s2=s3=0 và ADC trong ngưỡng
  bool sen_ok = (s1 == 0) && (s2 == 0) && (s3 == 0);
  bool overall_ok = sen_ok && ok1 && ok2;

  // Latch LED (chỉ reset khi Start/Stop)
  u57_set_bit(U57_LED_OK_BIT, overall_ok,  U57_ACTIVE_LOW_LED);
  u57_set_bit(U57_LED_NG_BIT, !overall_ok, U57_ACTIVE_LOW_LED);

  // Buzzer
  if (overall_ok) buzzFor(120);
  else            buzzFor(2000);

    // Đưa adc1/adc2 về cờ 0/1 theo yêu cầu (0=OK, 1=NG)
  int adc1_flag = ok1 ? 0 : 1;
  int adc2_flag = ok2 ? 0 : 1;


  // Print format yêu cầu
  //printResultLine(overall_ok, s1, s2, s3, v1, v2);
  printResultLine(overall_ok, s1, s2, s3, adc1_flag, adc2_flag);

  // Sang RESULT, sau 2s chỉ về IDLE và tắt solenoids (LED & buzzer không đụng)
  g_state = AppState::Result;
  if (resetTimer) { resetTimer->stop(); resetTimer->interval(2000); resetTimer->start(); }
}

// ---- Ticker callback ----
void solenoidSequence() {
  if (solenoidTimer) solenoidTimer->stop();   // one-shot re-arm

  if (g_state != AppState::Active) return;

  solenoidStep++;
  if (solenoidStep <= 3) {
    // Serial.printf("Step -> %u\n", solenoidStep);
    applyStepOutputs(solenoidStep);

    if (solenoidStep == 3) {
      // Sol4 vừa ON -> chờ 2s rồi đo
      if (checkTimer) { checkTimer->stop(); checkTimer->interval(2000); checkTimer->start(); }
    } else {
      scheduleNextStep(solenoidStep);
    }
  }
}

void resetSystem() {
  if (resetTimer) resetTimer->stop();      // one-shot
  solenoids_off_delayed();                 // S2/3/4 off ngay, S1 off sau 1s
  g_state = AppState::Idle;
  // Serial.println("RESET -> back to IDLE");
}

// ---- Setup / Loop ----
void setup() {
  Serial.begin(115200);
  delay(100);

  Wire.begin(I2C_SDA, I2C_SCL);

#if USE_OLED
  g_oled_ok = display.begin(SSD1306_SWITCHCAPVCC, I2C_ADDR_OLED);
  if (g_oled_ok) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.print("OLED OK");
    display.display();
  }
#endif

#if USE_ADS1115
  g_ads_ok = ads.begin(I2C_ADDR_ADS);
  if (g_ads_ok) ads.setGain(GAIN_TWOTHIRDS);
#endif

  // Init outputs
  pcf8575_write_word(I2C_ADDR_U57, u57_shadow);
  u57_all_off();   // boot: tắt ngay tất cả để an toàn (không trễ)

  // U58: đặt input (ghi 1 để release)
  pcf8575_write_word(I2C_ADDR_U58, 0xFFFF);

  // ---- Tạo các Ticker bằng new (tránh static init issues) ----
  solenoidTimer = new Ticker(solenoidSequence, 1000, 1, MILLIS); // interval sẽ đổi động
  buzzerTimer   = new Ticker(stopBuzzer,       2000, 1, MILLIS);
  checkTimer    = new Ticker(doCheck,          2000, 1, MILLIS);
  resetTimer    = new Ticker(resetSystem,      2000, 1, MILLIS);
  sol1OffTimer  = new Ticker(sol1OffCB,        SOL1_OFF_DELAY_MS, 1, MILLIS);

  // Buttons
  btnStart.attach(START_PIN, INPUT);
  btnStart.interval(BOUNCE_MS);
  btnStart.setPressedState(LOW);

  btnStop.attach(STOP_PIN, INPUT);
  btnStop.interval(BOUNCE_MS);
  btnStop.setPressedState(LOW);

  // Serial.println("\n== B16M (ESP32-S3) READY ==");
  // Serial.printf("I2C: SDA=%d SCL=%d\n", I2C_SDA, I2C_SCL);
  // Serial.printf("START=%d, STOP=%d\n", START_PIN, STOP_PIN);
  // Serial.printf("U57(0x%02X)=outputs, U58(0x%02X)=inputs\n", I2C_ADDR_U57, I2C_ADDR_U58);
// #if USE_ADS1115
//   Serial.printf("ADS1115: %s @0x%02X\n", g_ads_ok ? "OK": "NG", I2C_ADDR_ADS);
// #endif
// #if USE_OLED
//   Serial.printf("OLED   : %s @0x%02X\n", g_oled_ok ? "OK": "NG", I2C_ADDR_OLED);
// #endif
//   Serial.printf("solMs  : [%u,%u,%u,%u]\n", solMs[0], solMs[1], solMs[2], solMs[3]);
}

void loop() {
  btnStart.update();
  btnStop.update();

  if (btnStart.fell()) {
    // Nhấn START: reset LED/Buzzer & bắt đầu mới
    beginSequence();
  }

  if (btnStop.fell()) {
    // Nhấn STOP: reset LED/Buzzer & về Idle
    abortSequenceToIdle();
  }

  // Read sensors (UI)
  uint16_t sen;
  if (pcf8575_read_word(I2C_ADDR_U58, sen)) u58_inputs = sen;

  // Read ADC (UI)
  int v1 = readADCstable(0, 8);
  int v2 = readADCstable(1, 8);
  bool ok1 = (v1 >= 0) && inRange(v1, ADC1_MIN - ADC_HYS, ADC1_MAX + ADC_HYS);
  bool ok2 = (v2 >= 0) && inRange(v2, ADC2_MIN - ADC_HYS, ADC2_MAX + ADC_HYS);

  // OLED
  drawUI(v1, ok1, v2, ok2, u58_inputs);

  // Update timers
  if (solenoidTimer) solenoidTimer->update();
  if (checkTimer)    checkTimer->update();
  if (resetTimer)    resetTimer->update();
  if (buzzerTimer)   buzzerTimer->update();
  if (sol1OffTimer)  sol1OffTimer->update();

  delay(2);
}
