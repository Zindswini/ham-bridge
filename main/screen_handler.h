#ifndef SCREEN_HANDLER_H
#define SCREEN_HANDLER_H

#include "driver/i2c_types.h"
#include "input_handler.h"

#define MENU_TOP_PADDING 12
#define MENU_SPACING 2
#define MENU_PADDING 4
#define TEXT_HEIGHT 8
#define ICON_PADDING 4
#define ICON_WIDTH 8
#define ICON_HEIGHT 12
#define MENU_FRAME_HEIGHT (TEXT_HEIGHT + (2 * MENU_PADDING))
// how many pixels the inactive menu elements are squished by:
#define MENU_INACTIVE_SQUISH 10
#define MENU_MAX_DEPTH 4 // How many submenus deep is allowed

#ifdef __cplusplus
extern "C" {
#endif

void drawScreen();
void drawLoadingScreen(const char *loading_text);

void initializeU8G2(i2c_master_bus_handle_t *i2c_bus_handle);
void screenRefreshTask(void *args);

void processIncomingInput(ButtonTypes incoming_button);
#ifdef __cplusplus
}
#endif

#endif // SCREEN_HANDLER_H