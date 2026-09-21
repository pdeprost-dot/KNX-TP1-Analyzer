#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <Arduino_GFX_Library.h>
#include "acquisition.h"
#include "analysis.h"
#include "wifi_manager.h"
#include "hmi.h"
#include "web_server.h"

static constexpr char kVersion[] = "0.3.0-hmi";
static Arduino_DataBus *lcdBus = new Arduino_HWSPI(15, 14, 1, 2, 3);
static Arduino_GFX *lcd = new Arduino_ST7789(lcdBus, 22, 0, false, 172, 320, 34, 0, 34, 0);
static bool lcdReady = false;
static bool sdReady = false;
static uint8_t imuAddress = 0;
static uint8_t imuWhoAmI = 0;
static bool imuInterruptsHighZ = false;
static bool touchPresent = false;

static bool readRegister(uint8_t address, uint8_t reg, uint8_t &value) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(address, static_cast<uint8_t>(1)) != 1) return false;
  value = Wire.read();
  return true;
}

static bool writeRegister(uint8_t address, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

static void probeI2c() {
  Wire.begin(18, 19);
  Wire.setClock(100000);
  pinMode(21, INPUT);
  pinMode(20, OUTPUT);
  digitalWrite(20, LOW);
  delay(10);
  digitalWrite(20, HIGH);
  delay(100);
  for (uint8_t address = 8; address < 120; ++address) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      Serial.printf("{\"type\":\"I2C_DEVICE\",\"address\":%u}\n", address);
      if (address == 0x3B || address == 0x51 || address == 0x63) touchPresent = true;
      if (address == 0x6A || address == 0x6B) {
        imuAddress = address;
        readRegister(address, 0x00, imuWhoAmI);
      }
    }
  }
  if (imuAddress && imuWhoAmI == 5) {
    uint8_t control = 0;
    uint8_t verified = 0xFF;
    if (readRegister(imuAddress, 0x02, control) &&
        writeRegister(imuAddress, 0x02, control & ~0x18) &&
        readRegister(imuAddress, 0x02, verified)) {
      imuInterruptsHighZ = (verified & 0x18) == 0;
    }
    Serial.printf("{\"type\":\"IMU_INT_STATS\",\"ctrl1\":%u,\"high_z\":%s}\n", verified, imuInterruptsHighZ ? "true" : "false");
  }
  Serial.printf("{\"type\":\"TOUCH_STATS\",\"i2c_present\":%s,\"irq_level\":%d}\n", touchPresent ? "true" : "false", digitalRead(21));
  Serial.printf("{\"type\":\"IMU_STATS\",\"address\":%u,\"whoami\":%u}\n", imuAddress, imuWhoAmI);
}

static void probeDisplayAndSd() {
  pinMode(4, OUTPUT);
  digitalWrite(4, HIGH);
  pinMode(23, OUTPUT);
  digitalWrite(23, LOW);
  SPI.begin(1, 3, 2, 14);
  lcdReady = lcd->begin(40000000);
  if (lcdReady) {
    // The panel uses the ST7789 command set with JD9853 color inversion.
    lcd->invertDisplay(true);
    // JD9853 glass scans X in the opposite direction to the default ST7789 map.
    lcd->setRotation(6);
    lcd->fillScreen(0x0000);
    lcd->setTextColor(0xFFFF);
    lcd->setTextSize(2);
    lcd->setCursor(8, 45);
    lcd->println("KNX");
    lcd->setCursor(8, 70);
    lcd->println("ANALYZER");
    lcd->setTextSize(1);
    lcd->setCursor(8, 120);
    lcd->println(kVersion);
    lcd->setCursor(8, 145);
    lcd->printf("Heap %u", ESP.getFreeHeap());
    lcd->setCursor(8, 165);
    lcd->println("STOPPED");
    digitalWrite(23, HIGH);
  }
  Serial.printf("{\"type\":\"LCD_STATS\",\"init\":%s}\n", lcdReady ? "true" : "false");
  digitalWrite(14, HIGH);
  sdReady = SD.begin(4, SPI, 400000);
  Serial.printf("{\"type\":\"SD_STATS\",\"mounted\":%s,\"card_type\":%u,\"size_bytes\":%llu}\n", sdReady ? "true" : "false", static_cast<unsigned>(SD.cardType()), sdReady ? SD.cardSize() : 0ULL);
  if (sdReady) SD.end();
}

void setup() {
  Serial.begin(115200);
  delay(1200);
  Serial.printf("{\"type\":\"BOOT\",\"app\":\"KNX ANALYZER\",\"version\":\"%s\",\"chip\":\"%s\",\"flash_bytes\":%u,\"heap_free\":%u}\n",
                kVersion, ESP.getChipModel(), ESP.getFlashChipSize(), ESP.getFreeHeap());
  probeI2c();
  probeDisplayAndSd();
  Serial.printf("{\"type\":\"STATUS\",\"lcd_init\":%s,\"touch_i2c\":%s,\"sd_mounted\":%s,\"imu_whoami\":%u,\"imu_int_high_z\":%s,\"heap_free\":%u}\n",
                lcdReady ? "true" : "false", touchPresent ? "true" : "false", sdReady ? "true" : "false", imuWhoAmI, imuInterruptsHighZ ? "true" : "false", ESP.getFreeHeap());
  if (imuInterruptsHighZ) {
    scope::begin();
  } else {
    Serial.println("{\"type\":\"ADC_ERROR\",\"stage\":\"imu_interrupts_not_high_z\"}");
  }
  network::begin();
  if (lcdReady) hmi::begin(lcd);
  webui::setSdReady(sdReady);
  webui::begin();
}

void loop() {
  scope::service();
  scope::pollSerial();
  network::tick();
  webui::tick();
  hmi::tick();
  scope::printStats();
  static uint32_t last = 0;
  if (millis() - last >= 5000) {
    last = millis();
    Serial.printf("{\"type\":\"MEMORY_STATS\",\"uptime_ms\":%lu,\"heap_free\":%u,\"heap_min\":%u}\n",
                  millis(), ESP.getFreeHeap(), ESP.getMinFreeHeap());
  }
  delay(1);
}
