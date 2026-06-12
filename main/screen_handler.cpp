#include "screen_handler.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "icons.h"
#include "u8g2.h"
#include "u8g2_esp32_hal.h"
#include "u8x8.h"
#include <array>
#include <format>
#include <memory>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <sys/types.h>

static const char *TAG = "SCREEN_HANDLER";

enum menu_item_type : uint8_t {
  MENU_ITEM_TYPE_NUMERICAL = 0,
  MENU_ITEM_TYPE_TOGGLE = 1,
};

struct menuListObject {
  char const *text;
  menu_item_type item_type;
  bool interactable;
  bool highlighted;
  void *value;
};

struct iconListObject {
  uint8_t *icon;
  uint32_t size;
};

struct screenInformation {
  uint8_t time_hours = 0;
  uint8_t time_minutes = 0;
  uint8_t time_seconds = 0;

  std::array<std::shared_ptr<menuListObject>, 10> menuList;
  std::array<std::shared_ptr<iconListObject>, 4> iconList;
};

u8g2_t u8g2;
static struct screenInformation displayState;

extern "C" void drawScreen() {
  u8g2_ClearBuffer(&u8g2);
  u8g2_SetFont(&u8g2, u8g2_font_helvB08_tr);

  // Draw Icons
  int i = 0;
  while (displayState.iconList.at(i) != nullptr) {
    // draw icon
  }

  // Draw Time
  std::string timeString =
      std::format("%02u:%02u:%02u", displayState.time_hours,
                  displayState.time_minutes, displayState.time_seconds);
  u8g2_DrawStr(&u8g2,
               (u8g2.width - u8g2_GetStrWidth(&u8g2, timeString.c_str())), 8,
               timeString.c_str());

  // Draw Horizontal Line
  u8g2_DrawHLine(&u8g2, 0, 10, u8g2.width);

  // Draw menu items
  i = 0;

  while (displayState.menuList.at(i) != nullptr) {
    struct menuListObject *currentListObject =
        displayState.menuList.at(i).get();
    // Draw box around ACTIVE menu item
    if (currentListObject->highlighted) {
      u8g2_DrawFrame(
          &u8g2, 0, MENU_TOP_PADDING + ((MENU_FRAME_HEIGHT + MENU_SPACING) * i),
          u8g2.width, MENU_FRAME_HEIGHT);
    }
    // Draw text description element of menu item
    u8g2_DrawStr(&u8g2, MENU_PADDING,
                 MENU_TOP_PADDING + MENU_PADDING + TEXT_HEIGHT +
                     ((MENU_FRAME_HEIGHT + MENU_SPACING) * i),
                 displayState.menuList.at(i)->text);

    std::string value_string;
    switch (displayState.menuList.at(i)->item_type) {
    case MENU_ITEM_TYPE_NUMERICAL:
      // Interpret value as uint32 and
      // Draw number as string on right side
      value_string = std::format("%lu", currentListObject->value);
      u8g2_DrawStr(&u8g2,
                   (u8g2.width - u8g2_GetStrWidth(&u8g2, value_string.c_str()) -
                    MENU_PADDING),
                   MENU_TOP_PADDING + MENU_PADDING + TEXT_HEIGHT +
                       ((MENU_FRAME_HEIGHT + MENU_SPACING) * i),
                   value_string.c_str());
      break;
    case MENU_ITEM_TYPE_TOGGLE:
      // Interpret value as bool and
      // Draw true as filled box, false as empty
      break;
    }
    i++;
  }

  int current_timestamp = pdTICKS_TO_MS(xTaskGetTickCount());
  char timestamp_string[10];
  snprintf(timestamp_string, 9, "%d", current_timestamp);

  u8g2_SendBuffer(&u8g2);
}

extern "C" void initializeU8G2(i2c_master_bus_handle_t *i2c_bus_handle) {
  u8g2_esp32_hal_t u8g2_esp32_hal = U8G2_ESP32_HAL_DEFAULT;
  u8g2_esp32_hal.bus.i2c.i2c_bus_handle = *i2c_bus_handle;
  u8g2_esp32_hal_init(u8g2_esp32_hal);

  u8g2_Setup_ssd1306_i2c_128x64_noname_f(
      &u8g2, U8G2_R0, u8g2_esp32_i2c_byte_cb,
      u8g2_esp32_gpio_and_delay_cb); // I2C Callback functions for mapping

  ESP_LOGI(TAG, "Preparing to Initialize U8G2");
  u8x8_SetI2CAddress(&u8g2.u8x8, 0x78);
  u8g2_InitDisplay(&u8g2);
  u8g2_SetPowerSave(&u8g2, 0);
  ESP_LOGI(TAG, "Initialized U8G2 and Display");

  // Setup the screen object
  displayState.time_hours = 10;
  displayState.time_minutes = 11;
  displayState.time_seconds = 12;

  std::shared_ptr<menuListObject> firstOption =
      std::make_shared<menuListObject>();
  firstOption->text = "List item 1";
  firstOption->item_type = MENU_ITEM_TYPE_NUMERICAL,
  firstOption->highlighted = false, firstOption->interactable = false,
  firstOption->value = (void *)99999;
  displayState.menuList[0] = firstOption;
  displayState.menuList[1] = firstOption;
  displayState.menuList[2] = firstOption;
}

extern "C" void screenRefreshTask() {
  while (1) {
    drawScreen();
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}