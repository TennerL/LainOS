#include "usb.h"
#include "pci.h"

#define USB_MAX_CONTROLLERS 8u
#define USB_MAX_XHCI_SLOTS 32u
#define XHCI_COMMAND_RING_TRBS 64u
#define XHCI_EVENT_RING_TRBS 64u
#define XHCI_TRANSFER_RING_TRBS 32u
#define XHCI_CONTEXT_BYTES 64u
#define USB_DEVICE_DESCRIPTOR_SIZE 18u

#define PCI_CLASS_SERIAL_BUS 0x0Cu
#define PCI_SUBCLASS_USB 0x03u
#define PCI_PROGIF_UHCI 0x00u
#define PCI_PROGIF_OHCI 0x10u
#define PCI_PROGIF_EHCI 0x20u
#define PCI_PROGIF_XHCI 0x30u

#define PCI_COMMAND_IO_SPACE 0x0001u
#define PCI_COMMAND_MEMORY_SPACE 0x0002u
#define PCI_COMMAND_BUS_MASTER 0x0004u

#define PCI_BAR_IO 0x00000001u
#define PCI_BAR_MEM_TYPE_MASK 0x00000006u
#define PCI_BAR_MEM_TYPE_64 0x00000004u

#define XHCI_USBCMD 0x00u
#define XHCI_USBSTS 0x04u
#define XHCI_CRCR 0x18u
#define XHCI_DCBAAP 0x30u
#define XHCI_CONFIG 0x38u
#define XHCI_PORT_REGS 0x400u
#define XHCI_PORT_STRIDE 0x10u

#define XHCI_USBCMD_RUN 0x00000001u
#define XHCI_USBCMD_RESET 0x00000002u
#define XHCI_USBSTS_HALTED 0x00000001u
#define XHCI_USBSTS_CNR 0x00000800u

#define XHCI_PORTSC_CCS 0x00000001u
#define XHCI_PORTSC_RESET 0x00000010u
#define XHCI_PORTSC_POWER 0x00000200u
#define XHCI_PORTSC_CHANGE_BITS 0x00FE0000u

#define XHCI_INTR_IMAN 0x00u
#define XHCI_INTR_ERSTSZ 0x08u
#define XHCI_INTR_ERSTBA 0x10u
#define XHCI_INTR_ERDP 0x18u
#define XHCI_ERDP_EHB 0x00000008ull

#define XHCI_TRB_CYCLE 0x00000001u
#define XHCI_TRB_TYPE_SHIFT 10u
#define XHCI_TRB_TYPE_LINK 6u
#define XHCI_TRB_TYPE_ENABLE_SLOT 9u
#define XHCI_TRB_TYPE_ADDRESS_DEVICE 11u
#define XHCI_TRB_TYPE_TRANSFER_EVENT 32u
#define XHCI_TRB_TYPE_COMMAND_COMPLETION 33u
#define XHCI_TRB_TYPE_SETUP_STAGE 2u
#define XHCI_TRB_TYPE_DATA_STAGE 3u
#define XHCI_TRB_TYPE_STATUS_STAGE 4u
#define XHCI_TRB_LINK_TOGGLE_CYCLE 0x00000002u
#define XHCI_TRB_COMPLETION_SUCCESS 1u
#define XHCI_TRB_IOC 0x00000020u
#define XHCI_TRB_DIR_IN 0x00010000u
#define XHCI_ENDPOINT_CONTROL 4u

typedef struct {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
} xhci_trb_t;

typedef struct {
    uint64_t ring_segment_base;
    uint32_t ring_segment_size;
    uint32_t reserved;
} xhci_erst_entry_t;

static usb_controller_info_t controllers[USB_MAX_CONTROLLERS];
static uint32_t controller_count;
static uint32_t xhci_count;

