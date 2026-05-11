#include "kernel.h"
#include "net.h"
#include "pci.h"

#define E1000_MAX_CONTROLLERS 2u
#define E1000_RX_DESC_COUNT 32u
#define E1000_TX_DESC_COUNT 16u
#define E1000_RX_BUFFER_SIZE 2048u
#define E1000_TX_BUFFER_SIZE 2048u

#define PCI_VENDOR_INTEL 0x8086u
#define PCI_CLASS_NETWORK 0x02u
#define PCI_SUBCLASS_ETHERNET 0x00u
#define PCI_COMMAND_IO_SPACE 0x0001u
#define PCI_COMMAND_MEMORY_SPACE 0x0002u
#define PCI_COMMAND_BUS_MASTER 0x0004u
#define PCI_BAR_IO 0x00000001u
#define PCI_BAR_MEM_TYPE_MASK 0x00000006u
#define PCI_BAR_MEM_TYPE_64 0x00000004u

#define E1000_REG_CTRL 0x0000u
#define E1000_REG_STATUS 0x0008u
#define E1000_REG_EERD 0x0014u
#define E1000_REG_ICR 0x00C0u
#define E1000_REG_IMC 0x00D8u
#define E1000_REG_RCTL 0x0100u
#define E1000_REG_TCTL 0x0400u
#define E1000_REG_TIPG 0x0410u
#define E1000_REG_RAL0 0x5400u
#define E1000_REG_RAH0 0x5404u
#define E1000_REG_MTA 0x5200u
#define E1000_REG_MTA_COUNT 128u
#define E1000_REG_RDBAL 0x2800u
#define E1000_REG_RDBAH 0x2804u
#define E1000_REG_RDLEN 0x2808u
#define E1000_REG_RDH 0x2810u
#define E1000_REG_RDT 0x2818u
#define E1000_REG_TDBAL 0x3800u
#define E1000_REG_TDBAH 0x3804u
#define E1000_REG_TDLEN 0x3808u
#define E1000_REG_TDH 0x3810u
#define E1000_REG_TDT 0x3818u
#define E1000_CTRL_SLU 0x00000040u
#define E1000_CTRL_ASDE 0x00000020u
#define E1000_RAH_AV 0x80000000u
#define E1000_STATUS_LU 0x00000002u
#define E1000_RCTL_EN 0x00000002u
#define E1000_RCTL_BAM 0x00008000u
#define E1000_RCTL_SECRC 0x04000000u
#define E1000_TCTL_EN 0x00000002u
#define E1000_TCTL_PSP 0x00000008u
#define E1000_TCTL_CT_SHIFT 4u
#define E1000_TCTL_COLD_SHIFT 12u

#define E1000_RX_STATUS_DD 0x01u
#define E1000_RX_STATUS_EOP 0x02u
#define E1000_TX_CMD_EOP 0x01u
#define E1000_TX_CMD_IFCS 0x02u
#define E1000_TX_CMD_RS 0x08u
#define E1000_TX_STATUS_DD 0x01u

typedef struct __attribute__((packed)) {
    uint64_t address;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} e1000_rx_desc_t;

typedef struct __attribute__((packed)) {
    uint64_t address;
    uint16_t length;
    uint8_t checksum_offset;
    uint8_t command;
    uint8_t status;
    uint8_t checksum_start;
    uint16_t special;
} e1000_tx_desc_t;

typedef struct {
    uint64_t mmio_base;
    uint8_t mac[NET_MAC_SIZE];
    uint32_t rx_tail;
    uint32_t tx_tail;
    int net_index;
} e1000_controller_t;

static e1000_controller_t controllers[E1000_MAX_CONTROLLERS];
static uint32_t controller_count;
static e1000_rx_desc_t rx_desc[E1000_MAX_CONTROLLERS][E1000_RX_DESC_COUNT] __attribute__((aligned(4096)));
static e1000_tx_desc_t tx_desc[E1000_MAX_CONTROLLERS][E1000_TX_DESC_COUNT] __attribute__((aligned(4096)));
static uint8_t rx_buffers[E1000_MAX_CONTROLLERS][E1000_RX_DESC_COUNT][E1000_RX_BUFFER_SIZE] __attribute__((aligned(4096)));
static uint8_t tx_buffers[E1000_MAX_CONTROLLERS][E1000_TX_DESC_COUNT][E1000_TX_BUFFER_SIZE] __attribute__((aligned(4096)));

