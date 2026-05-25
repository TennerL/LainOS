#include "usb.h"
#include "keyboard.h"
#include "kernel.h"
#include "mouse.h"
#include "pci.h"

#define USB_MAX_CONTROLLERS 8u
#define USB_MAX_XHCI_SLOTS 32u
#define USB_MAX_XHCI_SCRATCHPADS 128u
#define XHCI_COMMAND_RING_TRBS 64u
#define XHCI_EVENT_RING_TRBS 64u
#define XHCI_TRANSFER_RING_TRBS 32u
#define XHCI_CONTEXT_BYTES 64u
#define XHCI_PAGE_SIZE 4096u
#define USB_DEVICE_DESCRIPTOR_SIZE 18u
#define USB_CONFIG_DESCRIPTOR_CAPACITY 1024u
#define USB_MOUSE_REPORT_SIZE 16u
#define USB_KEYBOARD_REPORT_SIZE 8u
#define USB_HID_REPORT_DESCRIPTOR_CAPACITY 256u
#define USB_ENDPOINT_MAX_PACKET_MASK 0x07FFu
#define USB_LATE_ENUM_INITIAL_DELAY_SECONDS 8u
#define USB_LATE_ENUM_RETRY_SECONDS 10u
#define USB_LATE_ENUM_MAX_ATTEMPTS 6u

#define PCI_CLASS_SERIAL_BUS 0x0Cu
#define PCI_SUBCLASS_USB 0x03u
#define PCI_PROGIF_UHCI 0x00u
#define PCI_PROGIF_OHCI 0x10u
#define PCI_PROGIF_EHCI 0x20u
#define PCI_PROGIF_XHCI 0x30u

#define PCI_COMMAND_IO_SPACE 0x0001u
#define PCI_COMMAND_MEMORY_SPACE 0x0002u
#define PCI_COMMAND_BUS_MASTER 0x0004u
#define PCI_COMMAND_INTERRUPT_DISABLE 0x0400u

#define PCI_BAR_IO 0x00000001u
#define PCI_BAR_MEM_TYPE_MASK 0x00000006u
#define PCI_BAR_MEM_TYPE_64 0x00000004u

#define XHCI_USBCMD 0x00u
#define XHCI_USBSTS 0x04u
#define XHCI_PAGESIZE 0x08u
#define XHCI_CRCR 0x18u
#define XHCI_DCBAAP 0x30u
#define XHCI_CONFIG 0x38u
#define XHCI_PORT_REGS 0x400u
#define XHCI_PORT_STRIDE 0x10u
#define XHCI_HCCPARAMS1 0x10u

#define XHCI_USBCMD_RUN 0x00000001u
#define XHCI_USBCMD_RESET 0x00000002u
#define XHCI_USBCMD_INTE 0x00000004u
#define XHCI_USBSTS_HALTED 0x00000001u
#define XHCI_USBSTS_HSE 0x00000004u
#define XHCI_USBSTS_EINT 0x00000008u
#define XHCI_USBSTS_PCD 0x00000010u
#define XHCI_USBSTS_SRE 0x00000400u
#define XHCI_USBSTS_CNR 0x00000800u
#define XHCI_USBSTS_CLEAR_BITS (XHCI_USBSTS_HSE | XHCI_USBSTS_EINT | XHCI_USBSTS_PCD | XHCI_USBSTS_SRE)

#define XHCI_PORTSC_CCS 0x00000001u
#define XHCI_PORTSC_PED 0x00000002u
#define XHCI_PORTSC_RESET 0x00000010u
#define XHCI_PORTSC_POWER 0x00000200u
#define XHCI_PORTSC_CHANGE_BITS 0x00FE0000u
#define XHCI_PORTSC_WRITE_1_CLEAR_BITS (XHCI_PORTSC_PED | XHCI_PORTSC_CHANGE_BITS)

#define XHCI_INTR_IMAN 0x00u
#define XHCI_INTR_ERSTSZ 0x08u
#define XHCI_INTR_ERSTBA 0x10u
#define XHCI_INTR_ERDP 0x18u
#define XHCI_INTR_IMAN_IP 0x00000001u
#define XHCI_INTR_IMAN_IE 0x00000002u
#define XHCI_ERDP_EHB 0x00000008ull

#define XHCI_TRB_CYCLE 0x00000001u
#define XHCI_TRB_TYPE_SHIFT 10u
#define XHCI_TRB_TYPE_NORMAL 1u
#define XHCI_TRB_TYPE_LINK 6u
#define XHCI_TRB_TYPE_ENABLE_SLOT 9u
#define XHCI_TRB_TYPE_ADDRESS_DEVICE 11u
#define XHCI_TRB_TYPE_CONFIGURE_ENDPOINT 12u
#define XHCI_TRB_TYPE_TRANSFER_EVENT 32u
#define XHCI_TRB_TYPE_COMMAND_COMPLETION 33u
#define XHCI_TRB_TYPE_SETUP_STAGE 2u
#define XHCI_TRB_TYPE_DATA_STAGE 3u
#define XHCI_TRB_TYPE_STATUS_STAGE 4u
#define XHCI_TRB_LINK_TOGGLE_CYCLE 0x00000002u
#define XHCI_TRB_COMPLETION_SUCCESS 1u
#define XHCI_TRB_COMPLETION_SHORT_PACKET 13u
#define XHCI_TRB_TRANSFER_LENGTH_MASK 0x00FFFFFFu
#define XHCI_TRB_IOC 0x00000020u
#define XHCI_TRB_IDT 0x00000040u
#define XHCI_TRB_DIR_IN 0x00010000u
#define XHCI_ENDPOINT_CONTROL 4u
#define XHCI_ENDPOINT_INTERRUPT_IN 7u
#define XHCI_EXT_CAP_ID_LEGACY 1u
#define XHCI_LEGSUP_BIOS_OWNED 0x00010000u
#define XHCI_LEGSUP_OS_OWNED 0x01000000u

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
static uint64_t xhci_scratchpad_array[USB_MAX_XHCI_SCRATCHPADS] __attribute__((aligned(XHCI_PAGE_SIZE)));
static uint8_t xhci_scratchpad_buffers[USB_MAX_XHCI_SCRATCHPADS][XHCI_PAGE_SIZE] __attribute__((aligned(XHCI_PAGE_SIZE)));
static xhci_trb_t xhci_command_ring[XHCI_COMMAND_RING_TRBS] __attribute__((aligned(64)));
static xhci_trb_t xhci_event_ring[XHCI_EVENT_RING_TRBS] __attribute__((aligned(64)));
static xhci_erst_entry_t xhci_erst[1] __attribute__((aligned(64)));
static uint8_t xhci_input_contexts[USB_MAX_XHCI_SLOTS + 1u][XHCI_CONTEXT_BYTES * 33u] __attribute__((aligned(64)));
static uint8_t xhci_device_contexts[USB_MAX_XHCI_SLOTS + 1u][XHCI_CONTEXT_BYTES * 32u] __attribute__((aligned(64)));
static xhci_trb_t xhci_transfer_rings[USB_MAX_XHCI_SLOTS + 1u][XHCI_TRANSFER_RING_TRBS] __attribute__((aligned(64)));
static uint8_t xhci_device_descriptors[USB_MAX_XHCI_SLOTS + 1u][USB_DEVICE_DESCRIPTOR_SIZE] __attribute__((aligned(64)));
static uint8_t xhci_config_descriptor[USB_CONFIG_DESCRIPTOR_CAPACITY] __attribute__((aligned(64)));
static uint8_t xhci_hid_report_descriptor[USB_HID_REPORT_DESCRIPTOR_CAPACITY] __attribute__((aligned(64)));
static uint8_t xhci_mouse_report[USB_MOUSE_REPORT_SIZE] __attribute__((aligned(64)));
static uint8_t xhci_keyboard_report[USB_KEYBOARD_REPORT_SIZE] __attribute__((aligned(64)));
static uint32_t xhci_transfer_enqueue[USB_MAX_XHCI_SLOTS + 1u];
static uint32_t xhci_transfer_cycle[USB_MAX_XHCI_SLOTS + 1u];
static uint32_t xhci_transfer_link_update_pending[USB_MAX_XHCI_SLOTS + 1u];
static uint32_t xhci_command_enqueue;
static uint32_t xhci_command_cycle;
static uint32_t xhci_command_link_update_pending;
static uint32_t xhci_event_dequeue;
static uint32_t xhci_event_cycle;
static uint32_t xhci_context_size = 32u;
static usb_controller_info_t *xhci_mouse_controller;
static uint32_t xhci_mouse_slot;
static uint32_t xhci_mouse_dci;
static uint32_t xhci_mouse_report_size;
static uint32_t xhci_mouse_pending;
static usb_controller_info_t *xhci_keyboard_controller;
static uint32_t xhci_keyboard_slot;
static uint32_t xhci_keyboard_dci;
static uint32_t xhci_keyboard_report_size;
static uint32_t xhci_keyboard_pending;
static uint32_t xhci_late_enum_attempts;
static unsigned long long xhci_next_late_enum_tick;

static uint64_t xhci_operational_base(const usb_controller_info_t *info);
static uint64_t xhci_doorbell_base(const usb_controller_info_t *info);

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