static uint64_t xhci_dcbaa[USB_MAX_XHCI_SLOTS + 1u] __attribute__((aligned(64)));
static xhci_trb_t xhci_command_ring[XHCI_COMMAND_RING_TRBS] __attribute__((aligned(64)));
static xhci_trb_t xhci_event_ring[XHCI_EVENT_RING_TRBS] __attribute__((aligned(64)));
static xhci_erst_entry_t xhci_erst[1] __attribute__((aligned(64)));
static uint8_t xhci_input_contexts[USB_MAX_XHCI_SLOTS + 1u][XHCI_CONTEXT_BYTES * 33u] __attribute__((aligned(64)));
static uint8_t xhci_device_contexts[USB_MAX_XHCI_SLOTS + 1u][XHCI_CONTEXT_BYTES * 32u] __attribute__((aligned(64)));
static xhci_trb_t xhci_transfer_rings[USB_MAX_XHCI_SLOTS + 1u][XHCI_TRANSFER_RING_TRBS] __attribute__((aligned(64)));
static uint8_t xhci_device_descriptors[USB_MAX_XHCI_SLOTS + 1u][USB_DEVICE_DESCRIPTOR_SIZE] __attribute__((aligned(64)));
static uint32_t xhci_transfer_enqueue[USB_MAX_XHCI_SLOTS + 1u];
static uint32_t xhci_transfer_cycle[USB_MAX_XHCI_SLOTS + 1u];
static uint32_t xhci_command_enqueue;
static uint32_t xhci_command_cycle;
static uint32_t xhci_event_dequeue;
static uint32_t xhci_event_cycle;
static uint32_t xhci_context_size = 32u;

static uint8_t mmio_read8(uint64_t base, uint32_t offset) {
    volatile uint8_t *ptr = (volatile uint8_t *)(uintptr_t)(base + offset);
    return *ptr;
}

static uint16_t mmio_read16(uint64_t base, uint32_t offset) {
    volatile uint16_t *ptr = (volatile uint16_t *)(uintptr_t)(base + offset);
    return *ptr;
}

static uint32_t mmio_read32(uint64_t base, uint32_t offset) {
    volatile uint32_t *ptr = (volatile uint32_t *)(uintptr_t)(base + offset);
    return *ptr;
}

static void mmio_write32(uint64_t base, uint32_t offset, uint32_t value) {
    volatile uint32_t *ptr = (volatile uint32_t *)(uintptr_t)(base + offset);
    *ptr = value;
}

static void mmio_write64(uint64_t base, uint32_t offset, uint64_t value) {
    mmio_write32(base, offset, (uint32_t)(value & 0xFFFFFFFFull));
    mmio_write32(base, offset + 4u, (uint32_t)(value >> 32));
}

static uint64_t phys_addr(const void *ptr) {
    return (uint64_t)(uintptr_t)ptr;
}

static void zero_bytes(void *ptr, uint32_t size) {
    uint8_t *p = (uint8_t *)ptr;
    for (uint32_t i = 0; i < size; ++i) {
        p[i] = 0;
    }
}

static void xhci_wait(uint32_t iterations) {
    for (uint32_t i = 0; i < iterations; ++i) {
        __asm__ __volatile__("pause");
    }
}

static int wait_bits_set(uint64_t base, uint32_t offset, uint32_t bits, uint32_t timeout) {
    for (uint32_t i = 0; i < timeout; ++i) {
        if ((mmio_read32(base, offset) & bits) == bits) {
            return 0;
        }
    }
    return -1;
}

static int wait_bits_clear(uint64_t base, uint32_t offset, uint32_t bits, uint32_t timeout) {
    for (uint32_t i = 0; i < timeout; ++i) {
        if ((mmio_read32(base, offset) & bits) == 0) {
            return 0;
        }
    }
    return -1;
}

static usb_controller_type_t controller_type_from_prog_if(uint8_t prog_if) {
    if (prog_if == PCI_PROGIF_UHCI) {
        return USB_CONTROLLER_UHCI;
    }
    if (prog_if == PCI_PROGIF_OHCI) {
        return USB_CONTROLLER_OHCI;
    }
    if (prog_if == PCI_PROGIF_EHCI) {
        return USB_CONTROLLER_EHCI;
    }
    if (prog_if == PCI_PROGIF_XHCI) {
        return USB_CONTROLLER_XHCI;
    }
    return USB_CONTROLLER_UNKNOWN;
}

