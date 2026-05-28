#ifndef LAINOS_FREESTANDING_DIRENT_H
#define LAINOS_FREESTANDING_DIRENT_H

#include <sys/types.h>

typedef struct DIR DIR;

struct dirent {
    ino_t d_ino;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[256];
};

int alphasort(const struct dirent **d1, const struct dirent **d2);
DIR *opendir(const char *name);
int closedir(DIR *dirp);
int dirfd(DIR *dirp);
struct dirent *readdir(DIR *dirp);
int scandir(const char *dir,
            struct dirent ***namelist,
            int (*sel)(const struct dirent *),
            int (*compar)(const struct dirent **, const struct dirent **));

#endif
