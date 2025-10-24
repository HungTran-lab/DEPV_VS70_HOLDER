#ifndef CONFIG_H
#define CONFIG_H

// ===== Feature toggles =====
#define USE_OLED        1         // SSD1306 0x3C
#define USE_ADS1115     1         // ADS1115 0x48
#define USE_RTC_DS3231  0         // DS3231 0x68
#define USE_BUZZER      1         // Buzzer qua U57 bit6

// ===== I2C addresses =====
#define I2C_ADDR_U57    0x25      // PCF8575 outputs
#define I2C_ADDR_U58    0x24      // PCF8575 inputs
#define I2C_ADDR_OLED   0x3C
#define I2C_ADDR_ADS    0x48
// #define I2C_ADDR_RTC    0x68

// ===== PCF8575 U57 bit map (outputs) =====
#define U57_SOL1_BIT    0   // SOLINOID  Pin 1
#define U57_SOL2_BIT    1   // 2
#define U57_SOL3_BIT    2   // 3
#define U57_SOL4_BIT    3   // 4
#define U57_LED_OK_BIT  4   // 5    LAMP
#define U57_LED_NG_BIT  5   // 6    LAMP
#define U57_BUZZER_BIT  6   // 7    BUZZER

#define U57_RELAY_BIT  7   // 8 Select Model

// ===== PCF8575 U58 bit map (inputs) =====
#define U58_SEN1_BIT    0    // SENSOR  Pin 1
#define U58_SEN2_BIT    1    // 2
#define U58_SEN3_BIT    2    // 3

// ===== Logic levels (đổi = 0 nếu phần cứng active-high) =====
#define U57_ACTIVE_LOW_SOLENOID   1
#define U57_ACTIVE_LOW_LED        1
#define U57_ACTIVE_LOW_BUZZER     1

// ===== Debounce =====
#define BOUNCE_MS       15

// ===== Solenoid timing (ms, mốc tuyệt đối từ START) =====
#define SOL_MS_MIN      1
#define SOL_MS_MAX      30000
#define SOL_MS_0        2000
#define SOL_MS_1        3000
#define SOL_MS_2        3500
#define SOL_MS_3        3500

// ===== OLED =====
#define SCREEN_WIDTH    128
#define SCREEN_HEIGHT   64

// ===== ADC thresholds (scale 0..4095) =====

//--------------for 267---------------------
#define ADC1_MIN        1000
#define ADC1_MAX        3600
#define ADC2_MIN        1000
#define ADC2_MAX        3595
//--------------for 269---------------------
#define ADC3_MIN        1000
#define ADC3_MAX        3600
#define ADC4_MIN        1000
#define ADC4_MAX        3595

#define ADC_HYS         20

// Trễ tắt Solenoid1 sau khi tắt 2/3/4 (ms)
#define SOL1_OFF_DELAY_MS 1000

#endif


