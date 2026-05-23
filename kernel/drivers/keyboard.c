#include <stdint.h>
#include "kernel.h"
#include "lainfs.h"
#include "keyboard.h"
#include "mouse.h"
#include "usb.h"

#define PS2_DATA_PORT 0x60
#define PS2_STATUS_PORT 0x64
#define PS2_STATUS_OUTPUT_FULL 0x01
#define PS2_STATUS_AUX_DATA 0x20
#define SC_LSHIFT 0x2A
#define SC_RSHIFT 0x36
#define SC_RALT 0x38
#define SC_CAPSLOCK 0x3A

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ __volatile__("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline unsigned long irq_save(void) {
    unsigned long flags;
    __asm__ __volatile__("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(unsigned long flags) {
    __asm__ __volatile__("push %0; popfq" : : "r"(flags) : "memory", "cc");
}

static int ps2_has_data(void) {
    return (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) != 0;
}

static int ps2_read_data_atomic(uint8_t *status, uint8_t *value) {
    unsigned long flags = irq_save();
    uint8_t local_status = inb(PS2_STATUS_PORT);

    if ((local_status & PS2_STATUS_OUTPUT_FULL) == 0) {
        irq_restore(flags);
        return 0;
    }

    *status = local_status;
    *value = inb(PS2_DATA_PORT);
    irq_restore(flags);
    return 1;
}

static int shift_down;
static int caps_lock_on;
static int ctrl_down;
static int altgr_down;
static int extended_scancode; 
static keyboard_layout_t current_layout;

static char apply_alpha_case(char ch) {
    if (ch < 'a' || ch > 'z') return ch;
    return ((shift_down ? 1 : 0) ^ (caps_lock_on ? 1 : 0)) ? (char)(ch - 'a' + 'A') : ch;
}

static char scancode_to_ascii_us(uint8_t sc) {
    switch (sc) {
        case 0x02: return shift_down ? '!' : '1';
        case 0x03: return shift_down ? '@' : '2';
        case 0x04: return shift_down ? '#' : '3';
        case 0x05: return shift_down ? '$' : '4';
        case 0x06: return shift_down ? '%' : '5';
        case 0x07: return shift_down ? '^' : '6';
        case 0x08: return shift_down ? '&' : '7';
        case 0x09: return shift_down ? '*' : '8';
        case 0x0A: return shift_down ? '(' : '9';
        case 0x0B: return shift_down ? ')' : '0';
        case 0x10: return apply_alpha_case('q'); case 0x11: return apply_alpha_case('w');
        case 0x12: return apply_alpha_case('e'); case 0x13: return apply_alpha_case('r');
        case 0x14: return apply_alpha_case('t'); case 0x15: return apply_alpha_case('y');
        case 0x16: return apply_alpha_case('u'); case 0x17: return apply_alpha_case('i');
        case 0x18: return apply_alpha_case('o'); case 0x19: return apply_alpha_case('p');
        case 0x1E: return apply_alpha_case('a'); case 0x1F: return apply_alpha_case('s');
        case 0x20: return apply_alpha_case('d'); case 0x21: return apply_alpha_case('f');
        case 0x22: return apply_alpha_case('g'); case 0x23: return apply_alpha_case('h');
        case 0x24: return apply_alpha_case('j'); case 0x25: return apply_alpha_case('k');
        case 0x26: return apply_alpha_case('l');
        case 0x2C: return apply_alpha_case('z'); case 0x2D: return apply_alpha_case('x');
        case 0x2E: return apply_alpha_case('c'); case 0x2F: return apply_alpha_case('v');
        case 0x30: return apply_alpha_case('b'); case 0x31: return apply_alpha_case('n');
        case 0x32: return apply_alpha_case('m');
        case 0x39: return ' ';
        case 0x0C: return shift_down ? '_' : '-';
        case 0x0D: return shift_down ? '+' : '=';
        case 0x1A: return shift_down ? '{' : '[';
        case 0x1B: return shift_down ? '}' : ']';
        case 0x27: return shift_down ? ':' : ';';
        case 0x28: return shift_down ? '"' : '\'';
        case 0x29: return shift_down ? '~' : '`';
        case 0x2B: return shift_down ? '|' : '\\';
        case 0x33: return shift_down ? '<' : ',';
        case 0x34: return shift_down ? '>' : '.';
        case 0x35: return shift_down ? '?' : '/';
        case 0x1C: return '\n';
        case 0x0E: return '\b';
        case 0x0F: return '\t';
        default: return 0;
    }
}

static char scancode_to_ascii_de(uint8_t sc) {
    if (altgr_down) {
        switch (sc) {
            case 0x08: return '{';
            case 0x09: return '[';
            case 0x0A: return ']';
            case 0x0B: return '}';
            case 0x0C: return '\\';
            case 0x10: return '@';
            case 0x1B: return '~';
            case 0x56: return '|';
            default: return 0;
        }
    }

    switch (sc) {
        case 0x02: return shift_down ? '!' : '1';
        case 0x03: return shift_down ? '"' : '2';
        case 0x04: return shift_down ? '#' : '3';
        case 0x05: return shift_down ? '$' : '4';
        case 0x06: return shift_down ? '%' : '5';
        case 0x07: return shift_down ? '&' : '6';
        case 0x08: return shift_down ? '/' : '7';
        case 0x09: return shift_down ? '(' : '8';
        case 0x0A: return shift_down ? ')' : '9';
        case 0x0B: return shift_down ? '=' : '0';
        case 0x10: return apply_alpha_case('q'); case 0x11: return apply_alpha_case('w');
        case 0x12: return apply_alpha_case('e'); case 0x13: return apply_alpha_case('r');
        case 0x14: return apply_alpha_case('t'); case 0x15: return apply_alpha_case('z');
        case 0x16: return apply_alpha_case('u'); case 0x17: return apply_alpha_case('i');
        case 0x18: return apply_alpha_case('o'); case 0x19: return apply_alpha_case('p');
        case 0x1E: return apply_alpha_case('a'); case 0x1F: return apply_alpha_case('s');
        case 0x20: return apply_alpha_case('d'); case 0x21: return apply_alpha_case('f');
        case 0x22: return apply_alpha_case('g'); case 0x23: return apply_alpha_case('h');
        case 0x24: return apply_alpha_case('j'); case 0x25: return apply_alpha_case('k');
        case 0x26: return apply_alpha_case('l');
        case 0x2C: return apply_alpha_case('y'); case 0x2D: return apply_alpha_case('x');
        case 0x2E: return apply_alpha_case('c'); case 0x2F: return apply_alpha_case('v');
        case 0x30: return apply_alpha_case('b'); case 0x31: return apply_alpha_case('n');
        case 0x32: return apply_alpha_case('m');
        case 0x39: return ' ';
        case 0x0C: return shift_down ? '?' : 0;
        case 0x0D: return shift_down ? '`' : '\'';
        case 0x1A: return 0;
        case 0x1B: return shift_down ? '*' : '+';
        case 0x27: return 0;
        case 0x28: return 0;
        case 0x29: return '^';
        case 0x2B: return shift_down ? '\'' : '#';
        case 0x33: return shift_down ? ';' : ',';
        case 0x34: return shift_down ? ':' : '.';
        case 0x35: return shift_down ? '_' : '-';
        case 0x56: return shift_down ? '>' : '<';
        case 0x1C: return '\n';
        case 0x0E: return '\b';
        case 0x0F: return '\t';
        default: return 0;
    }
}

static char scancode_to_ascii(uint8_t sc) {
    if (current_layout == KEYBOARD_LAYOUT_DE) {
        return scancode_to_ascii_de(sc);
    }

    return scancode_to_ascii_us(sc);
}

void keyboard_set_layout(keyboard_layout_t layout) {
    current_layout = layout;
}

keyboard_layout_t keyboard_get_layout(void) {
    return current_layout;
}

const char *keyboard_layout_name(keyboard_layout_t layout) {
    if (layout == KEYBOARD_LAYOUT_DE) {
        return "de";
    }

    return "us";
}

static int keyboard_decode_event(uint8_t status, uint8_t sc, key_event_t *out) {
    key_event_t none = { KEY_NONE, 0 };
    *out = none;

    if ((status & PS2_STATUS_AUX_DATA) != 0) {
        if (mouse_enabled()) {
            mouse_handle_byte(sc);
        }
        return 0;
    }

    if(sc == 0xE0) {
        extended_scancode = 1;
        return 0;
    }

    int extended = extended_scancode;
    extended_scancode = 0;

    int released = (sc & 0x80) != 0;
    uint8_t code = sc & 0x7F;

    if (extended && code == SC_RALT) {
        altgr_down = !released;
        return 0;
    }

    if (!extended && (code == SC_LSHIFT || code == SC_RSHIFT)) {
        shift_down = !released;
        return 0;
    }

    if (code == 0x1D) {
        ctrl_down = !released;
        return 0;
    }

    if (!extended && code == SC_CAPSLOCK && !released) {
        caps_lock_on = !caps_lock_on;
        return 0;
    }

    if (released) {
        return 0;
    }

    if (extended) {
        switch (code) {
            case 0x4B: *out = (key_event_t) { KEY_LEFT, 0 }; return 1;
            case 0x4D: *out = (key_event_t) { KEY_RIGHT, 0}; return 1;
            case 0x48: *out = (key_event_t) { KEY_UP, 0}; return 1;
            case 0x50: *out = (key_event_t) { KEY_DOWN, 0}; return 1;
            default: return 1;
        }
    }

    if (ctrl_down) {
        switch(code) {
            case 0x12: *out = (key_event_t) {KEY_CTRL_E, 0}; return 1; /* E */
            case 0x1F: *out = (key_event_t) {KEY_CTRL_S, 0}; return 1; /* S */
            case 0x10: *out = (key_event_t) {KEY_CTRL_Q, 0}; return 1; /* Q */
            case 0x11: *out = (key_event_t) {KEY_CTRL_W, 0}; return 1; /* W */
            default: break;
        }
    }

    switch (code) {
        case 0x01: *out = (key_event_t) { KEY_ESC, 0}; return 1;
        case 0x1C: *out = (key_event_t) { KEY_ENTER, '\n' }; return 1;
        case 0x0E: *out = (key_event_t) { KEY_BACKSPACE, '\b' }; return 1;
        case 0x0F: *out = (key_event_t) { KEY_TAB, '\t' }; return 1;
        default: break;
    }

    char ch = scancode_to_ascii(code);
    if(ch) {
        *out = (key_event_t){ KEY_CHAR, ch };
        return 1;
    }

    return 0;
}

void keyboard_init(void) {
    shift_down = 0;
    caps_lock_on = 0;
    ctrl_down = 0;
    altgr_down = 0;
    extended_scancode = 0;
    current_layout = KEYBOARD_LAYOUT_US;
    while (ps2_has_data()) {
        (void)inb(PS2_DATA_PORT);
    }
}

int keyboard_poll_key(key_event_t *out) {
    key_event_t none = { KEY_NONE, 0 };
    uint8_t status = 0;
    uint8_t sc = 0;

    if (out == 0) {
        return 0;
    }

    *out = none;
    while (ps2_read_data_atomic(&status, &sc)) {
        if (keyboard_decode_event(status, sc, out)) {
            return 1;
        }
    }

    return 0;
}

int console_read_line(char *buffer, unsigned int max_len) {
    unsigned int len = 0;
    if (max_len == 0) return 0;

    while (len + 1 < max_len && buffer[len] != '\0') {
        ++len;
    }

    if (len + 1 >= max_len) {
        buffer[max_len - 1] = '\0';
        len = max_len - 1;
    }

    console_cursor_enable(1);

    for(;;) {
        key_event_t key = keyboard_read_key();

        if (key.type == KEY_CTRL_W) {
            if (!console_split_enabled()) {
                if (console_split_enable() != 0) {
                    continue;
                }
                console_split_focus_next();
                console_cursor_enable(0);
                return 3;
            } else {
                console_split_focus_next();
            }
            console_cursor_enable(0);
            return 1;
        }

        if (key.type == KEY_CTRL_E) {
            if (console_split_enabled() && console_active_pane() != 0u) {
                console_split_disable();
                console_cursor_enable(0);
                return 2;
            }
            continue;
        }

        if(key.type == KEY_ENTER) {
            buffer[len] = '\0';
            console_puts("\n");
            console_cursor_enable(0);
            return 0;
        }

        if (key.type == KEY_BACKSPACE) {
            if (len > 0) {
                --len;
                buffer[len] = '\0';
                console_puts("\b");
            }
            continue;
        }

        if (key.type == KEY_TAB) {
           key.type = KEY_CHAR;
           key.ch = ' ';
        }

        if (key.type != KEY_CHAR) {
            continue;
        }

        if (len + 1 >= max_len) {
            continue;
        }

        buffer[len++] = key.ch;
        buffer[len] = '\0';

        char out[2] = { key.ch, 0 };
        console_puts(out);
    }
}

key_event_t keyboard_read_key(void) {
    key_event_t none = { KEY_NONE, 0 };

    for (;;) {
        while (!ps2_has_data()) {
            console_cursor_tick();
            statusbar_update_if_due();
            shell_modules_tick();
            usb_poll();
            (void)kernel_task_poll();
            status_cpu_enter_idle();
            __asm__ __volatile__("sti; hlt");
            status_cpu_leave_idle();
        }

        uint8_t status = 0;
        uint8_t sc = 0;

        if (!ps2_read_data_atomic(&status, &sc)) {
            continue;
        }

        key_event_t key = none;
        if (keyboard_decode_event(status, sc, &key)) {
            return key;
        }
    }
}
