#ifndef AHCI_H
#define AHCI_H

#include <stdint.h>

void ahci_init(void);
uint32_t ahci_controller_count(void);
uint32_t ahci_disk_count(void);

#endif
