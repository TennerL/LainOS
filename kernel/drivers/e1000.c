#include "kernel.h"
#include "dma.h"
#include "net.h"
#include "pci.h"

#define E1000_MAX_CONTROLLERS 2u
#define E1000_RX_DESC_COUNT 32u
#define E1000_TX_DESC_COUNT 16u
#define E1000_RX_BUFFER_SIZE 2048u
#define E1000_TX_BUFFER_SIZE 2048u
#define E1000_RX_RING_BYTES (sizeof(e1000_rx_desc_t) * E1000_RX_DESC_COUNT)
#define E1000_TX_RING_BYTES (sizeof(e1000_tx_desc_t) * E1000_TX_DESC_COUNT)

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
#define E1000_REG_CTRL_EXT 0x0018u
#define E1000_REG_MDIC 0x0020u
#define E1000_REG_ICR 0x00C0u
#define E1000_REG_IMC 0x00D8u
#define E1000_REG_RCTL 0x0100u
#define E1000_REG_TCTL 0x0400u
#define E1000_REG_TIPG 0x0410u
#define E1000_REG_IOSFPC 0x0F28u
#define E1000_REG_PBA 0x1000u
#define E1000_REG_GCR 0x5B00u
#define E1000_REG_H2ME 0x5B50u
#define E1000_REG_FWSM 0x5B54u
#define E1000_REG_RAL0 0x5400u
#define E1000_REG_RAH0 0x5404u
#define E1000_REG_MTA 0x5200u
#define E1000_REG_MTA_COUNT 128u
#define E1000_REG_RDBAL 0x2800u
#define E1000_REG_RDBAH 0x2804u
#define E1000_REG_RDLEN 0x2808u
#define E1000_REG_RDH 0x2810u
#define E1000_REG_RDT 0x2818u
#define E1000_REG_RXDCTL 0x2828u
#define E1000_REG_TDBAL 0x3800u
#define E1000_REG_TDBAH 0x3804u
#define E1000_REG_TDLEN 0x3808u
#define E1000_REG_TDH 0x3810u
#define E1000_REG_TDT 0x3818u
#define E1000_REG_TXDCTL 0x3828u
#define E1000_REG_TARC0 0x3840u
#define E1000_REG_TXDCTL1 0x3928u
#define E1000_REG_TARC1 0x3940u
#define E1000_CTRL_FD 0x00000001u
#define E1000_CTRL_GIO_MASTER_DISABLE 0x00000004u
#define E1000_CTRL_SLU 0x00000040u
#define E1000_CTRL_ASDE 0x00000020u
#define E1000_CTRL_RST 0x04000000u
#define E1000_CTRL_PHY_RST 0x80000000u
#define E1000_CTRL_EXT_DRV_LOAD 0x10000000u
#define E1000_CTRL_EXT_DPG_EN 0x00000008u
#define E1000_CTRL_EXT_RO_DIS 0x00020000u
#define E1000_CTRL_EXT_PHYPDEN 0x00100000u
#define E1000_RAH_AV 0x80000000u
#define E1000_STATUS_LU 0x00000002u
#define E1000_STATUS_GIO_MASTER_ENABLE 0x00080000u
#define E1000_RCTL_EN 0x00000002u
#define E1000_RCTL_SBP 0x00000004u
#define E1000_RCTL_UPE 0x00000008u
#define E1000_RCTL_MPE 0x00000010u
#define E1000_RCTL_LPE 0x00000020u
#define E1000_RCTL_BAM 0x00008000u
#define E1000_RCTL_BSIZE_2048 0x00000000u
#define E1000_RCTL_SECRC 0x04000000u
#define E1000_TCTL_EN 0x00000002u
#define E1000_TCTL_PSP 0x00000008u
#define E1000_TCTL_CT_MASK 0x00000FF0u
#define E1000_TCTL_COLD_MASK 0x003FF000u
#define E1000_TCTL_CT_SHIFT 4u
#define E1000_TCTL_COLD_SHIFT 12u
#define E1000_TCTL_RTLC 0x01000000u
#define E1000_TCTL_MULR 0x10000000u
#define E1000_DCTL_ENABLE 0x02000000u
#define E1000_DCTL_GRAN 0x01000000u
#define E1000_TXDCTL_COUNT_DESC 0x00400000u
#define E1000_TXDCTL_PTHRESH 0x0000003Fu
#define E1000_TXDCTL_HTHRESH 0x00003F00u
#define E1000_TXDCTL_WTHRESH 0x003F0000u
#define E1000_RXDCTL_PREFETCH_THRESH 0x20u
#define E1000_RXDCTL_HOST_THRESH (4u << 8)
#define E1000_RXDCTL_WRITEBACK_THRESH (4u << 16)
#define E1000_TXDCTL_PREFETCH_THRESH 0x1Fu
#define E1000_TXDCTL_HOST_THRESH (1u << 8)
#define E1000_TXDCTL_WRITEBACK_THRESH (1u << 16)
#define E1000_IOSFPC_RDMTS_HEX 0x00010000u
#define E1000_TARC0_CB_MULTIQ_3_REQ 0x30000000u
#define E1000_TARC0_CB_MULTIQ_2_REQ 0x20000000u
#define E1000_PBA_26K 26u
#define E1000_FWSM_FW_VALID 0x00008000u
#define E1000_FWSM_ULP_CFG_DONE 0x00000400u
#define E1000_H2ME_ULP 0x00000800u
#define E1000_H2ME_ENFORCE_SETTINGS 0x00001000u
#define E1000_MDIC_REG_SHIFT 16u
#define E1000_MDIC_PHY_SHIFT 21u
#define E1000_MDIC_OP_WRITE 0x04000000u
#define E1000_MDIC_OP_READ 0x08000000u
#define E1000_MDIC_READY 0x10000000u
#define E1000_MDIC_ERROR 0x40000000u
#define E1000_PHY_PAGE_SELECT 31u
#define E1000_PHY_PAGE_SHIFT 5u
#define E1000_PHY_PAGE_HV_PM 770u
#define E1000_PHY_REG_HV_PM_CTRL 17u
#define E1000_PHY_PAGE_I218_ULP 779u
#define E1000_PHY_REG_I218_ULP_CONFIG1 16u
#define E1000_HV_PM_CTRL_K1_CLK_REQ 0x0200u
#define E1000_HV_PM_CTRL_K1_ENABLE 0x4000u
#define E1000_GCR_RXD_NO_SNOOP 0x00000001u
#define E1000_GCR_RXDSCW_NO_SNOOP 0x00000002u
#define E1000_GCR_RXDSCR_NO_SNOOP 0x00000004u
#define E1000_GCR_TXD_NO_SNOOP 0x00000008u
#define E1000_GCR_TXDSCW_NO_SNOOP 0x00000010u
#define E1000_GCR_TXDSCR_NO_SNOOP 0x00000020u
#define E1000_GCR_NO_SNOOP_ALL (E1000_GCR_RXD_NO_SNOOP | \
                                E1000_GCR_RXDSCW_NO_SNOOP | \
                                E1000_GCR_RXDSCR_NO_SNOOP | \
                                E1000_GCR_TXD_NO_SNOOP | \
                                E1000_GCR_TXDSCW_NO_SNOOP | \
                                E1000_GCR_TXDSCR_NO_SNOOP)
