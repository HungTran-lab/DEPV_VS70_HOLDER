#include <Arduino.h>
#include <Wire.h>
#include <Bounce2.h>
#include <Ticker.h>
#include <cstring>  // strlen, strcmp, strncmp, strncpy
#include <new>      // placement new (no heap for Ticker)

#include "pins.h"
#include "config.h"

#ifndef KC_DEBUG
  #define KC_DEBUG 0
#endif

#if USE_ADS1115
  #include <Adafruit_ADS1X15.h>
  Adafruit_ADS1115 ads;
  static bool g_ads_ok = false;
#endif

// ===== Relay active level (có thể override trong config.h) =====
#ifndef U57_ACTIVE_LOW_RELAY
  #define U57_ACTIVE_LOW_RELAY U57_ACTIVE_LOW_SOLENOID
#endif

// ---- Forward declarations (Ticker callbacks) ----
void solenoidSequence();
void stopBuzzer();
void doCheck();
void resetSystem();
void sol1OffCB();   // tắt Sol1 trễ 1s

// ---- Tickers (con trỏ; tạo bằng placement-new trong setup để tránh heap) ----
Ticker *solenoidTimer = nullptr;
Ticker *buzzerTimer   = nullptr;
Ticker *checkTimer    = nullptr;
Ticker *resetTimer    = nullptr;
Ticker *sol1OffTimer  = nullptr;  // tắt Sol1 trễ

// buffer tĩnh cho placement-new (không dùng heap)
alignas(Ticker) static uint8_t _tbuf_solenoid[sizeof(Ticker)];
alignas(Ticker) static uint8_t _tbuf_buzzer[sizeof(Ticker)];
alignas(Ticker) static uint8_t _tbuf_check[sizeof(Ticker)];
alignas(Ticker) static uint8_t _tbuf_reset[sizeof(Ticker)];
alignas(Ticker) static uint8_t _tbuf_sol1off[sizeof(Ticker)];  // tắt Sol1 trễ

// ---- Buttons ----
Bounce2::Button btnStart;

// ---- STOP via interrupt ----
volatile bool g_stop_irq = false;
// Lưu ý: ISR chỉ set cờ. Debounce + timestamp làm ở loop() (an toàn hơn trên ESP32)
void IRAM_ATTR onStopISR() {
  g_stop_irq = true;
}

// ---- App state ----
enum class AppState : uint8_t { Idle=0, Active=1, Result=2 };
static AppState g_state = AppState::Idle;

static uint8_t  solenoidStep  = 0;     // 0..3
static uint32_t solMs[4]      = { SOL_MS_0, SOL_MS_1, SOL_MS_2, SOL_MS_3 };

// ---- PCF8575 shadows ----
static uint16_t u57_shadow = 0xFFFF;   // outputs: 1=release, 0=drive LOW
static uint16_t u57_last   = 0xFFFF;   // cache để tránh ghi I2C lặp
static bool     u57_dirty  = true;
static uint8_t  u57_batch  = 0;        // batch counter
static uint16_t u58_inputs = 0xFFFF;

// ---- Forward for U57 bit control (definition is below) ----
static void u57_set_bit(uint8_t bit, bool on, bool active_low);

// ================================================================
//                    MODEL SELECTION (267A / 269A)
// ================================================================
enum class ModelSel : uint8_t { M267A = 0, M269A = 1 };
static ModelSel g_model = ModelSel::M267A;   // mặc định 267A để dễ test

// ================================================================
//                  SERIAL COMMAND HANDLER
// ================================================================
static char    rx_buf[48];
static uint8_t rx_len = 0;

static const char* ltrim(const char* s) {
  while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') ++s;
  return s;
}
static void rtrim(char* s) {
  int n = strlen(s);
  while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r' || s[n-1] == '\n')) s[--n] = '\0';
}
static void toUpperInPlace(char* s) {
  for (; *s; ++s) {
    if (*s >= 'a' && *s <= 'z') *s = *s - 'a' + 'A';
  }
}
static inline const char* modelName(ModelSel m) {
  return (m == ModelSel::M267A) ? "DJ9600267A" : "DJ9600269A";
}

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
static inline void u57_begin_batch() { ++u57_batch; }

static inline bool u57_commit() {
  if (!u57_dirty || u57_shadow == u57_last) return true;
  bool ok = pcf8575_write_word(I2C_ADDR_U57, u57_shadow);
  if (ok) {
    u57_last  = u57_shadow;
    u57_dirty = false;
  }
  return ok;
}

