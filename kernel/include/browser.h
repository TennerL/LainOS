#ifndef BROWSER_H
#define BROWSER_H

#include <stdint.h>

int browser_run(char left_drive,
                uint32_t left_dir,
                const char *left_path,
                char right_drive,
                uint32_t right_dir,
                const char *right_path);

#endif