static uint64_t mmio_read64(uint64_t base, uint32_t offset) {
    uint64_t lo = mmio_read32(base, offset);
    uint64_t hi = mmio_read32(base, offset + 4u);
    return lo | (hi << 32);
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

static void dma_write_barrier(void) {
    __asm__ __volatile__("mfence" ::: "memory");
}

static void xhci_ring_doorbell(const usb_controller_info_t *info, uint32_t slot_id, uint32_t target) {
    uint64_t dbbase = xhci_doorbell_base(info);

    dma_write_barrier();
    mmio_write32(dbbase, slot_id * 4u, target);
    (void)mmio_read32(dbbase, slot_id * 4u);
}

static void xhci_wait(uint32_t iterations) {
    for (uint32_t i = 0; i < iterations; ++i) {
        __asm__ __volatile__("pause");
    }
}

static void xhci_wait_ms(uint32_t milliseconds) {
    unsigned int hz = timer_frequency();
    unsigned long long start;
    unsigned long long ticks;

    if (milliseconds == 0u) {
        return;
    }
    if (hz == 0u) {
        xhci_wait(milliseconds * 100000u);
        return;
    }

    ticks = ((unsigned long long)milliseconds * hz + 999ull) / 1000ull;
    if (ticks == 0ull) {
        ticks = 1ull;
    }

    start = timer_ticks();
    while (timer_ticks() - start < ticks) {
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
    uint32_t hcsparams2;
    uint32_t hccparams1;
    uint32_t page_size_bits;

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
    hcsparams2 = mmio_read32(info->bar0, 0x08);
    info->hcsparams2 = hcsparams2;
    info->scratchpad_count = (((hcsparams2 >> 21) & 0x1Fu) << 5) |
                             ((hcsparams2 >> 27) & 0x1Fu);
    hccparams1 = mmio_read32(info->bar0, 0x10);
    info->hccparams1 = hccparams1;
    xhci_context_size = (hccparams1 & 0x04u) ? 64u : 32u;

    page_size_bits = mmio_read32(xhci_operational_base(info), XHCI_PAGESIZE);
    info->page_size = 0;
    for (uint32_t i = 0; i < 16u; ++i) {
        if ((page_size_bits & (1u << i)) != 0u) {
            info->page_size = XHCI_PAGE_SIZE << i;
            break;
        }
    }
    if (info->page_size == 0u) {
        info->page_size = XHCI_PAGE_SIZE;
    }
}

static uint32_t xhci_first_extended_capability(const usb_controller_info_t *info) {
    return ((mmio_read32(info->bar0, XHCI_HCCPARAMS1) >> 16) & 0xFFFFu) * 4u;
}

static uint32_t xhci_next_extended_capability(const usb_controller_info_t *info, uint32_t offset) {
    uint32_t next = ((mmio_read32(info->bar0, offset) >> 8) & 0xFFu) * 4u;

    return next == 0u ? 0u : offset + next;
}

static int xhci_legacy_handoff(usb_controller_info_t *info) {
    uint32_t offset;

    if (info == 0 || info->type != USB_CONTROLLER_XHCI || info->bar0 == 0 || info->bar0_is_io) {
        return -1;
    }

    offset = xhci_first_extended_capability(info);
    for (uint32_t guard = 0; offset != 0u && guard < 64u; ++guard) {
        uint32_t cap = mmio_read32(info->bar0, offset);
        uint32_t cap_id = cap & 0xFFu;

        if (cap_id == XHCI_EXT_CAP_ID_LEGACY) {
            uint32_t legsup = cap;

            if ((legsup & XHCI_LEGSUP_BIOS_OWNED) != 0u) {
                mmio_write32(info->bar0, offset, legsup | XHCI_LEGSUP_OS_OWNED);
                for (uint32_t i = 0; i < 1000000u; ++i) {
                    legsup = mmio_read32(info->bar0, offset);
                    if ((legsup & XHCI_LEGSUP_BIOS_OWNED) == 0u &&
                        (legsup & XHCI_LEGSUP_OS_OWNED) != 0u) {
                        break;
                    }
                }
            } else if ((legsup & XHCI_LEGSUP_OS_OWNED) == 0u) {
                mmio_write32(info->bar0, offset, legsup | XHCI_LEGSUP_OS_OWNED);
            }

            mmio_write32(info->bar0, offset + 4u, 0u);
            legsup = mmio_read32(info->bar0, offset);
            info->last_completion_code = (legsup >> 16) & 0xFFFFu;
            return ((legsup & XHCI_LEGSUP_BIOS_OWNED) == 0u) ? 0 : -1;
        }

        offset = xhci_next_extended_capability(info, offset);
    }

    return 0;
}

static uint32_t trb_type(const xhci_trb_t *trb) {
    return (trb->control >> XHCI_TRB_TYPE_SHIFT) & 0x3Fu;
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

static uint32_t xhci_link_trb_control(uint32_t cycle) {
    return (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) |
           XHCI_TRB_LINK_TOGGLE_CYCLE |
           (cycle ? XHCI_TRB_CYCLE : 0u);
}

static void xhci_setup_command_ring(uint64_t opbase) {
    zero_bytes(xhci_command_ring, sizeof(xhci_command_ring));
    xhci_command_enqueue = 0;
    xhci_command_cycle = 1;
    xhci_command_link_update_pending = 0;

    xhci_command_ring[XHCI_COMMAND_RING_TRBS - 1u].parameter = phys_addr(xhci_command_ring);
    xhci_command_ring[XHCI_COMMAND_RING_TRBS - 1u].status = 0;
    xhci_command_ring[XHCI_COMMAND_RING_TRBS - 1u].control = xhci_link_trb_control(xhci_command_cycle);

    mmio_write64(opbase, XHCI_CRCR, phys_addr(xhci_command_ring) | 1ull);
    (void)mmio_read64(opbase, XHCI_CRCR);
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
    mmio_write32(intr0, XHCI_INTR_IMAN, XHCI_INTR_IMAN_IP);
    (void)mmio_read64(intr0, XHCI_INTR_ERSTBA);
    (void)mmio_read64(intr0, XHCI_INTR_ERDP);
}

static int xhci_next_event(xhci_trb_t *out) {
    volatile xhci_trb_t *event = &xhci_event_ring[xhci_event_dequeue];
    uint32_t control = event->control;

    if ((control & XHCI_TRB_CYCLE) != xhci_event_cycle) {
        return 0;
    }

    out->parameter = event->parameter;
    out->status = event->status;
    out->control = control;
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

    if (xhci_command_enqueue >= XHCI_COMMAND_RING_TRBS - 1u) {
        xhci_command_enqueue = 0;
        xhci_command_cycle ^= 1u;
        xhci_command_link_update_pending = 1;
    }

    cmd = &xhci_command_ring[xhci_command_enqueue++];
    cmd->parameter = parameter;
    cmd->status = status;
    cmd->control = control | (xhci_command_cycle ? XHCI_TRB_CYCLE : 0u);

    xhci_ring_doorbell(info, 0, 0);

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
        if (xhci_command_link_update_pending &&
            event.parameter >= phys_addr(xhci_command_ring) &&
            event.parameter < phys_addr(&xhci_command_ring[XHCI_COMMAND_RING_TRBS - 1u])) {
            xhci_command_ring[XHCI_COMMAND_RING_TRBS - 1u].control = xhci_link_trb_control(xhci_command_cycle);
            xhci_command_link_update_pending = 0;
        }
        if (slot_id) {
            *slot_id = event.control >> 24;
        }
        return ((event.status >> 24) == XHCI_TRB_COMPLETION_SUCCESS) ? 0 : -1;
    }

    if (completion_code) {
        *completion_code = 0xFFFFFFFFu;
    }
    return -1;
}

static uint32_t *xhci_context_dword(uint8_t *base, uint32_t context_index, uint32_t dword_index) {
    return (uint32_t *)(void *)(base + context_index * xhci_context_size + dword_index * 4u);
}

static uint32_t xhci_port_speed(uint32_t portsc) {
    return (portsc >> 10) & 0x0Fu;
}

static uint32_t xhci_portsc_write_value(uint32_t portsc, uint32_t set_bits, uint32_t clear_change_bits);

static uint32_t xhci_wait_port_ready(uint64_t opbase, uint32_t offset) {
    uint32_t portsc = mmio_read32(opbase, offset);

    for (uint32_t i = 0; i < 10000000u; ++i) {
        uint32_t speed = xhci_port_speed(portsc);

        if ((portsc & XHCI_PORTSC_RESET) == 0u &&
            (portsc & XHCI_PORTSC_CCS) != 0u &&
            (portsc & XHCI_PORTSC_PED) != 0u &&
            speed != 0u) {
            return portsc;
        }
        portsc = mmio_read32(opbase, offset);
    }

    return portsc;
}

static int xhci_reset_port(uint64_t opbase, uint32_t offset, uint32_t *out_portsc) {
    uint32_t portsc = mmio_read32(opbase, offset);

    mmio_write32(opbase,
                 offset,
                 xhci_portsc_write_value(portsc,
                                         XHCI_PORTSC_POWER | XHCI_PORTSC_RESET,
                                         0));
    if (wait_bits_clear(opbase, offset, XHCI_PORTSC_RESET, 10000000u) != 0) {
        if (out_portsc) {
            *out_portsc = mmio_read32(opbase, offset);
        }
        return -1;
    }

    xhci_wait_ms(120u);
    portsc = mmio_read32(opbase, offset);
    mmio_write32(opbase,
                 offset,
                 xhci_portsc_write_value(portsc, XHCI_PORTSC_POWER, 1));
    portsc = xhci_wait_port_ready(opbase, offset);
    if (out_portsc) {
        *out_portsc = portsc;
    }
    return ((portsc & XHCI_PORTSC_PED) != 0u && xhci_port_speed(portsc) != 0u) ? 0 : -1;
}

static uint32_t ilog2_ceil_u32(uint32_t value) {
    uint32_t result = 0;
    uint32_t power = 1;

    if (value <= 1u) {
        return 0;
    }
    while (power < value && result < 31u) {
        power <<= 1;
        ++result;
    }
    return result;
}

static uint32_t xhci_interval_from_usb_interval(uint32_t speed, uint8_t interval) {
    uint32_t value = interval == 0u ? 1u : interval;

    if (speed >= 3u) {
        if (value > 16u) {
            value = 16u;
        }
        return value - 1u;
    }

    value = ilog2_ceil_u32(value) + 3u;
    if (value < 3u) {
        value = 3u;
    }
    if (value > 10u) {
        value = 10u;
    }
    return value;
}

static uint32_t xhci_portsc_write_value(uint32_t portsc, uint32_t set_bits, uint32_t clear_change_bits) {
    uint32_t value = portsc & ~XHCI_PORTSC_WRITE_1_CLEAR_BITS;

    value &= ~XHCI_PORTSC_RESET;
    value |= set_bits;
    if (clear_change_bits) {
        value |= XHCI_PORTSC_CHANGE_BITS;
    }
    return value;
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

static uint64_t usb_setup_packet(uint8_t request_type,
                                 uint8_t request,
                                 uint16_t value,
                                 uint16_t index,
                                 uint16_t length) {
    return (uint64_t)request_type |
           ((uint64_t)request << 8) |
           ((uint64_t)value << 16) |
           ((uint64_t)index << 32) |
           ((uint64_t)length << 48);
}

static void xhci_enqueue_transfer_trb(uint32_t slot_id,
                                      uint64_t parameter,
                                      uint32_t status,
                                      uint32_t control);
static int xhci_wait_transfer_event(const usb_controller_info_t *info,
                                    uint32_t slot_id,
                                    uint32_t *completion_code);

static void xhci_setup_transfer_ring(uint32_t slot_id) {
    xhci_trb_t *ring = xhci_transfer_rings[slot_id];

    zero_bytes(ring, sizeof(xhci_transfer_rings[slot_id]));
    xhci_transfer_enqueue[slot_id] = 0;
    xhci_transfer_cycle[slot_id] = 1;
    xhci_transfer_link_update_pending[slot_id] = 0;

    ring[XHCI_TRANSFER_RING_TRBS - 1u].parameter = phys_addr(ring);
    ring[XHCI_TRANSFER_RING_TRBS - 1u].status = 0;
    ring[XHCI_TRANSFER_RING_TRBS - 1u].control = xhci_link_trb_control(xhci_transfer_cycle[slot_id]);
}

static int xhci_control_transfer(usb_controller_info_t *info,
                                 uint32_t slot_id,
                                 uint64_t setup,
                                 void *data,
                                 uint32_t length,
                                 int data_in) {
    uint32_t completion = 0;
    uint32_t setup_control = XHCI_TRB_TYPE_SETUP_STAGE << XHCI_TRB_TYPE_SHIFT;
    uint32_t status_control = (XHCI_TRB_TYPE_STATUS_STAGE << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IOC;

    if (slot_id == 0 || slot_id > USB_MAX_XHCI_SLOTS) {
        return -1;
    }

    if (length != 0) {
        setup_control |= (data_in ? 3u : 2u) << 16;
    }
    xhci_enqueue_transfer_trb(slot_id,
                              setup,
                              8u,
                              setup_control | XHCI_TRB_IDT);
    if (length != 0) {
        xhci_enqueue_transfer_trb(slot_id,
                                  phys_addr(data),
                                  length,
                                  (XHCI_TRB_TYPE_DATA_STAGE << XHCI_TRB_TYPE_SHIFT) |
                                  (data_in ? XHCI_TRB_DIR_IN : 0u));
    }
    if (!data_in) {
        status_control |= XHCI_TRB_DIR_IN;
    }
    xhci_enqueue_transfer_trb(slot_id, 0, 0, status_control);
    xhci_ring_doorbell(info, slot_id, 1u);

    if (xhci_wait_transfer_event(info, slot_id, &completion) != 0) {
        info->last_completion_code = completion;
        return -1;
    }

    info->last_completion_code = completion;
    return 0;
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
        xhci_transfer_link_update_pending[slot_id] = 1;
    }

    trb = &ring[xhci_transfer_enqueue[slot_id]++];
    trb->parameter = parameter;
    trb->status = status;
    trb->control = control | (xhci_transfer_cycle[slot_id] ? XHCI_TRB_CYCLE : 0u);
}

static void xhci_finish_transfer_link_update(uint32_t slot_id, uint64_t event_parameter) {
    uint64_t ring_start;
    uint64_t ring_link;

    if (slot_id == 0 || slot_id > USB_MAX_XHCI_SLOTS ||
        !xhci_transfer_link_update_pending[slot_id]) {
        return;
    }

    ring_start = phys_addr(xhci_transfer_rings[slot_id]);
    ring_link = phys_addr(&xhci_transfer_rings[slot_id][XHCI_TRANSFER_RING_TRBS - 1u]);
    if (event_parameter >= ring_start && event_parameter < ring_link) {
        xhci_transfer_rings[slot_id][XHCI_TRANSFER_RING_TRBS - 1u].control =
            xhci_link_trb_control(xhci_transfer_cycle[slot_id]);
        xhci_transfer_link_update_pending[slot_id] = 0;
    }
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
        xhci_finish_transfer_link_update(slot_id, event.parameter);
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
    ctx = xhci_context_dword(input, 1, 1);
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
    zero_bytes(xhci_device_descriptors[slot_id], USB_DEVICE_DESCRIPTOR_SIZE);
    if (xhci_control_transfer(info,
                              slot_id,
                              usb_setup_packet(0x80u, 0x06u, 0x0100u, 0, USB_DEVICE_DESCRIPTOR_SIZE),
                              xhci_device_descriptors[slot_id],
                              USB_DEVICE_DESCRIPTOR_SIZE,
                              1) != 0) {
        return -1;
    }

    ++info->descriptor_count;
    return 0;
}

static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint16_t usb_endpoint_max_packet_size(uint16_t value) {
    value &= USB_ENDPOINT_MAX_PACKET_MASK;
    return value == 0u ? 1u : value;
}

static int xhci_get_config_descriptor(usb_controller_info_t *info,
                                      uint32_t slot_id,
                                      uint32_t *out_size) {
    uint32_t total;

    zero_bytes(xhci_config_descriptor, sizeof(xhci_config_descriptor));
    if (xhci_control_transfer(info,
                              slot_id,
                              usb_setup_packet(0x80u, 0x06u, 0x0200u, 0, 9u),
                              xhci_config_descriptor,
                              9u,
                              1) != 0) {
        return -1;
    }

    if (xhci_config_descriptor[1] != 2u) {
        return -1;
    }

    total = read_le16(&xhci_config_descriptor[2]);
    if (total < 9u) {
        return -1;
    }
    if (total > USB_CONFIG_DESCRIPTOR_CAPACITY) {
        total = USB_CONFIG_DESCRIPTOR_CAPACITY;
    }

    zero_bytes(xhci_config_descriptor, sizeof(xhci_config_descriptor));
    if (xhci_control_transfer(info,
                              slot_id,
                              usb_setup_packet(0x80u, 0x06u, 0x0200u, 0, (uint16_t)total),
                              xhci_config_descriptor,
                              total,
                              1) != 0) {
        return -1;
    }

    *out_size = total;
    return 0;
}

static int xhci_find_boot_mouse(uint32_t config_size,
                                uint8_t *configuration_value,
                                uint8_t *interface_number,
                                uint8_t *endpoint_address,
                                uint16_t *max_packet,
                                uint8_t *interval,
                                uint16_t *hid_report_size) {
    uint32_t offset = 0;
    uint8_t current_interface = 0;
    uint8_t current_protocol = 0;
    uint16_t current_hid_report_size = 0;
    int in_hid_interface = 0;
    int have_fallback = 0;
    uint8_t fallback_interface = 0;
    uint8_t fallback_endpoint = 0;
    uint16_t fallback_max_packet = 0;
    uint8_t fallback_interval = 0;
    uint16_t fallback_hid_report_size = 0;

    if (config_size < 9u || xhci_config_descriptor[1] != 2u) {
        return -1;
    }

    *configuration_value = xhci_config_descriptor[5];
    while (offset + 2u <= config_size) {
        uint8_t length = xhci_config_descriptor[offset];
        uint8_t type = xhci_config_descriptor[offset + 1u];

        if (length < 2u || offset + length > config_size) {
            break;
        }

        if (type == 4u && length >= 9u) {
            current_interface = xhci_config_descriptor[offset + 2u];
            current_protocol = xhci_config_descriptor[offset + 7u];
            current_hid_report_size = 0;
            in_hid_interface = xhci_config_descriptor[offset + 5u] == 3u;
        } else if (type == 0x21u && length >= 9u && in_hid_interface) {
            if (xhci_config_descriptor[offset + 6u] == 0x22u) {
                current_hid_report_size = read_le16(&xhci_config_descriptor[offset + 7u]);
            }
        } else if (type == 5u && length >= 7u && in_hid_interface) {
            uint8_t address = xhci_config_descriptor[offset + 2u];
            uint8_t attributes = xhci_config_descriptor[offset + 3u];
            if ((address & 0x80u) != 0 && (attributes & 0x03u) == 3u) {
                uint16_t endpoint_max_packet =
                    usb_endpoint_max_packet_size(read_le16(&xhci_config_descriptor[offset + 4u]));

                if (endpoint_max_packet == 0u || endpoint_max_packet > USB_MOUSE_REPORT_SIZE) {
                    endpoint_max_packet = USB_MOUSE_REPORT_SIZE;
                }

                if (current_protocol == 2u) {
                    *interface_number = current_interface;
                    *endpoint_address = address;
                    *max_packet = endpoint_max_packet;
                    *interval = xhci_config_descriptor[offset + 6u];
                    *hid_report_size = current_hid_report_size;
                    return 0;
                }

                if (!have_fallback && current_protocol != 1u) {
                    fallback_interface = current_interface;
                    fallback_endpoint = address;
                    fallback_max_packet = endpoint_max_packet;
                    fallback_interval = xhci_config_descriptor[offset + 6u];
                    fallback_hid_report_size = current_hid_report_size;
                    have_fallback = 1;
                }
            }
        }

        offset += length;
    }

    if (have_fallback) {
        *interface_number = fallback_interface;
        *endpoint_address = fallback_endpoint;
        *max_packet = fallback_max_packet;
        *interval = fallback_interval;
        *hid_report_size = fallback_hid_report_size;
        return 0;
    }

    return -1;
}

static int xhci_find_boot_keyboard(uint32_t config_size,
                                   uint8_t *configuration_value,
                                   uint8_t *interface_number,
                                   uint8_t *endpoint_address,
                                   uint16_t *max_packet,
                                   uint8_t *interval) {
    uint32_t offset = 0;
    uint8_t current_interface = 0;
    uint8_t current_protocol = 0;
    int in_hid_interface = 0;

    if (config_size < 9u || xhci_config_descriptor[1] != 2u) {
        return -1;
    }

    *configuration_value = xhci_config_descriptor[5];
    while (offset + 2u <= config_size) {
        uint8_t length = xhci_config_descriptor[offset];
        uint8_t type = xhci_config_descriptor[offset + 1u];

        if (length < 2u || offset + length > config_size) {
            break;
        }

        if (type == 4u && length >= 9u) {
            current_interface = xhci_config_descriptor[offset + 2u];
            current_protocol = xhci_config_descriptor[offset + 7u];
            in_hid_interface = xhci_config_descriptor[offset + 5u] == 3u;
        } else if (type == 5u && length >= 7u && in_hid_interface && current_protocol == 1u) {
            uint8_t address = xhci_config_descriptor[offset + 2u];
            uint8_t attributes = xhci_config_descriptor[offset + 3u];
            if ((address & 0x80u) != 0 && (attributes & 0x03u) == 3u) {
                uint16_t endpoint_max_packet =
                    usb_endpoint_max_packet_size(read_le16(&xhci_config_descriptor[offset + 4u]));

                if (endpoint_max_packet == 0u || endpoint_max_packet > USB_KEYBOARD_REPORT_SIZE) {
                    endpoint_max_packet = USB_KEYBOARD_REPORT_SIZE;
                }

                *interface_number = current_interface;
                *endpoint_address = address;
                *max_packet = endpoint_max_packet;
                *interval = xhci_config_descriptor[offset + 6u];
                return 0;
            }
        }

        offset += length;
    }

    return -1;
}

static int32_t sign_extend_bits(uint32_t value, uint32_t bits) {
    uint32_t sign;

    if (bits == 0u || bits >= 32u) {
        return (int32_t)value;
    }

    sign = 1u << (bits - 1u);
    if ((value & sign) != 0u) {
        value |= ~((1u << bits) - 1u);
    }
    return (int32_t)value;
}

static uint32_t hid_extract_bits(const uint8_t *report,
                                 uint32_t transferred,
                                 uint32_t bit_offset,
                                 uint32_t bit_size) {
    uint32_t value = 0;

    if (bit_size > 32u) {
        bit_size = 32u;
    }

    for (uint32_t i = 0; i < bit_size; ++i) {
        uint32_t bit = bit_offset + i;
        uint32_t byte = bit / 8u;

        if (byte >= transferred) {
            break;
        }
        if ((report[byte] & (uint8_t)(1u << (bit & 7u))) != 0u) {
            value |= 1u << i;
        }
    }

    return value;
}

static int hid_descriptor_item_value(const uint8_t *data, uint32_t size, int signed_value) {
    uint32_t value = 0;

    if (size > 0u) {
        value |= data[0];
    }
    if (size > 1u) {
        value |= (uint32_t)data[1] << 8;
    }
    if (size > 2u) {
        value |= (uint32_t)data[2] << 16;
        value |= (uint32_t)data[3] << 24;
    }

    if (!signed_value) {
        return (int)value;
    }
    return (int)sign_extend_bits(value, size * 8u);
}

static int xhci_get_hid_report_descriptor(usb_controller_info_t *info,
                                          uint32_t slot_id,
                                          uint8_t interface_number,
                                          uint16_t report_size) {
    uint32_t size = report_size;

    if (size == 0u) {
        return -1;
    }
    if (size > USB_HID_REPORT_DESCRIPTOR_CAPACITY) {
        size = USB_HID_REPORT_DESCRIPTOR_CAPACITY;
    }

    zero_bytes(xhci_hid_report_descriptor, sizeof(xhci_hid_report_descriptor));
    if (xhci_control_transfer(info,
                              slot_id,
                              usb_setup_packet(0x81u, 0x06u, 0x2200u, interface_number, (uint16_t)size),
                              xhci_hid_report_descriptor,
                              size,
                              1) != 0) {
        return -1;
    }

    info->mouse_hid_report_size = size;
    return 0;
}

static void xhci_mouse_clear_report_layout(usb_controller_info_t *info) {
    info->mouse_report_id = 0;
    info->mouse_report_parsed = 0;
    info->mouse_buttons_bit = 0;
    info->mouse_x_bit = 0;
    info->mouse_y_bit = 0;
    info->mouse_wheel_bit = 0;
    info->mouse_axis_size = 0;
    info->mouse_wheel_size = 0;
}

static uint32_t hid_usage_at(uint32_t index,
                             const uint32_t *usages,
                             uint32_t usage_count,
                             uint32_t usage_page,
                             uint32_t usage_min,
                             uint32_t usage_max) {
    if (index < usage_count) {
        return usages[index];
    }
    if (usage_min != 0u && usage_min + index <= usage_max) {
        return ((usage_page & 0xFFFFu) << 16) | ((usage_min + index) & 0xFFFFu);
    }
    return 0;
}

static int xhci_parse_mouse_report_descriptor(usb_controller_info_t *info, uint32_t size) {
    uint32_t offset = 0;
    uint32_t bit_pos = 0;
    uint32_t usage_page = 0;
    uint32_t report_size = 0;
    uint32_t report_count = 0;
    uint32_t report_id = 0;
    int logical_min = 0;
    uint32_t usages[16];
    uint32_t usage_count = 0;
    uint32_t usage_min = 0;
    uint32_t usage_max = 0;
    uint32_t have_buttons = 0;
    uint32_t have_x = 0;
    uint32_t have_y = 0;
    uint32_t have_wheel = 0;
    uint32_t parsed_report_id = 0;

    xhci_mouse_clear_report_layout(info);

    while (offset < size) {
        uint8_t prefix = xhci_hid_report_descriptor[offset++];
        uint32_t item_size = prefix & 0x03u;
        uint32_t item_type = (prefix >> 2) & 0x03u;
        uint32_t item_tag = (prefix >> 4) & 0x0Fu;
        const uint8_t *item_data;
        int value;

        if (prefix == 0xFEu) {
            if (offset + 2u > size) {
                break;
            }
            offset += 2u + xhci_hid_report_descriptor[offset];
            continue;
        }

        if (item_size == 3u) {
            item_size = 4u;
        }
        if (offset + item_size > size) {
            break;
        }

        item_data = &xhci_hid_report_descriptor[offset];
        value = hid_descriptor_item_value(item_data, item_size, item_type == 1u && item_tag == 1u);
        offset += item_size;

        if (item_type == 1u) {
            if (item_tag == 0u) {
                usage_page = (uint32_t)value;
            } else if (item_tag == 1u) {
                logical_min = value;
            } else if (item_tag == 7u) {
                report_size = (uint32_t)value;
            } else if (item_tag == 8u) {
                report_id = (uint32_t)value & 0xFFu;
                bit_pos = 8u;
            } else if (item_tag == 9u) {
                report_count = (uint32_t)value;
            }
        } else if (item_type == 2u) {
            if (item_tag == 0u) {
                if (usage_count < 16u) {
                    usages[usage_count++] = ((usage_page & 0xFFFFu) << 16) | ((uint32_t)value & 0xFFFFu);
                }
            } else if (item_tag == 1u) {
                usage_min = (uint32_t)value & 0xFFFFu;
            } else if (item_tag == 2u) {
                usage_max = (uint32_t)value & 0xFFFFu;
            }
        } else if (item_type == 0u && item_tag == 8u) {
            uint32_t flags = (uint32_t)value;

            if ((flags & 0x01u) == 0u && report_size != 0u && report_count != 0u) {
                for (uint32_t i = 0; i < report_count; ++i) {
                    uint32_t usage = hid_usage_at(i, usages, usage_count, usage_page, usage_min, usage_max);
                    uint32_t page = usage >> 16;
                    uint32_t id = usage & 0xFFFFu;
                    uint32_t field_bit = bit_pos + i * report_size;

                    if (page == 0x09u && id >= 1u && id <= 3u) {
                        if (!have_buttons || field_bit < info->mouse_buttons_bit) {
                            info->mouse_buttons_bit = field_bit;
                        }
                        have_buttons = 1u;
                        parsed_report_id = report_id;
                    } else if (page == 0x01u && id == 0x30u) {
                        info->mouse_x_bit = field_bit;
                        info->mouse_axis_size = report_size;
                        have_x = 1u;
                        parsed_report_id = report_id;
                    } else if (page == 0x01u && id == 0x31u) {
                        info->mouse_y_bit = field_bit;
                        info->mouse_axis_size = report_size;
                        have_y = 1u;
                        parsed_report_id = report_id;
                    } else if (page == 0x01u && id == 0x38u) {
                        info->mouse_wheel_bit = field_bit;
                        info->mouse_wheel_size = report_size;
                        have_wheel = 1u;
                        parsed_report_id = report_id;
                    }
                }
            }

            bit_pos += report_size * report_count;
            usage_count = 0;
            usage_min = 0;
            usage_max = 0;
        } else if (item_type == 0u && (item_tag == 10u || item_tag == 12u)) {
            usage_count = 0;
            usage_min = 0;
            usage_max = 0;
        }
    }

    if (have_buttons && have_x && have_y) {
        info->mouse_report_id = parsed_report_id;
        info->mouse_report_parsed = 1u;
        if (!have_wheel) {
            info->mouse_wheel_size = 0;
        }
        (void)logical_min;
        return 0;
    }

    xhci_mouse_clear_report_layout(info);
    return -1;
}

static uint32_t xhci_endpoint_dci(uint8_t endpoint_address) {
    uint32_t endpoint = endpoint_address & 0x0Fu;
    uint32_t in = (endpoint_address & 0x80u) != 0 ? 1u : 0u;
    return endpoint * 2u + in;
}

static int xhci_configure_interrupt_in_endpoint(usb_controller_info_t *info,
                                                uint32_t slot_id,
                                                uint32_t speed,
                                                uint8_t endpoint_address,
                                                uint16_t max_packet,
                                                uint8_t interval,
                                                uint32_t *out_dci) {
    uint8_t *input;
    uint32_t dci;
    uint32_t context_index;
    uint32_t *ctx;
    uint32_t completion = 0;

    dci = xhci_endpoint_dci(endpoint_address);
    if (dci == 0u || dci > 31u || slot_id == 0u || slot_id > USB_MAX_XHCI_SLOTS) {
        return -1;
    }

    input = xhci_input_contexts[slot_id];
    zero_bytes(input, sizeof(xhci_input_contexts[slot_id]));
    xhci_setup_transfer_ring(slot_id);
    context_index = dci + 1u;

    ctx = xhci_context_dword(input, 0, 1);
    *ctx = (1u << 0) | (1u << dci);

    ctx = xhci_context_dword(input, 1, 0);
    *ctx = dci << 27;

    ctx = xhci_context_dword(input, context_index, 0);
    *ctx = xhci_interval_from_usb_interval(speed, interval) << 16;
    ctx = xhci_context_dword(input, context_index, 1);
    *ctx = (3u << 1) | (XHCI_ENDPOINT_INTERRUPT_IN << 3) | ((uint32_t)max_packet << 16);
    ctx = xhci_context_dword(input, context_index, 2);
    *ctx = (uint32_t)((phys_addr(xhci_transfer_rings[slot_id]) | 1ull) & 0xFFFFFFFFull);
    ctx = xhci_context_dword(input, context_index, 3);
    *ctx = (uint32_t)((phys_addr(xhci_transfer_rings[slot_id]) | 1ull) >> 32);
    ctx = xhci_context_dword(input, context_index, 4);
    *ctx = (uint32_t)max_packet | ((uint32_t)max_packet << 16);

    if (xhci_ring_command(info,
                          phys_addr(input),
                          0,
                          (XHCI_TRB_TYPE_CONFIGURE_ENDPOINT << XHCI_TRB_TYPE_SHIFT) |
                          (slot_id << 24),
                          0,
                          &completion) != 0) {
        info->last_completion_code = completion;
        return -1;
    }

    *out_dci = dci;
    return 0;
}

static int xhci_try_configure_boot_keyboard(usb_controller_info_t *info, uint32_t slot_id, uint32_t speed) {
    uint32_t config_size = 0;
    uint8_t configuration_value = 0;
    uint8_t interface_number = 0;
    uint8_t endpoint_address = 0;
    uint16_t max_packet = 0;
    uint8_t interval = 0;
    uint32_t dci = 0;

    if (xhci_keyboard_controller != 0) {
        return -1;
    }
    if (xhci_get_config_descriptor(info, slot_id, &config_size) != 0) {
        return -1;
    }
    if (xhci_find_boot_keyboard(config_size,
                                &configuration_value,
                                &interface_number,
                                &endpoint_address,
                                &max_packet,
                                &interval) != 0) {
        return -1;
    }

    if (xhci_control_transfer(info,
                              slot_id,
                              usb_setup_packet(0x00u, 0x09u, configuration_value, 0, 0),
                              0,
                              0,
                              0) != 0) {
        return -1;
    }

    (void)xhci_control_transfer(info,
                                slot_id,
                                usb_setup_packet(0x21u, 0x0Bu, 0u, interface_number, 0),
                                0,
                                0,
                                0);
    (void)xhci_control_transfer(info,
                                slot_id,
                                usb_setup_packet(0x21u, 0x0Au, 0u, interface_number, 0),
                                0,
                                0,
                                0);

    if (xhci_configure_interrupt_in_endpoint(info,
                                             slot_id,
                                             speed,
                                             endpoint_address,
                                             max_packet,
                                             interval,
                                             &dci) != 0) {
        return -1;
    }

    xhci_keyboard_controller = info;
    xhci_keyboard_slot = slot_id;
    xhci_keyboard_dci = dci;
    xhci_keyboard_report_size = max_packet;
    xhci_keyboard_pending = 0;
    return 0;
}

static int xhci_try_configure_boot_mouse(usb_controller_info_t *info, uint32_t slot_id, uint32_t speed) {
    uint32_t config_size = 0;
    uint8_t configuration_value = 0;
    uint8_t interface_number = 0;
    uint8_t endpoint_address = 0;
    uint16_t max_packet = 0;
    uint8_t interval = 0;
    uint16_t hid_report_size = 0;
    uint32_t dci = 0;
    int report_descriptor_ok = 0;

    info->mouse_stage = 1;
    if (xhci_get_config_descriptor(info, slot_id, &config_size) != 0) {
        info->mouse_last_completion_code = info->last_completion_code;
        return -1;
    }

    info->mouse_stage = 2;
    if (xhci_find_boot_mouse(config_size,
                             &configuration_value,
                             &interface_number,
                             &endpoint_address,
                             &max_packet,
                             &interval,
                             &hid_report_size) != 0) {
        info->mouse_last_completion_code = 0xF2u;
        return -1;
    }
    info->mouse_interface = interface_number;
    info->mouse_endpoint = endpoint_address;
    info->mouse_interval = interval;
    info->mouse_report_size = max_packet;
    info->mouse_hid_report_size = hid_report_size;

    info->mouse_stage = 3;
    if (xhci_control_transfer(info,
                              slot_id,
                              usb_setup_packet(0x00u, 0x09u, configuration_value, 0, 0),
                              0,
                              0,
                              0) != 0) {
        info->mouse_last_completion_code = info->last_completion_code;
        return -1;
    }

    if (xhci_get_hid_report_descriptor(info, slot_id, interface_number, hid_report_size) == 0 &&
        xhci_parse_mouse_report_descriptor(info, info->mouse_hid_report_size) == 0) {
        report_descriptor_ok = 1;
    }

    if (report_descriptor_ok &&
        xhci_control_transfer(info,
                              slot_id,
                              usb_setup_packet(0x21u, 0x0Bu, 1u, interface_number, 0),
                              0,
                              0,
                              0) == 0) {
        info->mouse_protocol = 1u;
    } else if (xhci_control_transfer(info,
                                     slot_id,
                                     usb_setup_packet(0x21u, 0x0Bu, 0u, interface_number, 0),
                                     0,
                                     0,
                                     0) == 0) {
        info->mouse_protocol = 0u;
        if (!report_descriptor_ok) {
            xhci_mouse_clear_report_layout(info);
        }
    } else {
        info->mouse_protocol = 0xffffffffu;
        xhci_mouse_clear_report_layout(info);
    }

    info->mouse_stage = 4;
    if (xhci_configure_interrupt_in_endpoint(info,
                                             slot_id,
                                             speed,
                                             endpoint_address,
                                             max_packet,
                                             interval,
                                             &dci) != 0) {
        info->mouse_last_completion_code = info->last_completion_code;
        return -1;
    }

    xhci_mouse_controller = info;
    xhci_mouse_slot = slot_id;
    xhci_mouse_dci = dci;
    xhci_mouse_report_size = max_packet;
    xhci_mouse_pending = 0;
    info->mouse_configured = 1;
    info->mouse_stage = 5;
    info->mouse_slot = slot_id;
    info->mouse_dci = dci;
    info->mouse_report_size = max_packet;
    info->mouse_pending = 0;
    info->mouse_report_count = 0;
    info->mouse_last_completion_code = 0;
    info->mouse_last_transferred = 0;
    info->mouse_last_report0 = 0;
    info->mouse_last_report1 = 0;
    info->mouse_last_report2 = 0;
    info->mouse_last_report3 = 0;
    info->mouse_last_report4 = 0;
    info->mouse_last_wheel = 0;
    info->mouse_last_nonzero_transferred = 0;
    info->mouse_last_nonzero_report0 = 0;
    info->mouse_last_nonzero_report1 = 0;
    info->mouse_last_nonzero_report2 = 0;
    info->mouse_last_nonzero_report3 = 0;
    info->mouse_last_nonzero_report4 = 0;
    info->mouse_last_nonzero_wheel = 0;
    return 0;
}

static void xhci_reset_connected_ports(usb_controller_info_t *info, uint64_t opbase) {
    uint32_t max_ports = info->port_count;

    info->enum_stage = 1;
    info->enum_port = 0;
    info->enum_portsc = 0;
    info->enum_speed = 0;
    info->enum_slot = 0;
    info->enum_completion_code = 0;
    info->connected_port_count = 0;
    info->reset_port_count = 0;
    info->enabled_slot_count = 0;
    info->addressed_device_count = 0;
    info->descriptor_count = 0;
    info->mouse_configured = 0;
    info->mouse_stage = 0;
    info->mouse_slot = 0;
    info->mouse_dci = 0;
    info->mouse_report_size = 0;
    info->mouse_pending = 0;
    info->mouse_report_count = 0;
    info->mouse_last_completion_code = 0;
    info->mouse_interface = 0;
    info->mouse_endpoint = 0;
    info->mouse_interval = 0;
    info->mouse_protocol = 0;
    info->mouse_hid_report_size = 0;
    info->mouse_report_id = 0;
    info->mouse_report_parsed = 0;
    info->mouse_buttons_bit = 0;
    info->mouse_x_bit = 0;
    info->mouse_y_bit = 0;
    info->mouse_wheel_bit = 0;
    info->mouse_axis_size = 0;
    info->mouse_wheel_size = 0;
    info->mouse_last_transferred = 0;
    info->mouse_last_report0 = 0;
    info->mouse_last_report1 = 0;
    info->mouse_last_report2 = 0;
    info->mouse_last_report3 = 0;
    info->mouse_last_report4 = 0;
    info->mouse_last_wheel = 0;
    info->mouse_last_nonzero_transferred = 0;
    info->mouse_last_nonzero_report0 = 0;
    info->mouse_last_nonzero_report1 = 0;
    info->mouse_last_nonzero_report2 = 0;
    info->mouse_last_nonzero_report3 = 0;
    info->mouse_last_nonzero_report4 = 0;
    info->mouse_last_nonzero_wheel = 0;
    xhci_mouse_controller = 0;
    xhci_mouse_slot = 0;
    xhci_mouse_dci = 0;
    xhci_mouse_report_size = 0;
    xhci_mouse_pending = 0;
    xhci_keyboard_controller = 0;
    xhci_keyboard_slot = 0;
    xhci_keyboard_dci = 0;
    xhci_keyboard_report_size = 0;
    xhci_keyboard_pending = 0;

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

        info->enum_stage = 2;
        info->enum_port = port + 1u;
        info->enum_portsc = portsc;
        ++info->connected_port_count;
        (void)xhci_reset_port(opbase, offset, &portsc);
        info->enum_stage = 3;
        info->enum_portsc = portsc;
        ++info->reset_port_count;

        speed = xhci_port_speed(portsc);
        info->enum_speed = speed;
        if (speed == 0u || (portsc & XHCI_PORTSC_PED) == 0u) {
            info->enum_completion_code = 0xFEu;
            continue;
        }
        info->enum_stage = 4;
        if (xhci_ring_command(info,
                              0,
                              0,
                              XHCI_TRB_TYPE_ENABLE_SLOT << XHCI_TRB_TYPE_SHIFT,
                              &slot_id,
                              &completion) != 0 ||
            slot_id == 0) {
            info->last_completion_code = completion;
            info->enum_completion_code = completion;
            continue;
        }

        ++info->enabled_slot_count;
        info->enum_slot = slot_id;
        info->enum_stage = 5;
        if (xhci_address_device(info, slot_id, port, speed) == 0) {
                    info->enum_stage = 6;
                    if (xhci_get_device_descriptor(info, slot_id) == 0) {
                        info->enum_stage = 7;
                        if (xhci_try_configure_boot_keyboard(info, slot_id, speed) != 0) {
                            (void)xhci_try_configure_boot_mouse(info, slot_id, speed);
                        }
                    }
        } else {
            info->enum_completion_code = info->last_completion_code;
        }
        info->last_completion_code = completion;
    }
    if (info->addressed_device_count != 0u || info->enabled_slot_count == 0u) {
        info->enum_stage = 8;
    }
}

static int xhci_ready_for_start(usb_controller_info_t *info) {
    return info != 0 && info->bar0 != 0 && !info->bar0_is_io && info->max_slots != 0;
}

static int xhci_halt(usb_controller_info_t *info) {
    uint64_t opbase = xhci_operational_base(info);

    mmio_write32(opbase, XHCI_USBCMD, mmio_read32(opbase, XHCI_USBCMD) & ~XHCI_USBCMD_RUN);
    if (wait_bits_set(opbase, XHCI_USBSTS, XHCI_USBSTS_HALTED, 10000000u) != 0) {
        return -1;
    }
    return 0;
}

static int xhci_reset_controller(usb_controller_info_t *info) {
    uint64_t opbase = xhci_operational_base(info);

    mmio_write32(opbase, XHCI_USBCMD, mmio_read32(opbase, XHCI_USBCMD) | XHCI_USBCMD_RESET);
    if (wait_bits_clear(opbase, XHCI_USBCMD, XHCI_USBCMD_RESET, 10000000u) != 0 ||
        wait_bits_clear(opbase, XHCI_USBSTS, XHCI_USBSTS_CNR, 10000000u) != 0) {
        return -1;
    }
    info->running = 0;
    return 0;
}

static int xhci_setup_rings_and_contexts(usb_controller_info_t *info) {
    uint64_t opbase = xhci_operational_base(info);
    uint32_t slots;

    if (info->page_size != XHCI_PAGE_SIZE) {
        info->last_completion_code = info->page_size;
        return -1;
    }
    if (info->scratchpad_count > USB_MAX_XHCI_SCRATCHPADS) {
        info->last_completion_code = info->scratchpad_count;
        return -1;
    }

    zero_bytes(xhci_dcbaa, sizeof(xhci_dcbaa));
    zero_bytes(xhci_scratchpad_array, sizeof(xhci_scratchpad_array));
    if (info->scratchpad_count != 0u) {
        for (uint32_t i = 0; i < info->scratchpad_count; ++i) {
            zero_bytes(xhci_scratchpad_buffers[i], XHCI_PAGE_SIZE);
            xhci_scratchpad_array[i] = phys_addr(xhci_scratchpad_buffers[i]);
        }
        xhci_dcbaa[0] = phys_addr(xhci_scratchpad_array);
    }

    slots = info->max_slots;
    if (slots > USB_MAX_XHCI_SLOTS) {
        slots = USB_MAX_XHCI_SLOTS;
    }
    mmio_write32(opbase, XHCI_CONFIG, slots);
    mmio_write64(opbase, XHCI_DCBAAP, phys_addr(xhci_dcbaa));
    (void)mmio_read64(opbase, XHCI_DCBAAP);

    xhci_setup_command_ring(opbase);
    xhci_setup_event_ring(info);
    info->dcbaa_phys = phys_addr(xhci_dcbaa);
    info->command_ring_phys = phys_addr(xhci_command_ring);
    info->event_ring_phys = phys_addr(xhci_event_ring);
    info->erst_phys = phys_addr(xhci_erst);
    dma_write_barrier();
    return 0;
}

static int xhci_run(usb_controller_info_t *info) {
    uint64_t opbase = xhci_operational_base(info);

    mmio_write32(opbase, XHCI_USBSTS, XHCI_USBSTS_CLEAR_BITS);
    mmio_write32(opbase,
                 XHCI_USBCMD,
                 (mmio_read32(opbase, XHCI_USBCMD) & ~XHCI_USBCMD_INTE) | XHCI_USBCMD_RUN);
    if (wait_bits_clear(opbase, XHCI_USBSTS, XHCI_USBSTS_HALTED, 10000000u) != 0) {
        return -1;
    }

    info->running = 1;
    return 0;
}

static int xhci_poke_run(usb_controller_info_t *info) {
    uint64_t opbase = xhci_operational_base(info);
    uint32_t command;

    mmio_write32(opbase, XHCI_USBSTS, XHCI_USBSTS_CLEAR_BITS);
    command = mmio_read32(opbase, XHCI_USBCMD);
    command &= ~XHCI_USBCMD_INTE;
    command |= XHCI_USBCMD_RUN;
    mmio_write32(opbase, XHCI_USBCMD, command);
    info->running = 1;
    return 0;
}

static void visit_pci(uint8_t bus, uint8_t device, uint8_t function, void *ctx) {
    usb_controller_info_t *info;
    uint8_t class_code = pci_read_config8(bus, device, function, 0x0B);
    uint8_t subclass = pci_read_config8(bus, device, function, 0x0A);
    uint8_t prog_if = pci_read_config8(bus, device, function, 0x09);

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
    info->scratchpad_count = 0;
    info->page_size = 0;
    info->hcsparams2 = 0;
    info->hccparams1 = 0;
    info->dcbaa_phys = 0;
    info->command_ring_phys = 0;
    info->event_ring_phys = 0;
    info->erst_phys = 0;
    info->initialized = 0;
    info->running = 0;
    info->connected_port_count = 0;
    info->reset_port_count = 0;
    info->enabled_slot_count = 0;
    info->addressed_device_count = 0;
    info->descriptor_count = 0;
    info->last_completion_code = 0;
    info->mouse_configured = 0;
    info->mouse_slot = 0;
    info->mouse_dci = 0;
    info->mouse_report_size = 0;
    info->mouse_pending = 0;
    info->mouse_report_count = 0;
    info->mouse_last_completion_code = 0;
    info->mouse_stage = 0;
    info->mouse_interface = 0;
    info->mouse_endpoint = 0;
    info->mouse_interval = 0;
    info->mouse_protocol = 0;
    info->mouse_hid_report_size = 0;
    info->mouse_report_id = 0;
    info->mouse_report_parsed = 0;
    info->mouse_buttons_bit = 0;
    info->mouse_x_bit = 0;
    info->mouse_y_bit = 0;
    info->mouse_wheel_bit = 0;
    info->mouse_axis_size = 0;
    info->mouse_wheel_size = 0;
    info->mouse_last_transferred = 0;
    info->mouse_last_report0 = 0;
    info->mouse_last_report1 = 0;
    info->mouse_last_report2 = 0;
    info->mouse_last_report3 = 0;
    info->mouse_last_report4 = 0;
    info->mouse_last_wheel = 0;
    info->mouse_last_nonzero_transferred = 0;
    info->mouse_last_nonzero_report0 = 0;
    info->mouse_last_nonzero_report1 = 0;
    info->mouse_last_nonzero_report2 = 0;
    info->mouse_last_nonzero_report3 = 0;
    info->mouse_last_nonzero_report4 = 0;
    info->mouse_last_nonzero_wheel = 0;
    info->enum_stage = 0;
    info->enum_port = 0;
    info->enum_portsc = 0;
    info->enum_speed = 0;
    info->enum_slot = 0;
    info->enum_completion_code = 0;
    info->bar0 = read_bar0(bus, device, function, &info->bar0_is_io);

    if (info->type == USB_CONTROLLER_XHCI) {
        ++xhci_count;
    }

    ++controller_count;
}

void usb_init(void) {
    unsigned int hz;

    controller_count = 0;
    xhci_count = 0;
    xhci_mouse_controller = 0;
    xhci_mouse_slot = 0;
    xhci_mouse_dci = 0;
    xhci_mouse_report_size = 0;
    xhci_mouse_pending = 0;
    xhci_keyboard_controller = 0;
    xhci_keyboard_slot = 0;
    xhci_keyboard_dci = 0;
    xhci_keyboard_report_size = 0;
    xhci_keyboard_pending = 0;
    xhci_late_enum_attempts = 0;
    xhci_next_late_enum_tick = 0;
    pci_scan(visit_pci, 0);

    hz = timer_frequency();
    if (hz == 0u) {
        hz = 250u;
    }
    xhci_next_late_enum_tick = timer_ticks() +
                               (unsigned long long)hz * USB_LATE_ENUM_INITIAL_DELAY_SECONDS;
}

int usb_xhci_init_controller(uint32_t index) {
    usb_controller_info_t *info;
    uint16_t command;

    if (index >= controller_count) {
        return -1;
    }

    info = &controllers[index];
    if (info->type != USB_CONTROLLER_XHCI) {
        return -1;
    }
    if (info->initialized) {
        return 0;
    }

    command = pci_read_config16(info->bus, info->device, info->function, 0x04);
    command |= PCI_COMMAND_IO_SPACE |
               PCI_COMMAND_MEMORY_SPACE |
               PCI_COMMAND_INTERRUPT_DISABLE;
    pci_write_config16(info->bus, info->device, info->function, 0x04, command);

    probe_xhci(info);
    if (info->max_slots == 0 || info->port_count == 0) {
        return -1;
    }
    info->initialized = 1;
    return info->initialized ? 0 : -1;
}

int usb_xhci_handoff_controller(uint32_t index) {
    usb_controller_info_t *info;

    if (index >= controller_count) {
        return -1;
    }

    info = &controllers[index];
    if (info->type != USB_CONTROLLER_XHCI) {
        return -1;
    }
    if (!info->initialized && usb_xhci_init_controller(index) != 0) {
        return -1;
    }

    return xhci_legacy_handoff(info);
}

static usb_controller_info_t *xhci_controller_for_stage(uint32_t index) {
    usb_controller_info_t *info;

    if (index >= controller_count) {
        return 0;
    }

    info = &controllers[index];
    if (info->type != USB_CONTROLLER_XHCI) {
        return 0;
    }
    if (!info->initialized && usb_xhci_init_controller(index) != 0) {
        return 0;
    }
    if (!xhci_ready_for_start(info)) {
        return 0;
    }
    return info;
}

static int xhci_enable_bus_mastering(usb_controller_info_t *info) {
    uint16_t command;

    if (info == 0) {
        return -1;
    }

    command = pci_read_config16(info->bus, info->device, info->function, 0x04);
    command |= PCI_COMMAND_IO_SPACE |
               PCI_COMMAND_MEMORY_SPACE |
               PCI_COMMAND_BUS_MASTER |
               PCI_COMMAND_INTERRUPT_DISABLE;
    pci_write_config16(info->bus, info->device, info->function, 0x04, command);
    return 0;
}

static int xhci_disable_bus_mastering(usb_controller_info_t *info) {
    uint16_t command;

    if (info == 0) {
        return -1;
    }

    command = pci_read_config16(info->bus, info->device, info->function, 0x04);
    command |= PCI_COMMAND_IO_SPACE |
               PCI_COMMAND_MEMORY_SPACE |
               PCI_COMMAND_INTERRUPT_DISABLE;
    command &= (uint16_t)~PCI_COMMAND_BUS_MASTER;
    pci_write_config16(info->bus, info->device, info->function, 0x04, command);
    return 0;
}

int usb_xhci_halt_controller(uint32_t index) {
    usb_controller_info_t *info = xhci_controller_for_stage(index);

    if (info == 0 || xhci_legacy_handoff(info) != 0) {
        return -1;
    }
    return xhci_halt(info);
}

int usb_xhci_reset_controller(uint32_t index) {
    usb_controller_info_t *info = xhci_controller_for_stage(index);

    if (info == 0 || xhci_legacy_handoff(info) != 0) {
        return -1;
    }
    if (xhci_halt(info) != 0) {
        return -1;
    }
    return xhci_reset_controller(info);
}

int usb_xhci_setup_rings(uint32_t index) {
    usb_controller_info_t *info = xhci_controller_for_stage(index);

    if (info == 0) {
        return -1;
    }
    return xhci_setup_rings_and_contexts(info);
}

int usb_xhci_run_controller(uint32_t index) {
    usb_controller_info_t *info = xhci_controller_for_stage(index);

    if (info == 0 || xhci_enable_bus_mastering(info) != 0) {
        return -1;
    }
    return xhci_run(info);
}

int usb_xhci_poke_run_controller(uint32_t index) {
    usb_controller_info_t *info = xhci_controller_for_stage(index);

    if (info == 0 || xhci_enable_bus_mastering(info) != 0) {
        return -1;
    }
    return xhci_poke_run(info);
}

int usb_xhci_poke_no_dma_controller(uint32_t index) {
    usb_controller_info_t *info = xhci_controller_for_stage(index);

    if (info == 0 || xhci_disable_bus_mastering(info) != 0) {
        return -1;
    }
    return xhci_poke_run(info);
}

int usb_xhci_busmaster_controller(uint32_t index) {
    usb_controller_info_t *info = xhci_controller_for_stage(index);

    if (info == 0) {
        return -1;
    }
    return xhci_enable_bus_mastering(info);
}

int usb_xhci_no_busmaster_controller(uint32_t index) {
    usb_controller_info_t *info = xhci_controller_for_stage(index);

    if (info == 0) {
        return -1;
    }
    return xhci_disable_bus_mastering(info);
}

int usb_xhci_status_controller(uint32_t index, uint32_t *usbcmd, uint32_t *usbsts) {
    usb_controller_info_t *info = xhci_controller_for_stage(index);
    uint64_t opbase;

    if (info == 0 || usbcmd == 0 || usbsts == 0) {
        return -1;
    }

    opbase = xhci_operational_base(info);
    *usbcmd = mmio_read32(opbase, XHCI_USBCMD);
    *usbsts = mmio_read32(opbase, XHCI_USBSTS);
    return 0;
}

int usb_xhci_start_controller(uint32_t index) {
    usb_controller_info_t *info;

    if (index >= controller_count) {
        return -1;
    }

    info = &controllers[index];
    if (info->type != USB_CONTROLLER_XHCI) {
        return -1;
    }
    if (info->running) {
        return 0;
    }
    if (!info->initialized && usb_xhci_init_controller(index) != 0) {
        return -1;
    }
    if (xhci_legacy_handoff(info) != 0) {
        return -1;
    }
    if (xhci_halt(info) != 0 ||
        xhci_reset_controller(info) != 0 ||
        xhci_setup_rings_and_contexts(info) != 0 ||
        xhci_enable_bus_mastering(info) != 0 ||
        xhci_run(info) != 0) {
        return -1;
    }
    return info->running ? 0 : -1;
}

int usb_xhci_enumerate_controller(uint32_t index) {
    usb_controller_info_t *info;

    if (index >= controller_count) {
        return -1;
    }

    info = &controllers[index];
    if (info->type != USB_CONTROLLER_XHCI) {
        return -1;
    }
    if (!info->running) {
        if (usb_xhci_start_controller(index) != 0) {
            return -1;
        }
    }

    xhci_reset_connected_ports(info, xhci_operational_base(info));
    return 0;
}

static int xhci_recover_controller_for_mouse(uint32_t index) {
    if (index >= controller_count || controllers[index].type != USB_CONTROLLER_XHCI) {
        return -1;
    }

    if (usb_xhci_reset_controller(index) != 0) {
        return -1;
    }
    xhci_wait_ms(500u);
    if (usb_xhci_start_controller(index) != 0) {
        return -1;
    }
    xhci_wait_ms(500u);
    return usb_xhci_enumerate_controller(index);
}

int usb_xhci_scan_ports(uint32_t index) {
    usb_controller_info_t *info;
    uint16_t command;
    uint64_t opbase;
    uint32_t max_ports;
    uint32_t connected = 0;

    if (index >= controller_count) {
        return -1;
    }

    info = &controllers[index];
    if (info->type != USB_CONTROLLER_XHCI ||
        info->bar0 == 0 ||
        info->bar0_is_io) {
        return -1;
    }

    command = pci_read_config16(info->bus, info->device, info->function, 0x04);
    command |= PCI_COMMAND_IO_SPACE | PCI_COMMAND_MEMORY_SPACE | PCI_COMMAND_INTERRUPT_DISABLE;
    pci_write_config16(info->bus, info->device, info->function, 0x04, command);

    probe_xhci(info);
    if (info->port_count == 0u) {
        return -1;
    }

    opbase = xhci_operational_base(info);
    max_ports = info->port_count;
    if (max_ports > 255u) {
        max_ports = 255u;
    }

    for (uint32_t port = 0; port < max_ports; ++port) {
        uint32_t offset = XHCI_PORT_REGS + port * XHCI_PORT_STRIDE;
        uint32_t portsc = mmio_read32(opbase, offset);

        if ((portsc & XHCI_PORTSC_CCS) != 0u) {
            ++connected;
        }
    }

    info->connected_port_count = connected;
    return 0;
}

static void xhci_try_late_mouse_enumeration(void) {
    unsigned long long now;
    unsigned int hz;

    if (xhci_mouse_controller != 0 ||
        xhci_keyboard_controller != 0 ||
        xhci_late_enum_attempts >= USB_LATE_ENUM_MAX_ATTEMPTS) {
        return;
    }

    now = timer_ticks();
    if (now < xhci_next_late_enum_tick) {
        return;
    }

    hz = timer_frequency();
    if (hz == 0u) {
        hz = 250u;
    }
    xhci_next_late_enum_tick = now + (unsigned long long)hz * USB_LATE_ENUM_RETRY_SECONDS;
    ++xhci_late_enum_attempts;

    for (uint32_t i = 0; i < controller_count; ++i) {
        if (controllers[i].type == USB_CONTROLLER_XHCI &&
            controllers[i].mouse_configured == 0u) {
            (void)xhci_recover_controller_for_mouse(i);
            if (xhci_mouse_controller != 0) {
                return;
            }
        }
    }
}

void usb_poll(void) {
    if ((xhci_mouse_controller == 0 ||
         xhci_mouse_slot == 0 ||
         xhci_mouse_dci == 0 ||
         xhci_mouse_report_size == 0) &&
        (xhci_keyboard_controller == 0 ||
         xhci_keyboard_slot == 0 ||
         xhci_keyboard_dci == 0 ||
         xhci_keyboard_report_size == 0)) {
        xhci_try_late_mouse_enumeration();
        return;
    }

    if (xhci_mouse_pending || xhci_keyboard_pending) {
        for (uint32_t i = 0; i < 16u; ++i) {
            xhci_trb_t event;
            uint32_t slot;
            uint32_t completion;

            if (!xhci_next_event(&event)) {
                break;
            }

            slot = event.control >> 24;
            if (slot == xhci_mouse_slot && xhci_mouse_controller != 0) {
                xhci_update_erdp(xhci_mouse_controller);
            } else if (slot == xhci_keyboard_slot && xhci_keyboard_controller != 0) {
                xhci_update_erdp(xhci_keyboard_controller);
            } else if (xhci_mouse_controller != 0) {
                xhci_update_erdp(xhci_mouse_controller);
            } else if (xhci_keyboard_controller != 0) {
                xhci_update_erdp(xhci_keyboard_controller);
            }

            if (trb_type(&event) != XHCI_TRB_TYPE_TRANSFER_EVENT) {
                continue;
            }

            completion = event.status >> 24;
            if (slot == xhci_keyboard_slot &&
                xhci_keyboard_controller != 0 &&
                xhci_keyboard_pending) {
                xhci_keyboard_controller->last_completion_code = completion;
                xhci_finish_transfer_link_update(xhci_keyboard_slot, event.parameter);
                xhci_keyboard_pending = 0;
                if (completion == XHCI_TRB_COMPLETION_SUCCESS ||
                    completion == XHCI_TRB_COMPLETION_SHORT_PACKET) {
                    uint32_t residual = event.status & XHCI_TRB_TRANSFER_LENGTH_MASK;
                    uint32_t transferred = xhci_keyboard_report_size > residual ?
                                           xhci_keyboard_report_size - residual :
                                           xhci_keyboard_report_size;
                    keyboard_apply_usb_boot_report(xhci_keyboard_report, transferred);
                }
                continue;
            }

            if (slot != xhci_mouse_slot ||
                xhci_mouse_controller == 0 ||
                !xhci_mouse_pending) {
                continue;
            }

            xhci_mouse_controller->last_completion_code = completion;
            xhci_mouse_controller->mouse_last_completion_code = completion;
            xhci_finish_transfer_link_update(xhci_mouse_slot, event.parameter);
            xhci_mouse_pending = 0;
            xhci_mouse_controller->mouse_pending = 0;
            if (completion == XHCI_TRB_COMPLETION_SUCCESS ||
                completion == XHCI_TRB_COMPLETION_SHORT_PACKET) {
                uint32_t residual = event.status & XHCI_TRB_TRANSFER_LENGTH_MASK;
                uint32_t transferred = xhci_mouse_report_size > residual ?
                                       xhci_mouse_report_size - residual :
                                       xhci_mouse_report_size;
                int dx;
                int dy;
                int wheel;
                int nonzero_report;

                xhci_mouse_controller->mouse_last_transferred = transferred;
                xhci_mouse_controller->mouse_last_report0 = xhci_mouse_report[0];
                xhci_mouse_controller->mouse_last_report1 = transferred > 1u ? xhci_mouse_report[1] : 0u;
                xhci_mouse_controller->mouse_last_report2 = transferred > 2u ? xhci_mouse_report[2] : 0u;
                xhci_mouse_controller->mouse_last_report3 = transferred > 3u ? xhci_mouse_report[3] : 0u;
                xhci_mouse_controller->mouse_last_report4 = transferred > 4u ? xhci_mouse_report[4] : 0u;
                nonzero_report =
                    xhci_mouse_controller->mouse_last_report0 != 0u ||
                    xhci_mouse_controller->mouse_last_report1 != 0u ||
                    xhci_mouse_controller->mouse_last_report2 != 0u ||
                    xhci_mouse_controller->mouse_last_report3 != 0u ||
                    xhci_mouse_controller->mouse_last_report4 != 0u;

                if (xhci_mouse_controller->mouse_protocol == 1u &&
                    xhci_mouse_controller->mouse_report_parsed != 0u &&
                    (xhci_mouse_controller->mouse_report_id == 0u ||
                     (transferred > 0u && xhci_mouse_report[0] == xhci_mouse_controller->mouse_report_id))) {
                    uint8_t parsed_buttons =
                        (uint8_t)(hid_extract_bits(xhci_mouse_report,
                                                   transferred,
                                                   xhci_mouse_controller->mouse_buttons_bit,
                                                   3u) & 0x07u);
                    uint32_t x_raw = hid_extract_bits(xhci_mouse_report,
                                                      transferred,
                                                      xhci_mouse_controller->mouse_x_bit,
                                                      xhci_mouse_controller->mouse_axis_size);
                    uint32_t y_raw = hid_extract_bits(xhci_mouse_report,
                                                      transferred,
                                                      xhci_mouse_controller->mouse_y_bit,
                                                      xhci_mouse_controller->mouse_axis_size);
                    uint32_t wheel_raw = xhci_mouse_controller->mouse_wheel_size != 0u ?
                                         hid_extract_bits(xhci_mouse_report,
                                                          transferred,
                                                          xhci_mouse_controller->mouse_wheel_bit,
                                                          xhci_mouse_controller->mouse_wheel_size) :
                                         0u;

                    dx = (int)sign_extend_bits(x_raw, xhci_mouse_controller->mouse_axis_size);
                    dy = (int)sign_extend_bits(y_raw, xhci_mouse_controller->mouse_axis_size);
                    wheel = xhci_mouse_controller->mouse_wheel_size != 0u ?
                            (int)sign_extend_bits(wheel_raw, xhci_mouse_controller->mouse_wheel_size) :
                            0;
                    xhci_mouse_report[0] = parsed_buttons;
                } else {
                    dx = (int)(int8_t)xhci_mouse_report[1];
                    dy = (int)(int8_t)xhci_mouse_report[2];
                    wheel = xhci_mouse_controller->mouse_protocol == 0u && transferred >= 4u ?
                            (int)(int8_t)xhci_mouse_report[3] :
                            0;
                }
                xhci_mouse_controller->mouse_last_wheel = wheel;
                if (nonzero_report) {
                    xhci_mouse_controller->mouse_last_nonzero_transferred = transferred;
                    xhci_mouse_controller->mouse_last_nonzero_report0 = xhci_mouse_controller->mouse_last_report0;
                    xhci_mouse_controller->mouse_last_nonzero_report1 = xhci_mouse_controller->mouse_last_report1;
                    xhci_mouse_controller->mouse_last_nonzero_report2 = xhci_mouse_controller->mouse_last_report2;
                    xhci_mouse_controller->mouse_last_nonzero_report3 = xhci_mouse_controller->mouse_last_report3;
                    xhci_mouse_controller->mouse_last_nonzero_report4 = xhci_mouse_controller->mouse_last_report4;
                    xhci_mouse_controller->mouse_last_nonzero_wheel = wheel;
                }
                if (transferred >= 3u) {
                    mouse_apply_usb_report(xhci_mouse_report[0], dx, dy, wheel);
                    ++xhci_mouse_controller->mouse_report_count;
                }
            }
        }
    }

    if (xhci_mouse_controller != 0 &&
        xhci_mouse_slot != 0 &&
        xhci_mouse_dci != 0 &&
        xhci_mouse_report_size != 0 &&
        !xhci_mouse_pending) {
        zero_bytes(xhci_mouse_report, sizeof(xhci_mouse_report));
        xhci_enqueue_transfer_trb(xhci_mouse_slot,
                                  phys_addr(xhci_mouse_report),
                                  xhci_mouse_report_size,
                                  (XHCI_TRB_TYPE_NORMAL << XHCI_TRB_TYPE_SHIFT) |
                                  XHCI_TRB_IOC);
        xhci_ring_doorbell(xhci_mouse_controller, xhci_mouse_slot, xhci_mouse_dci);
        xhci_mouse_pending = 1;
        xhci_mouse_controller->mouse_pending = 1;
    }

    if (xhci_keyboard_controller != 0 &&
        xhci_keyboard_slot != 0 &&
        xhci_keyboard_dci != 0 &&
        xhci_keyboard_report_size != 0 &&
        !xhci_keyboard_pending) {
        zero_bytes(xhci_keyboard_report, sizeof(xhci_keyboard_report));
        xhci_enqueue_transfer_trb(xhci_keyboard_slot,
                                  phys_addr(xhci_keyboard_report),
                                  xhci_keyboard_report_size,
                                  (XHCI_TRB_TYPE_NORMAL << XHCI_TRB_TYPE_SHIFT) |
                                  XHCI_TRB_IOC);
        xhci_ring_doorbell(xhci_keyboard_controller, xhci_keyboard_slot, xhci_keyboard_dci);
        xhci_keyboard_pending = 1;
    }
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
