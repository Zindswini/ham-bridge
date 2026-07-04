#include "screen_handler.h"

#include "icons.h"
#include "input_handler.h"
#include "u8g2.h"
#include "u8g2_esp32_hal.h"
#include "u8x8.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sys/_timeval.h>
#include <sys/cdefs.h>
#include <sys/time.h>
#include <sys/types.h>
#include <variant>

static const char *tag = "SCREEN_HANDLER";

// Stack-allocated buffer for menu strings
typedef std::array<char, 16> screen_label;

// Helper template used in std::visit calls
template <class... Ts> struct Overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts> Overloaded(Ts...) -> Overloaded<Ts...>;

// Placeholder type to MenuItems with "back action" as their value
struct MenuBack {};

/*
Base class for drawable menu items.
*/
class MenuItem {
public:
  // String Constructor
  MenuItem(screen_label label, std::array<uint8_t, 12> icon, bool hidden,
           screen_label value)
      : label_(label), icon_(icon), hidden_(hidden), value_(value) {};
  // Int Constructor
  MenuItem(screen_label label, std::array<uint8_t, 12> icon, bool hidden,
           int value)
      : label_(label), icon_(icon), hidden_(hidden), value_(value) {};
  // Float Constructor
  MenuItem(screen_label label, std::array<uint8_t, 12> icon, bool hidden,
           float value)
      : label_(label), icon_(icon), hidden_(hidden), value_(value) {};
  // Bool Constructor
  MenuItem(screen_label label, std::array<uint8_t, 12> icon, bool hidden,
           bool value)
      : label_(label), icon_(icon), hidden_(hidden), value_(value) {};
  // MenuItemList Constructor
  MenuItem(screen_label label, std::array<uint8_t, 12> icon, bool hidden,
           std::shared_ptr<std::array<std::shared_ptr<MenuItem>, 10>> value)
      : label_(label), icon_(icon), hidden_(hidden), value_(value) {};
  // Back Constructor
  MenuItem(screen_label label, std::array<uint8_t, 12> icon, bool hidden,
           MenuBack value)
      : label_(label), icon_(icon), hidden_(hidden), value_(value) {};

  void draw(u8g2_t &u8g2, u8g2_int_t base_y, bool active);

  const auto &getValue() { return value_; }

private:
  screen_label label_ = screen_label{"UNDEFINED"};
  std::array<uint8_t, 12> icon_ __unused;
  bool hidden_ __unused = false;

  std::variant<screen_label, int, float, bool,
               std::shared_ptr<std::array<std::shared_ptr<MenuItem>, 10>>,
               MenuBack>
      value_;
};

// Helper type to make definitions more concise
typedef std::shared_ptr<std::array<std::shared_ptr<MenuItem>, 10>>
    menu_item_list;

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

// Layer in the navigation stack
struct NavLayer {
  menu_item_list list = nullptr;
  uint32_t menu_index = 0;
};

// Holder for overall menu state
// Top level object each screen draw starts from
struct ScreenInformation {
  // Time object used for drawing clock
  tm timeinfo{};

  // List of icons displayed in the top left
  std::array<std::array<uint8_t, 12>, 4> icon_list{};

  // Navigation stack
  // Pushed when entering submenu, popped when navigating back
  std::array<NavLayer, MENU_MAX_DEPTH> nav_stack{};
  size_t nav_depth = 0;
};

