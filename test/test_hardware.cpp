/**
 * Hardware Diagnostic Test for ESP32-S3 Keypad & Rotary Knob
 * 
 * วิธีใช้:
 * 1. ถ้าต้องการทดสอบ สามารถนำโค้ดในไฟล์นี้ไปวางแทนที่ src/main.cpp ชั่วคราว
 * 2. Flash ลงบอร์ด แล้วเปิด Serial Monitor ที่ Baudrate 115200
 * 3. หมุน Knob, กด Knob, และกดปุ่ม K1-K3 เพื่อดูค่าดิบ (Raw Values) ทันที
 */

#include <Arduino.h>

// Pin Definitions
constexpr uint8_t PIN_K1 = 4;
constexpr uint8_t PIN_K2 = 5;
constexpr uint8_t PIN_K3 = 6;
constexpr uint8_t PIN_ENC_A = 7;
constexpr uint8_t PIN_ENC_B = 8;
constexpr uint8_t PIN_ENC_SW = 9;

int32_t encoderCount = 0;
uint8_t lastEncoded = 0;

void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println("\n==============================================");
  Serial.println("  ESP32-S3 Keypad & Knob Hardware Diagnostics ");
  Serial.println("==============================================");
  Serial.println("Monitoring GPIOs:");
  Serial.println("  - K1 (Pin 4), K2 (Pin 5), K3 (Pin 6)");
  Serial.println("  - Knob A (Pin 7), Knob B (Pin 8), Knob SW (Pin 9)");
  Serial.println("Tip: Normal state with INPUT_PULLUP should be HIGH (1)");
  Serial.println("     When pressed/contacted, state should drop to LOW (0)");
  Serial.println("----------------------------------------------\n");

  pinMode(PIN_K1, INPUT_PULLUP);
  pinMode(PIN_K2, INPUT_PULLUP);
  pinMode(PIN_K3, INPUT_PULLUP);
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  pinMode(PIN_ENC_SW, INPUT_PULLUP);

  lastEncoded = (digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B);
}

void loop() {
  static uint32_t lastPrintMs = 0;
  static bool lastK1 = HIGH, lastK2 = HIGH, lastK3 = HIGH, lastSW = HIGH;

  // 1. ตรวจสอบปุ่ม K1, K2, K3
  bool curK1 = digitalRead(PIN_K1);
  bool curK2 = digitalRead(PIN_K2);
  bool curK3 = digitalRead(PIN_K3);
  bool curSW = digitalRead(PIN_ENC_SW);

  if (curK1 != lastK1) {
    lastK1 = curK1;
    Serial.printf("[BUTTON] K1 (GPIO4): %s\n", curK1 == LOW ? ">>> PRESSED (LOW) <<<" : "RELEASED (HIGH)");
  }
  if (curK2 != lastK2) {
    lastK2 = curK2;
    Serial.printf("[BUTTON] K2 (GPIO5): %s\n", curK2 == LOW ? ">>> PRESSED (LOW) <<<" : "RELEASED (HIGH)");
  }
  if (curK3 != lastK3) {
    lastK3 = curK3;
    Serial.printf("[BUTTON] K3 (GPIO6): %s\n", curK3 == LOW ? ">>> PRESSED (LOW) <<<" : "RELEASED (HIGH)");
  }
  if (curSW != lastSW) {
    lastSW = curSW;
    Serial.printf("[KNOB SW] Push Switch (GPIO9): %s\n", curSW == LOW ? ">>> PRESSED (LOW) <<<" : "RELEASED (HIGH)");
  }

  // 2. ตรวจสอบการหมุนของ Rotary Encoder (อ่าน State การเปลี่ยนระดับสัญญาณ)
  uint8_t a = digitalRead(PIN_ENC_A);
  uint8_t b = digitalRead(PIN_ENC_B);
  uint8_t encoded = (a << 1) | b;

  if (encoded != lastEncoded) {
    uint8_t sum = (lastEncoded << 2) | encoded;
    
    // Gray Code transition table
    // 00->01 (1), 01->11 (7), 11->10 (14), 10->00 (8) = CW
    // 00->10 (2), 10->11 (11), 11->01 (13), 01->00 (4) = CCW
    if (sum == 0b0001 || sum == 0b0111 || sum == 0b1110 || sum == 0b1000) {
      encoderCount++;
      Serial.printf("[ROTARY] >> หมุนตามเข็ม (CW) | Pin A=%d, B=%d | Count: %ld\n", a, b, (long)encoderCount);
    } else if (sum == 0b0010 || sum == 0b1011 || sum == 0b1101 || sum == 0b0100) {
      encoderCount--;
      Serial.printf("[ROTARY] << หมุนทวนเข็ม (CCW) | Pin A=%d, B=%d | Count: %ld\n", a, b, (long)encoderCount);
    } else {
      // Transition กระโดด (Bounce หรือหมุนเร็วมาก)
      Serial.printf("[ROTARY] ~ สัญญาณกระโดด (Bounce/Skip) | Pin A=%d, B=%d | Transition: 0x%X\n", a, b, sum);
    }
    lastEncoded = encoded;
  }

  // แสดง Heartbeat ทุกๆ 3 วินาที เพื่อดูสถานะ Pin ปัจจุบัน
  if (millis() - lastPrintMs >= 3000) {
    lastPrintMs = millis();
    Serial.printf("[STATUS HEARTBEAT] Pin A(7)=%d | Pin B(8)=%d | SW(9)=%d | Current Count=%ld\n",
                  digitalRead(PIN_ENC_A), digitalRead(PIN_ENC_B), digitalRead(PIN_ENC_SW), (long)encoderCount);
  }

  delay(1); // Small delay to yield
}

