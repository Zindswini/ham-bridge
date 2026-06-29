#include "input_handler.h"

#include "config.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "hal/gpio_types.h"
#include "screen_handler.h"
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <sys/cdefs.h>

static const char *tag = "INPUT_HANDLER";

static volatile uint64_t last_press_time = 0;

static ButtonState current_button_state = {.increment_button_state = false,
                                           .decrement_button_state = false,
                                           .confirm_button_state = false};
static ButtonState previous_button_state = {.increment_button_state = false,
                                            .decrement_button_state = false,
                                            .confirm_button_state = false};
static QueueHandle_t input_queue;

static void IRAM_ATTR buttonPollingISR(void *args __unused) {
  // Debouncing
  previous_button_state = current_button_state;

  current_button_state.confirm_button_state =
      gpio_get_level(CONFIRM_BUTTON_PIN) != 0;
  current_button_state.increment_button_state =
      gpio_get_level(INCREMENT_BUTTON_PIN) != 0;
  current_button_state.decrement_button_state =
      gpio_get_level(DECREMENT_BUTTON_PIN) != 0;

  if (!previous_button_state.confirm_button_state &&
      (static_cast<int>(current_button_state.confirm_button_state) != 0)) {
    BaseType_t hp_task_woken = pdFALSE;
    ButtonTypes button_type = kButtonTypeConfirm;
    xQueueSendToBackFromISR(input_queue, &button_type, &hp_task_woken);
    if (hp_task_woken != 0) {
      portYIELD_FROM_ISR();
    }
  }

  if (!previous_button_state.increment_button_state &&
      (static_cast<int>(current_button_state.increment_button_state) != 0)) {
    BaseType_t hp_task_woken = pdFALSE;
    ButtonTypes button_type = kButtonTypeIncrement;
    xQueueSendToBackFromISR(input_queue, &button_type, &hp_task_woken);
    if (hp_task_woken != 0) {
      portYIELD_FROM_ISR();
    }
  }

  if (!previous_button_state.decrement_button_state &&
      (static_cast<int>(current_button_state.decrement_button_state) != 0)) {
    BaseType_t hp_task_woken = pdFALSE;
    ButtonTypes button_type = kButtonTypeDecrement;
    xQueueSendToBackFromISR(input_queue, &button_type, &hp_task_woken);
    if (hp_task_woken != 0) {
      portYIELD_FROM_ISR();
    }
  }
}

void setupButtonGPIOTimer(void) {
  input_queue = xQueueCreate(10, sizeof(uint32_t));

  gpio_config_t gpio_conf = {
      .pin_bit_mask = (1ULL << INCREMENT_BUTTON_PIN) |
                      (1ULL << DECREMENT_BUTTON_PIN) |
                      (1ULL << CONFIRM_BUTTON_PIN),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_NEGEDGE,
  };
  ESP_LOGD(tag, "Configuring GPIO");
  ESP_ERROR_CHECK(gpio_config(&gpio_conf));

  ESP_LOGD(tag, "Setting up Button Polling ISR Timer");
  esp_timer_create_args_t timer_args = {
      .callback = buttonPollingISR,
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "Button Polling Timer",
      .skip_unhandled_events = false,
  };
  esp_timer_handle_t timer_handle = nullptr;
  ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle));
  ESP_ERROR_CHECK(
      esp_timer_start_periodic(timer_handle, 10ULL * 1000)); // Run every 10ms
  ESP_LOGD(tag, "Set up Button Polling ISR Timer");
}

void processInputsTask(void *args __unused) {
  uint8_t next_button = 0;
  while (true) {
    xQueueReceive(input_queue, &next_button, portMAX_DELAY);
    ESP_LOGD(tag, "Received button press from queue: %i", next_button);
    processIncomingInput(ButtonTypes{next_button});
  }
}