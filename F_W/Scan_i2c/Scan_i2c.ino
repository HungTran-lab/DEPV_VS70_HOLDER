#include <Arduino.h>
#include <Wire.h>

#define SDA_PIN 38
#define SCL_PIN 39

void scanOnce() {
  byte count = 0;
  Serial.println(F("\n--- I2C scan start ---"));
  for (uint8_t address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    uint8_t error = Wire.endTransmission(true);
    if (error == 0) {
      Serial.print(F("Found I2C device at 0x"));
      if (address < 16) Serial.print('0');
      Serial.print(address, HEX);
      Serial.println(F("  ✅"));
      count++;
    } else if (error == 4) {
      Serial.print(F("Unknown error at 0x"));
      if (address < 16) Serial.print('0');
      Serial.println(address, HEX);
    }
  }
  if (count == 0) {
    Serial.println(F("No I2C devices found ❌"));
  }
  Serial.println(F("--- I2C scan done ---"));
}

void setup() {
  Serial.begin(115200);
  // Chờ Serial (hữu ích khi dùng USB CDC)
  unsigned long t0 = millis();
  while (!Serial && (millis() - t0 < 1500)) {}

  // Khởi tạo I2C với chân tùy chọn
  Wire.begin(SDA_PIN, SCL_PIN);   // SDA=38, SCL=39
  Wire.setClock(400000);          // 400 kHz (có thể đổi 100000 nếu cần)

  Serial.println(F("ESP32-S3 I2C Scanner"));
  Serial.print(F("SDA=")); Serial.print(SDA_PIN);
  Serial.print(F("  SCL=")); Serial.println(SCL_PIN);

  scanOnce();
}

void loop() {
  // Quét lại mỗi 5 giây (nếu không cần thì xóa phần dưới)
  static unsigned long last = 0;
  if (millis() - last > 5000) {
    last = millis();
    scanOnce();
  }
}