const char *usb_controller_type_name(usb_controller_type_t type) {
    switch (type) {
        case USB_CONTROLLER_UHCI: return "UHCI";
        case USB_CONTROLLER_OHCI: return "OHCI";
        case USB_CONTROLLER_EHCI: return "EHCI";
        case USB_CONTROLLER_XHCI: return "xHCI";
        default: return "USB";
    }
}

static uint64_t read_bar0(uint8_t bus,
                          uint8_t device,
                          uint8_t function,
                          uint32_t *is_io) {
    uint32_t bar0 = pci_read_config32(bus, device, function, 0x10);

    *is_io = (bar0 & PCI_BAR_IO) ? 1u : 0u;
    if (*is_io) {
        return (uint64_t)(bar0 & 0xFFFFFFFCu);
    }

    if ((bar0 & PCI_BAR_MEM_TYPE_MASK) == PCI_BAR_MEM_TYPE_64) {
        uint32_t bar1 = pci_read_config32(bus, device, function, 0x14);
        return (((uint64_t)bar1) << 32) | (uint64_t)(bar0 & 0xFFFFFFF0u);
    }

    return (uint64_t)(bar0 & 0xFFFFFFF0u);
}

static void probe_xhci(usb_controller_info_t *info) {
    uint32_t hcsparams1;
    uint32_t hccparams1;

    if (info->bar0 == 0 || info->bar0_is_io) {
        return;
    }

    if (mmio_read8(info->bar0, 0x00) == 0) {
        return;
    }

    info->hci_version = mmio_read16(info->bar0, 0x02);
    hcsparams1 = mmio_read32(info->bar0, 0x04);
    info->max_slots = hcsparams1 & 0xFFu;
    info->interrupter_count = (hcsparams1 >> 8) & 0x7FFu;
    info->port_count = (hcsparams1 >> 24) & 0xFFu;
    hccparams1 = mmio_read32(info->bar0, 0x10);
    xhci_context_size = (hccparams1 & 0x04u) ? 64u : 32u;
}

static uint32_t trb_type(const xhci_trb_t *trb) {
    return (trb->control >> XHCI_TRB_TYPE_SHIFT) & 0x3Fu;
}

static uint32_t trb_cycle(const xhci_trb_t *trb) {
    return trb->control & XHCI_TRB_CYCLE;
}

static uint64_t xhci_runtime_base(const usb_controller_info_t *info) {
    return info->bar0 + (uint64_t)(mmio_read32(info->bar0, 0x18) & 0xFFFFFFE0u);
}

static uint64_t xhci_doorbell_base(const usb_controller_info_t *info) {
    return info->bar0 + (uint64_t)(mmio_read32(info->bar0, 0x14) & 0xFFFFFFFCu);
}

static uint64_t xhci_operational_base(const usb_controller_info_t *info) {
    return info->bar0 + (uint64_t)mmio_read8(info->bar0, 0x00);
}

static void xhci_setup_command_ring(uint64_t opbase) {
    zero_bytes(xhci_command_ring, sizeof(xhci_command_ring));
    xhci_command_enqueue = 0;
    xhci_command_cycle = 1;

    xhci_command_ring[XHCI_COMMAND_RING_TRBS - 1u].parameter = phys_addr(xhci_command_ring);
    xhci_command_ring[XHCI_COMMAND_RING_TRBS - 1u].status = 0;
    xhci_command_ring[XHCI_COMMAND_RING_TRBS - 1u].control =
        (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) |
        XHCI_TRB_LINK_TOGGLE_CYCLE |
        XHCI_TRB_CYCLE;

    mmio_write64(opbase, XHCI_CRCR, phys_addr(xhci_command_ring) | 1ull);
}

