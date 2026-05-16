#include <stdint.h>
#include "graphics.h"
#include "mouse.h"

#define PS2_DATA_PORT 0x60u
#define PS2_STATUS_PORT 0x64u
#define PS2_COMMAND_PORT 0x64u
#define PS2_STATUS_OUTPUT_FULL 0x01u
#define PS2_STATUS_INPUT_FULL 0x02u
#define PS2_STATUS_AUX_DATA 0x20u
#define PS2_COMMAND_READ_CONFIG 0x20u
#define PS2_COMMAND_WRITE_CONFIG 0x60u
#define PS2_COMMAND_ENABLE_AUX 0xA8u
#define PS2_COMMAND_WRITE_AUX 0xD4u
#define PS2_CONFIG_ENABLE_IRQ12 0x02u
#define PS2_CONFIG_DISABLE_AUX_CLOCK 0x20u
#define MOUSE_CMD_SET_DEFAULTS 0xF6u
#define MOUSE_CMD_SET_SAMPLE_RATE 0xF3u
#define MOUSE_CMD_SET_RESOLUTION 0xE8u
#define MOUSE_CMD_SET_SCALING_1_1 0xE6u
#define MOUSE_CMD_ENABLE_STREAMING 0xF4u
#define MOUSE_CMD_GET_DEVICE_ID 0xF2u
#define MOUSE_ACK 0xFAu
#define MOUSE_DEVICE_ID_STANDARD 0x00u
#define MOUSE_DEVICE_ID_WHEEL 0x03u
#define MOUSE_PACKET_ALWAYS_ONE 0x08u
#define MOUSE_PACKET_X_SIGN 0x10u
#define MOUSE_PACKET_Y_SIGN 0x20u
#define MOUSE_PACKET_X_OVERFLOW 0x40u
#define MOUSE_PACKET_Y_OVERFLOW 0x80u
#define MOUSE_BUTTON_BITS 0x07u
#define MOUSE_PS2_MAX_DELTA 96
#define MOUSE_USB_MAX_DELTA 128
#define MOUSE_PS2_RESOLUTION 2u
#define MOUSE_PS2_SAMPLE_RATE 100u
#define PIC1_COMMAND 0x20u
#define PIC1_DATA 0x21u
#define PIC2_COMMAND 0xA0u
#define PIC2_DATA 0xA1u
#define PIC_EOI 0x20u
#define PS2_WAIT_LIMIT 100000u

static volatile int enabled;
static volatile int ps2_enabled;
static volatile int ps2_has_wheel;
static volatile int ps2_packet_size;
static volatile int packet_index;
static volatile uint8_t packet[4];
static volatile int pos_x;
static volatile int pos_y;
static volatile int buttons;
static volatile int last_dx;
static volatile int last_dy;
static volatile int last_wheel;
static volatile int pending_dx;
static volatile int pending_dy;
static volatile int pending_wheel;
static volatile uint32_t rejected_packets;
static volatile uint32_t ps2_packet_count;
static volatile uint32_t usb_report_count;
static volatile int last_source;

