#ifndef LAINOS_FREESTANDING_SYS_STAT_H
#define LAINOS_FREESTANDING_SYS_STAT_H

#include <sys/types.h>
#include <time.h>

struct stat {
    dev_t st_dev;
    ino_t st_ino;
    nlink_t st_nlink;
    mode_t st_mode;
    uid_t st_uid;
    gid_t st_gid;
    dev_t st_rdev;
    off_t st_size;
    blksize_t st_blksize;
    blkcnt_t st_blocks;
    time_t st_atime;
    time_t st_mtime;
    time_t st_ctime;
};

#ifndef S_IFMT
#define S_IFMT 0170000
#endif

#ifndef S_IFDIR
#define S_IFDIR 0040000
#endif

#ifndef S_IFREG
#define S_IFREG 0100000
#endif

#ifndef S_IRUSR
#define S_IRUSR 0400
#endif

#ifndef S_IWUSR
#define S_IWUSR 0200
#endif

#ifndef S_IXUSR
#define S_IXUSR 0100
#endif

#ifndef S_IRWXU
#define S_IRWXU (S_IRUSR | S_IWUSR | S_IXUSR)
#endif

#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & S_IFMT) == S_IFDIR)
#endif

#ifndef S_ISREG
#define S_ISREG(mode) (((mode) & S_IFMT) == S_IFREG)
#endif

int fstatat(int dirfd, const char *path, struct stat *buf, int flag);
int mkdir(const char *path, mode_t mode);
int stat(const char *path, struct stat *buf);

#endif
