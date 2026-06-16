#ifndef CONFIG_H
#define CONFIG_H

// OLED Display and ES8388 Control
#define PIN_SDA 1
#define PIN_SCL 15

// ES8388 I2S Pins
#define PIN_DOUT 3
#define PIN_LRCLK 21
#define PIN_DIN 16
#define PIN_SCLK 2
#define PIN_MCLK 0
#define PIN_PA 6
#define VCC_VOLTAGE 3.3F

// I2S Global Sample Rate
#define I2S_SAMPLE_RATE 44100
#define I2S_MCLK_MULITPLE 256

// Front Interface Button Pins
#define INCREMENT_BUTTON_PIN 40
#define DECREMENT_BUTTON_PIN 39
#define CONFIRM_BUTTON_PIN 38
#define DEBOUNCE_DELAY_MS 50

// HTTPS Server Settings
#define MAX_HTTPS_CLIENTS 4

#endif