static void xhci_setup_event_ring(const usb_controller_info_t *info) {
    uint64_t rtbase = xhci_runtime_base(info);
    uint64_t intr0 = rtbase + 0x20ull;

    zero_bytes(xhci_event_ring, sizeof(xhci_event_ring));
    zero_bytes(xhci_erst, sizeof(xhci_erst));

    xhci_event_dequeue = 0;
    xhci_event_cycle = 1;

    xhci_erst[0].ring_segment_base = phys_addr(xhci_event_ring);
    xhci_erst[0].ring_segment_size = XHCI_EVENT_RING_TRBS;
    xhci_erst[0].reserved = 0;

    mmio_write32(intr0, XHCI_INTR_ERSTSZ, 1);
    mmio_write64(intr0, XHCI_INTR_ERSTBA, phys_addr(xhci_erst));
    mmio_write64(intr0, XHCI_INTR_ERDP, phys_addr(xhci_event_ring) | XHCI_ERDP_EHB);
    mmio_write32(intr0, XHCI_INTR_IMAN, 0x00000002u);
}

static int xhci_next_event(xhci_trb_t *out) {
    xhci_trb_t *event = &xhci_event_ring[xhci_event_dequeue];

    if (trb_cycle(event) != xhci_event_cycle) {
        return 0;
    }

    *out = *event;
    ++xhci_event_dequeue;
    if (xhci_event_dequeue >= XHCI_EVENT_RING_TRBS) {
        xhci_event_dequeue = 0;
        xhci_event_cycle ^= 1u;
    }

    return 1;
}

static void xhci_update_erdp(const usb_controller_info_t *info) {
    uint64_t intr0 = xhci_runtime_base(info) + 0x20ull;
    mmio_write64(intr0,
                 XHCI_INTR_ERDP,
                 phys_addr(&xhci_event_ring[xhci_event_dequeue]) | XHCI_ERDP_EHB);
}

static int xhci_ring_command(const usb_controller_info_t *info,
                             uint64_t parameter,
                             uint32_t status,
                             uint32_t control,
                             uint32_t *slot_id,
                             uint32_t *completion_code) {
    xhci_trb_t *cmd;
    uint64_t dbbase;

    if (xhci_command_enqueue >= XHCI_COMMAND_RING_TRBS - 1u) {
        xhci_command_enqueue = 0;
        xhci_command_cycle ^= 1u;
    }

    cmd = &xhci_command_ring[xhci_command_enqueue++];
    cmd->parameter = parameter;
    cmd->status = status;
    cmd->control = control | (xhci_command_cycle ? XHCI_TRB_CYCLE : 0u);

    dbbase = xhci_doorbell_base(info);
    mmio_write32(dbbase, 0, 0);

    for (uint32_t i = 0; i < 10000000u; ++i) {
        xhci_trb_t event;

        if (!xhci_next_event(&event)) {
            continue;
        }

        xhci_update_erdp(info);
        if (trb_type(&event) != XHCI_TRB_TYPE_COMMAND_COMPLETION) {
            continue;
        }

        if (completion_code) {
            *completion_code = event.status >> 24;
        }
        if (slot_id) {
            *slot_id = event.control >> 24;
        }
        return ((event.status >> 24) == XHCI_TRB_COMPLETION_SUCCESS) ? 0 : -1;
    }

    return -1;
}

static uint32_t *xhci_context_dword(uint8_t *base, uint32_t context_index, uint32_t dword_index) {
    return (uint32_t *)(void *)(base + context_index * xhci_context_size + dword_index * 4u);
}

static uint32_t xhci_port_speed(uint32_t portsc) {
    return (portsc >> 10) & 0x0Fu;
}

static uint32_t xhci_default_control_packet_size(uint32_t speed) {
    if (speed >= 4u) {
        return 512u;
    }
    if (speed == 3u) {
        return 64u;
    }
    return 8u;
}

