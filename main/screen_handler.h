#ifndef SCREEN_HANDLER_H
#define SCREEN_HANDLER_H

#include "driver/i2c_types.h"
#include "u8g2.h"
#include <stdint.h>

#define MENU_TOP_PADDING 12
#define MENU_SPACING 2
#define MENU_PADDING 4
#define TEXT_HEIGHT 8
#define MENU_FRAME_HEIGHT (TEXT_HEIGHT + (2 * MENU_PADDING))

typedef enum {
  MENU_ITEM_TYPE_NUMERICAL = 0,
  MENU_ITEM_TYPE_TOGGLE = 1,
} menu_item_type;

struct menuListObject {
  char *text;
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
  uint8_t time_hours;
  uint8_t time_minutes;
  uint8_t time_seconds;

  struct menuListObject *menuList[10];
  struct iconListObject *iconList[4];
};

void drawScreen();

void initializeU8G2(i2c_master_bus_handle_t *i2c_bus_handle);
void screenRefreshTask(void);

#endif