static uint32_t mmio_read32(uint64_t base, uint32_t offset) {
    volatile uint32_t *ptr = (volatile uint32_t *)(uintptr_t)(base + offset);
    return *ptr;
}

static void mmio_write32(uint64_t base, uint32_t offset, uint32_t value) {
    volatile uint32_t *ptr = (volatile uint32_t *)(uintptr_t)(base + offset);
    *ptr = value;
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

static void copy_bytes(void *dst, const void *src, uint32_t size) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    for (uint32_t i = 0; i < size; ++i) {
        d[i] = s[i];
    }
}

static uint16_t eeprom_read(e1000_controller_t *ctrl, uint8_t address) {
    uint32_t command = 1u | ((uint32_t)address << 8);

    mmio_write32(ctrl->mmio_base, E1000_REG_EERD, command);
    for (uint32_t i = 0; i < 100000u; ++i) {
        uint32_t value = mmio_read32(ctrl->mmio_base, E1000_REG_EERD);
        if (value & (1u << 4)) {
            return (uint16_t)(value >> 16);
        }
    }

    return 0;
}

static int read_mac(e1000_controller_t *ctrl) {
    uint32_t ral = mmio_read32(ctrl->mmio_base, E1000_REG_RAL0);
    uint32_t rah = mmio_read32(ctrl->mmio_base, E1000_REG_RAH0);

    if (ral != 0u || rah != 0u) {
        ctrl->mac[0] = (uint8_t)(ral & 0xFFu);
        ctrl->mac[1] = (uint8_t)((ral >> 8) & 0xFFu);
        ctrl->mac[2] = (uint8_t)((ral >> 16) & 0xFFu);
        ctrl->mac[3] = (uint8_t)((ral >> 24) & 0xFFu);
        ctrl->mac[4] = (uint8_t)(rah & 0xFFu);
        ctrl->mac[5] = (uint8_t)((rah >> 8) & 0xFFu);
        return 0;
    }

    for (uint8_t i = 0; i < 3u; ++i) {
        uint16_t word = eeprom_read(ctrl, i);
        ctrl->mac[i * 2u] = (uint8_t)(word & 0xFFu);
        ctrl->mac[i * 2u + 1u] = (uint8_t)(word >> 8);
    }

    return ctrl->mac[0] || ctrl->mac[1] || ctrl->mac[2] ||
           ctrl->mac[3] || ctrl->mac[4] || ctrl->mac[5] ? 0 : -1;
}

static void program_receive_address(e1000_controller_t *ctrl) {
    uint32_t ral = (uint32_t)ctrl->mac[0] |
                   ((uint32_t)ctrl->mac[1] << 8) |
                   ((uint32_t)ctrl->mac[2] << 16) |
                   ((uint32_t)ctrl->mac[3] << 24);
    uint32_t rah = (uint32_t)ctrl->mac[4] |
                   ((uint32_t)ctrl->mac[5] << 8) |
                   E1000_RAH_AV;

    mmio_write32(ctrl->mmio_base, E1000_REG_RAL0, ral);
    mmio_write32(ctrl->mmio_base, E1000_REG_RAH0, rah);
    for (uint32_t i = 0; i < E1000_REG_MTA_COUNT; ++i) {
        mmio_write32(ctrl->mmio_base, E1000_REG_MTA + i * 4u, 0);
    }
}