static int clamp_coord(int value, unsigned int limit);
static void mouse_handle_ps2_byte_locked(uint8_t value);

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ __volatile__("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ __volatile__("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline unsigned long irq_save(void) {
    unsigned long flags;
    __asm__ __volatile__("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(unsigned long flags) {
    __asm__ __volatile__("push %0; popfq" : : "r"(flags) : "memory", "cc");
}

static void mouse_apply_state(int new_buttons, int dx, int dy, int wheel, int apply_motion, int usb_motion) {
    int screen_dy = usb_motion ? dy : -dy;

    buttons = new_buttons & MOUSE_BUTTON_BITS;
    last_wheel = wheel;
    pending_wheel += wheel;
    if (apply_motion) {
        last_dx = dx;
        last_dy = screen_dy;
        pos_x = clamp_coord(pos_x + dx, graphics_width());
        pos_y = clamp_coord(pos_y + screen_dy, graphics_height());
        pending_dx += dx;
        pending_dy += screen_dy;
    } else {
        last_dx = 0;
        last_dy = 0;
    }
}

static int wait_input_empty(void) {
    for (unsigned int i = 0; i < PS2_WAIT_LIMIT; ++i) {
        if ((inb(PS2_STATUS_PORT) & PS2_STATUS_INPUT_FULL) == 0) {
            return 0;
        }
    }
    return -1;
}

static int wait_output_full(void) {
    for (unsigned int i = 0; i < PS2_WAIT_LIMIT; ++i) {
        if ((inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) != 0) {
            return 0;
        }
    }
    return -1;
}

static int read_data_with_aux_filter(uint8_t *value, int want_aux) {
    for (unsigned int i = 0; i < PS2_WAIT_LIMIT; ++i) {
        uint8_t status = inb(PS2_STATUS_PORT);

        if ((status & PS2_STATUS_OUTPUT_FULL) == 0) {
            continue;
        }

        *value = inb(PS2_DATA_PORT);
        if (want_aux < 0 || (((status & PS2_STATUS_AUX_DATA) != 0) == (want_aux != 0))) {
            return 0;
        }
    }

    return -1;
}

static int write_controller(uint8_t command) {
    if (wait_input_empty() != 0) {
        return -1;
    }
    outb(PS2_COMMAND_PORT, command);
    return 0;
}

static int write_data(uint8_t value) {
    if (wait_input_empty() != 0) {
        return -1;
    }
    outb(PS2_DATA_PORT, value);
    return 0;
}

static int read_data(uint8_t *value) {
    if (wait_output_full() != 0) {
        return -1;
    }
    *value = inb(PS2_DATA_PORT);
    return 0;
}

static int write_mouse(uint8_t command) {
    uint8_t response;

    if (write_controller(PS2_COMMAND_WRITE_AUX) != 0 || write_data(command) != 0) {
        return -1;
    }

    if (read_data_with_aux_filter(&response, 1) != 0 || response != MOUSE_ACK) {
        return -1;
    }

    return 0;
}

static int write_mouse_arg(uint8_t command, uint8_t argument) {
    uint8_t response;

    if (write_controller(PS2_COMMAND_WRITE_AUX) != 0 || write_data(command) != 0) {
        return -1;
    }
    if (read_data_with_aux_filter(&response, 1) != 0 || response != MOUSE_ACK) {
        return -1;
    }
    if (write_controller(PS2_COMMAND_WRITE_AUX) != 0 || write_data(argument) != 0) {
        return -1;
    }
    if (read_data_with_aux_filter(&response, 1) != 0 || response != MOUSE_ACK) {
        return -1;
    }

    return 0;
}

static int read_mouse_id(uint8_t *id) {
    uint8_t response;

    if (id == 0 ||
        write_controller(PS2_COMMAND_WRITE_AUX) != 0 ||
        write_data(MOUSE_CMD_GET_DEVICE_ID) != 0) {
        return -1;
    }
    if (read_data_with_aux_filter(&response, 1) != 0 || response != MOUSE_ACK) {
        return -1;
    }
    return read_data_with_aux_filter(id, 1);
}

static int enable_ps2_wheel_mode(void) {
    uint8_t id = MOUSE_DEVICE_ID_STANDARD;

    if (write_mouse_arg(MOUSE_CMD_SET_SAMPLE_RATE, 200u) != 0 ||
        write_mouse_arg(MOUSE_CMD_SET_SAMPLE_RATE, 100u) != 0 ||
        write_mouse_arg(MOUSE_CMD_SET_SAMPLE_RATE, 80u) != 0 ||
        read_mouse_id(&id) != 0) {
        return 0;
    }

    return id == MOUSE_DEVICE_ID_WHEEL;
}

static void flush_output(void) {
    for (unsigned int i = 0; i < 64u; ++i) {
        if ((inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) == 0) {
            return;
        }
        (void)inb(PS2_DATA_PORT);
    }
}

static void unmask_mouse_irq(void) {
    uint8_t master_mask = inb(PIC1_DATA);
    uint8_t slave_mask = inb(PIC2_DATA);

    master_mask &= (uint8_t)~0x04u;
    slave_mask &= (uint8_t)~0x10u;

    outb(PIC1_DATA, master_mask);
    outb(PIC2_DATA, slave_mask);
}

static int clamp_coord(int value, unsigned int limit) {
    if (value < 0) {
        return 0;
    }
    if (limit == 0) {
        return 0;
    }
    if ((unsigned int)value >= limit) {
        return (int)(limit - 1u);
    }
    return value;
}

static int sign_extend_byte(uint8_t value, uint8_t sign_bit) {
    int result = (int)value;
    if (sign_bit != 0) {
        result -= 256;
    }
    return result;
}

static int sign_extend_nibble(uint8_t value) {
    int result = (int)(value & 0x0Fu);

    if ((value & 0x08u) != 0) {
        result -= 16;
    }
    return result;
}

static int abs_int(int value) {
    return value < 0 ? -value : value;
}

static int mouse_ps2_header_valid(uint8_t value) {
    if ((value & MOUSE_PACKET_ALWAYS_ONE) == 0) {
        return 0;
    }
    if ((value & (MOUSE_PACKET_X_OVERFLOW | MOUSE_PACKET_Y_OVERFLOW)) != 0) {
        return 0;
    }
    return 1;
}

static int mouse_ps2_packet_valid(void) {
    int x_sign;
    int y_sign;

    if (!mouse_ps2_header_valid(packet[0])) {
        return 0;
    }

    x_sign = (packet[0] & MOUSE_PACKET_X_SIGN) != 0;
    y_sign = (packet[0] & MOUSE_PACKET_Y_SIGN) != 0;
    if (((packet[1] & 0x80u) != 0) != x_sign) {
        return 0;
    }
    if (((packet[2] & 0x80u) != 0) != y_sign) {
        return 0;
    }

    return 1;
}

static void mouse_reject_packet(void) {
    packet_index = 0;
    last_dx = 0;
    last_dy = 0;
    last_wheel = 0;
    ++rejected_packets;
}

static void mouse_handle_ps2_byte_locked(uint8_t value) {
    if (packet_index == 0 && (value & MOUSE_PACKET_ALWAYS_ONE) == 0) {
        return;
    }

    if (packet_index == 0 && !mouse_ps2_header_valid(value)) {
        mouse_reject_packet();
        return;
    }

    packet[packet_index++] = value;
    if (packet_index < ps2_packet_size) {
        return;
    }

    if (!mouse_ps2_packet_valid()) {
        mouse_reject_packet();
        return;
    }

    int dx = sign_extend_byte(packet[1], packet[0] & MOUSE_PACKET_X_SIGN);
    int dy = sign_extend_byte(packet[2], packet[0] & MOUSE_PACKET_Y_SIGN);
    int wheel = ps2_has_wheel ? sign_extend_nibble(packet[3]) : 0;

    if (abs_int(dx) > MOUSE_PS2_MAX_DELTA || abs_int(dy) > MOUSE_PS2_MAX_DELTA) {
        mouse_reject_packet();
        return;
    }

    mouse_apply_state(packet[0], dx, dy, wheel, 1, 0);
    ++ps2_packet_count;
    last_source = 1;
    packet_index = 0;
}

void mouse_handle_byte(uint8_t value) {
    unsigned long flags = irq_save();

    mouse_handle_ps2_byte_locked(value);
    irq_restore(flags);
}

void mouse_apply_usb_report(uint8_t report_buttons, int dx, int dy, int wheel) {
    unsigned long flags;

    if (abs_int(dx) > MOUSE_USB_MAX_DELTA || abs_int(dy) > MOUSE_USB_MAX_DELTA) {
        return;
    }

    flags = irq_save();
    mouse_apply_state(report_buttons, dx, dy, wheel, 1, 1);
    ++usb_report_count;
    last_source = 2;
    enabled = 1;
    irq_restore(flags);
}

int mouse_init(void) {
    uint8_t config;

    enabled = 0;
    ps2_enabled = 0;
    ps2_has_wheel = 0;
    ps2_packet_size = 3;
    packet_index = 0;
    buttons = 0;
    last_dx = 0;
    last_dy = 0;
    last_wheel = 0;
    pending_dx = 0;
    pending_dy = 0;
    pending_wheel = 0;
    rejected_packets = 0;
    ps2_packet_count = 0;
    usb_report_count = 0;
    last_source = 0;
    pos_x = (int)(graphics_width() / 2u);
    pos_y = (int)(graphics_height() / 2u);

    flush_output();
    if (write_controller(PS2_COMMAND_ENABLE_AUX) != 0) {
        return -1;
    }

    if (write_controller(PS2_COMMAND_READ_CONFIG) != 0 || read_data(&config) != 0) {
        return -1;
    }

    config |= PS2_CONFIG_ENABLE_IRQ12;
    config &= (uint8_t)~PS2_CONFIG_DISABLE_AUX_CLOCK;
    if (write_controller(PS2_COMMAND_WRITE_CONFIG) != 0 || write_data(config) != 0) {
        return -1;
    }

    if (write_mouse(MOUSE_CMD_SET_DEFAULTS) != 0) {
        return -1;
    }
    (void)write_mouse(MOUSE_CMD_SET_SCALING_1_1);
    ps2_has_wheel = enable_ps2_wheel_mode();
    ps2_packet_size = ps2_has_wheel ? 4 : 3;
    (void)write_mouse_arg(MOUSE_CMD_SET_RESOLUTION, MOUSE_PS2_RESOLUTION);
    (void)write_mouse_arg(MOUSE_CMD_SET_SAMPLE_RATE, MOUSE_PS2_SAMPLE_RATE);
    if (write_mouse(MOUSE_CMD_ENABLE_STREAMING) != 0) {
        return -1;
    }

    ps2_enabled = 1;
    enabled = 1;
    unmask_mouse_irq();
    return 0;
}

void mouse_irq_handler(void) {
    for (unsigned int i = 0; i < 16u; ++i) {
        uint8_t status = inb(PS2_STATUS_PORT);

        if ((status & PS2_STATUS_OUTPUT_FULL) == 0 ||
            (status & PS2_STATUS_AUX_DATA) == 0) {
            break;
        }

        {
            uint8_t value = inb(PS2_DATA_PORT);
            if (ps2_enabled) {
                mouse_handle_byte(value);
            }
        }
    }

    outb(PIC2_COMMAND, PIC_EOI);
    outb(PIC1_COMMAND, PIC_EOI);
}

int mouse_enabled(void) {
    return enabled;
}

int mouse_x(void) {
    return pos_x;
}

int mouse_y(void) {
    return pos_y;
}

int mouse_buttons(void) {
    return buttons;
}

void mouse_snapshot(int *out_x, int *out_y, int *out_buttons) {
    unsigned long flags = irq_save();
    int x = pos_x;
    int y = pos_y;
    int b = buttons;

    irq_restore(flags);
    if (out_x != 0) {
        *out_x = x;
    }
    if (out_y != 0) {
        *out_y = y;
    }
    if (out_buttons != 0) {
        *out_buttons = b;
    }
}

void mouse_set_position(int x, int y) {
    unsigned long flags = irq_save();

    pos_x = clamp_coord(x, graphics_width());
    pos_y = clamp_coord(y, graphics_height());
    last_dx = 0;
    last_dy = 0;
    pending_dx = 0;
    pending_dy = 0;
    pending_wheel = 0;
    irq_restore(flags);
}

void mouse_consume_motion(int *out_dx, int *out_dy, int *out_buttons) {
    unsigned long flags = irq_save();
    int dx = pending_dx;
    int dy = pending_dy;
    int b = buttons;

    pending_dx = 0;
    pending_dy = 0;
    irq_restore(flags);

    if (out_dx != 0) {
        *out_dx = dx;
    }
    if (out_dy != 0) {
        *out_dy = dy;
    }
    if (out_buttons != 0) {
        *out_buttons = b;
    }
}

int mouse_consume_wheel(void) {
    unsigned long flags = irq_save();
    int wheel = pending_wheel;

    pending_wheel = 0;
    irq_restore(flags);
    return wheel;
}

int mouse_dx(void) {
    return last_dx;
}

int mouse_dy(void) {
    return last_dy;
}

int mouse_wheel(void) {
    return last_wheel;
}

void mouse_debug_info(mouse_debug_info_t *out) {
    unsigned long flags;

    if (out == 0) {
        return;
    }

    flags = irq_save();
    out->enabled = enabled;
    out->ps2_enabled = ps2_enabled;
    out->ps2_has_wheel = ps2_has_wheel;
    out->ps2_packet_size = ps2_packet_size;
    out->last_source = last_source;
    out->x = pos_x;
    out->y = pos_y;
    out->buttons = buttons;
    out->dx = last_dx;
    out->dy = last_dy;
    out->wheel = last_wheel;
    out->pending_wheel = pending_wheel;
    out->ps2_packets = ps2_packet_count;
    out->usb_reports = usb_report_count;
    out->rejected_packets = rejected_packets;
    irq_restore(flags);
}
