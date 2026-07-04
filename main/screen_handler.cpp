#include "screen_handler.h"

#include "icons.h"
#include "input_handler.h"
#include "u8g2.h"
#include "u8g2_esp32_hal.h"
#include "u8x8.h"
#include <FreeRTOSConfig.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <esp_log.h>
#include <esp_netif.h>
#include <format>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/projdefs.h>
#include <memory>
#include <shared_mutex>
#include <string>
#include <sys/_timeval.h>
#include <sys/cdefs.h>
#include <sys/time.h>
#include <sys/types.h>
#include <utility>
#include <variant>

static const char *tag = "SCREEN_HANDLER";

/*
Base class for drawable menu items.
*/
class MenuItem {
public:
  // String Constructor
  MenuItem(std::string label, std::array<uint8_t, 12> icon, bool hidden,
           std::string value)
      : label_(std::move(label)), icon_(icon), hidden_(hidden),
        value_(value) {};
  // Int Constructor
  MenuItem(std::string label, std::array<uint8_t, 12> icon, bool hidden,
           int value)
      : label_(std::move(label)), icon_(icon), hidden_(hidden),
        value_(value) {};
  // Float Constructor
  MenuItem(std::string label, std::array<uint8_t, 12> icon, bool hidden,
           float value)
      : label_(std::move(label)), icon_(icon), hidden_(hidden),
        value_(value) {};
  // Bool Constructor
  MenuItem(std::string label, std::array<uint8_t, 12> icon, bool hidden,
           bool value)
      : label_(std::move(label)), icon_(icon), hidden_(hidden),
        value_(value) {};
  // MenuItemList Constructor
  MenuItem(std::string label, std::array<uint8_t, 12> icon, bool hidden,
           std::shared_ptr<std::array<std::shared_ptr<MenuItem>, 10>> value)
      : label_(std::move(label)), icon_(icon), hidden_(hidden),
        value_(value) {};

  void draw(u8g2_t &u8g2, u8g2_int_t base_y, bool active);

  auto getValue() { return value_; }

private:
  std::string label_ = "UNDEFINED";
  std::array<uint8_t, 12> icon_ __unused;
  bool hidden_ __unused = false;

  std::variant<std::string, int, float, bool,
               std::shared_ptr<std::array<std::shared_ptr<MenuItem>, 10>>>
      value_;
};

class MenuItemEditable : public MenuItem {
public:
  void valueUp();
  void valueDown();
  void toggleEditing() {
    if (editable_) {
      active_ = !active_;
    }
  }
  void confirm();
  void draw(u8g2_t &u8g2, u8g2_int_t base_y, bool active);

private:
  bool editable_ = true;
  bool active_ = false;
};

typedef std::shared_ptr<std::array<std::shared_ptr<MenuItem>, 10>>
    menu_item_list;

template <class... Ts> struct Overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts> Overloaded(Ts...) -> Overloaded<Ts...>;

void MenuItem::draw(u8g2_t &u8g2, u8g2_int_t base_y, bool active) {
  uint32_t squish = !active ? MENU_INACTIVE_SQUISH : 0;
  // Draw box around ACTIVE menu item
  if (active) {
    u8g2_DrawFrame(&u8g2, squish, base_y, u8g2.width - (2 * squish),
                   MENU_FRAME_HEIGHT);
  }
  // Draw text description element of menu item
  u8g2_DrawStr(&u8g2, squish + MENU_PADDING,
               base_y + MENU_PADDING + TEXT_HEIGHT, label_.c_str());

  // Draw Value based on type
  std::visit(
      Overloaded{
          [&](const std::string &str_val) {
            u8g2_DrawStr(&u8g2,
                         (u8g2.width - MENU_PADDING - squish -
                          u8g2_GetStrWidth(&u8g2, str_val.c_str())),
                         base_y + MENU_PADDING + TEXT_HEIGHT, str_val.c_str());
          },
          [&](int int_val) {
            // Draw number as string on right side
            std::string value_string = std::format("{:d}", int_val);
            u8g2_DrawStr(&u8g2,
                         (u8g2.width - MENU_PADDING - squish -
                          u8g2_GetStrWidth(&u8g2, value_string.c_str())),
                         base_y + MENU_PADDING + TEXT_HEIGHT,
                         value_string.c_str());
          },
          [&](float float_val) {
            // Draw number with up to 2 decimals as string on right side
            std::string value_string = std::format("{:.2f}", float_val);
            u8g2_DrawStr(&u8g2,
                         (u8g2.width - MENU_PADDING - squish -
                          u8g2_GetStrWidth(&u8g2, value_string.c_str())),
                         base_y + MENU_PADDING + TEXT_HEIGHT,
                         value_string.c_str());
          },
          [&](bool bool_val) {
            // Draw Box Frame (size is textheight x textheight)
            u8g2_DrawFrame(&u8g2,
                           (u8g2.width - MENU_PADDING - TEXT_HEIGHT - squish),
                           base_y + MENU_PADDING, TEXT_HEIGHT, TEXT_HEIGHT);

            if (bool_val) {
              // Fill in box
              u8g2_DrawBox(
                  &u8g2, (u8g2.width - MENU_PADDING - TEXT_HEIGHT - squish + 2),
                  base_y + MENU_PADDING + 2, TEXT_HEIGHT - 4, TEXT_HEIGHT - 4);
            }
          },
          [&](const menu_item_list &) {
            u8g2_DrawXBM(&u8g2,
                         (u8g2.width - MENU_PADDING - squish - ICON_WIDTH),
                         base_y + (MENU_PADDING / 2), ICON_WIDTH, ICON_HEIGHT,
                         kHamBridgeIconArrowFWD.data());
          },

      },
      value_);
}

