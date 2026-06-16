#include "screen_handler.h"

#include "FreeRTOSConfig.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "ethernet_handler.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "icons.h"
#include "input_handler.h"
#include "u8g2.h"
#include "u8g2_esp32_hal.h"
#include "u8x8.h"
#include <array>
#include <ctime>
#include <format>
#include <memory>
#include <shared_mutex>
#include <stdint.h>
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
  MENU_ITEM_TYPE_STRING = 4,
};

struct menuListObject {
  const char *text = "UNSET_MENU";
  const menu_item_type item_type = MENU_ITEM_TYPE_NUMERICAL;
  const bool editable = false;

  // If numerical, value is uint32_t
  // If toggle, value is bool
  // if submenu, value is menuList
  // if back, value is menuList
  void *value = nullptr;
  const std::shared_ptr<std::array<std::shared_ptr<menuListObject>, 10>>
      menuListValue = nullptr;

  // Method called to get its value
  void *(*getterMethod)() = nullptr;
  // Whether the getter is called on refresh
  const bool poll = false;

  // If interactable, method called on confirm
  void (*setterMethod)() = nullptr;
};

struct screenInformation {
  tm timeinfo;
  uint32_t menuIndex = 0;
  std::shared_ptr<std::array<std::shared_ptr<menuListObject>, 10>> menuList;
  std::array<const uint8_t *, 4> iconList;
};

u8g2_t u8g2;

static struct screenInformation displayState;
std::shared_mutex displayMutex;

extern "C" void drawLoadingScreen(char *loadingText) {
  u8g2_ClearBuffer(&u8g2);
  u8g2_SetFont(&u8g2, u8g2_font_helvB08_tr);

  // Draw message as centered text
  u8g2_DrawStr(&u8g2,
               u8g2.width / 2 - (u8g2_GetStrWidth(&u8g2, loadingText) / 2),
               u8g2.height / 2, loadingText);

  u8g2_SendBuffer(&u8g2);
}

void drawMenuItem(u8g2_uint_t y, menuListObject *menuItem, bool active,
                  bool editing) {
  uint32_t squish = !active ? MENU_INACTIVE_SQUISH : 0;
  // Draw box around ACTIVE menu item
  if (active) {
    u8g2_DrawFrame(&u8g2, squish, y, u8g2.width - (2 * squish),
                   MENU_FRAME_HEIGHT);
  }
  // Draw text description element of menu item
  u8g2_DrawStr(&u8g2, squish + MENU_PADDING, y + MENU_PADDING + TEXT_HEIGHT,
               menuItem->text);

  std::string value_string;
  bool value_bool;
  switch (menuItem->item_type) {
  case MENU_ITEM_TYPE_NUMERICAL:
    // Interpret value as uint32 and
    // Draw number as string on right side
    value_string = std::format("{:d}", (uint32_t)menuItem->value);
    u8g2_DrawStr(&u8g2,
                 (u8g2.width - MENU_PADDING - squish -
                  u8g2_GetStrWidth(&u8g2, value_string.c_str())),
                 y + MENU_PADDING + TEXT_HEIGHT, value_string.c_str());
    break;
  case MENU_ITEM_TYPE_TOGGLE:
    // Interpret value as bool and
    // Draw true as filled box, false as empty
    value_bool = (menuItem->value != 0);
    // Draw Box Frame (size is textheight x textheight)
    u8g2_DrawFrame(&u8g2, (u8g2.width - MENU_PADDING - TEXT_HEIGHT - squish),
                   y + MENU_PADDING, TEXT_HEIGHT, TEXT_HEIGHT);

    if (value_bool) {
      // Fill in box
      u8g2_DrawBox(&u8g2,
                   (u8g2.width - MENU_PADDING - TEXT_HEIGHT - squish + 2),
                   y + MENU_PADDING + 2, TEXT_HEIGHT - 4, TEXT_HEIGHT - 4);
    }
    break;
  case MENU_ITEM_TYPE_STRING:
    value_string = std::format("{}", (char *)menuItem->value);
    u8g2_DrawStr(&u8g2,
                 (u8g2.width - MENU_PADDING - squish -
                  u8g2_GetStrWidth(&u8g2, value_string.c_str())),
                 y + MENU_PADDING + TEXT_HEIGHT, value_string.c_str());
    break;
  case MENU_ITEM_TYPE_SUBMENU:
    u8g2_DrawXBM(&u8g2, (u8g2.width - MENU_PADDING - squish - ICON_WIDTH),
                 y + (MENU_PADDING / 2), ICON_WIDTH, ICON_HEIGHT,
                 HAM_BRIDGE_ICON_arrowFWD);
    break;
  case MENU_ITEM_TYPE_BACK:
    u8g2_DrawXBM(&u8g2, (u8g2.width - MENU_PADDING - squish - ICON_WIDTH),
                 y + (MENU_PADDING / 2), ICON_WIDTH, ICON_HEIGHT,
                 HAM_BRIDGE_ICON_arrowBWD);
    break;
  }
}

