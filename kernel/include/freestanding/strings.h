#ifndef LAINOS_FREESTANDING_STRINGS_H
#define LAINOS_FREESTANDING_STRINGS_H

#include <stddef.h>

int strcasecmp(const char *lhs, const char *rhs);
int strncasecmp(const char *lhs, const char *rhs, size_t count);

#endif