#define E1000_I218_ULP_CONFIG1_START 0x0001u
#define E1000_I218_ULP_CONFIG1_IND 0x0004u
#define E1000_I218_ULP_CONFIG1_STICKY_ULP 0x0010u
#define E1000_I218_ULP_CONFIG1_INBAND_EXIT 0x0020u
#define E1000_I218_ULP_CONFIG1_WOL_HOST 0x0040u
#define E1000_I218_ULP_CONFIG1_RESET_TO_SMBUS 0x0100u
#define E1000_I218_ULP_CONFIG1_EN_ULP_LANPHYPC 0x0400u
#define E1000_I218_ULP_CONFIG1_DIS_CLR_STICKY_ON_PERST 0x0800u
#define E1000_I218_ULP_CONFIG1_DISABLE_SMB_PERST 0x1000u

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
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint32_t rx_tail;
    uint32_t tx_tail;
    uint32_t last_rx_status;
    uint32_t last_rx_errors;
    uint32_t last_rx_length;
    uint64_t last_rx_desc_addr;
    uint8_t last_rx_bytes[16];
    uint32_t last_tx_status;
    uint32_t last_tx_command;
    uint32_t last_tx_length;
    uint64_t last_tx_desc_addr;
    uint64_t last_tx_desc_phys;
    uint64_t last_tx_buffer_phys;
    uint8_t last_tx_desc_bytes[16];
    uint32_t phy_result;
    uint16_t phy_pm_ctrl;
    uint16_t phy_ulp_cfg;
    uint32_t reset_result;
    int net_index;
} e1000_controller_t;

typedef struct {
    uint16_t device_id;
    const char *name;
} e1000_supported_device_t;

typedef struct {
    uint16_t device_id;
    const char *name;
    const char *needed_driver;
} intel_unsupported_device_t;

typedef struct {
    uint16_t vendor_id;
    uint16_t device_id;
    const char *name;
    const char *needed_driver;
} intel_unsupported_seen_t;

static const e1000_supported_device_t supported_devices[] = {
    { 0x100Eu, "82540EM" },
    { 0x100Fu, "82545EM" },
    { 0x1019u, "82547EI" },
    { 0x1026u, "82545GM" },
    { 0x1027u, "82545GM" },
    { 0x105Eu, "82571EB" },
    { 0x105Fu, "82571EB" },
    { 0x107Du, "82572EI" },
    { 0x10D3u, "82574L" },
    { 0x10EAu, "82577LM" },
    { 0x10EBu, "82577LC" },
    { 0x1502u, "82579LM" },
    { 0x1503u, "82579V" },
    { 0x153Au, "I217-LM" },
    { 0x153Bu, "I217-V" },
    { 0x155Au, "I218-LM" },
    { 0x1559u, "I218-V" },
    { 0x15A0u, "I218-LM" },
    { 0x15A1u, "I218-V" },
    { 0x15A2u, "I218-LM" },
    { 0x15A3u, "I218-V" },
    { 0x15B7u, "I219-LM" },
    { 0x15B8u, "I219-V" },
    { 0x15D6u, "I219-V" },
    { 0x15D7u, "I219-LM" },
    { 0x15D8u, "I219-V" },
};

static const intel_unsupported_device_t unsupported_devices[] = {
    { 0x1588u, "XL710/700-series", "i40e" },
};

static e1000_controller_t controllers[E1000_MAX_CONTROLLERS];
static uint32_t controller_count;
static intel_unsupported_seen_t unsupported_seen[4];
static uint32_t unsupported_seen_count;
static e1000_rx_desc_t *rx_desc[E1000_MAX_CONTROLLERS];
static e1000_tx_desc_t *tx_desc[E1000_MAX_CONTROLLERS];
static uint8_t *rx_buffers[E1000_MAX_CONTROLLERS];
static uint8_t *tx_buffers[E1000_MAX_CONTROLLERS];

static void pci_force_power_d0(uint8_t bus, uint8_t device, uint8_t function);

