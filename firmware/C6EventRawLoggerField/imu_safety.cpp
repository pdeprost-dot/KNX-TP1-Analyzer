#include "imu_safety.h"
#include <Arduino.h>
#include <Wire.h>

bool ensureImuHighZ() {
  Wire.begin(18, 19);
  Wire.setClock(100000);
  pinMode(20, OUTPUT);
  digitalWrite(20, LOW);
  delay(10);
  digitalWrite(20, HIGH);
  delay(100);
  for (const uint8_t address : {uint8_t(0x6A), uint8_t(0x6B)}) {
    Wire.beginTransmission(address);
    Wire.write(0x00);
    if (Wire.endTransmission(false) != 0 || Wire.requestFrom(address, uint8_t(1)) != 1) continue;
    if (Wire.read() != 0x05) continue;
    Wire.beginTransmission(address);
    Wire.write(0x02);
    if (Wire.endTransmission(false) != 0 || Wire.requestFrom(address, uint8_t(1)) != 1) return false;
    const uint8_t control = Wire.read();
    Wire.beginTransmission(address);
    Wire.write(0x02);
    Wire.write(control & ~0x18);
    if (Wire.endTransmission() != 0) return false;
    Wire.beginTransmission(address);
    Wire.write(0x02);
    if (Wire.endTransmission(false) != 0 || Wire.requestFrom(address, uint8_t(1)) != 1) return false;
    return (Wire.read() & 0x18) == 0;
  }
  return false;
}