static void setup_rx(e1000_controller_t *ctrl, uint32_t slot) {
    zero_bytes(rx_desc[slot], sizeof(rx_desc[slot]));
    for (uint32_t i = 0; i < E1000_RX_DESC_COUNT; ++i) {
        rx_desc[slot][i].address = phys_addr(rx_buffers[slot][i]);
    }

    uint64_t base = phys_addr(rx_desc[slot]);
    mmio_write32(ctrl->mmio_base, E1000_REG_RDBAL, (uint32_t)(base & 0xFFFFFFFFu));
    mmio_write32(ctrl->mmio_base, E1000_REG_RDBAH, (uint32_t)(base >> 32));
    mmio_write32(ctrl->mmio_base, E1000_REG_RDLEN, sizeof(rx_desc[slot]));
    mmio_write32(ctrl->mmio_base, E1000_REG_RDH, 0);
    ctrl->rx_tail = E1000_RX_DESC_COUNT - 1u;
    mmio_write32(ctrl->mmio_base, E1000_REG_RDT, ctrl->rx_tail);
    mmio_write32(ctrl->mmio_base, E1000_REG_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_SECRC);
}

static void setup_tx(e1000_controller_t *ctrl, uint32_t slot) {
    zero_bytes(tx_desc[slot], sizeof(tx_desc[slot]));
    for (uint32_t i = 0; i < E1000_TX_DESC_COUNT; ++i) {
        tx_desc[slot][i].address = phys_addr(tx_buffers[slot][i]);
        tx_desc[slot][i].status = E1000_TX_STATUS_DD;
    }

    uint64_t base = phys_addr(tx_desc[slot]);
    mmio_write32(ctrl->mmio_base, E1000_REG_TDBAL, (uint32_t)(base & 0xFFFFFFFFu));
    mmio_write32(ctrl->mmio_base, E1000_REG_TDBAH, (uint32_t)(base >> 32));
    mmio_write32(ctrl->mmio_base, E1000_REG_TDLEN, sizeof(tx_desc[slot]));
    mmio_write32(ctrl->mmio_base, E1000_REG_TDH, 0);
    ctrl->tx_tail = 0;
    mmio_write32(ctrl->mmio_base, E1000_REG_TDT, ctrl->tx_tail);
    mmio_write32(ctrl->mmio_base, E1000_REG_TCTL,
                 E1000_TCTL_EN | E1000_TCTL_PSP | (0x10u << E1000_TCTL_CT_SHIFT) |
                 (0x40u << E1000_TCTL_COLD_SHIFT));
    mmio_write32(ctrl->mmio_base, E1000_REG_TIPG, 10u | (8u << 10) | (6u << 20));
}

static int e1000_send_frame(void *ctx, const void *data, uint32_t size) {
    e1000_controller_t *ctrl = (e1000_controller_t *)ctx;
    uint32_t slot = (uint32_t)(ctrl - controllers);
    uint32_t tail = ctrl->tx_tail;
    e1000_tx_desc_t *desc = &tx_desc[slot][tail];

    if (size > E1000_TX_BUFFER_SIZE) {
        net_record_tx_error((uint32_t)ctrl->net_index);
        return -1;
    }

    copy_bytes(tx_buffers[slot][tail], data, size);
    desc->address = phys_addr(tx_buffers[slot][tail]);
    desc->length = (uint16_t)size;
    desc->checksum_offset = 0;
    desc->command = E1000_TX_CMD_EOP | E1000_TX_CMD_IFCS | E1000_TX_CMD_RS;
    desc->status = 0;
    desc->checksum_start = 0;
    desc->special = 0;

    ctrl->tx_tail = (tail + 1u) % E1000_TX_DESC_COUNT;
    __asm__ __volatile__("mfence" ::: "memory");
    mmio_write32(ctrl->mmio_base, E1000_REG_TDT, ctrl->tx_tail);
    (void)mmio_read32(ctrl->mmio_base, E1000_REG_TDT);

    for (uint32_t i = 0; i < 10000000u; ++i) {
        if (desc->status & E1000_TX_STATUS_DD) {
            net_record_tx((uint32_t)ctrl->net_index);
            return 0;
        }
        __asm__ __volatile__("pause");
    }

    net_record_tx_error((uint32_t)ctrl->net_index);
    return -1;
}

