
#include "kernel.h"
#include "ahci.h"
#include "editor.h"
#include "lainfs.h"
#include "shell.h"
#include "storage.h"

#define MAX_DRIVES 26

typedef void (*command_handler_t)(const char *args, const boot_info_t *info);

typedef struct {
    const char *name;
    const char *help;
    command_handler_t handler;
} command_t;

typedef struct {
    int present;
    char label[12];
} drive_t;

static drive_t drives[MAX_DRIVES];
static int current_drive = -1;

static char *skip_spaces(char *s) {
    while (*s == ' ' || *s == '\t') {
        ++s;
    }
    return s;
}

static const char *skip_const_spaces(const char *s) {
    while (*s == ' ' || *s == '\t') {
        ++s;
    }
    return s;
}

static int streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

static char to_upper(char c) {
    if (c >= 'a' && c <= 'z') {
        return (char)(c - ('a' - 'A'));
    }
    return c;
}

static int parse_drive_spec(const char *s) {
    s = skip_const_spaces(s);

    if (!((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z'))) {
        return -1;
    }

    char drive_letter = to_upper(s[0]);

    if (s[1] != ':') {
        return -1;
    }

    s = skip_const_spaces(s + 2);
    if (*s != '\0') {
        return -1;
    }

    return drive_letter - 'A';
}

static int parse_drive_arg(const char *s) {
    s = skip_const_spaces(s);

    if (!((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z'))) {
        return -1;
    }

    char drive_letter = to_upper(s[0]);
    ++s;

    if (*s == ':') {
        ++s;
    }

    s = skip_const_spaces(s);
    if (*s != '\0') {
        return -1;
    }

    return drive_letter - 'A';
}

static void copy_label(char *dst, const char *src) {
    unsigned int i = 0;

    while (src[i] && i < 11) {
        dst[i] = src[i];
        ++i;
    }

    dst[i] = '\0';
}

static void print_drive_name(int index) {
    char name[3];

    name[0] = (char)('A' + index);
    name[1] = ':';
    name[2] = '\0';
    console_puts(name);
}

static void cmd_help(const char *args, const boot_info_t *info);
static void cmd_bgcolor(const char *args, const boot_info_t *info);
static void cmd_clear(const char *args, const boot_info_t *info);
static void cmd_echo(const char *args, const boot_info_t *info);
static void cmd_info(const char *args, const boot_info_t *info);
static void cmd_mkdrive(const char *args, const boot_info_t *info);
static void cmd_drives(const char *args, const boot_info_t *info);
static void cmd_blk(const char *args, const boot_info_t *info);
static void cmd_part(const char *args, const boot_info_t *info);
static void cmd_mount(const char *args, const boot_info_t *info);
static void cmd_mounts(const char *args, const boot_info_t *info);
static void cmd_format(const char *args, const boot_info_t *info);
static void cmd_ls(const char *args, const boot_info_t *info);
static void cmd_write(const char *args, const boot_info_t *info);
static void cmd_cat(const char *args, const boot_info_t *info);
static void cmd_edit(const char *args, const boot_info_t *info);
static void cmd_ahci(const char *args, const boot_info_t *info);
static void cmd_ticks(const char *args, const boot_info_t *info);

static const command_t commands[] = {
    { "help",    "show commands",             cmd_help },
    { "bgcolor", "set background color",      cmd_bgcolor },
    { "clear",   "clear screen",              cmd_clear },
    { "echo",    "print text",                cmd_echo },
    { "info",    "show kernel info",          cmd_info },
    { "mkdrive", "create a virtual drive",    cmd_mkdrive },
    { "drives",  "list virtual drives",       cmd_drives },
    { "blk",     "list block devices",        cmd_blk },
    { "part",    "list partitions",           cmd_part },
    { "mount",   "mount partition to drive",  cmd_mount },
    { "mounts",  "list mounted filesystems",  cmd_mounts },
    { "format",  "format drive as lainfs",    cmd_format },
    { "ls",      "list files",                cmd_ls },
    { "write",   "write a text file",         cmd_write },
    { "cat",     "print a text file",         cmd_cat },
    { "edit",    "edit a text file",          cmd_edit },
    { "ahci",    "show AHCI status",          cmd_ahci },
    { "ticks",   "show timer ticks",          cmd_ticks },
};

static const unsigned int command_count = sizeof(commands) / sizeof(commands[0]);

static void cmd_help(const char *args, const boot_info_t *info) {
    (void)args;
    (void)info;

    for (unsigned int i = 0; i < command_count; ++i) {
        console_puts(commands[i].name);
        console_puts(" - ");
        console_puts(commands[i].help);
        console_puts("\n");
    }
}

static void cmd_clear(const char *args, const boot_info_t *info) {
    (void)args;
    (void)info;
    console_clear();
}

static void cmd_echo(const char *args, const boot_info_t *info) {
    (void)info;
    console_puts(args);
    console_puts("\n");
}

static void cmd_info(const char *args, const boot_info_t *info) {
    (void)args;

    console_kprintf2("Kernel base: 0x%x, framebuffer: 0x%x\n",
                     info->kernel_base,
                     info->framebuffer_base);
    console_kprintf2("Resolution: %u x %u\n",
                     info->framebuffer_width,
                     info->framebuffer_height);
    console_kprintf1("Memory map bytes: %u\n", info->memory_map_size);
}

static void cmd_mkdrive(const char *args, const boot_info_t *info) {
    (void)info;

    int drive = parse_drive_spec(args);
    if (drive < 0 || drive >= MAX_DRIVES) {
        console_puts("usage: mkdrive C:\n");
        return;
    }

    if (drive == 0 || drive == 1) {
        console_puts("A: and B: are reserved.\n");
        return;
    }

    drives[drive].present = 1;
    copy_label(drives[drive].label, "VIRTUAL");
    current_drive = drive;

    console_puts("created ");
    print_drive_name(drive);
    console_puts(" as a virtual drive\n");
}

static void cmd_drives(const char *args, const boot_info_t *info) {
    (void)args;
    (void)info;

    int found = 0;

    for (int i = 0; i < MAX_DRIVES; ++i) {
        if (!drives[i].present) {
            continue;
        }

        found = 1;
        if (i == current_drive) {
            console_puts("* ");
        } else {
            console_puts("  ");
        }

        print_drive_name(i);
        console_puts(" ");
        console_puts(drives[i].label);
        console_puts("\n");
    }

    if (!found) {
        console_puts("no virtual drives. try: mkdrive C:\n");
    }
}

static void print_size_mib(uint64_t blocks, uint32_t block_size) {
    uint64_t bytes = blocks * block_size;
    console_put_dec64(bytes / (1024ull * 1024ull));
    console_puts(" MiB");
}

static void cmd_blk(const char *args, const boot_info_t *info) {
    (void)args;
    (void)info;

    uint32_t count = storage_block_device_count();
    if (count == 0) {
        console_puts("no block devices\n");
        return;
    }

    for (uint32_t i = 0; i < count; ++i) {
        const block_device_t *dev = storage_get_block_device(i);
        if (!dev) {
            continue;
        }

        console_puts(dev->name);
        console_puts(dev->write ? " rw" : " ro");
        console_puts(" blocks=");
        console_put_dec64(dev->block_count);
        console_puts(" block_size=");
        console_put_dec64(dev->block_size);
        console_puts(" size=");
        print_size_mib(dev->block_count, dev->block_size);
        console_puts("\n");
    }
}

static void cmd_part(const char *args, const boot_info_t *info) {
    (void)args;
    (void)info;

    uint32_t count = storage_partition_count();
    if (count == 0) {
        console_puts("no partitions discovered\n");
        return;
    }

    for (uint32_t i = 0; i < count; ++i) {
        const partition_t *part = storage_get_partition(i);
        if (!part) {
            continue;
        }

        console_puts(part->name);
        console_puts(" start=");
        console_put_dec64(part->start_lba);
        console_puts(" blocks=");
        console_put_dec64(part->block_count);
        console_puts(" type=0x");
        console_put_hex32(part->mbr_type);
        console_puts(" fs=");
        console_puts(part->fs_hint);
        console_puts("\n");
    }
}

static void split_first_arg(char *s, char **first, char **rest) {
    s = skip_spaces(s);
    *first = s;

    while (*s && *s != ' ' && *s != '\t') {
        ++s;
    }

    if (*s) {
        *s++ = '\0';
        s = skip_spaces(s);
    }

    *rest = s;
}

static void cmd_mount(const char *args, const boot_info_t *info) {
    (void)info;

    char *mutable_args = (char *)args;
    char *drive_arg = 0;
    char *part_arg = 0;
    int drive = -1;
    int status = 0;

    split_first_arg(mutable_args, &drive_arg, &part_arg);
    drive = parse_drive_arg(drive_arg);

    if (drive < 0 || drive >= MAX_DRIVES || *part_arg == '\0') {
        console_puts("usage: mount C: rd0p1\n");
        return;
    }

    status = storage_mount((char)('A' + drive), part_arg);
    if (status == -1) {
        console_puts("partition not found: ");
        console_puts(part_arg);
        console_puts("\n");
        return;
    }

    if (status == -2) {
        console_puts("A: and B: are reserved.\n");
        return;
    }

    drives[drive].present = 1;
    copy_label(drives[drive].label, "MOUNTED");
    current_drive = drive;

    console_puts("mounted ");
    console_puts(part_arg);
    console_puts(" at ");
    print_drive_name(drive);
    console_puts("\n");
}

static void cmd_mounts(const char *args, const boot_info_t *info) {
    (void)args;
    (void)info;

    int found = 0;

    for (uint32_t i = 0; i < STORAGE_MAX_MOUNTS; ++i) {
        const mount_t *mount = storage_get_mount(i);
        if (!mount) {
            continue;
        }

        const partition_t *part = storage_get_partition(mount->partition_index);
        found = 1;
        console_puts("  ");
        print_drive_name((int)(mount->drive_letter - 'A'));
        console_puts(" ");
        console_puts(part ? part->name : "?");
        console_puts(" ");
        console_puts(mount->fs_name);
        console_puts("\n");
    }

    if (!found) {
        console_puts("no mounts. try: mount C: rd0p1\n");
    }
}

static int active_drive(void) {
    if (current_drive >= 0 && current_drive < MAX_DRIVES) {
        return current_drive;
    }

    return -1;
}

static int drive_from_args_or_current(const char *args) {
    int drive = parse_drive_arg(args);
    if (drive >= 0) {
        return drive;
    }

    if (*skip_const_spaces(args) != '\0') {
        return -1;
    }

    return active_drive();
}

static void cmd_format(const char *args, const boot_info_t *info) {
    (void)info;

    int drive = drive_from_args_or_current(args);
    if (drive < 0 || drive >= MAX_DRIVES) {
        console_puts("usage: format C:\n");
        return;
    }

    int status = lainfs_format((char)('A' + drive));
    if (status == -1) {
        console_puts("drive is not mounted\n");
        return;
    }

    if (status == -4) {
        console_puts("format failed: mounted partition is read-only\n");
        return;
    }

    if (status == -5) {
        console_puts("format failed: disk write failed\n");
        return;
    }

    if (status != 0) {
        console_puts("format failed\n");
        return;
    }

    console_puts("formatted ");
    print_drive_name(drive);
    console_puts(" as lainfs\n");
}

static void cmd_ls(const char *args, const boot_info_t *info) {
    (void)info;

    int drive = drive_from_args_or_current(args);
    if (drive < 0 || drive >= MAX_DRIVES) {
        console_puts("usage: ls [C:]\n");
        return;
    }

    int status = lainfs_list((char)('A' + drive));
    if (status == -1) {
        console_puts("drive is not mounted\n");
    } else if (status == -2) {
        console_puts("drive is not formatted as lainfs\n");
    } else if (status != 0) {
        console_puts("ls failed\n");
    }
}

static void cmd_write(const char *args, const boot_info_t *info) {
    (void)info;

    int drive = active_drive();
    char *name = 0;
    char *text = 0;

    if (drive < 0) {
        console_puts("select a mounted drive first, for example C:\n");
        return;
    }

    split_first_arg((char *)args, &name, &text);
    if (*name == '\0' || *text == '\0') {
        console_puts("usage: write name text\n");
        return;
    }

    int status = lainfs_write_file((char)('A' + drive), name, text);
    if (status == -3) {
        console_puts("drive is not formatted as lainfs\n");
    } else if (status == -5) {
        console_puts("directory is full\n");
    } else if (status != 0) {
        console_puts("write failed\n");
    }
}

static void cmd_cat(const char *args, const boot_info_t *info) {
    (void)info;

    int drive = active_drive();
    const char *name = skip_const_spaces(args);

    if (drive < 0) {
        console_puts("select a mounted drive first, for example C:\n");
        return;
    }

    if (*name == '\0') {
        console_puts("usage: cat name\n");
        return;
    }

    int status = lainfs_read_file((char)('A' + drive), name);
    if (status == -2) {
        console_puts("drive is not formatted as lainfs\n");
    } else if (status == -5) {
        console_puts("file not found\n");
    } else if (status != 0) {
        console_puts("cat failed\n");
    }
}

static void cmd_edit(const char *args, const boot_info_t *info) {
    (void)info;

    int drive = active_drive();
    const char *name = skip_const_spaces(args);

    if (drive < 0) {
        console_puts("select a mounted drive first, for example C:\n");
        return;
    }

    if (*name == '\0') {
        console_puts("usage: edit name\n");
        return;
    }

    int status = editor_run((char)('A' + drive), name);
    if (status == -1) {
        console_puts("edit failed: invalid filename\n");
    } else if (status == -3) {
        console_puts("drive is not formatted as lainfs\n");
    } else if (status == -6) {
        console_puts("edit failed: file is too large\n");
    } else if (status != 0) {
        console_puts("edit failed\n");
    }
}

static void cmd_ahci(const char *args, const boot_info_t *info) {
    (void)args;
    (void)info;

    console_puts("AHCI controllers=");
    console_put_dec64(ahci_controller_count());
    console_puts(" disks=");
    console_put_dec64(ahci_disk_count());
    console_puts("\n");
}

static void cmd_ticks(const char *args, const boot_info_t *info) {
    (void)args;
    (void)info;

    console_puts("ticks=");
    console_put_dec64(timer_ticks());
    console_puts(" hz=");
    console_put_dec64(timer_frequency());
    console_puts("\n");
}

void cmd_bgcolor(const char *args, const boot_info_t *info) {
    (void)info;

    if (args == 0 || *args == '\0') {
        console_puts("Invalid color, use 6 digit hex code! \n");
        return;
    }

    int32_t color = 0;
    int base = 10;

    if(args[0] == '0' && (args[1] == 'x' || args[1] == 'X')) {
        base = 16;
        args += 2;
    }

    if (*args == '\0') {
        return;
    }

    while (*args) {
        char c = *args;
        int digit;

        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (base == 16 && c >= 'a' && c <= 'f') {
            digit = 10 + (c - 'a');
        } else if (base == 16 && c >= 'A' && c <= 'F') {
            digit = 10 + (c - 'A');
        }
        else {
            return;
        }

        color = color * base + digit;
        args++;
    }

    fill_screen_color(color);
}

void shell_init(void) {
    for (int i = 0; i < MAX_DRIVES; ++i) {
        drives[i].present = 0;
        drives[i].label[0] = '\0';
    }

    current_drive = -1;
}

// static void cmd_run(const char *args, const boot_info_t *info) {
//     char script[512];
//     uint32_t size = 0;

//     status = lainfs_load_file(current_drive, args, script, sizeof(script), &size);
//     if (status != 0) {
//         console_puts("run failed\n");
//         return;
//     }

//     split script into lines;
//     for each line:
//         shell_run_command(line, info);
// }


void shell_print_prompt(void) {
    if (current_drive >= 0 && drives[current_drive].present) {
        print_drive_name(current_drive);
        console_puts("\\> ");
        return;
    }

    console_puts("> ");
}

void shell_run_command(char *line, const boot_info_t *info) {
    char *name = skip_spaces(line);
    char *args = name;

    while (*args && *args != ' ' && *args != '\t') {
        ++args;
    }

    if (*args) {
        *args++ = '\0';
        args = skip_spaces(args);
    }

    if (*name == '\0') {
        return;
    }

    int requested_drive = parse_drive_spec(name);
    if (requested_drive >= 0 && requested_drive < MAX_DRIVES) {
        if (drives[requested_drive].present || storage_drive_is_mounted((char)('A' + requested_drive))) {
            current_drive = requested_drive;
        } else {
            print_drive_name(requested_drive);
            console_puts(" does not exist. create it with mkdrive ");
            print_drive_name(requested_drive);
            console_puts("\n");
        }
        return;
    }

    for (unsigned int i = 0; i < command_count; ++i) {
        if (streq(name, commands[i].name)) {
            commands[i].handler(args, info);
            return;
        }
    }

    console_puts("unknown command: ");
    console_puts(name);
    console_puts("\n");
}
