#ifndef SHELL_H
#define SHELL_H

#include "bootinfo.h"

void shell_init(void);
void shell_print_prompt(void);
void shell_run_command(char *line, const boot_info_t *info);

#endif
