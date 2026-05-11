#ifndef DESKTOP_H
#define DESKTOP_H

#include "bootinfo.h"

void desktop_run(const boot_info_t *info);
int desktop_api_open_editor(const char *path);

#endif
