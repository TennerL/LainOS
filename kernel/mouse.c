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
#define MOUSE_CMD_ENABLE_STREAMING 0xF4u
#define MOUSE_ACK 0xFAu
#define PIC1_COMMAND 0x20u
#define PIC1_DATA 0x21u
#define PIC2_COMMAND 0xA0u
#define PIC2_DATA 0xA1u
#define PIC_EOI 0x20u
#define PS2_WAIT_LIMIT 100000u

static volatile int enabled;
static volatile int packet_index;
static volatile uint8_t packet[3];
static volatile int pos_x;
static volatile int pos_y;
static volatile int buttons;
static volatile int last_dx;
static volatile int last_dy;

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ __volatile__("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ __volatile__("outb %0, %1" : : "a"(value), "Nd"(port));
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

    if (read_data(&response) != 0 || response != MOUSE_ACK) {
        return -1;
    }

    return 0;
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

static void receive_packet_byte(uint8_t value) {
    if (packet_index == 0 && (value & 0x08u) == 0) {
        return;
    }

    packet[packet_index++] = value;
    if (packet_index < 3) {
        return;
    }

    int dx = sign_extend_byte(packet[1], packet[0] & 0x10u);
    int dy = sign_extend_byte(packet[2], packet[0] & 0x20u);

    buttons = packet[0] & 0x07u;
    last_dx = dx;
    last_dy = dy;
    pos_x = clamp_coord(pos_x + dx, graphics_width());
    pos_y = clamp_coord(pos_y - dy, graphics_height());
    packet_index = 0;
}

void mouse_init(void) {
    uint8_t config;

    enabled = 0;
    packet_index = 0;
    buttons = 0;
    last_dx = 0;
    last_dy = 0;
    pos_x = (int)(graphics_width() / 2u);
    pos_y = (int)(graphics_height() / 2u);

    flush_output();
    if (write_controller(PS2_COMMAND_ENABLE_AUX) != 0) {
        return;
    }

    if (write_controller(PS2_COMMAND_READ_CONFIG) != 0 || read_data(&config) != 0) {
        return;
    }

    config |= PS2_CONFIG_ENABLE_IRQ12;
    config &= (uint8_t)~PS2_CONFIG_DISABLE_AUX_CLOCK;
    if (write_controller(PS2_COMMAND_WRITE_CONFIG) != 0 || write_data(config) != 0) {
        return;
    }

    if (write_mouse(MOUSE_CMD_SET_DEFAULTS) != 0 || write_mouse(MOUSE_CMD_ENABLE_STREAMING) != 0) {
        return;
    }

    enabled = 1;
    unmask_mouse_irq();
}

void mouse_irq_handler(void) {
    uint8_t status = inb(PS2_STATUS_PORT);

    if ((status & PS2_STATUS_OUTPUT_FULL) != 0) {
        uint8_t value = inb(PS2_DATA_PORT);
        if ((status & PS2_STATUS_AUX_DATA) != 0 && enabled) {
            receive_packet_byte(value);
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

int mouse_dx(void) {
    return last_dx;
}

int mouse_dy(void) {
    return last_dy;
}
