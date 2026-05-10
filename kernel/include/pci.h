#ifndef PCI_H
#define PCI_H

#include <stdint.h>

typedef void (*pci_visit_t)(uint8_t bus, uint8_t device, uint8_t function, void *ctx);

uint32_t pci_read_config32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
uint16_t pci_read_config16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
uint8_t pci_read_config8(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
void pci_write_config32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value);
void pci_write_config16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint16_t value);
void pci_scan(pci_visit_t visitor, void *ctx);

#endif
