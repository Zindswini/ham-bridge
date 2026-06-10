#ifndef INPUT_HANDLER_H
#define INPUT_HANDLER_H

typedef enum {
  BUTTON_TYPE_INCREMENT = 0,
  BUTTON_TYPE_DECREMENT = 1,
  BUTTON_TYPE_CONFIRM = 2
} button_types;

void setupGpio(void);
void processInputs(void);

#endif