// void MenuItemEditable::draw(u8g2_t &u8g2, u8g2_int_t base_y) {}

/*
Holder for overall menu state
Top level object each screen draw starts from
*/
struct ScreenInformation {
  tm timeinfo{};
  uint32_t menu_index = 0;
  menu_item_list menu_list = nullptr;
  std::array<std::array<uint8_t, 12>, 4> icon_list{};
};

u8g2_t u8g2;

static struct ScreenInformation display_state;
std::shared_mutex display_mutex;

extern "C" void drawLoadingScreen(const char *loading_text) {
  u8g2_ClearBuffer(&u8g2);
  u8g2_SetFont(&u8g2, static_cast<const uint8_t *>(u8g2_font_helvB08_tr));

  // Draw message as centered text
  u8g2_DrawStr(&u8g2,
               u8g2.width / 2 - (u8g2_GetStrWidth(&u8g2, loading_text) / 2),
               u8g2.height / 2, loading_text);

  u8g2_SendBuffer(&u8g2);
}

extern "C" void drawScreen() {
  std::shared_lock lock(display_mutex);

  u8g2_ClearBuffer(&u8g2);
  u8g2_SetFont(&u8g2, static_cast<const uint8_t *>(u8g2_font_helvB08_tr));

  // Draw Icons
  for (size_t i = 0; i < display_state.icon_list.size(); i++) {
    const std::array<uint8_t, 12> current_icon_object =
        display_state.icon_list.at(i);
    u8g2_DrawXBM(&u8g2, i * (ICON_WIDTH + ICON_PADDING), 0, ICON_WIDTH,
                 ICON_HEIGHT, current_icon_object.data());
  }

  // Draw Time
  std::string time_string =
      std::format("{:02d}:{:02d}:{:02d}", display_state.timeinfo.tm_hour,
                  display_state.timeinfo.tm_min, display_state.timeinfo.tm_sec);
  u8g2_DrawStr(&u8g2,
               (u8g2.width - u8g2_GetStrWidth(&u8g2, time_string.c_str())), 8,
               time_string.c_str());

  // Draw Horizontal Line
  u8g2_DrawHLine(&u8g2, 0, 10, u8g2.width);

  // Draw menu items
  // Draw top (previous) item
  if (display_state.menu_index > 0 &&
      display_state.menu_list->at(display_state.menu_index - 1) != nullptr) {
    display_state.menu_list->at(display_state.menu_index - 1)
        ->draw(u8g2,
               MENU_TOP_PADDING + ((MENU_FRAME_HEIGHT + MENU_SPACING) * 0),
               false);
  }
  // Draw Center (current) item
  if (display_state.menu_list->at(display_state.menu_index) != nullptr) {
    display_state.menu_list->at(display_state.menu_index)
        ->draw(u8g2,
               MENU_TOP_PADDING + ((MENU_FRAME_HEIGHT + MENU_SPACING) * 1),
               true);
  }
  // Draw bottom (next) item
  if (display_state.menu_index < display_state.menu_list->size() &&
      display_state.menu_list->at(display_state.menu_index + 1) != nullptr) {
    display_state.menu_list->at(display_state.menu_index + 1)
        ->draw(u8g2,
               MENU_TOP_PADDING + ((MENU_FRAME_HEIGHT + MENU_SPACING) * 2),
               false);
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

  ESP_LOGI(tag, "Preparing to Initialize U8G2");
  u8x8_SetI2CAddress(&u8g2.u8x8, 0x78);
  u8g2_InitDisplay(&u8g2);     // NOLINT(cppcoreguidelines-pro-type-cstyle-cast)
  u8g2_SetPowerSave(&u8g2, 0); // NOLINT(cppcoreguidelines-pro-type-cstyle-cast)
  ESP_LOGI(tag, "Initialized U8G2 and Display");

  auto top_level_menu =
      std::make_shared<std::array<std::shared_ptr<MenuItem>, 10>>();
  auto debug_stat_menu =
      std::make_shared<std::array<std::shared_ptr<MenuItem>, 10>>();

  // Put elements in top level menu
  top_level_menu->at(0) = std::make_unique<MenuItem>(
      MenuItem("Example 1", kHamBridgeIconLanEnable, false, 1234));

  top_level_menu->at(1) = std::make_unique<MenuItem>(
      MenuItem("Example 2", kHamBridgeIconLanEnable, false, true));

  top_level_menu->at(2) = std::make_unique<MenuItem>(
      MenuItem("Debug Info", kHamBridgeIconLanEnable, false, debug_stat_menu));

  // Put elements in debug stat menu
  debug_stat_menu->at(0) = std::make_unique<MenuItem>(
      MenuItem("Return", kHamBridgeIconLanEnable, false, top_level_menu));
  // debugStatMenu->at(1) = std::make_shared<menuListObject>(menuListObject{
  //     .text = "IP Address",
  //     .item_type = MENU_ITEM_TYPE_STRING,
  //     .value = (void *)"Test",
  //     .getterMethod = getIpWrapper,
  //     .poll = true,
  // });
  debug_stat_menu->at(2) = std::make_unique<MenuItem>(
      MenuItem("Example 1", kHamBridgeIconLanEnable, false, 1234));

  debug_stat_menu->at(3) = std::make_unique<MenuItem>(
      MenuItem("Example 2", kHamBridgeIconLanEnable, false, 1234));

  display_state.menu_list = top_level_menu;

  display_state.icon_list[0] = kHamBridgeIconMicEnable;
  display_state.icon_list[1] = kHamBridgeIconLanEnable;
  display_state.icon_list[2] = kHamBridgeIconLanDisable;
}

void updateDisplayState() {
  // Update current time
  std::shared_lock lock(display_mutex);
  time_t now = 0;
  struct tm timeinfo{};
  time(&now);
  localtime_r(&now, &timeinfo);

  display_state.timeinfo = timeinfo;

  // If nearby menu items have polling enabled,
  // call their getter and update the value
  /*
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
  */

  lock.release();
}

extern "C" void screenRefreshTask(void *args __unused) {
  while (true) {
    updateDisplayState();
    drawScreen();
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
extern "C" void processIncomingInput(ButtonTypes incoming_button) {
  // Make sure display won't draw on incomplete data
  std::shared_lock lock(display_mutex);
  // Handle up/increment button pressed
  switch (incoming_button) {
  case kButtonTypeIncrement:
    if (display_state.menu_index < display_state.menu_list->size() - 1 &&
        display_state.menu_list->at(display_state.menu_index + 1) != nullptr) {
      display_state.menu_index++;
    }
    break;

  // Handle down/decrement button pressed
  case kButtonTypeDecrement:
    if (display_state.menu_index > 0 &&
        display_state.menu_list->at(display_state.menu_index - 1) != nullptr) {
      display_state.menu_index--;
    }
    break;

    // Handle confirm button pressed
    // Action depends on current menu object
  case kButtonTypeConfirm:
    std::shared_ptr<MenuItem> active_menu_item =
        display_state.menu_list->at(display_state.menu_index);

    std::visit(Overloaded{
                   [&](const std::string &) {},
                   [&](const int &) {},
                   [&](const float &) {},
                   [&](const bool &) {},
                   [&](const menu_item_list &new_list) {
                     display_state.menu_list = new_list;
                     display_state.menu_index = 0;
                   },
               },
               active_menu_item->getValue());
    break;
  }
  lock.release();
}