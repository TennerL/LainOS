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
    uint32_t scratchpad_count;
    uint32_t page_size;
    uint32_t hcsparams2;
    uint32_t hccparams1;
    uint64_t dcbaa_phys;
    uint64_t command_ring_phys;
    uint64_t event_ring_phys;
    uint64_t erst_phys;
    uint32_t initialized;
    uint32_t running;
    uint32_t connected_port_count;
    uint32_t reset_port_count;
    uint32_t enabled_slot_count;
    uint32_t addressed_device_count;
    uint32_t descriptor_count;
    uint32_t last_completion_code;
    uint32_t mouse_configured;
    uint32_t mouse_slot;
    uint32_t mouse_dci;
    uint32_t mouse_report_size;
    uint32_t mouse_pending;
    uint32_t mouse_report_count;
    uint32_t mouse_last_completion_code;
    uint32_t mouse_stage;
    uint32_t mouse_interface;
    uint32_t mouse_endpoint;
    uint32_t mouse_interval;
    uint32_t enum_stage;
    uint32_t enum_port;
    uint32_t enum_portsc;
    uint32_t enum_speed;
    uint32_t enum_slot;
    uint32_t enum_completion_code;
} usb_controller_info_t;

void usb_init(void);
int usb_xhci_init_controller(uint32_t index);
int usb_xhci_handoff_controller(uint32_t index);
int usb_xhci_halt_controller(uint32_t index);
int usb_xhci_reset_controller(uint32_t index);
int usb_xhci_setup_rings(uint32_t index);
int usb_xhci_busmaster_controller(uint32_t index);
int usb_xhci_no_busmaster_controller(uint32_t index);
int usb_xhci_run_controller(uint32_t index);
int usb_xhci_poke_run_controller(uint32_t index);
int usb_xhci_poke_no_dma_controller(uint32_t index);
int usb_xhci_status_controller(uint32_t index, uint32_t *usbcmd, uint32_t *usbsts);
int usb_xhci_start_controller(uint32_t index);
int usb_xhci_enumerate_controller(uint32_t index);
int usb_xhci_scan_ports(uint32_t index);
void usb_poll(void);
uint32_t usb_controller_count(void);
uint32_t usb_xhci_controller_count(void);
const usb_controller_info_t *usb_controller_info(uint32_t index);
const char *usb_controller_type_name(usb_controller_type_t type);

#endif
