#ifndef SCREEN_HANDLER_H
#define SCREEN_HANDLER_H

#include "driver/i2c_types.h"
#include "u8g2.h"
#include <stdint.h>

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

  struct menuListObject *menuList;
  uint8_t menuListLength;

  struct iconListObject *iconList;
  uint8_t iconListLength;
};

void drawScreen();

void initializeU8G2(i2c_master_bus_handle_t *i2c_bus_handle);
void screenRefreshTask(void);

#endif