#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

#define MOUSE_LEFT 1
#define MOUSE_RIGHT 2
#define MOUSE_MIDDLE 4

int mouse_init(void);
void mouse_irq_handler(void);
void mouse_handle_byte(uint8_t value);
void mouse_apply_usb_report(uint8_t report_buttons, int dx, int dy);
int mouse_enabled(void);
int mouse_x(void);
int mouse_y(void);
int mouse_buttons(void);
void mouse_snapshot(int *out_x, int *out_y, int *out_buttons);
void mouse_set_position(int x, int y);
void mouse_consume_motion(int *out_dx, int *out_dy, int *out_buttons);
int mouse_dx(void);
int mouse_dy(void);

#endif