static const char *supported_device_name(uint16_t device_id) {
    for (uint32_t i = 0; i < sizeof(supported_devices) / sizeof(supported_devices[0]); ++i) {
        if (supported_devices[i].device_id == device_id) {
            return supported_devices[i].name;
        }
    }

    return 0;
}

static const intel_unsupported_device_t *unsupported_device_info(uint16_t device_id) {
    for (uint32_t i = 0; i < sizeof(unsupported_devices) / sizeof(unsupported_devices[0]); ++i) {
        if (unsupported_devices[i].device_id == device_id) {
            return &unsupported_devices[i];
        }
    }

    return 0;
}

static void record_unsupported(uint16_t vendor_id, uint16_t device_id) {
    const intel_unsupported_device_t *info = unsupported_device_info(device_id);

    if (!info || unsupported_seen_count >= sizeof(unsupported_seen) / sizeof(unsupported_seen[0])) {
        return;
    }

    for (uint32_t i = 0; i < unsupported_seen_count; ++i) {
        if (unsupported_seen[i].vendor_id == vendor_id && unsupported_seen[i].device_id == device_id) {
            return;
        }
    }

    unsupported_seen[unsupported_seen_count].vendor_id = vendor_id;
    unsupported_seen[unsupported_seen_count].device_id = device_id;
    unsupported_seen[unsupported_seen_count].name = info->name;
    unsupported_seen[unsupported_seen_count].needed_driver = info->needed_driver;
    ++unsupported_seen_count;
}

static uint32_t mmio_read32(uint64_t base, uint32_t offset) {
    volatile uint32_t *ptr = (volatile uint32_t *)(uintptr_t)(base + offset);
    return *ptr;
}

static void mmio_write32(uint64_t base, uint32_t offset, uint32_t value) {
    volatile uint32_t *ptr = (volatile uint32_t *)(uintptr_t)(base + offset);
    *ptr = value;
}

