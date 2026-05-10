#include "pci.h"

#define PCI_CONFIG_ADDRESS 0xCF8u
#define PCI_CONFIG_DATA 0xCFCu

static void outl(uint16_t port, uint32_t value) {
    __asm__ __volatile__("outl %0, %1" : : "a"(value), "Nd"(port));
}

static void outw(uint16_t port, uint16_t value) {
    __asm__ __volatile__("outw %0, %1" : : "a"(value), "Nd"(port));
}

static uint32_t inl(uint16_t port) {
    uint32_t value;
    __asm__ __volatile__("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static uint32_t pci_address(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    return 0x80000000u |
           ((uint32_t)bus << 16) |
           ((uint32_t)device << 11) |
           ((uint32_t)function << 8) |
           (offset & 0xFCu);
}

uint32_t pci_read_config32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, device, function, offset));
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_read_config16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    uint32_t value = pci_read_config32(bus, device, function, offset);
    return (uint16_t)(value >> ((offset & 2u) * 8u));
}

uint8_t pci_read_config8(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    uint32_t value = pci_read_config32(bus, device, function, offset);
    return (uint8_t)(value >> ((offset & 3u) * 8u));
}

void pci_write_config32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value) {
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, device, function, offset));
    outl(PCI_CONFIG_DATA, value);
}

void pci_write_config16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint16_t value) {
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, device, function, offset));
    outw((uint16_t)(PCI_CONFIG_DATA + (offset & 2u)), value);
}

void pci_scan(pci_visit_t visitor, void *ctx) {
    for (uint16_t bus = 0; bus < 256; ++bus) {
        for (uint8_t device = 0; device < 32; ++device) {
            uint16_t vendor = pci_read_config16((uint8_t)bus, device, 0, 0x00);
            if (vendor == 0xFFFFu) {
                continue;
            }

            uint8_t header_type = pci_read_config8((uint8_t)bus, device, 0, 0x0E);
            uint8_t functions = (header_type & 0x80u) ? 8u : 1u;

            for (uint8_t function = 0; function < functions; ++function) {
                vendor = pci_read_config16((uint8_t)bus, device, function, 0x00);
                if (vendor == 0xFFFFu) {
                    continue;
                }

                visitor((uint8_t)bus, device, function, ctx);
            }
        }
    }
}