static void xhci_setup_transfer_ring(uint32_t slot_id) {
    xhci_trb_t *ring = xhci_transfer_rings[slot_id];

    zero_bytes(ring, sizeof(xhci_transfer_rings[slot_id]));
    xhci_transfer_enqueue[slot_id] = 0;
    xhci_transfer_cycle[slot_id] = 1;

    ring[XHCI_TRANSFER_RING_TRBS - 1u].parameter = phys_addr(ring);
    ring[XHCI_TRANSFER_RING_TRBS - 1u].status = 0;
    ring[XHCI_TRANSFER_RING_TRBS - 1u].control =
        (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) |
        XHCI_TRB_LINK_TOGGLE_CYCLE |
        XHCI_TRB_CYCLE;
}

static void xhci_enqueue_transfer_trb(uint32_t slot_id,
                                      uint64_t parameter,
                                      uint32_t status,
                                      uint32_t control) {
    xhci_trb_t *ring = xhci_transfer_rings[slot_id];
    xhci_trb_t *trb;

    if (xhci_transfer_enqueue[slot_id] >= XHCI_TRANSFER_RING_TRBS - 1u) {
        xhci_transfer_enqueue[slot_id] = 0;
        xhci_transfer_cycle[slot_id] ^= 1u;
    }

    trb = &ring[xhci_transfer_enqueue[slot_id]++];
    trb->parameter = parameter;
    trb->status = status;
    trb->control = control | (xhci_transfer_cycle[slot_id] ? XHCI_TRB_CYCLE : 0u);
}

static int xhci_wait_transfer_event(const usb_controller_info_t *info,
                                    uint32_t slot_id,
                                    uint32_t *completion_code) {
    for (uint32_t i = 0; i < 10000000u; ++i) {
        xhci_trb_t event;

        if (!xhci_next_event(&event)) {
            continue;
        }

        xhci_update_erdp(info);
        if (trb_type(&event) != XHCI_TRB_TYPE_TRANSFER_EVENT ||
            (event.control >> 24) != slot_id) {
            continue;
        }

        if (completion_code) {
            *completion_code = event.status >> 24;
        }
        return ((event.status >> 24) == XHCI_TRB_COMPLETION_SUCCESS) ? 0 : -1;
    }

    return -1;
}

static int xhci_address_device(usb_controller_info_t *info,
                               uint32_t slot_id,
                               uint32_t port_index,
                               uint32_t speed) {
    uint8_t *input;
    uint8_t *device;
    uint32_t *ctx;
    uint32_t completion = 0;
    uint32_t packet_size;

    if (slot_id == 0 || slot_id > USB_MAX_XHCI_SLOTS) {
        return -1;
    }

    input = xhci_input_contexts[slot_id];
    device = xhci_device_contexts[slot_id];
    zero_bytes(input, sizeof(xhci_input_contexts[slot_id]));
    zero_bytes(device, sizeof(xhci_device_contexts[slot_id]));
    xhci_setup_transfer_ring(slot_id);

    xhci_dcbaa[slot_id] = phys_addr(device);

    ctx = xhci_context_dword(input, 0, 1);
    *ctx = 0x00000003u;

    ctx = xhci_context_dword(input, 1, 0);
    *ctx = (speed << 20) | (1u << 27);
    ctx = xhci_context_dword(input, 1, 2);
    *ctx = ((port_index + 1u) << 16);

    packet_size = xhci_default_control_packet_size(speed);
    ctx = xhci_context_dword(input, 2, 1);
    *ctx = (3u << 1) | (XHCI_ENDPOINT_CONTROL << 3) | (packet_size << 16);
    ctx = xhci_context_dword(input, 2, 2);
    *ctx = (uint32_t)((phys_addr(xhci_transfer_rings[slot_id]) | 1ull) & 0xFFFFFFFFull);
    ctx = xhci_context_dword(input, 2, 3);
    *ctx = (uint32_t)((phys_addr(xhci_transfer_rings[slot_id]) | 1ull) >> 32);
    ctx = xhci_context_dword(input, 2, 4);
    *ctx = 8u;

    if (xhci_ring_command(info,
                          phys_addr(input),
                          0,
                          (XHCI_TRB_TYPE_ADDRESS_DEVICE << XHCI_TRB_TYPE_SHIFT) |
                          (slot_id << 24),
                          0,
                          &completion) != 0) {
        info->last_completion_code = completion;
        return -1;
    }

    info->last_completion_code = completion;
    ++info->addressed_device_count;
    return 0;
}

