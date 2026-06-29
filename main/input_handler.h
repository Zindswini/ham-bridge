#ifndef INPUT_HANDLER_H
#define INPUT_HANDLER_H

#include <cstdint>
enum button_types : uint32_t {
  BUTTON_TYPE_INCREMENT = 0,
  BUTTON_TYPE_DECREMENT = 1,
  BUTTON_TYPE_CONFIRM = 2
};

struct button_state {
  bool increment_button_state;
  bool decrement_button_state;
  bool confirm_button_state;
};

#ifdef __cplusplus
extern "C" {
#endif

void setupButtonGPIOTimer(void);
void processInputsTask(void *args);

#ifdef __cplusplus
}
#endif

#endif // INPUT_HANDLER_H