static uint64_t phys_addr(const void *ptr) {
    return dma_phys(ptr);
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

static void quiesce_controller(e1000_controller_t *ctrl) {
    mmio_write32(ctrl->mmio_base, E1000_REG_RCTL, 0);
    mmio_write32(ctrl->mmio_base, E1000_REG_TCTL, 0);
    (void)mmio_read32(ctrl->mmio_base, E1000_REG_STATUS);
    mmio_write32(ctrl->mmio_base, E1000_REG_IMC, 0xFFFFFFFFu);
    (void)mmio_read32(ctrl->mmio_base, E1000_REG_ICR);
}

static void mark_driver_loaded(e1000_controller_t *ctrl) {
    uint32_t ctrl_ext = mmio_read32(ctrl->mmio_base, E1000_REG_CTRL_EXT);

    ctrl_ext |= E1000_CTRL_EXT_DRV_LOAD;
    mmio_write32(ctrl->mmio_base, E1000_REG_CTRL_EXT, ctrl_ext);
    (void)mmio_read32(ctrl->mmio_base, E1000_REG_CTRL_EXT);
}

static void configure_pcie_dma_behavior(e1000_controller_t *ctrl) {
    uint32_t value;

    value = mmio_read32(ctrl->mmio_base, E1000_REG_GCR);
    value &= ~E1000_GCR_NO_SNOOP_ALL;
    mmio_write32(ctrl->mmio_base, E1000_REG_GCR, value);

    value = mmio_read32(ctrl->mmio_base, E1000_REG_CTRL_EXT);
    value |= E1000_CTRL_EXT_RO_DIS | E1000_CTRL_EXT_PHYPDEN | (1u << 22);
    value &= ~E1000_CTRL_EXT_DPG_EN;
    mmio_write32(ctrl->mmio_base, E1000_REG_CTRL_EXT, value);
    (void)mmio_read32(ctrl->mmio_base, E1000_REG_CTRL_EXT);
}

static void exit_ulp_with_firmware(e1000_controller_t *ctrl) {
    uint32_t fwsm = mmio_read32(ctrl->mmio_base, E1000_REG_FWSM);
    uint32_t h2me;

    if ((fwsm & E1000_FWSM_FW_VALID) == 0) {
        return;
    }

    h2me = mmio_read32(ctrl->mmio_base, E1000_REG_H2ME);
    h2me &= ~E1000_H2ME_ULP;
    h2me |= E1000_H2ME_ENFORCE_SETTINGS;
    mmio_write32(ctrl->mmio_base, E1000_REG_H2ME, h2me);

    for (uint32_t i = 0; i < 2500000u; ++i) {
        if ((mmio_read32(ctrl->mmio_base, E1000_REG_FWSM) & E1000_FWSM_ULP_CFG_DONE) == 0) {
            break;
        }
        __asm__ __volatile__("pause");
    }

    h2me = mmio_read32(ctrl->mmio_base, E1000_REG_H2ME);
    h2me &= ~E1000_H2ME_ENFORCE_SETTINGS;
    h2me &= ~E1000_H2ME_ULP;
    mmio_write32(ctrl->mmio_base, E1000_REG_H2ME, h2me);
    (void)mmio_read32(ctrl->mmio_base, E1000_REG_H2ME);
}

static int mdic_wait(e1000_controller_t *ctrl, uint32_t *value) {
    for (uint32_t i = 0; i < 100000u; ++i) {
        uint32_t mdic = mmio_read32(ctrl->mmio_base, E1000_REG_MDIC);
        if (mdic & E1000_MDIC_READY) {
            if (value) {
                *value = mdic;
            }
            return (mdic & E1000_MDIC_ERROR) ? -1 : 0;
        }
        __asm__ __volatile__("pause");
    }

    return -1;
}

static int mdic_read(e1000_controller_t *ctrl, uint8_t phy, uint8_t reg, uint16_t *out) {
    uint32_t mdic;

    mmio_write32(ctrl->mmio_base,
                 E1000_REG_MDIC,
                 ((uint32_t)reg << E1000_MDIC_REG_SHIFT) |
                 ((uint32_t)phy << E1000_MDIC_PHY_SHIFT) |
                 E1000_MDIC_OP_READ);
    if (mdic_wait(ctrl, &mdic) != 0) {
        return -1;
    }

    if (out) {
        *out = (uint16_t)(mdic & 0xFFFFu);
    }
    return 0;
}

static int mdic_write(e1000_controller_t *ctrl, uint8_t phy, uint8_t reg, uint16_t data) {
    mmio_write32(ctrl->mmio_base,
                 E1000_REG_MDIC,
                 (uint32_t)data |
                 ((uint32_t)reg << E1000_MDIC_REG_SHIFT) |
                 ((uint32_t)phy << E1000_MDIC_PHY_SHIFT) |
                 E1000_MDIC_OP_WRITE);
    return mdic_wait(ctrl, 0);
}

static int hv_phy_select_page(e1000_controller_t *ctrl, uint16_t page) {
    return mdic_write(ctrl,
                      1u,
                      E1000_PHY_PAGE_SELECT,
                      (uint16_t)(page << E1000_PHY_PAGE_SHIFT));
}

static int hv_phy_read(e1000_controller_t *ctrl, uint16_t page, uint8_t reg, uint16_t *out) {
    if (hv_phy_select_page(ctrl, page) != 0) {
        return -1;
    }
    return mdic_read(ctrl, 1u, reg, out);
}

static int hv_phy_write(e1000_controller_t *ctrl, uint16_t page, uint8_t reg, uint16_t data) {
    if (hv_phy_select_page(ctrl, page) != 0) {
        return -1;
    }
    return mdic_write(ctrl, 1u, reg, data);
}

static void exit_phy_low_power(e1000_controller_t *ctrl) {
    uint16_t value;

    ctrl->phy_result = 0;
    ctrl->phy_pm_ctrl = 0xFFFFu;
    ctrl->phy_ulp_cfg = 0xFFFFu;

    if (hv_phy_read(ctrl, E1000_PHY_PAGE_HV_PM, E1000_PHY_REG_HV_PM_CTRL, &value) != 0) {
        ctrl->phy_result |= 1u;
        return;
    }
    ctrl->phy_pm_ctrl = value;
    value &= (uint16_t)~(E1000_HV_PM_CTRL_K1_ENABLE | E1000_HV_PM_CTRL_K1_CLK_REQ);
    if (hv_phy_write(ctrl, E1000_PHY_PAGE_HV_PM, E1000_PHY_REG_HV_PM_CTRL, value) != 0) {
        ctrl->phy_result |= 2u;
        return;
    }
    ctrl->phy_pm_ctrl = value;

    if (hv_phy_read(ctrl, E1000_PHY_PAGE_I218_ULP, E1000_PHY_REG_I218_ULP_CONFIG1, &value) != 0) {
        ctrl->phy_result |= 4u;
        return;
    }
    ctrl->phy_ulp_cfg = value;
    value &= (uint16_t)~(E1000_I218_ULP_CONFIG1_IND |
                         E1000_I218_ULP_CONFIG1_STICKY_ULP |
                         E1000_I218_ULP_CONFIG1_RESET_TO_SMBUS |
                         E1000_I218_ULP_CONFIG1_WOL_HOST |
                         E1000_I218_ULP_CONFIG1_INBAND_EXIT |
                         E1000_I218_ULP_CONFIG1_EN_ULP_LANPHYPC |
                         E1000_I218_ULP_CONFIG1_DIS_CLR_STICKY_ON_PERST |
                         E1000_I218_ULP_CONFIG1_DISABLE_SMB_PERST);
    if (hv_phy_write(ctrl, E1000_PHY_PAGE_I218_ULP, E1000_PHY_REG_I218_ULP_CONFIG1, value) != 0) {
        ctrl->phy_result |= 8u;
        return;
    }
    value |= E1000_I218_ULP_CONFIG1_START;
    if (hv_phy_write(ctrl, E1000_PHY_PAGE_I218_ULP, E1000_PHY_REG_I218_ULP_CONFIG1, value) != 0) {
        ctrl->phy_result |= 16u;
        return;
    }
    ctrl->phy_ulp_cfg = value;
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

static uint8_t *rx_buffer(uint32_t slot, uint32_t index) {
    return rx_buffers[slot] + index * E1000_RX_BUFFER_SIZE;
}

static uint8_t *tx_buffer(uint32_t slot, uint32_t index) {
    return tx_buffers[slot] + index * E1000_TX_BUFFER_SIZE;
}

static int allocate_dma_objects(uint32_t slot) {
    rx_desc[slot] = (e1000_rx_desc_t *)dma_alloc(E1000_RX_RING_BYTES, 4096u);
    tx_desc[slot] = (e1000_tx_desc_t *)dma_alloc(E1000_TX_RING_BYTES, 4096u);
    rx_buffers[slot] = (uint8_t *)dma_alloc(E1000_RX_BUFFER_SIZE * E1000_RX_DESC_COUNT, 4096u);
    tx_buffers[slot] = (uint8_t *)dma_alloc(E1000_TX_BUFFER_SIZE * E1000_TX_DESC_COUNT, 4096u);

    return rx_desc[slot] && tx_desc[slot] && rx_buffers[slot] && tx_buffers[slot] ? 0 : -1;
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
    zero_bytes(rx_desc[slot], E1000_RX_RING_BYTES);
    zero_bytes(rx_buffers[slot], E1000_RX_BUFFER_SIZE * E1000_RX_DESC_COUNT);
    for (uint32_t i = 0; i < E1000_RX_DESC_COUNT; ++i) {
        rx_desc[slot][i].address = phys_addr(rx_buffer(slot, i));
    }

    uint64_t base = phys_addr(rx_desc[slot]);
    mmio_write32(ctrl->mmio_base, E1000_REG_RDBAL, (uint32_t)(base & 0xFFFFFFFFu));
    mmio_write32(ctrl->mmio_base, E1000_REG_RDBAH, (uint32_t)(base >> 32));
    mmio_write32(ctrl->mmio_base, E1000_REG_RDLEN, E1000_RX_RING_BYTES);
    mmio_write32(ctrl->mmio_base, E1000_REG_RDH, 0);
    ctrl->rx_tail = E1000_RX_DESC_COUNT - 1u;
    mmio_write32(ctrl->mmio_base, E1000_REG_RDT, ctrl->rx_tail);
    mmio_write32(ctrl->mmio_base,
                 E1000_REG_RXDCTL,
                 E1000_DCTL_ENABLE |
                 E1000_DCTL_GRAN |
                 E1000_RXDCTL_WRITEBACK_THRESH |
                 E1000_RXDCTL_HOST_THRESH |
                 E1000_RXDCTL_PREFETCH_THRESH);
    (void)mmio_read32(ctrl->mmio_base, E1000_REG_RXDCTL);
    mmio_write32(ctrl->mmio_base,
                 E1000_REG_RCTL,
                 E1000_RCTL_EN |
                 E1000_RCTL_SBP |
                 E1000_RCTL_UPE |
                 E1000_RCTL_MPE |
                 E1000_RCTL_LPE |
                 E1000_RCTL_BAM |
                 E1000_RCTL_BSIZE_2048 |
                 E1000_RCTL_SECRC);
}

static void setup_packet_buffer(e1000_controller_t *ctrl) {
    mmio_write32(ctrl->mmio_base, E1000_REG_PBA, E1000_PBA_26K);
    (void)mmio_read32(ctrl->mmio_base, E1000_REG_PBA);
}

static void setup_tx(e1000_controller_t *ctrl, uint32_t slot) {
    uint32_t txdctl;
    uint32_t tctl;

    zero_bytes(tx_desc[slot], E1000_TX_RING_BYTES);
    zero_bytes(tx_buffers[slot], E1000_TX_BUFFER_SIZE * E1000_TX_DESC_COUNT);
    for (uint32_t i = 0; i < E1000_TX_DESC_COUNT; ++i) {
        tx_desc[slot][i].address = phys_addr(tx_buffer(slot, i));
        tx_desc[slot][i].status = E1000_TX_STATUS_DD;
    }

    uint64_t base = phys_addr(tx_desc[slot]);
    mmio_write32(ctrl->mmio_base, E1000_REG_TDBAL, (uint32_t)(base & 0xFFFFFFFFu));
    mmio_write32(ctrl->mmio_base, E1000_REG_TDBAH, (uint32_t)(base >> 32));
    mmio_write32(ctrl->mmio_base, E1000_REG_TDLEN, E1000_TX_RING_BYTES);
    mmio_write32(ctrl->mmio_base, E1000_REG_TDH, 0);
    ctrl->tx_tail = 0;
    mmio_write32(ctrl->mmio_base, E1000_REG_TDT, ctrl->tx_tail);

    txdctl = mmio_read32(ctrl->mmio_base, E1000_REG_TXDCTL);
    txdctl &= ~(E1000_TXDCTL_PTHRESH | E1000_TXDCTL_HTHRESH | E1000_TXDCTL_WTHRESH);
    txdctl |= E1000_DCTL_ENABLE |
              E1000_DCTL_GRAN |
              E1000_TXDCTL_COUNT_DESC |
              E1000_TXDCTL_WRITEBACK_THRESH |
              E1000_TXDCTL_HOST_THRESH |
              E1000_TXDCTL_PREFETCH_THRESH;
    mmio_write32(ctrl->mmio_base, E1000_REG_TXDCTL, txdctl);
    (void)mmio_read32(ctrl->mmio_base, E1000_REG_TXDCTL);
    mmio_write32(ctrl->mmio_base, E1000_REG_TXDCTL1, txdctl);

    if (ctrl->device_id == 0x15B8u) {
        uint32_t value = mmio_read32(ctrl->mmio_base, E1000_REG_IOSFPC);
        value |= E1000_IOSFPC_RDMTS_HEX;
        mmio_write32(ctrl->mmio_base, E1000_REG_IOSFPC, value);

        value = mmio_read32(ctrl->mmio_base, E1000_REG_TARC0);
        value &= ~E1000_TARC0_CB_MULTIQ_3_REQ;
        value |= E1000_TARC0_CB_MULTIQ_2_REQ;
        mmio_write32(ctrl->mmio_base, E1000_REG_TARC0, value);
    }

    tctl = mmio_read32(ctrl->mmio_base, E1000_REG_TCTL);
    tctl &= ~(E1000_TCTL_CT_MASK | E1000_TCTL_COLD_MASK);
    tctl |= E1000_TCTL_EN |
            E1000_TCTL_PSP |
            E1000_TCTL_RTLC |
            (0x0Fu << E1000_TCTL_CT_SHIFT) |
            (0x3Fu << E1000_TCTL_COLD_SHIFT);
    mmio_write32(ctrl->mmio_base, E1000_REG_TCTL, tctl);
    mmio_write32(ctrl->mmio_base, E1000_REG_TIPG, 10u | (8u << 10) | (6u << 20));
}

static int reinitialize_controller(e1000_controller_t *ctrl, uint32_t slot) {
    uint16_t command;

    pci_force_power_d0(ctrl->bus, ctrl->device, ctrl->function);
    command = pci_read_config16(ctrl->bus, ctrl->device, ctrl->function, 0x04);
    command |= PCI_COMMAND_MEMORY_SPACE | PCI_COMMAND_BUS_MASTER;
    command &= (uint16_t)~PCI_COMMAND_IO_SPACE;
    pci_write_config32(ctrl->bus, ctrl->device, ctrl->function, 0x04,
        (pci_read_config32(ctrl->bus, ctrl->device, ctrl->function, 0x04) & 0xFFFF0000u) | command);

    quiesce_controller(ctrl);
    mark_driver_loaded(ctrl);
    configure_pcie_dma_behavior(ctrl);
    exit_ulp_with_firmware(ctrl);
    exit_phy_low_power(ctrl);
    mmio_write32(ctrl->mmio_base, E1000_REG_CTRL,
                 (mmio_read32(ctrl->mmio_base, E1000_REG_CTRL) &
                  ~E1000_CTRL_GIO_MASTER_DISABLE) |
                 E1000_CTRL_FD |
                 E1000_CTRL_SLU |
                 E1000_CTRL_ASDE);

    if (read_mac(ctrl) != 0) {
        return -1;
    }

    program_receive_address(ctrl);
    setup_packet_buffer(ctrl);
    setup_rx(ctrl, slot);
    setup_tx(ctrl, slot);
    if (ctrl->net_index >= 0) {
        net_set_link((uint32_t)ctrl->net_index,
                     (mmio_read32(ctrl->mmio_base, E1000_REG_STATUS) & E1000_STATUS_LU) != 0);
    }
    return 0;
}

static int e1000_send_frame(void *ctx, const void *data, uint32_t size) {
    e1000_controller_t *ctrl = (e1000_controller_t *)ctx;
    uint32_t slot = (uint32_t)(ctrl - controllers);
    uint32_t tail = ctrl->tx_tail;
    volatile e1000_tx_desc_t *desc = &tx_desc[slot][tail];

    if (size > E1000_TX_BUFFER_SIZE) {
        net_record_tx_error((uint32_t)ctrl->net_index);
        return -1;
    }

    copy_bytes(tx_buffer(slot, tail), data, size);
    desc->address = phys_addr(tx_buffer(slot, tail));
    desc->length = (uint16_t)size;
    desc->checksum_offset = 0;
    desc->command = E1000_TX_CMD_EOP | E1000_TX_CMD_IFCS | E1000_TX_CMD_RS;
    desc->status = 0;
    desc->checksum_start = 0;
    desc->special = 0;
    ctrl->last_tx_status = desc->status;
    ctrl->last_tx_command = desc->command;
    ctrl->last_tx_length = desc->length;
    ctrl->last_tx_desc_addr = desc->address;
    ctrl->last_tx_desc_phys = phys_addr((const void *)desc);
    ctrl->last_tx_buffer_phys = phys_addr(tx_buffer(slot, tail));
    for (uint32_t i = 0; i < sizeof(ctrl->last_tx_desc_bytes); ++i) {
        ctrl->last_tx_desc_bytes[i] = ((volatile uint8_t *)desc)[i];
    }

    ctrl->tx_tail = (tail + 1u) % E1000_TX_DESC_COUNT;
    __asm__ __volatile__("mfence" ::: "memory");
    mmio_write32(ctrl->mmio_base, E1000_REG_TDT, ctrl->tx_tail);
    (void)mmio_read32(ctrl->mmio_base, E1000_REG_TDT);

    for (uint32_t i = 0; i < 10000000u; ++i) {
        if (desc->status & E1000_TX_STATUS_DD) {
            ctrl->last_tx_status = desc->status;
            net_record_tx((uint32_t)ctrl->net_index);
            return 0;
        }
        __asm__ __volatile__("pause");
    }

    net_record_tx_error((uint32_t)ctrl->net_index);
    ctrl->last_tx_status = desc->status;
    for (uint32_t i = 0; i < sizeof(ctrl->last_tx_desc_bytes); ++i) {
        ctrl->last_tx_desc_bytes[i] = ((volatile uint8_t *)desc)[i];
    }
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
        volatile e1000_rx_desc_t *desc = &rx_desc[slot][next];

        if ((desc->status & E1000_RX_STATUS_DD) == 0) {
            uint32_t found = E1000_RX_DESC_COUNT;

            for (uint32_t i = 0; i < E1000_RX_DESC_COUNT; ++i) {
                if (((volatile e1000_rx_desc_t *)&rx_desc[slot][i])->status & E1000_RX_STATUS_DD) {
                    found = i;
                    break;
                }
            }

            if (found == E1000_RX_DESC_COUNT) {
                break;
            }

            next = found;
            desc = &rx_desc[slot][next];
        }

        if (desc->status & E1000_RX_STATUS_EOP) {
            ctrl->last_rx_status = desc->status;
            ctrl->last_rx_errors = desc->errors;
            ctrl->last_rx_length = desc->length;
            ctrl->last_rx_desc_addr = desc->address;
            for (uint32_t i = 0; i < sizeof(ctrl->last_rx_bytes); ++i) {
                ctrl->last_rx_bytes[i] = rx_buffer(slot, next)[i];
            }
            net_record_rx((uint32_t)ctrl->net_index);
            net_receive_frame((uint32_t)ctrl->net_index,
                              rx_buffer(slot, next),
                              desc->length);
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

static uint8_t pci_find_capability(uint8_t bus, uint8_t device, uint8_t function, uint8_t cap_id) {
    uint16_t status = pci_read_config16(bus, device, function, 0x06);
    if ((status & 0x0010u) == 0) {
        return 0;
    }

    uint8_t ptr = pci_read_config8(bus, device, function, 0x34) & 0xFCu;
    for (uint32_t i = 0; i < 48u && ptr >= 0x40u; ++i) {
        uint8_t id = pci_read_config8(bus, device, function, ptr);
        uint8_t next = pci_read_config8(bus, device, function, ptr + 1u) & 0xFCu;
        if (id == cap_id) {
            return ptr;
        }
        ptr = next;
    }

    return 0;
}

static uint16_t pci_pmcsr(uint8_t bus, uint8_t device, uint8_t function) {
    uint8_t pm = pci_find_capability(bus, device, function, 0x01u);
    return pm ? pci_read_config16(bus, device, function, (uint8_t)(pm + 4u)) : 0xFFFFu;
}

static void pci_force_power_d0(uint8_t bus, uint8_t device, uint8_t function) {
    uint8_t pm = pci_find_capability(bus, device, function, 0x01u);
    if (!pm) {
        return;
    }

    uint16_t pmcsr = pci_read_config16(bus, device, function, (uint8_t)(pm + 4u));
    pmcsr &= (uint16_t)~0x0003u;
    pci_write_config16(bus, device, function, (uint8_t)(pm + 4u), pmcsr);
    for (uint32_t i = 0; i < 100000u; ++i) {
        __asm__ __volatile__("pause");
    }
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

    uint16_t vendor = pci_read_config16(bus, device, function, 0x00);
    uint16_t device_id = pci_read_config16(bus, device, function, 0x02);
    uint8_t class_code = pci_read_config8(bus, device, function, 0x0B);
    uint8_t subclass = pci_read_config8(bus, device, function, 0x0A);
    const char *device_name = supported_device_name(device_id);

    if (vendor != PCI_VENDOR_INTEL ||
        class_code != PCI_CLASS_NETWORK ||
        subclass != PCI_SUBCLASS_ETHERNET) {
        return;
    }

    if (!device_name) {
        record_unsupported(vendor, device_id);
        return;
    }

    if (controller_count >= E1000_MAX_CONTROLLERS) {
        return;
    }

    uint64_t mmio_base = pci_bar0(bus, device, function);
    if (mmio_base == 0) {
        return;
    }

    pci_force_power_d0(bus, device, function);

    uint16_t command = pci_read_config16(bus, device, function, 0x04);
    command |= PCI_COMMAND_MEMORY_SPACE | PCI_COMMAND_BUS_MASTER;
    command &= (uint16_t)~PCI_COMMAND_IO_SPACE;
    pci_write_config32(bus, device, function, 0x04,
        (pci_read_config32(bus, device, function, 0x04) & 0xFFFF0000u) | command);

    uint32_t index = controller_count;
    e1000_controller_t *ctrl = &controllers[index];
    zero_bytes(ctrl, sizeof(*ctrl));
    ctrl->mmio_base = mmio_base;
    ctrl->bus = bus;
    ctrl->device = device;
    ctrl->function = function;
    ctrl->vendor_id = vendor;
    ctrl->device_id = device_id;
    ctrl->net_index = -1;

    if (allocate_dma_objects(index) != 0) {
        return;
    }

    quiesce_controller(ctrl);
    mark_driver_loaded(ctrl);
    configure_pcie_dma_behavior(ctrl);
    exit_ulp_with_firmware(ctrl);
    exit_phy_low_power(ctrl);
    mmio_write32(ctrl->mmio_base, E1000_REG_CTRL,
                 (mmio_read32(ctrl->mmio_base, E1000_REG_CTRL) &
                  ~E1000_CTRL_GIO_MASTER_DISABLE) |
                 E1000_CTRL_FD |
                 E1000_CTRL_SLU |
                 E1000_CTRL_ASDE);

    if (read_mac(ctrl) != 0) {
        return;
    }

    program_receive_address(ctrl);
    setup_packet_buffer(ctrl);
    setup_rx(ctrl, index);
    setup_tx(ctrl, index);

    char name[8];
    make_name(index, name);
    int net_index = net_register_device(name,
                                        device_name,
                                        vendor,
                                        device_id,
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
    unsupported_seen_count = 0;
    pci_scan(visit_pci, 0);
}

uint32_t intel_net_unsupported_count(void) {
    return unsupported_seen_count;
}

int intel_net_unsupported_info(uint32_t index,
                               uint16_t *vendor_id,
                               uint16_t *device_id,
                               const char **name,
                               const char **needed_driver) {
    if (index >= unsupported_seen_count) {
        return -1;
    }

    if (vendor_id) {
        *vendor_id = unsupported_seen[index].vendor_id;
    }
    if (device_id) {
        *device_id = unsupported_seen[index].device_id;
    }
    if (name) {
        *name = unsupported_seen[index].name;
    }
    if (needed_driver) {
        *needed_driver = unsupported_seen[index].needed_driver;
    }

    return 0;
}

int e1000_reset_controller(uint32_t index) {
    e1000_controller_t *ctrl;

    if (index >= controller_count) {
        return -1;
    }

    ctrl = &controllers[index];
    ctrl->reset_result = 0;

    if (reinitialize_controller(ctrl, index) != 0) {
        ctrl->reset_result |= 2u;
    }

    return ctrl->reset_result == 0 ? 0 : -1;
}

int e1000_debug_info(uint32_t index, e1000_debug_info_t *out) {
    e1000_controller_t *ctrl;
    uint32_t slot;
    uint32_t first;

    if (!out) {
        return -1;
    }

    zero_bytes(out, sizeof(*out));
    if (index >= controller_count) {
        return -1;
    }

    ctrl = &controllers[index];
    slot = index;
    first = (ctrl->rx_tail + 1u) % E1000_RX_DESC_COUNT;

    out->present = 1;
    out->status = mmio_read32(ctrl->mmio_base, E1000_REG_STATUS);
    out->ctrl = mmio_read32(ctrl->mmio_base, E1000_REG_CTRL);
    out->ctrl_ext = mmio_read32(ctrl->mmio_base, E1000_REG_CTRL_EXT);
    out->pci_command = pci_read_config16(ctrl->bus, ctrl->device, ctrl->function, 0x04);
    out->pmcsr = pci_pmcsr(ctrl->bus, ctrl->device, ctrl->function);
    out->rctl = mmio_read32(ctrl->mmio_base, E1000_REG_RCTL);
    out->tctl = mmio_read32(ctrl->mmio_base, E1000_REG_TCTL);
    out->rxdctl = mmio_read32(ctrl->mmio_base, E1000_REG_RXDCTL);
    out->txdctl = mmio_read32(ctrl->mmio_base, E1000_REG_TXDCTL);
    out->tarc0 = mmio_read32(ctrl->mmio_base, E1000_REG_TARC0);
    out->tarc1 = mmio_read32(ctrl->mmio_base, E1000_REG_TARC1);
    out->iosfpc = mmio_read32(ctrl->mmio_base, E1000_REG_IOSFPC);
    out->pba = mmio_read32(ctrl->mmio_base, E1000_REG_PBA);
    out->gcr = mmio_read32(ctrl->mmio_base, E1000_REG_GCR);
    out->fwsm = mmio_read32(ctrl->mmio_base, E1000_REG_FWSM);
    out->h2me = mmio_read32(ctrl->mmio_base, E1000_REG_H2ME);
    out->phy_result = ctrl->phy_result;
    out->phy_pm_ctrl = ctrl->phy_pm_ctrl;
    out->phy_ulp_cfg = ctrl->phy_ulp_cfg;
    out->reset_result = ctrl->reset_result;
    out->rdh = mmio_read32(ctrl->mmio_base, E1000_REG_RDH);
    out->rdt = mmio_read32(ctrl->mmio_base, E1000_REG_RDT);
    out->tdh = mmio_read32(ctrl->mmio_base, E1000_REG_TDH);
    out->tdt = mmio_read32(ctrl->mmio_base, E1000_REG_TDT);
    out->tx_reg_base = (uint64_t)mmio_read32(ctrl->mmio_base, E1000_REG_TDBAL) |
                       ((uint64_t)mmio_read32(ctrl->mmio_base, E1000_REG_TDBAH) << 32);
    out->tx_reg_len = mmio_read32(ctrl->mmio_base, E1000_REG_TDLEN);
    out->rx_tail = ctrl->rx_tail;
    out->tx_tail = ctrl->tx_tail;
    out->last_tx_status = ctrl->last_tx_status;
    out->last_tx_command = ctrl->last_tx_command;
    out->last_tx_length = ctrl->last_tx_length;
    out->last_tx_desc_addr = ctrl->last_tx_desc_addr;
    out->last_tx_desc_phys = ctrl->last_tx_desc_phys;
    out->last_tx_buffer_phys = ctrl->last_tx_buffer_phys;
    for (uint32_t i = 0; i < sizeof(out->last_tx_desc_bytes); ++i) {
        out->last_tx_desc_bytes[i] = ctrl->last_tx_desc_bytes[i];
    }
    out->first_rx_status = rx_desc[slot][first].status;
    out->first_rx_errors = rx_desc[slot][first].errors;
    out->first_rx_length = rx_desc[slot][first].length;
    out->first_rx_desc_addr = rx_desc[slot][first].address;
    out->rx_ring_phys = phys_addr(rx_desc[slot]);
    out->tx_ring_phys = phys_addr(tx_desc[slot]);
    out->first_rx_phys = phys_addr(rx_buffer(slot, first));
    for (uint32_t i = 0; i < sizeof(out->first_rx_bytes); ++i) {
        out->first_rx_bytes[i] = rx_buffer(slot, first)[i];
    }
    out->rx_scan_index = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < E1000_RX_DESC_COUNT; ++i) {
        if (rx_desc[slot][i].status & E1000_RX_STATUS_DD) {
            ++out->rx_scan_dd_count;
            if (out->rx_scan_index == 0xFFFFFFFFu) {
                out->rx_scan_index = i;
                out->rx_scan_status = rx_desc[slot][i].status;
                out->rx_scan_errors = rx_desc[slot][i].errors;
                out->rx_scan_length = rx_desc[slot][i].length;
                for (uint32_t b = 0; b < sizeof(out->rx_scan_bytes); ++b) {
                    out->rx_scan_bytes[b] = rx_buffer(slot, i)[b];
                }
            }
        }
    }
    out->last_rx_status = ctrl->last_rx_status;
    out->last_rx_errors = ctrl->last_rx_errors;
    out->last_rx_length = ctrl->last_rx_length;
    out->last_rx_desc_addr = ctrl->last_rx_desc_addr;
    for (uint32_t i = 0; i < sizeof(out->last_rx_bytes); ++i) {
        out->last_rx_bytes[i] = ctrl->last_rx_bytes[i];
    }
    return 0;
}