static int xhci_get_device_descriptor(usb_controller_info_t *info, uint32_t slot_id) {
    uint64_t setup = 0x0012000001000680ull;
    uint32_t completion = 0;

    if (slot_id == 0 || slot_id > USB_MAX_XHCI_SLOTS) {
        return -1;
    }

    zero_bytes(xhci_device_descriptors[slot_id], USB_DEVICE_DESCRIPTOR_SIZE);
    xhci_enqueue_transfer_trb(slot_id,
                              setup,
                              8u,
                              (XHCI_TRB_TYPE_SETUP_STAGE << XHCI_TRB_TYPE_SHIFT) |
                              (3u << 16));
    xhci_enqueue_transfer_trb(slot_id,
                              phys_addr(xhci_device_descriptors[slot_id]),
                              USB_DEVICE_DESCRIPTOR_SIZE,
                              (XHCI_TRB_TYPE_DATA_STAGE << XHCI_TRB_TYPE_SHIFT) |
                              XHCI_TRB_DIR_IN);
    xhci_enqueue_transfer_trb(slot_id,
                              0,
                              0,
                              (XHCI_TRB_TYPE_STATUS_STAGE << XHCI_TRB_TYPE_SHIFT) |
                              XHCI_TRB_IOC);
    mmio_write32(xhci_doorbell_base(info), slot_id * 4u, 1u);

    if (xhci_wait_transfer_event(info, slot_id, &completion) != 0) {
        info->last_completion_code = completion;
        return -1;
    }

    info->last_completion_code = completion;
    ++info->descriptor_count;
    return 0;
}

static void xhci_reset_connected_ports(usb_controller_info_t *info, uint64_t opbase) {
    uint32_t max_ports = info->port_count;

    if (max_ports > 255u) {
        max_ports = 255u;
    }

    for (uint32_t port = 0; port < max_ports; ++port) {
        uint32_t offset = XHCI_PORT_REGS + port * XHCI_PORT_STRIDE;
        uint32_t portsc = mmio_read32(opbase, offset);
        uint32_t speed;
        uint32_t slot_id = 0;
        uint32_t completion = 0;

        if ((portsc & XHCI_PORTSC_CCS) == 0) {
            continue;
        }

        ++info->connected_port_count;
        mmio_write32(opbase,
                     offset,
                     (portsc & ~XHCI_PORTSC_CHANGE_BITS) |
                     XHCI_PORTSC_POWER |
                     XHCI_PORTSC_RESET);
        xhci_wait(1000000u);
        (void)wait_bits_clear(opbase, offset, XHCI_PORTSC_RESET, 10000000u);
        portsc = mmio_read32(opbase, offset);
        mmio_write32(opbase, offset, portsc | XHCI_PORTSC_CHANGE_BITS);
        ++info->reset_port_count;

        speed = xhci_port_speed(mmio_read32(opbase, offset));
        if (xhci_ring_command(info,
                              0,
                              0,
                              XHCI_TRB_TYPE_ENABLE_SLOT << XHCI_TRB_TYPE_SHIFT,
                              &slot_id,
                              &completion) == 0 &&
            slot_id != 0) {
            ++info->enabled_slot_count;
            if (xhci_address_device(info, slot_id, port, speed) == 0) {
                (void)xhci_get_device_descriptor(info, slot_id);
            }
        }
        info->last_completion_code = completion;
    }
}

