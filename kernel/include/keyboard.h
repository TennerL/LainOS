#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

typedef enum {
    KEY_NONE,
    KEY_CHAR,
    KEY_ENTER,
    KEY_BACKSPACE,
    KEY_TAB,
    KEY_ESC,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_UP,
    KEY_DOWN,
    KEY_CTRL_E,
    KEY_CTRL_S,
    KEY_CTRL_Q,
    KEY_CTRL_W
} key_type_t;

typedef struct {
    key_type_t type;
    char ch;
} key_event_t;

typedef enum {
    KEYBOARD_LAYOUT_US,
    KEYBOARD_LAYOUT_DE
} keyboard_layout_t;

void keyboard_set_layout(keyboard_layout_t layout);
keyboard_layout_t keyboard_get_layout(void);
const char *keyboard_layout_name(keyboard_layout_t layout);
int keyboard_poll_key(key_event_t *out);
key_event_t keyboard_read_key(void);
void keyboard_apply_usb_boot_report(const uint8_t *report, unsigned int report_len);

#endif