static inline void u57_end_batch() {
  if (u57_batch == 0) return;
  --u57_batch;
  if (u57_batch == 0) (void)u57_commit();
}

static void u57_set_bit(uint8_t bit, bool on, bool active_low) {
  bool level = logic_to_pcf_level(on, active_low);
  uint16_t next = u57_shadow;
  if (level) next |=  (1u << bit);
  else       next &= ~(1u << bit);

  if (next != u57_shadow) {
    u57_shadow = next;
    u57_dirty  = true;
  }
  if (u57_batch == 0) (void)u57_commit();
}

static void applyModelSideEffects(ModelSel m) {
  // Theo yêu cầu: 267A -> relay ON, 269A -> relay OFF
  if (m == ModelSel::M267A) {
    u57_set_bit(U57_RELAY_BIT, true,  U57_ACTIVE_LOW_RELAY);
  } else {
    u57_set_bit(U57_RELAY_BIT, false, U57_ACTIVE_LOW_RELAY);
  }
}

static void handleLine(const char* raw) {
  // copy ra buffer tạm để trim/uppercase an toàn
  char line[48];
  strncpy(line, raw, sizeof(line)-1);
  line[sizeof(line)-1] = '\0';

  char* p = (char*)ltrim(line);
  rtrim(p);
  toUpperInPlace(p);

  // Format: MODEL=XXXXXXXXXX
  const char* key = "MODEL=";
  const size_t keylen = 6;
  if (strncmp(p, key, keylen) != 0) {
    // Serial.printf("NG:UNKNOWN_CMD [%s]\n", p);
    return;
  }
  const char* val = p + keylen;

  if (strcmp(val, "DJ9600267A") == 0) {
    g_model = ModelSel::M267A;
    applyModelSideEffects(g_model);
    // Serial.println("ACK MODEL=DJ9600267A RELAY=ON ADCPAIR=1-2");
  } else if (strcmp(val, "DJ9600269A") == 0) {
    g_model = ModelSel::M269A;
    applyModelSideEffects(g_model);
    // Serial.println("ACK MODEL=DJ9600269A RELAY=OFF ADCPAIR=3-4");
  } else {
    // Serial.printf("NG:UNKNOWN_MODEL [%s]\n", val);
  }
}

static void pollSerial() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();

    // Kết thúc dòng: xử lý
    if (c == '\n' || c == '\r') {
      if (rx_len > 0) {
        rx_buf[rx_len] = '\0';
        handleLine(rx_buf);
        rx_len = 0;
      }
      continue;
    }

    // Bỏ ký tự không in được
    if ((uint8_t)c < 0x20 && c != '\t') continue;

    // Tích lũy vào buffer (chống tràn)
    if (rx_len < sizeof(rx_buf)-1) {
      rx_buf[rx_len++] = c;
    } else {
      // tràn: reset buffer
      rx_len = 0;
      // Serial.println("NG:RX_OVERFLOW");
    }
  }
}
// ================================================================

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

// ---- Output helpers ----
static void solenoids_off_only() {
  u57_begin_batch();
  u57_set_bit(U57_SOL1_BIT, false, U57_ACTIVE_LOW_SOLENOID);
  u57_set_bit(U57_SOL2_BIT, false, U57_ACTIVE_LOW_SOLENOID);
  u57_set_bit(U57_SOL3_BIT, false, U57_ACTIVE_LOW_SOLENOID);
  u57_set_bit(U57_SOL4_BIT, false, U57_ACTIVE_LOW_SOLENOID);
  u57_end_batch();
}
static void leds_off_only() {
  u57_begin_batch();
  u57_set_bit(U57_LED_OK_BIT, false, U57_ACTIVE_LOW_LED);
  u57_set_bit(U57_LED_NG_BIT, false, U57_ACTIVE_LOW_LED);
  u57_end_batch();
}
static void relay_off_only() {
  u57_set_bit(U57_RELAY_BIT, false, U57_ACTIVE_LOW_RELAY);
}
static void buzzer_off_only() {
#if USE_BUZZER
  u57_set_bit(U57_BUZZER_BIT, false, U57_ACTIVE_LOW_BUZZER);
#endif
}
static void u57_all_off() { u57_begin_batch(); solenoids_off_only(); leds_off_only(); relay_off_only(); buzzer_off_only(); u57_end_batch(); }

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