static void init_xhci(usb_controller_info_t *info) {
    uint64_t opbase;
    uint32_t slots;

    if (info->bar0 == 0 || info->bar0_is_io || info->max_slots == 0) {
        return;
    }

    opbase = xhci_operational_base(info);
    mmio_write32(opbase, XHCI_USBCMD, mmio_read32(opbase, XHCI_USBCMD) & ~XHCI_USBCMD_RUN);
    if (wait_bits_set(opbase, XHCI_USBSTS, XHCI_USBSTS_HALTED, 10000000u) != 0) {
        return;
    }

    mmio_write32(opbase, XHCI_USBCMD, mmio_read32(opbase, XHCI_USBCMD) | XHCI_USBCMD_RESET);
    if (wait_bits_clear(opbase, XHCI_USBCMD, XHCI_USBCMD_RESET, 10000000u) != 0 ||
        wait_bits_clear(opbase, XHCI_USBSTS, XHCI_USBSTS_CNR, 10000000u) != 0) {
        return;
    }

    zero_bytes(xhci_dcbaa, sizeof(xhci_dcbaa));
    slots = info->max_slots;
    if (slots > USB_MAX_XHCI_SLOTS) {
        slots = USB_MAX_XHCI_SLOTS;
    }
    mmio_write32(opbase, XHCI_CONFIG, slots);
    mmio_write64(opbase, XHCI_DCBAAP, phys_addr(xhci_dcbaa));

    xhci_setup_command_ring(opbase);
    xhci_setup_event_ring(info);

    mmio_write32(opbase, XHCI_USBCMD, mmio_read32(opbase, XHCI_USBCMD) | XHCI_USBCMD_RUN);
    if (wait_bits_clear(opbase, XHCI_USBSTS, XHCI_USBSTS_HALTED, 10000000u) != 0) {
        return;
    }

    info->running = 1;
    xhci_reset_connected_ports(info, opbase);
    info->initialized = 1;
}

static void visit_pci(uint8_t bus, uint8_t device, uint8_t function, void *ctx) {
    usb_controller_info_t *info;
    uint8_t class_code = pci_read_config8(bus, device, function, 0x0B);
    uint8_t subclass = pci_read_config8(bus, device, function, 0x0A);
    uint8_t prog_if = pci_read_config8(bus, device, function, 0x09);
    uint16_t command;

    (void)ctx;

    if (class_code != PCI_CLASS_SERIAL_BUS || subclass != PCI_SUBCLASS_USB) {
        return;
    }
    if (controller_count >= USB_MAX_CONTROLLERS) {
        return;
    }

    info = &controllers[controller_count];
    info->bus = bus;
    info->device = device;
    info->function = function;
    info->prog_if = prog_if;
    info->vendor_id = pci_read_config16(bus, device, function, 0x00);
    info->device_id = pci_read_config16(bus, device, function, 0x02);
    info->type = controller_type_from_prog_if(prog_if);
    info->hci_version = 0;
    info->max_slots = 0;
    info->interrupter_count = 0;
    info->port_count = 0;
    info->initialized = 0;
    info->running = 0;
    info->connected_port_count = 0;
    info->reset_port_count = 0;
    info->enabled_slot_count = 0;
    info->addressed_device_count = 0;
    info->descriptor_count = 0;
    info->last_completion_code = 0;
    info->bar0 = read_bar0(bus, device, function, &info->bar0_is_io);

    command = pci_read_config16(bus, device, function, 0x04);
    command |= PCI_COMMAND_IO_SPACE | PCI_COMMAND_MEMORY_SPACE | PCI_COMMAND_BUS_MASTER;
    pci_write_config32(bus,
                       device,
                       function,
                       0x04,
                       (pci_read_config32(bus, device, function, 0x04) & 0xFFFF0000u) |
                       command);

    if (info->type == USB_CONTROLLER_XHCI) {
        ++xhci_count;
        probe_xhci(info);
        init_xhci(info);
    }

    ++controller_count;
}

void usb_init(void) {
    controller_count = 0;
    xhci_count = 0;
    pci_scan(visit_pci, 0);
}

uint32_t usb_controller_count(void) {
    return controller_count;
}

uint32_t usb_xhci_controller_count(void) {
    return xhci_count;
}

const usb_controller_info_t *usb_controller_info(uint32_t index) {
    if (index >= controller_count) {
        return 0;
    }
    return &controllers[index];
}