static int e1000_poll(void *ctx) {
    e1000_controller_t *ctrl = (e1000_controller_t *)ctx;
    uint32_t slot = (uint32_t)(ctrl - controllers);
    uint32_t handled = 0;

    net_set_link((uint32_t)ctrl->net_index,
                 (mmio_read32(ctrl->mmio_base, E1000_REG_STATUS) & E1000_STATUS_LU) != 0);

    for (;;) {
        uint32_t next = (ctrl->rx_tail + 1u) % E1000_RX_DESC_COUNT;
        e1000_rx_desc_t *desc = &rx_desc[slot][next];

        if ((desc->status & E1000_RX_STATUS_DD) == 0) {
            break;
        }

        if (desc->status & E1000_RX_STATUS_EOP) {
            net_record_rx((uint32_t)ctrl->net_index);
        } else {
            net_record_rx_drop((uint32_t)ctrl->net_index);
        }

        desc->status = 0;
        desc->errors = 0;
        ctrl->rx_tail = next;
        mmio_write32(ctrl->mmio_base, E1000_REG_RDT, ctrl->rx_tail);
        ++handled;
    }

    return (int)handled;
}

static uint64_t pci_bar0(uint8_t bus, uint8_t device, uint8_t function) {
    uint32_t bar0 = pci_read_config32(bus, device, function, 0x10);

    if (bar0 & PCI_BAR_IO) {
        return 0;
    }

    if ((bar0 & PCI_BAR_MEM_TYPE_MASK) == PCI_BAR_MEM_TYPE_64) {
        uint32_t bar1 = pci_read_config32(bus, device, function, 0x14);
        return ((uint64_t)bar1 << 32) | (uint64_t)(bar0 & 0xFFFFFFF0u);
    }

    return (uint64_t)(bar0 & 0xFFFFFFF0u);
}

static void make_name(uint32_t index, char *name) {
    name[0] = 'e';
    name[1] = 't';
    name[2] = 'h';
    name[3] = (char)('0' + index);
    name[4] = '\0';
}

static void visit_pci(uint8_t bus, uint8_t device, uint8_t function, void *ctx) {
    (void)ctx;

    if (controller_count >= E1000_MAX_CONTROLLERS) {
        return;
    }

    uint16_t vendor = pci_read_config16(bus, device, function, 0x00);
    uint8_t class_code = pci_read_config8(bus, device, function, 0x0B);
    uint8_t subclass = pci_read_config8(bus, device, function, 0x0A);

    if (vendor != PCI_VENDOR_INTEL ||
        class_code != PCI_CLASS_NETWORK ||
        subclass != PCI_SUBCLASS_ETHERNET) {
        return;
    }

    uint64_t mmio_base = pci_bar0(bus, device, function);
    if (mmio_base == 0) {
        return;
    }

    uint16_t command = pci_read_config16(bus, device, function, 0x04);
    command |= PCI_COMMAND_MEMORY_SPACE | PCI_COMMAND_BUS_MASTER;
    command &= (uint16_t)~PCI_COMMAND_IO_SPACE;
    pci_write_config32(bus, device, function, 0x04,
        (pci_read_config32(bus, device, function, 0x04) & 0xFFFF0000u) | command);

    uint32_t index = controller_count;
    e1000_controller_t *ctrl = &controllers[index];
    zero_bytes(ctrl, sizeof(*ctrl));
    ctrl->mmio_base = mmio_base;
    ctrl->net_index = -1;

    mmio_write32(ctrl->mmio_base, E1000_REG_IMC, 0xFFFFFFFFu);
    (void)mmio_read32(ctrl->mmio_base, E1000_REG_ICR);
    mmio_write32(ctrl->mmio_base, E1000_REG_CTRL,
                 mmio_read32(ctrl->mmio_base, E1000_REG_CTRL) | E1000_CTRL_SLU | E1000_CTRL_ASDE);

    if (read_mac(ctrl) != 0) {
        return;
    }

    program_receive_address(ctrl);
    setup_rx(ctrl, index);
    setup_tx(ctrl, index);

    char name[8];
    make_name(index, name);
    int net_index = net_register_device(name,
                                        ctrl->mac,
                                        (mmio_read32(ctrl->mmio_base, E1000_REG_STATUS) & E1000_STATUS_LU) != 0,
                                        e1000_send_frame,
                                        e1000_poll,
                                        ctrl);
    if (net_index < 0) {
        return;
    }

    ctrl->net_index = net_index;
    ++controller_count;
}

void e1000_init(void) {
    controller_count = 0;
    pci_scan(visit_pci, 0);
}