// ---- UI placeholder (no OLED) ----
static inline void drawUI(int, bool, int, bool, uint16_t) {}

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
  // BẬT CỘNG DỒN: step0 -> S1 ON; step1 -> S1+S2 ON; step2 -> S1+S2+S3 ON; step3 -> S1..S4 ON
  u57_begin_batch();
  u57_set_bit(U57_SOL1_BIT, true,      U57_ACTIVE_LOW_SOLENOID);   // step>=0 luôn true với uint8_t
  u57_set_bit(U57_SOL2_BIT, step >= 1, U57_ACTIVE_LOW_SOLENOID);
  u57_set_bit(U57_SOL3_BIT, step >= 2, U57_ACTIVE_LOW_SOLENOID);
  u57_set_bit(U57_SOL4_BIT, step >= 3, U57_ACTIVE_LOW_SOLENOID);
  u57_end_batch();
}

// Tắt Sol1 trễ 1s
static inline void sol1_off_immediate() {
  u57_set_bit(U57_SOL1_BIT, false, U57_ACTIVE_LOW_SOLENOID);
}
void sol1OffCB() 
{
  if (sol1OffTimer) sol1OffTimer->stop(); // one-shot
  sol1_off_immediate();
}

// OFF theo yêu cầu: tắt 2/3/4 ngay, 1s sau tắt 1
static void solenoids_off_delayed() {
  u57_begin_batch();
  u57_set_bit(U57_SOL2_BIT, false, U57_ACTIVE_LOW_SOLENOID);
  u57_set_bit(U57_SOL3_BIT, false, U57_ACTIVE_LOW_SOLENOID);
  u57_set_bit(U57_SOL4_BIT, false, U57_ACTIVE_LOW_SOLENOID);
  u57_end_batch();

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
static void printResultLine(bool overall_ok, int s1, int s2, int s3, int adcA, int adcB) 
{
  Serial.printf("%sdata=%d,%d,%d,%d,%d\n",
                overall_ok ? "OK:": "NG:",
                s1, s2, s3, adcA, adcB);
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

  // ADC: chỉ đọc đúng cặp theo MODEL (giảm thời gian đo)
  int v1 = -1, v2 = -1, v3 = -1, v4 = -1;
  if (g_model == ModelSel::M267A) {
    v1 = readADCstable(0, 8);
    v2 = readADCstable(1, 8);
#if KC_DEBUG
    v3 = readADCstable(2, 4);
    v4 = readADCstable(3, 4);
#endif
  } else { // M269A
    v3 = readADCstable(2, 8);
    v4 = readADCstable(3, 8);
#if KC_DEBUG
    v1 = readADCstable(0, 4);
    v2 = readADCstable(1, 4);
#endif
  }

#if KC_DEBUG
    //  Serial.printf("A1=%5d- A2=%5d- A3=%5d- A4=%5d-", v1,v2,v3,v4);
  Serial.printf("A1=%5d- A2=%5d- A3=%5d- A4=%5d-\r\n", v1,v2,v3,v4);
#endif
  // Chọn cặp theo MODEL
  int a = 0, b = 0;
  bool okA = false, okB = false;
  if (g_model == ModelSel::M267A) {
    a   = (v1 < 0) ? -1 : v1;
    b   = (v2 < 0) ? -1 : v2;
    okA = (a >= 0) && inRange(a, ADC1_MIN - ADC_HYS, ADC1_MAX + ADC_HYS);
    okB = (b >= 0) && inRange(b, ADC2_MIN - ADC_HYS, ADC2_MAX + ADC_HYS);
  } else { // M269A
    a   = (v3 < 0) ? -1 : v3;
    b   = (v4 < 0) ? -1 : v4;
    okA = (a >= 0) && inRange(a, ADC3_MIN - ADC_HYS, ADC3_MAX + ADC_HYS);
    okB = (b >= 0) && inRange(b, ADC4_MIN - ADC_HYS, ADC4_MAX + ADC_HYS);
  }

  // Điều kiện OK: s1=s2=s3=0 và 2 ADC trong ngưỡng
  bool sen_ok = (s1 == 0) && (s2 == 0) && (s3 == 0);
  bool overall_ok = sen_ok && okA && okB;

  // Latch LED (chỉ reset khi Start/Stop)
  u57_begin_batch();
  u57_set_bit(U57_LED_OK_BIT, overall_ok,  U57_ACTIVE_LOW_LED);
  u57_set_bit(U57_LED_NG_BIT, !overall_ok, U57_ACTIVE_LOW_LED);
  u57_end_batch();

  // Buzzer
  if (overall_ok) buzzFor(120);
  else            buzzFor(2000);

  // Đưa adcA/adcB về cờ 0/1 theo yêu cầu (0=OK, 1=NG)
  int adcA_flag = okA ? 0 : 1;
  int adcB_flag = okB ? 0 : 1;

  // Print format yêu cầu (giữ nguyên: data=s1,s2,s3,adc_flag,adc_flag)
  printResultLine(overall_ok, s1, s2, s3, adcA_flag, adcB_flag);

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
}

// ---- Setup / Loop ----
void setup() {
  Serial.begin(115200);
  delay(100);

  Wire.begin(I2C_SDA, I2C_SCL);
  // Wire.setClock(400000); // nếu muốn tăng tốc I2C

#if USE_ADS1115
  g_ads_ok = ads.begin(I2C_ADDR_ADS);
  if (g_ads_ok) ads.setGain(GAIN_TWOTHIRDS);
#endif

  // Init outputs
  pcf8575_write_word(I2C_ADDR_U57, u57_shadow);
  u57_last = u57_shadow;
  u57_dirty = false;
  u57_all_off();   // boot: tắt ngay tất cả để an toàn (không trễ)

  // U58: đặt input (ghi 1 để release)
  pcf8575_write_word(I2C_ADDR_U58, 0xFFFF);

  // ---- Tạo các Ticker bằng new (tránh static init issues) ----
  solenoidTimer = new (_tbuf_solenoid) Ticker(solenoidSequence, 1000, 1, MILLIS); // interval sẽ đổi động
  buzzerTimer   = new (_tbuf_buzzer)   Ticker(stopBuzzer,       2000, 1, MILLIS);
  checkTimer    = new (_tbuf_check)    Ticker(doCheck,          2000, 1, MILLIS);
  resetTimer    = new (_tbuf_reset)    Ticker(resetSystem,      2000, 1, MILLIS);
  sol1OffTimer  = new (_tbuf_sol1off)  Ticker(sol1OffCB,        SOL1_OFF_DELAY_MS, 1, MILLIS);

  // START dùng Bounce2 (kéo lên nội bộ; đổi sang INPUT nếu có pull-up ngoài)
  btnStart.attach(START_PIN, INPUT_PULLUP);
  btnStart.interval(BOUNCE_MS);
  btnStart.setPressedState(LOW);

  // STOP dùng interrupt cạnh xuống, kéo lên nội bộ (đổi sang INPUT nếu có pull-up ngoài)
  pinMode(STOP_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(STOP_PIN), onStopISR, FALLING);

  // Áp dụng tác động ban đầu theo MODEL mặc định
  applyModelSideEffects(g_model);

  // Serial.print("READY: MODEL=");
  // Serial.print(modelName(g_model));
  // Serial.println(" (267A->ADC1-2 ON relay, 269A->ADC3-4 OFF relay)");
  // Serial.println("Serial: send MODEL=DJ9600267A or MODEL=DJ9600269A");
}

void loop() 
{
  // Nhận lệnh từ PC
  pollSerial();

  // START (debounce bằng Bounce2)
  btnStart.update();
  if (btnStart.fell()) {
    if (g_state == AppState::Idle) beginSequence();   // chặn double-start khi đang chạy
  }

// STOP từ interrupt: debounce ở loop để an toàn
static uint32_t stop_debounce_ms = 0;
if (g_stop_irq) {
  uint32_t now = millis();
  if (stop_debounce_ms == 0) stop_debounce_ms = now;   // chốt mốc lần đầu

  bool low = (digitalRead(STOP_PIN) == LOW);
  if (low && (now - stop_debounce_ms) >= BOUNCE_MS) {
    g_stop_irq = false;
    stop_debounce_ms = 0;
    abortSequenceToIdle();
  }
  // nhả nút: hết thời gian debounce thì clear cờ
  else if (!low && (now - stop_debounce_ms) >= BOUNCE_MS) {
    g_stop_irq = false;
    stop_debounce_ms = 0;
  }
}
// Poll sensors theo chu kỳ để giảm traffic I2C (UI placeholder)
static uint32_t last_sen_ms = 0;
const uint32_t now_ms = millis();
if ((now_ms - last_sen_ms) >= 10) {   // 10ms là đủ cho UI & logic
  last_sen_ms = now_ms;
  uint16_t sen;
  if (pcf8575_read_word(I2C_ADDR_U58, sen)) u58_inputs = sen;
}
  // Update timers
  if (solenoidTimer) solenoidTimer->update();
  if (checkTimer)    checkTimer->update();
  if (resetTimer)    resetTimer->update();
  if (buzzerTimer)   buzzerTimer->update();
  if (sol1OffTimer)  sol1OffTimer->update();

  delay(1); // yield cho FreeRTOS/WDT
}
