#ifndef INPUT_HANDLER_H
#define INPUT_HANDLER_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define INCREMENT_BUTTON_PIN 40
#define DECREMENT_BUTTON_PIN 39
#define CONFIRM_BUTTON_PIN   38
#define DEBOUNCE_DELAY_MS    50

typedef enum {BUTTON_TYPE_INCREMENT = 0, BUTTON_TYPE_DECREMENT = 1, BUTTON_TYPE_CONFIRM = 2} button_types;

void setup_gpio(void);
void process_inputs(void *args);

#endif