extern "C" void drawScreen() {
  std::shared_lock lock(displayMutex);

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
  // Draw top (previous) item
  if (displayState.menuIndex > 0 &&
      displayState.menuList.get()->at(displayState.menuIndex - 1) != nullptr) {
    drawMenuItem(
        MENU_TOP_PADDING + ((MENU_FRAME_HEIGHT + MENU_SPACING) * 0),
        displayState.menuList.get()->at(displayState.menuIndex - 1).get(),
        false, false);
  }
  // Draw Center (current) item
  if (displayState.menuList.get()->at(displayState.menuIndex) != nullptr) {
    drawMenuItem(MENU_TOP_PADDING + ((MENU_FRAME_HEIGHT + MENU_SPACING) * 1),
                 displayState.menuList.get()->at(displayState.menuIndex).get(),
                 true, false);
  }
  // Draw bottom (next) item
  if (displayState.menuIndex < displayState.menuList->size() &&
      displayState.menuList.get()->at(displayState.menuIndex + 1) != nullptr) {
    drawMenuItem(
        MENU_TOP_PADDING + ((MENU_FRAME_HEIGHT + MENU_SPACING) * 2),
        displayState.menuList.get()->at(displayState.menuIndex + 1).get(),
        false, false);
  }

  u8g2_SendBuffer(&u8g2);
  lock.release();
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

  auto topLevelMenu =
      std::make_shared<std::array<std::shared_ptr<menuListObject>, 10>>();
  auto debugStatMenu =
      std::make_shared<std::array<std::shared_ptr<menuListObject>, 10>>();

  // Put elements in top level menu
  topLevelMenu->at(0) = std::make_shared<menuListObject>(menuListObject{
      .text = "Example",
      .item_type = MENU_ITEM_TYPE_NUMERICAL,
      .value = (void *)9999,
  });

  topLevelMenu->at(1) = std::make_shared<menuListObject>(menuListObject{
      .text = "Example 2",
      .item_type = MENU_ITEM_TYPE_TOGGLE,
      .value = (void *)true,
  });

  topLevelMenu->at(2) = std::make_shared<menuListObject>(menuListObject{
      .text = "Debug Info",
      .item_type = MENU_ITEM_TYPE_SUBMENU,
      .menuListValue = debugStatMenu,
  });

  // Put elements in debug stat menu
  debugStatMenu->at(0) = std::make_shared<menuListObject>(
      menuListObject{.text = "Return",
                     .item_type = MENU_ITEM_TYPE_BACK,
                     .menuListValue = topLevelMenu});
  debugStatMenu->at(1) = std::make_shared<menuListObject>(menuListObject{
      .text = "IP Address",
      .item_type = MENU_ITEM_TYPE_STRING,
      .value = (void *)"Test",
      .getterMethod = getIpAddr,
      .poll = true,
  });
  debugStatMenu->at(2) = std::make_shared<menuListObject>(menuListObject{
      .text = "Example 2",
      .item_type = MENU_ITEM_TYPE_NUMERICAL,
      .value = (void *)9999,
  });
  debugStatMenu->at(3) = std::make_shared<menuListObject>(menuListObject{
      .text = "Example 3",
      .item_type = MENU_ITEM_TYPE_NUMERICAL,
      .value = (void *)9999,
  });

  displayState.menuList = topLevelMenu;

  displayState.iconList[0] = HAM_BRIDGE_ICON_mic;
  displayState.iconList[1] = HAM_BRIDGE_ICON_lan_enable;
  displayState.iconList[2] = HAM_BRIDGE_ICON_lan_disable;
}

void updateDisplayState() {
  // Update current time
  std::shared_lock lock(displayMutex);
  time_t now;
  struct tm timeinfo;
  time(&now);
  localtime_r(&now, &timeinfo);

  displayState.timeinfo = timeinfo;

  // If nearby menu items have polling enabled,
  // call their getter and update the value
  menuListObject *objectToDraw;
  // Update top slot
  if (displayState.menuIndex > 0) {
    objectToDraw =
        displayState.menuList.get()->at(displayState.menuIndex - 1).get();
    if (objectToDraw != nullptr && objectToDraw->poll) {
      objectToDraw->value = objectToDraw->getterMethod();
    }
  }
  // Update selected item (center slot)
  objectToDraw = displayState.menuList.get()->at(displayState.menuIndex).get();
  if (objectToDraw != nullptr && objectToDraw->poll) {
    objectToDraw->value = objectToDraw->getterMethod();
  }

  // Update bottom slot
  if (displayState.menuIndex < displayState.menuList->size() - 1) {
    objectToDraw =
        displayState.menuList.get()->at(displayState.menuIndex + 1).get();
    if (objectToDraw != nullptr && objectToDraw->poll) {
      objectToDraw->value = objectToDraw->getterMethod();
    }
  }

  lock.release();
}

extern "C" void screenRefreshTask() {
  while (1) {
    updateDisplayState();
    drawScreen();
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
extern "C" void processIncomingInput(button_types incomingButton) {
  // Make sure display won't draw on incomplete data
  std::shared_lock lock(displayMutex);
  // Handle up/increment button pressed
  switch (incomingButton) {
  case BUTTON_TYPE_INCREMENT:
    if (displayState.menuIndex < displayState.menuList->size() - 1 &&
        displayState.menuList->at(displayState.menuIndex + 1) != nullptr) {
      displayState.menuIndex++;
    }
    break;

  // Handle down/decrement button pressed
  case BUTTON_TYPE_DECREMENT:
    if (displayState.menuIndex > 0 &&
        displayState.menuList->at(displayState.menuIndex - 1) != nullptr) {
      displayState.menuIndex--;
    }
    break;

    // Handle confirm button pressed
    // Action depends on current menu object
  case BUTTON_TYPE_CONFIRM:
    menuListObject *activeMenuItem =
        displayState.menuList->at(displayState.menuIndex).get();
    switch (activeMenuItem->item_type) {
    case MENU_ITEM_TYPE_NUMERICAL:
    case MENU_ITEM_TYPE_TOGGLE:
    case MENU_ITEM_TYPE_STRING:
      break;

    case MENU_ITEM_TYPE_SUBMENU:
    case MENU_ITEM_TYPE_BACK:
      if (activeMenuItem->menuListValue != nullptr) {
        displayState.menuList = activeMenuItem->menuListValue;
        displayState.menuIndex = 0;
      }
      break;
    }
    break;
  }
  lock.release();
}