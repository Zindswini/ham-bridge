#ifndef INPUT_HANDLER_H
#define INPUT_HANDLER_H

#include <cstdint>
enum ButtonTypes : uint8_t {
  kButtonTypeIncrement = 0,
  kButtonTypeDecrement = 1,
  kButtonTypeConfirm = 2
};

struct ButtonState {
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