void MenuItem::draw(u8g2_t &u8g2, u8g2_int_t base_y, bool active) {
  uint32_t squish = !active ? MENU_INACTIVE_SQUISH : 0;
  // Draw box around ACTIVE menu item
  if (active) {
    u8g2_DrawFrame(&u8g2, squish, base_y, u8g2.width - (2 * squish),
                   MENU_FRAME_HEIGHT);
  }
  // Draw text description element of menu item
  u8g2_DrawStr(&u8g2, squish + MENU_PADDING,
               base_y + MENU_PADDING + TEXT_HEIGHT, label_.data());

  // Draw Value based on type
  std::visit(
      Overloaded{
          [&](const screen_label &str_val) {
            u8g2_DrawStr(&u8g2,
                         (u8g2.width - MENU_PADDING - squish -
                          u8g2_GetStrWidth(&u8g2, str_val.data())),
                         base_y + MENU_PADDING + TEXT_HEIGHT, str_val.data());
          },
          [&](int int_val) {
            // Draw number as string on right side
            screen_label value_string;
            std::snprintf(value_string.data(), value_string.size(), "%d",
                          int_val);
            // = std::format("{:d}", int_val);
            u8g2_DrawStr(&u8g2,
                         (u8g2.width - MENU_PADDING - squish -
                          u8g2_GetStrWidth(&u8g2, value_string.data())),
                         base_y + MENU_PADDING + TEXT_HEIGHT,
                         value_string.data());
          },
          [&](float float_val) {
            // Draw number with up to 2 decimals as string on right side
            screen_label value_string;
            std::snprintf(value_string.data(), value_string.size(), "%.2f",
                          static_cast<double>(float_val));
            u8g2_DrawStr(&u8g2,
                         (u8g2.width - MENU_PADDING - squish -
                          u8g2_GetStrWidth(&u8g2, value_string.data())),
                         base_y + MENU_PADDING + TEXT_HEIGHT,
                         value_string.data());
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
          [&](const MenuBack &) {
            u8g2_DrawXBM(&u8g2,
                         (u8g2.width - MENU_PADDING - squish - ICON_WIDTH),
                         base_y + (MENU_PADDING / 2), ICON_WIDTH, ICON_HEIGHT,
                         kHamBridgeIconArrowBWD.data());
          },

      },
      value_);
}

// void MenuItemEditable::draw(u8g2_t &u8g2, u8g2_int_t base_y) {}

u8g2_t u8g2;

static struct ScreenInformation display_state;
std::shared_mutex display_mutex;

extern "C" void drawLoadingScreen(const char *loading_text) {
  std::unique_lock lock(display_mutex);

  u8g2_ClearBuffer(&u8g2);
  u8g2_SetFont(&u8g2, static_cast<const uint8_t *>(u8g2_font_helvB08_tr));

  // Draw message as centered text
  u8g2_DrawStr(&u8g2,
               u8g2.width / 2 - (u8g2_GetStrWidth(&u8g2, loading_text) / 2),
               u8g2.height / 2, loading_text);

  u8g2_SendBuffer(&u8g2);
}

extern "C" void drawScreen() {
  std::unique_lock lock(display_mutex);
  if (display_state.nav_stack.at(display_state.nav_depth).list == nullptr) {
    return;
  }

  u8g2_ClearBuffer(&u8g2);
  u8g2_SetFont(&u8g2, static_cast<const uint8_t *>(u8g2_font_helvB08_tr));

  // Draw Icons
  for (size_t i = 0; i < display_state.icon_list.size(); i++) {
    const std::array<uint8_t, 12> &current_icon_object =
        display_state.icon_list.at(i);
    u8g2_DrawXBM(&u8g2, i * (ICON_WIDTH + ICON_PADDING), 0, ICON_WIDTH,
                 ICON_HEIGHT, current_icon_object.data());
  }

  // Draw Time
  screen_label time_string;
  std::snprintf(time_string.data(), time_string.size(), "%02d:%02d:%02d",
                display_state.timeinfo.tm_hour, display_state.timeinfo.tm_min,
                display_state.timeinfo.tm_sec);
  u8g2_DrawStr(&u8g2,
               (u8g2.width - u8g2_GetStrWidth(&u8g2, time_string.data())), 8,
               time_string.data());

  // Draw Horizontal Line
  u8g2_DrawHLine(&u8g2, 0, 10, u8g2.width);

  // Draw menu items
  // Draw top (previous) item
  uint32_t &menu_index =
      display_state.nav_stack.at(display_state.nav_depth).menu_index;
  menu_item_list &menu_list =
      display_state.nav_stack.at(display_state.nav_depth).list;

  if (menu_index > 0 && menu_list->at(menu_index - 1) != nullptr) {
    menu_list->at(menu_index - 1)
        ->draw(u8g2,
               MENU_TOP_PADDING + ((MENU_FRAME_HEIGHT + MENU_SPACING) * 0),
               false);
  }
  // Draw Center (current) item
  if (menu_list->at(menu_index) != nullptr) {
    menu_list->at(menu_index)
        ->draw(u8g2,
               MENU_TOP_PADDING + ((MENU_FRAME_HEIGHT + MENU_SPACING) * 1),
               true);
  }
  // Draw bottom (next) item
  if (menu_index + 1 < menu_list->size() &&
      menu_list->at(menu_index + 1) != nullptr) {
    menu_list->at(menu_index + 1)
        ->draw(u8g2,
               MENU_TOP_PADDING + ((MENU_FRAME_HEIGHT + MENU_SPACING) * 2),
               false);
  }

  u8g2_SendBuffer(&u8g2);
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
  top_level_menu->at(0) = std::make_shared<MenuItem>(
      screen_label{"Example 1"}, kHamBridgeIconLanEnable, false, 1234);

  top_level_menu->at(1) = std::make_shared<MenuItem>(
      screen_label{"Example 2"}, kHamBridgeIconLanEnable, false, true);

  top_level_menu->at(2) = std::make_shared<MenuItem>(screen_label{"Debug Info"},
                                                     kHamBridgeIconLanEnable,
                                                     false, debug_stat_menu);

  // Put elements in debug stat menu
  debug_stat_menu->at(0) = std::make_shared<MenuItem>(
      screen_label{"Return"}, kHamBridgeIconLanEnable, false, MenuBack{});
  // debugStatMenu->at(1) = std::make_shared<menuListObject>(menuListObject{
  //     .text = "IP Address",
  //     .item_type = MENU_ITEM_TYPE_STRING,
  //     .value = (void *)"Test",
  //     .getterMethod = getIpWrapper,
  //     .poll = true,
  // });
  debug_stat_menu->at(1) = std::make_shared<MenuItem>(
      screen_label{"Example 1"}, kHamBridgeIconLanEnable, false, 1234);

  debug_stat_menu->at(2) = std::make_shared<MenuItem>(
      screen_label{"Example 2"}, kHamBridgeIconLanEnable, false, 1234);

  display_state.nav_stack[0] =
      NavLayer{.list = top_level_menu, .menu_index = 0};
  display_state.nav_depth = 0;

  display_state.icon_list[0] = kHamBridgeIconMicEnable;
  display_state.icon_list[1] = kHamBridgeIconLanEnable;
  display_state.icon_list[2] = kHamBridgeIconLanDisable;
}

void updateDisplayState() {
  // Update current time
  std::unique_lock lock(display_mutex);
  time_t now = 0;
  struct tm timeinfo{};
  time(&now);
  localtime_r(&now, &timeinfo);

  display_state.timeinfo = timeinfo;

  // If nearby menu items have polling enabled,
  // call their getter and update the value
  /*
  if (display_state.menu_list != nullptr) {}
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
}

extern "C" void screenRefreshTask(void *args __unused) {
  while (true) {
    updateDisplayState();
    drawScreen();
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
extern "C" void processIncomingInput(ButtonTypes incoming_button) {
  uint32_t &menu_index =
      display_state.nav_stack.at(display_state.nav_depth).menu_index;
  menu_item_list &menu_list =
      display_state.nav_stack.at(display_state.nav_depth).list;

  std::shared_ptr<MenuItem> active_menu_item;
  // Make sure display won't draw on incomplete data
  std::unique_lock lock(display_mutex);
  if (menu_list == nullptr) {
    // Drop event
    return;
  }
  // Handle up/increment button pressed
  switch (incoming_button) {
  case kButtonTypeIncrement:
    if (menu_index < menu_list->size() - 1 &&
        menu_list->at(menu_index + 1) != nullptr) {
      menu_index++;
    }
    break;

  // Handle down/decrement button pressed
  case kButtonTypeDecrement:
    if (menu_index > 0 && menu_list->at(menu_index - 1) != nullptr) {
      menu_index--;
    }
    break;

    // Handle confirm button pressed
    // Action depends on current menu object
  case kButtonTypeConfirm:
    active_menu_item = menu_list->at(menu_index);

    std::visit(Overloaded{[&](const screen_label &) {}, [&](const int &) {},
                          [&](const float &) {}, [&](const bool &) {},
                          [&](const menu_item_list &new_list) {
                            size_t &nav_depth = display_state.nav_depth;
                            if (nav_depth < display_state.nav_stack.size()) {
                              // Update last index on current object
                              display_state.nav_stack.at(nav_depth).menu_index =
                                  menu_index;

                              // Increment stack
                              nav_depth++;

                              // Put new menu into stack
                              display_state.nav_stack.at(nav_depth) = {
                                  .list = new_list, .menu_index = 0};
                            }
                          },
                          [&](const MenuBack &) {
                            size_t &nav_depth = display_state.nav_depth;
                            if (nav_depth > 0) {
                              // Clear current stack item (decrement shared_ptr)
                              display_state.nav_stack.at(nav_depth) = {
                                  .list = nullptr, .menu_index = 0};

                              // Decrement stack
                              nav_depth--;
                            }
                          }},
               active_menu_item->getValue());
    break;
  }
}