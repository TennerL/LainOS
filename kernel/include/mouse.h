#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

#define MOUSE_LEFT 1
#define MOUSE_RIGHT 2
#define MOUSE_MIDDLE 4

int mouse_init(void);
void mouse_irq_handler(void);
void mouse_handle_byte(uint8_t value);
int mouse_enabled(void);
int mouse_x(void);
int mouse_y(void);
int mouse_buttons(void);
int mouse_dx(void);
int mouse_dy(void);

#endif
