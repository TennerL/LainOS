#ifndef LAINOS_FREESTANDING_LIMITS_H
#define LAINOS_FREESTANDING_LIMITS_H

#define CHAR_BIT __CHAR_BIT__

#define INT_MIN (-__INT_MAX__ - 1)
#define INT_MAX __INT_MAX__

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#endif
