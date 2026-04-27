#ifndef SHELL_H
#define SHELL_H

#include "bootinfo.h"

void shell_init(void);
int shell_mount_first_lainfs(char drive_letter);
void shell_set_session(unsigned int session);
void shell_run_autoexec(const char *name, const boot_info_t *info);
void shell_print_prompt(void);
void shell_run_command(char *line, const boot_info_t *info);

#endif
