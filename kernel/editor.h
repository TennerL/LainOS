#ifndef EDITOR_H
#define EDITOR_H

#include <stdint.h>

int editor_run(char drive_letter, const char *name);
int editor_run_in_dir(char drive_letter, uint32_t parent_id, const char *name);

#endif
