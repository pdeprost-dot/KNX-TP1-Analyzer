#pragma once

#include <Arduino.h>
#include <driver/adc.h>

namespace board {
constexpr const char *NAME = "XIAO_ESP32S3_SENSE";
constexpr gpio_num_t ANALOG_GPIO = GPIO_NUM_1;       // D0 / ADC1_CH0
constexpr adc_channel_t ADC_CHANNEL = ADC_CHANNEL_0;
constexpr uint8_t FUTURE_UART_TX = 43;               // D6, reserved
constexpr uint8_t FUTURE_UART_RX = 44;               // D7, reserved
constexpr uint8_t SD_CS = 21, SD_SCK = 7, SD_MISO = 8, SD_MOSI = 9;
}
