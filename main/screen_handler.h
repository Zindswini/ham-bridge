#ifndef SCREEN_HANDLER_H
#define SCREEN_HANDLER_H

#include "driver/i2c_types.h"
#include "u8g2.h"

void drawScreen(u8g2_t *u8g2);

void initializeU8G2(u8g2_t *u8g2, i2c_master_bus_handle_t *i2c_bus_handle);

#endif