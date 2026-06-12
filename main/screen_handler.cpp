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
#include <ctime>
#include <format>
#include <memory>
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <sys/_timeval.h>
#include <sys/time.h>
#include <sys/types.h>

static const char *TAG = "SCREEN_HANDLER";

enum menu_item_type : uint8_t {
  MENU_ITEM_TYPE_NUMERICAL = 0,
  MENU_ITEM_TYPE_TOGGLE = 1,
  MENU_ITEM_TYPE_SUBMENU = 2,
  MENU_ITEM_TYPE_BACK = 3,
};

struct menuListObject {
  const char *text;
  const menu_item_type item_type;
  const bool interactable;

  // If numerical, value is uint32_t
  // If toggle, value is bool
  // if submenu, value is menuList
  // if back, value is menuList
  const void *value;

  // Method called to get its value
  const void *getterMethod;
  // Whether the getter is called on refresh
  const bool poll;

  // If interactable, method called on confirm
  const void *setterMethod;
};

struct screenInformation {
  tm timeinfo;

  std::shared_ptr<std::array<std::shared_ptr<menuListObject>, 10>> menuList;
  std::array<const uint8_t *, 4> iconList;
};

u8g2_t u8g2;
static struct screenInformation displayState;

extern "C" void drawLoadingScreen(char *loadingText) {
  u8g2_ClearBuffer(&u8g2);
  u8g2_SetFont(&u8g2, u8g2_font_helvB08_tr);

  // Draw message as centered text
  u8g2_DrawStr(&u8g2,
               u8g2.width / 2 - (u8g2_GetStrWidth(&u8g2, loadingText) / 2),
               u8g2.height / 2, loadingText);

  u8g2_SendBuffer(&u8g2);
}

extern "C" void drawScreen() {
  u8g2_ClearBuffer(&u8g2);
  u8g2_SetFont(&u8g2, u8g2_font_helvB08_tr);

  // Draw Icons
  int i = 0;
  while (displayState.iconList.at(i) != nullptr) {
    const uint8_t *currentIconObject =
        reinterpret_cast<const uint8_t *>(displayState.iconList.at(i));
    u8g2_DrawXBM(&u8g2, i * (ICON_WIDTH + ICON_PADDING), 0, ICON_WIDTH,
                 ICON_HEIGHT, currentIconObject);
    i++;
  }

  // Draw Time
  std::string timeString =
      std::format("{:02d}:{:02d}:{:02d}", displayState.timeinfo.tm_hour,
                  displayState.timeinfo.tm_min, displayState.timeinfo.tm_sec);
  u8g2_DrawStr(&u8g2,
               (u8g2.width - u8g2_GetStrWidth(&u8g2, timeString.c_str())), 8,
               timeString.c_str());

  // Draw Horizontal Line
  u8g2_DrawHLine(&u8g2, 0, 10, u8g2.width);

  // Draw menu items
  i = 0;

  while (displayState.menuList.get()->at(i) != nullptr) {
    struct menuListObject *currentListObject =
        displayState.menuList.get()->at(i).get();
    // Draw box around ACTIVE menu item
    u8g2_DrawFrame(&u8g2, 0,
                   MENU_TOP_PADDING + ((MENU_FRAME_HEIGHT + MENU_SPACING) * i),
                   u8g2.width, MENU_FRAME_HEIGHT);
    // Draw text description element of menu item
    u8g2_DrawStr(&u8g2, MENU_PADDING,
                 MENU_TOP_PADDING + MENU_PADDING + TEXT_HEIGHT +
                     ((MENU_FRAME_HEIGHT + MENU_SPACING) * i),
                 currentListObject->text);

    std::string value_string;
    switch (currentListObject->item_type) {
    case MENU_ITEM_TYPE_NUMERICAL:
      // Interpret value as uint32 and
      // Draw number as string on right side
      value_string = std::format(
          "{:d}", reinterpret_cast<uint32_t>(currentListObject->value));
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
    case MENU_ITEM_TYPE_SUBMENU:
      break;
    case MENU_ITEM_TYPE_BACK:
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

  // Construct Top Level Menu
  auto topLevelMenu =
      std::make_shared<std::array<std::shared_ptr<menuListObject>, 10>>();
  topLevelMenu->at(0) = std::make_shared<menuListObject>(
      menuListObject{.text = "Example",
                     .item_type = MENU_ITEM_TYPE_NUMERICAL,
                     .interactable = false,
                     .value = (void *)9999,
                     .getterMethod = nullptr,
                     .poll = false,
                     .setterMethod = nullptr});

  displayState.menuList = topLevelMenu;

  displayState.iconList[0] = HAM_BRIDGE_ICON_mic;
  displayState.iconList[1] = HAM_BRIDGE_ICON_lan_enable;
  displayState.iconList[2] = HAM_BRIDGE_ICON_lan_disable;
}

void updateDisplayState() {
  // Update current time
  time_t now;
  struct tm timeinfo;
  time(&now);
  localtime_r(&now, &timeinfo);

  displayState.timeinfo = timeinfo;
}

extern "C" void screenRefreshTask() {
  while (1) {
    updateDisplayState();
    drawScreen();
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}