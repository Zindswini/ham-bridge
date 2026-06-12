#ifndef INPUT_HANDLER_H
#define INPUT_HANDLER_H

typedef enum {
  BUTTON_TYPE_INCREMENT = 0,
  BUTTON_TYPE_DECREMENT = 1,
  BUTTON_TYPE_CONFIRM = 2
} button_types;

struct button_state {
  bool increment_button_state;
  bool decrement_button_state;
  bool confirm_button_state;
};

#ifdef __cplusplus
extern "C" {
#endif

void setupButtonGPIOTimer(void);
void processInputsTask(void);

#ifdef __cplusplus
}
#endif

#endif // INPUT_HANDLER_H