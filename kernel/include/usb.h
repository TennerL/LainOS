#ifndef USB_H
#define USB_H

#include <stdint.h>

typedef enum {
    USB_CONTROLLER_UHCI = 0,
    USB_CONTROLLER_OHCI = 1,
    USB_CONTROLLER_EHCI = 2,
    USB_CONTROLLER_XHCI = 3,
    USB_CONTROLLER_UNKNOWN = 4,
} usb_controller_type_t;

typedef struct {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint8_t prog_if;
    uint16_t vendor_id;
    uint16_t device_id;
    uint64_t bar0;
    uint32_t bar0_is_io;
    usb_controller_type_t type;
    uint32_t hci_version;
    uint32_t max_slots;
    uint32_t interrupter_count;
    uint32_t port_count;
    uint32_t initialized;
    uint32_t running;
    uint32_t connected_port_count;
    uint32_t reset_port_count;
    uint32_t enabled_slot_count;
    uint32_t addressed_device_count;
    uint32_t descriptor_count;
    uint32_t last_completion_code;
} usb_controller_info_t;

void usb_init(void);
uint32_t usb_controller_count(void);
uint32_t usb_xhci_controller_count(void);
const usb_controller_info_t *usb_controller_info(uint32_t index);
const char *usb_controller_type_name(usb_controller_type_t type);

#endif
