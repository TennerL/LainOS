
#include "version.h"
#include "kernel.h"
#include "ahci.h"
#include "assembler.h"
#include "editor.h"
#include "keyboard.h"
#include "lainfs.h"
#include "shell.h"
#include "storage.h"
#include "zobject.h"
#include "zscript.h"

#define MAX_DRIVES 26
#define SCRIPT_BUFFER_SIZE LAINFS_FILE_CAPACITY
#define SCRIPT_LINE_SIZE 128u
#define SCRIPT_MAX_DEPTH 4
#define EXEC_BUFFER_SIZE LAINFS_FILE_CAPACITY
#define EXEC_API_MAGIC 0x4C41494E45584543ull
#define ASM_SOURCE_SIZE LAINFS_FILE_CAPACITY
#define SHELL_PATH_SIZE 128u
#define SHELL_MAX_SESSIONS 2u
#define ZLINK_MAX_OBJECTS 4u
#define ZMODULE_MAX_MODULES 4u
#define ZMODULE_MAX_EXPORTS 16u
#define ZMODULE_NAME_SIZE 32u

static int script_depth;
static char script_buffers[SCRIPT_MAX_DEPTH][SCRIPT_BUFFER_SIZE + 1];
static unsigned char exec_buffer[EXEC_BUFFER_SIZE] __attribute__((aligned(16)));
static unsigned char asm_output[EXEC_BUFFER_SIZE];
static char zscript_output[ASM_SOURCE_SIZE + 1];

typedef struct {
    int loaded;
    char name[ZMODULE_NAME_SIZE];
    unsigned char image[EXEC_BUFFER_SIZE] __attribute__((aligned(16)));
    uint32_t image_size;
    uint32_t object_count;
    uint32_t export_count;
    zobject_resolved_symbol_t exports[ZMODULE_MAX_EXPORTS];
} zmodule_slot_t;

static zmodule_slot_t zmodule_slots[ZMODULE_MAX_MODULES];

typedef void (*command_handler_t)(const char *args, const boot_info_t *info);

typedef struct {
    uint64_t magic;
    uint64_t version;
    void (*puts)(const char *s);
    void (*put_hex64)(unsigned long long value);
    void (*put_dec64)(unsigned long long value);
    unsigned long long (*ticks)(void);
} exec_api_t;

typedef void (*exec_program_t)(const exec_api_t *api);

typedef struct {
    const char *name;
    const char *help;
    command_handler_t handler;
} command_t;

typedef struct {
    int present;
    char label[12];
} drive_t;

typedef struct {
    int initialized;
    int drive;
    uint32_t dir_ids[MAX_DRIVES];
    char paths[MAX_DRIVES][SHELL_PATH_SIZE];
} shell_session_t;

static drive_t drives[MAX_DRIVES];
static shell_session_t shell_sessions[SHELL_MAX_SESSIONS];
static unsigned int active_session_index;

static shell_session_t *active_session(void) {
    return &shell_sessions[active_session_index];
}

#define current_drive (active_session()->drive)
#define cwd_dirs (active_session()->dir_ids)
#define cwd_paths (active_session()->paths)

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

static void zero_memory(void *ptr, uint32_t size) {
    unsigned char *p = (unsigned char *)ptr;
    for (uint32_t i = 0; i < size; ++i) {
        p[i] = 0;
    }
}

static int parse_color_arg(const char *args, unsigned int *color) {
    int base = 10;
    unsigned int value = 0;

    if (args == 0 || *args == '\0') {
        return -1;
    }

    if (args[0] == '0' && (args[1] == 'x' || args[1] == 'X')) {
        base = 16;
        args += 2;
    }

    if (*args == '\0') {
        return -1;
    }

    while (*args) {
        char c = *args;
        unsigned int digit;

        if (c >= '0' && c <= '9') {
            digit = (unsigned int)(c - '0');
        } else if (base == 16 && c >= 'a' && c <= 'f') {
            digit = 10u + (unsigned int)(c - 'a');
        } else if (base == 16 && c >= 'A' && c <= 'F') {
            digit = 10u + (unsigned int)(c - 'A');
        } else {
            return -1;
        }

        if (digit >= (unsigned int)base) {
            return -1;
        }

        value = value * (unsigned int)base + digit;
        ++args;
    }

    *color = value;
    return 0;
}

static char to_upper(char c) {
    if (c >= 'a' && c <= 'z') {
        return (char)(c - ('a' - 'A'));
    }
    return c;
}

static char hex_digit(unsigned int value) {
    value &= 0xFu;
    if (value < 10u) {
        return (char)('0' + value);
    }
    return (char)('a' + (value - 10u));
}

static void make_zobject_prefix(const char *name, char *out, uint32_t out_capacity) {
    uint32_t hash = 0x811u;

    while (*name) {
        hash = ((hash << 5) ^ (hash >> 2) ^ (unsigned char)*name) & 0xFFFu;
        ++name;
    }

    if (out_capacity < 6u) {
        if (out_capacity != 0) {
            out[0] = '\0';
        }
        return;
    }

    out[0] = 'o';
    out[1] = hex_digit(hash >> 8);
    out[2] = hex_digit(hash >> 4);
    out[3] = hex_digit(hash);
    out[4] = '_';
    out[5] = '\0';
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

static void copy_text_limited(char *dst, uint32_t dst_size, const char *src) {
    uint32_t i = 0;

    if (dst_size == 0) {
        return;
    }

    while (src[i] && i + 1u < dst_size) {
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

static int active_drive(void);

static void reset_cwd_in_session(shell_session_t *session, int drive) {
    if (drive < 0 || drive >= MAX_DRIVES) {
        return;
    }

    session->dir_ids[drive] = LAINFS_ROOT_DIR;
    session->paths[drive][0] = '\\';
    session->paths[drive][1] = '\0';
}

static void init_session_blank(shell_session_t *session) {
    session->initialized = 1;
    session->drive = -1;

    for (int i = 0; i < MAX_DRIVES; ++i) {
        reset_cwd_in_session(session, i);
    }
}

static void ensure_session_initialized(unsigned int index) {
    if (index >= SHELL_MAX_SESSIONS || shell_sessions[index].initialized) {
        return;
    }

    if (shell_sessions[active_session_index].initialized) {
        shell_sessions[index] = shell_sessions[active_session_index];
        shell_sessions[index].initialized = 1;
    } else {
        init_session_blank(&shell_sessions[index]);
    }
}

static void reset_cwd(int drive) {
    reset_cwd_in_session(active_session(), drive);
}

static uint32_t active_dir(void) {
    int drive = active_drive();
    if (drive < 0) {
        return LAINFS_ROOT_DIR;
    }

    return cwd_dirs[drive];
}

static void append_path_part(int drive, const char *name) {
    unsigned int len = 0;
    unsigned int i = 0;

    if (drive < 0 || drive >= MAX_DRIVES) {
        return;
    }

    while (cwd_paths[drive][len]) {
        ++len;
    }

    if (len > 1 && len + 1 < SHELL_PATH_SIZE) {
        cwd_paths[drive][len++] = '\\';
        cwd_paths[drive][len] = '\0';
    }

    while (name[i] && len + 1 < SHELL_PATH_SIZE) {
        cwd_paths[drive][len++] = name[i++];
    }

    cwd_paths[drive][len] = '\0';
}

static void pop_path_part(int drive) {
    unsigned int len = 0;

    if (drive < 0 || drive >= MAX_DRIVES) {
        return;
    }

    while (cwd_paths[drive][len]) {
        ++len;
    }

    if (len <= 1) {
        cwd_paths[drive][0] = '\\';
        cwd_paths[drive][1] = '\0';
        return;
    }

    while (len > 1 && cwd_paths[drive][len - 1] != '\\') {
        --len;
    }

    if (len <= 1) {
        cwd_paths[drive][1] = '\0';
    } else {
        cwd_paths[drive][len - 1] = '\0';
    }
}

static int resolve_dir_arg(int drive, const char *arg, uint32_t *out_dir) {
    arg = skip_const_spaces(arg);

    if (*arg == '\0' || streq(arg, ".")) {
        *out_dir = cwd_dirs[drive];
        return 0;
    }

    if (streq(arg, "\\") || streq(arg, "/")) {
        *out_dir = LAINFS_ROOT_DIR;
        return 0;
    }

    if (streq(arg, "..")) {
        return lainfs_parent_dir((char)('A' + drive), cwd_dirs[drive], out_dir);
    }

    return lainfs_find_dir((char)('A' + drive), cwd_dirs[drive], arg, out_dir);
}

static int is_script_comment_or_blank(const char *line) {
    line = skip_const_spaces(line);
    return *line == '\0' || *line == '#';
}

static void run_script_text(char *script, uint32_t size, const boot_info_t *info) {
    char line[SCRIPT_LINE_SIZE];
    uint32_t line_len = 0;

    for (uint32_t i = 0; i <= size; ++i) {
        char ch = i < size ? script[i] : '\n';

        if (ch == '\r') {
            continue;
        }

        if (ch == '\n') {
            line[line_len] = '\0';

            if (!is_script_comment_or_blank(line)) {
                console_puts("> ");
                console_puts(line);
                console_puts("\n");
                shell_run_command(line, info);
            }

            line_len = 0;
            continue;
        }

        if (line_len + 1 < SCRIPT_LINE_SIZE) {
            line[line_len++] = ch;
        }
    }
}

static void cmd_help(const char *args, const boot_info_t *info);
static void cmd_bgcolor(const char *args, const boot_info_t *info);
static void cmd_fgcolor(const char *args, const boot_info_t *info);
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
static void cmd_cd(const char *args, const boot_info_t *info);
static void cmd_pwd(const char *args, const boot_info_t *info);
static void cmd_mkdir(const char *args, const boot_info_t *info);
static void cmd_rm(const char *args, const boot_info_t *info);
static void cmd_rename(const char *args, const boot_info_t *info);
static void cmd_write(const char *args, const boot_info_t *info);
static void cmd_cat(const char *args, const boot_info_t *info);
static void cmd_edit(const char *args, const boot_info_t *info);
static void cmd_ahci(const char *args, const boot_info_t *info);
static void cmd_ticks(const char *args, const boot_info_t *info);
static void cmd_run(const char *args, const boot_info_t *info);
static void cmd_exec(const char *args, const boot_info_t *info);
static void cmd_asm(const char *args, const boot_info_t *info);
static void cmd_zc(const char *args, const boot_info_t *info);
static void cmd_zco(const char *args, const boot_info_t *info);
static void cmd_zlink(const char *args, const boot_info_t *info);
static void cmd_zmod(const char *args, const boot_info_t *info);
static void cmd_zmods(const char *args, const boot_info_t *info);
static void cmd_zrun(const char *args, const boot_info_t *info);
static void cmd_zasm(const char *args, const boot_info_t *info);
static void cmd_keymap(const char *args, const boot_info_t *info);

static const command_t commands[] = {
    { "help",    "show commands",             cmd_help },
    { "bgcolor", "set background color",      cmd_bgcolor },
    { "fgcolor", "set text color",            cmd_fgcolor },
    { "clear",   "clear screen",              cmd_clear },
    { "echo",    "print text",                cmd_echo },
    { "info",    "show kernel info",          cmd_info },
    { "mkdrive", "create a virtual drive",    cmd_mkdrive },
    { "drives",  "list virtual drives",       cmd_drives },
    { "blk",     "list block devices",        cmd_blk },
    { "part",    "list partitions",           cmd_part },
    { "mount",   "mount partition to drive",  cmd_mount },
    { "mounts",  "list mounted filesystems",  cmd_mounts },
    { "format",  "format drive, partition, or disk as lainfs", cmd_format },
    { "ls",      "list directory entries",    cmd_ls },
    { "cd",      "change directory",          cmd_cd },
    { "pwd",     "show current directory",    cmd_pwd },
    { "mkdir",   "create a directory",         cmd_mkdir },
    { "rm",      "delete a file or directory", cmd_rm },
    { "del",     "delete a file or directory", cmd_rm },
    { "rename",  "rename a file or directory", cmd_rename },
    { "mv",      "rename a file or directory", cmd_rename },
    { "write",   "write a text file",         cmd_write },
    { "cat",     "print a text file",         cmd_cat },
    { "edit",    "edit a text file",          cmd_edit },
    { "keymap",  "set keyboard layout",       cmd_keymap },
    { "ahci",    "show AHCI status",          cmd_ahci },
    { "ticks",   "show timer ticks",          cmd_ticks },
    { "run",     "run a script file",         cmd_run },
    { "exec",    "run a flat binary file",     cmd_exec },
    { "asm",     "assemble a tiny asm file",   cmd_asm },
    { "zc",      "compile a tiny .Z file",     cmd_zc },
    { "zco",     "compile .Z to a .zo object", cmd_zco },
    { "zlink",   "link a .zo object",          cmd_zlink },
    { "zmod",    "load and run .zo module(s)",  cmd_zmod },
    { "zmods",   "list loaded .zo modules",     cmd_zmods },
    { "zrun",    "compile and run a .Z file",  cmd_zrun },
    { "zasm",    "dump generated asm for a .Z file", cmd_zasm },
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

    console_puts("Kernel version: ");
    console_puts(BUILD_VERSION_STRING);
    console_puts("\n");
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
    reset_cwd(drive);

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
    reset_cwd(drive);

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
        console_puts(mount->partition_name[0] ? mount->partition_name : (part ? part->name : "?"));
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

static int set_mounted_drive(char drive_letter, const char *label) {
    int drive = to_upper(drive_letter) - 'A';

    if (drive < 0 || drive >= MAX_DRIVES) {
        return -1;
    }

    drives[drive].present = 1;
    copy_label(drives[drive].label, label);
    current_drive = drive;
    reset_cwd(drive);
    return 0;
}

static int has_real_writable_block_device(void) {
    for (uint32_t i = 0; i < storage_block_device_count(); ++i) {
        const block_device_t *dev = storage_get_block_device(i);

        if (!dev || !dev->write) {
            continue;
        }

        if (streq(dev->name, "rd0")) {
            continue;
        }

        return 1;
    }

    return 0;
}

int shell_mount_first_lainfs(char drive_letter) {
    drive_letter = to_upper(drive_letter);

    for (uint32_t i = 0; i < storage_partition_count(); ++i) {
        const partition_t *part = storage_get_partition(i);
        if (!part || !streq(part->fs_hint, "lainfs")) {
            continue;
        }

        if (storage_mount(drive_letter, part->name) == 0) {
            set_mounted_drive(drive_letter, "SYSTEM");
            console_puts("mounted ");
            console_puts(part->name);
            console_puts(" at ");
            print_drive_name((int)(drive_letter - 'A'));
            console_puts("\n");
            return 0;
        }
    }

    if (!has_real_writable_block_device() && storage_find_partition("rd0p1", 0)) {
        if (lainfs_format_partition("rd0p1") == 0 && storage_mount(drive_letter, "rd0p1") == 0) {
            set_mounted_drive(drive_letter, "LIVE");
            console_puts("created live ramdisk rd0p1 at ");
            print_drive_name((int)(drive_letter - 'A'));
            console_puts("\n");
            return 0;
        }
    }

    return -1;
}

static void cmd_format(const char *args, const boot_info_t *info) {
    (void)info;

    const char *target = skip_const_spaces(args);
    int drive = drive_from_args_or_current(args);

    if (drive >= 0 && drive < MAX_DRIVES) {
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
        reset_cwd(drive);
        return;
    }

    if (*target == '\0') {
        console_puts("usage: format C: | format hd1p1 | format hd1\n");
        return;
    }

    if (storage_find_partition(target, 0)) {
        int status = lainfs_format_partition(target);
        if (status == -4) {
            console_puts("format failed: partition is read-only\n");
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
        console_puts(target);
        console_puts(" as lainfs\n");
        return;
    }

    if (storage_find_block_device(target, 0)) {
        char partition_name[12];
        int status = lainfs_format_block_device(target, partition_name, sizeof(partition_name));
        if (status == -2) {
            console_puts("format failed: disk is not writable, mounted, or too small\n");
            return;
        }
        if (status == -3 || status == -5) {
            console_puts("format failed: disk write failed\n");
            return;
        }
        if (status != 0) {
            console_puts("format failed\n");
            return;
        }

        console_puts("formatted ");
        console_puts(target);
        console_puts(" as ");
        console_puts(partition_name);
        console_puts(" (lainfs)\n");
        return;
    }

    console_puts("unknown drive, partition, or block device\n");
}

static void cmd_ls(const char *args, const boot_info_t *info) {
    (void)info;

    uint32_t dir_id = LAINFS_ROOT_DIR;
    const char *target = skip_const_spaces(args);
    int drive = active_drive();

    if (parse_drive_arg(target) >= 0) {
        drive = parse_drive_arg(target);
        dir_id = LAINFS_ROOT_DIR;
    } else if (drive >= 0) {
        int status = resolve_dir_arg(drive, target, &dir_id);
        if (status != 0) {
            console_puts("ls failed: directory not found\n");
            return;
        }
    }

    if (drive < 0 || drive >= MAX_DRIVES) {
        console_puts("usage: ls [directory|C:]\n");
        return;
    }

    int status = lainfs_list_dir((char)('A' + drive), dir_id);
    if (status == -1) {
        console_puts("drive is not mounted\n");
    } else if (status == -2) {
        console_puts("drive is not formatted as lainfs\n");
    } else if (status == -5) {
        console_puts("ls failed: directory not found\n");
    } else if (status != 0) {
        console_puts("ls failed\n");
    }
}

static void cmd_cd(const char *args, const boot_info_t *info) {
    (void)info;

    int drive = active_drive();
    const char *target = skip_const_spaces(args);
    uint32_t dir_id = LAINFS_ROOT_DIR;

    if (drive < 0) {
        console_puts("select a mounted drive first, for example C:\n");
        return;
    }

    if (*target == '\0') {
        reset_cwd(drive);
        return;
    }

    if (streq(target, "\\") || streq(target, "/")) {
        reset_cwd(drive);
        return;
    }

    if (streq(target, ".")) {
        return;
    }

    if (streq(target, "..")) {
        int status = lainfs_parent_dir((char)('A' + drive), cwd_dirs[drive], &dir_id);
        if (status != 0) {
            console_puts("cd failed\n");
            return;
        }

        cwd_dirs[drive] = dir_id;
        pop_path_part(drive);
        return;
    }

    int status = lainfs_find_dir((char)('A' + drive), cwd_dirs[drive], target, &dir_id);
    if (status == -3) {
        console_puts("drive is not formatted as lainfs\n");
    } else if (status == -5) {
        console_puts("cd failed: directory not found\n");
    } else if (status != 0) {
        console_puts("cd failed\n");
    } else {
        cwd_dirs[drive] = dir_id;
        append_path_part(drive, target);
    }
}

static void cmd_pwd(const char *args, const boot_info_t *info) {
    (void)args;
    (void)info;

    int drive = active_drive();
    if (drive < 0) {
        console_puts("select a mounted drive first, for example C:\n");
        return;
    }

    print_drive_name(drive);
    console_puts(cwd_paths[drive]);
    console_puts("\n");
}

static void cmd_mkdir(const char *args, const boot_info_t *info) {
    (void)info;

    int drive = active_drive();
    const char *name = skip_const_spaces(args);

    if (drive < 0) {
        console_puts("select a mounted drive first, for example C:\n");
        return;
    }

    if (*name == '\0') {
        console_puts("usage: mkdir name\n");
        return;
    }

    int status = lainfs_make_dir_in_dir((char)('A' + drive), cwd_dirs[drive], name);
    if (status == -2) {
        console_puts("mkdir failed: invalid name\n");
    } else if (status == -3) {
        console_puts("drive is not formatted as lainfs\n");
    } else if (status == -5) {
        console_puts("directory is full\n");
    } else if (status == -6) {
        console_puts("mkdir failed: name already exists\n");
    } else if (status != 0) {
        console_puts("mkdir failed\n");
    }
}

static void cmd_rm(const char *args, const boot_info_t *info) {
    (void)info;

    int drive = active_drive();
    const char *name = skip_const_spaces(args);

    if (drive < 0) {
        console_puts("select a mounted drive first, for example C:\n");
        return;
    }

    if (*name == '\0') {
        console_puts("usage: rm name\n");
        return;
    }

    int status = lainfs_delete_in_dir((char)('A' + drive), cwd_dirs[drive], name);
    if (status == -2) {
        console_puts("delete failed: invalid name\n");
    } else if (status == -3) {
        console_puts("drive is not formatted as lainfs\n");
    } else if (status == -5) {
        console_puts("delete failed: not found\n");
    } else if (status == -9) {
        console_puts("delete failed: directory is not empty\n");
    } else if (status != 0) {
        console_puts("delete failed\n");
    }
}

static void cmd_rename(const char *args, const boot_info_t *info) {
    (void)info;

    int drive = active_drive();
    char *old_name = 0;
    char *new_name = 0;
    uint32_t target_parent = LAINFS_ROOT_DIR;
    const char *target_name = 0;
    int status = 0;

    if (drive < 0) {
        console_puts("select a mounted drive first, for example C:\n");
        return;
    }

    split_first_arg((char *)args, &old_name, &new_name);
    if (*old_name == '\0' || *new_name == '\0') {
        console_puts("usage: rename old new\n");
        return;
    }

    if (streq(new_name, ".")) {
        target_parent = cwd_dirs[drive];
        target_name = old_name;
    } else if (streq(new_name, "..")) {
        status = lainfs_parent_dir((char)('A' + drive), cwd_dirs[drive], &target_parent);
        if (status != 0) {
            console_puts("rename failed\n");
            return;
        }
        target_name = old_name;
    } else if (lainfs_find_dir((char)('A' + drive), cwd_dirs[drive], new_name, &target_parent) == 0) {
        target_name = old_name;
    } else {
        target_parent = cwd_dirs[drive];
        target_name = new_name;
    }

    status = lainfs_rename_in_dir((char)('A' + drive),
                                  cwd_dirs[drive],
                                  old_name,
                                  target_parent,
                                  target_name);
    if (status == -2) {
        console_puts("rename failed: invalid name\n");
    } else if (status == -3) {
        console_puts("drive is not formatted as lainfs\n");
    } else if (status == -5) {
        console_puts("rename failed: not found\n");
    } else if (status == -6) {
        console_puts("rename failed: target exists\n");
    } else if (status == -8) {
        console_puts("rename failed: cannot move a directory into itself\n");
    } else if (status != 0) {
        console_puts("rename failed\n");
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

    int status = lainfs_write_file_in_dir((char)('A' + drive), cwd_dirs[drive], name, text);
    if (status == -2) {
        console_puts("write failed: invalid filename\n");
    } else if (status == -3) {
        console_puts("drive is not formatted as lainfs\n");
    } else if (status == -5) {
        console_puts("directory is full\n");
    } else if (status == -8) {
        console_puts("write failed: name is a directory\n");
    } else if (status == -9) {
        console_puts("write failed: disk is full\n");
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

    int status = lainfs_read_file_in_dir((char)('A' + drive), cwd_dirs[drive], name);
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

    int status = editor_run_in_dir((char)('A' + drive), cwd_dirs[drive], name);
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

static void cmd_keymap(const char *args, const boot_info_t *info) {
    const char *layout = skip_const_spaces(args);

    (void)info;

    if (*layout == '\0') {
        console_puts("keyboard layout: ");
        console_puts(keyboard_layout_name(keyboard_get_layout()));
        console_puts("\n");
        return;
    }

    if (streq(layout, "us")) {
        keyboard_set_layout(KEYBOARD_LAYOUT_US);
    } else if (streq(layout, "de")) {
        keyboard_set_layout(KEYBOARD_LAYOUT_DE);
    } else {
        console_puts("usage: keymap us|de\n");
        return;
    }

    console_puts("keyboard layout set to ");
    console_puts(keyboard_layout_name(keyboard_get_layout()));
    console_puts("\n");
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

static int run_script_file(const char *name, const boot_info_t *info, int quiet_missing) {
    char *script = 0;
    uint32_t size = 0;
    int drive = active_drive();

    if (drive < 0) {
        return -1;
    }

    if (script_depth >= SCRIPT_MAX_DEPTH) {
        console_puts("run failed: script nesting too deep\n");
        return -2;
    }

    script = script_buffers[script_depth];

    int status = lainfs_load_file_in_dir((char)('A' + drive),
                                         active_dir(),
                                         name,
                                         script,
                                         SCRIPT_BUFFER_SIZE,
                                         &size);
    if (status == -3) {
        if (!quiet_missing) {
            console_puts("drive is not formatted as lainfs\n");
        }

        return status;
    }

    if (status == -5) {
        if (!quiet_missing) {
            console_puts("script not found\n");
        }

        return status;
    }

    if (status != 0) {
        console_puts("run failed\n");
        return status;
    }

    script[size] = '\0';

    ++script_depth;
    run_script_text(script, size, info);
    --script_depth;

    return 0;
}

static void cmd_run(const char *args, const boot_info_t *info) {
    const char *name = skip_const_spaces(args);

    if (active_drive() < 0) {
        console_puts("select a mounted drive first, for example S:\n");
        return;
    }

    if (*name == '\0') {
        console_puts("usage: run scriptname\n");
        return;
    }

    run_script_file(name, info, 0);
}

static void cmd_exec(const char *args, const boot_info_t *info) {
    (void)info;

    const char *name = skip_const_spaces(args);
    uint32_t size = 0;
    int drive = active_drive();

    static const exec_api_t api = {
        EXEC_API_MAGIC,
        1,
        console_puts,
        console_put_hex64,
        console_put_dec64,
        timer_ticks,
    };

    if (drive < 0) {
        console_puts("select a mounted drive first, for example S:\n");
        return;
    }

    if (*name == '\0') {
        console_puts("usage: exec file.bin\n");
        return;
    }

    zero_memory(exec_buffer, EXEC_BUFFER_SIZE);

    int status = lainfs_load_file_in_dir((char)('A' + drive),
                                         cwd_dirs[drive],
                                         name,
                                         (char *)exec_buffer,
                                         EXEC_BUFFER_SIZE,
                                         &size);
    if (status == -3) {
        console_puts("drive is not formatted as lainfs\n");
        return;
    }

    if (status == -5) {
        console_puts("binary not found\n");
        return;
    }

    if (status != 0) {
        console_puts("exec failed\n");
        return;
    }

    if (size == 0) {
        console_puts("exec failed: empty binary\n");
        return;
    }

    console_puts("running ");
    console_puts(name);
    console_puts("\n");

    exec_program_t program = (exec_program_t)(uintptr_t)exec_buffer;
    program(&api);

    zero_memory(exec_buffer, EXEC_BUFFER_SIZE);

    console_puts("\nprogram returned\n");
}

static void cmd_asm(const char *args, const boot_info_t *info) {
    (void)info;

    static char source[ASM_SOURCE_SIZE + 1];
    uint32_t source_size = 0;
    uint32_t output_size = 0;
    uint32_t error_line = 0;
    char *mutable_args = (char *)args;
    char *source_name = 0;
    char *output_name = 0;
    int drive = active_drive();

    if (drive < 0) {
        console_puts("select a mounted drive first, for example S:\n");
        return;
    }

    split_first_arg(mutable_args, &source_name, &output_name);
    if (*source_name == '\0' || *output_name == '\0') {
        console_puts("usage: asm source.asm output.bin\n");
        return;
    }

    int status = lainfs_load_file_in_dir((char)('A' + drive),
                                         cwd_dirs[drive],
                                         source_name,
                                         source,
                                         ASM_SOURCE_SIZE,
                                         &source_size);
    if (status == -3) {
        console_puts("drive is not formatted as lainfs\n");
        return;
    }

    if (status == -5) {
        console_puts("source not found\n");
        return;
    }

    if (status != 0) {
        console_puts("asm failed: could not load source\n");
        return;
    }

    source[source_size] = '\0';
    zero_memory(asm_output, EXEC_BUFFER_SIZE);
    zero_memory(exec_buffer, EXEC_BUFFER_SIZE);
    if (assembler_assemble_source_ex(source,
                                     source_size,
                                     asm_output,
                                     EXEC_BUFFER_SIZE,
                                     (uint64_t)(uintptr_t)exec_buffer,
                                     &output_size,
                                     &error_line) != 0) {
        console_puts("asm failed: unsupported syntax");
        if (error_line != 0) {
            console_puts(" on line ");
            console_put_dec64(error_line);
        }
        console_puts("\n");
        return;
    }

    status = lainfs_save_file_in_dir((char)('A' + drive),
                                     cwd_dirs[drive],
                                     output_name,
                                     (const char *)asm_output,
                                     output_size);
    if (status == -9) {
        console_puts("asm failed: disk is full\n");
        return;
    }

    if (status != 0) {
        console_puts("asm failed: could not save output\n");
        return;
    }

    console_puts("assembled ");
    console_puts(output_name);
    console_puts(" bytes=");
    console_put_dec64(output_size);
    console_puts("\n");
}

static int compile_z_source_file(const char *name,
                                 char *source,
                                 uint32_t *source_size,
                                 uint32_t *compile_error_line,
                                 uint32_t *asm_size,
                                 int *drive_out) {
    int drive = active_drive();
    int status;

    if (drive_out) {
        *drive_out = drive;
    }

    if (drive < 0) {
        return -10;
    }

    status = lainfs_load_file_in_dir((char)('A' + drive),
                                     cwd_dirs[drive],
                                     name,
                                     source,
                                     ASM_SOURCE_SIZE,
                                     source_size);
    if (status != 0) {
        return status;
    }

    source[*source_size] = '\0';
    zero_memory(zscript_output, sizeof(zscript_output));

    if (zscript_compile_source(source,
                               *source_size,
                               zscript_output,
                               ASM_SOURCE_SIZE,
                               asm_size,
                               compile_error_line) != 0) {
        return -20;
    }

    zscript_output[*asm_size] = '\0';
    return 0;
}

static void cmd_zc(const char *args, const boot_info_t *info) {
    (void)info;

    static char source[ASM_SOURCE_SIZE + 1];
    uint32_t source_size = 0;
    uint32_t asm_size = 0;
    uint32_t output_size = 0;
    uint32_t compile_error_line = 0;
    uint32_t assemble_error_line = 0;
    char *mutable_args = (char *)args;
    char *source_name = 0;
    char *output_name = 0;
    int drive = active_drive();
    int status = 0;

    if (drive < 0) {
        console_puts("select a mounted drive first, for example S:\n");
        return;
    }

    split_first_arg(mutable_args, &source_name, &output_name);
    if (*source_name == '\0' || *output_name == '\0') {
        console_puts("usage: zc source.Z output.bin\n");
        return;
    }

    status = compile_z_source_file(source_name,
                                   source,
                                   &source_size,
                                   &compile_error_line,
                                   &asm_size,
                                   &drive);
    if (status == -3) {
        console_puts("drive is not formatted as lainfs\n");
        return;
    }
    if (status == -5) {
        console_puts(".Z source not found\n");
        return;
    }
    if (status == -20) {
        console_puts("zc failed: unsupported .Z syntax");
        if (compile_error_line != 0) {
            console_puts(" on line ");
            console_put_dec64(compile_error_line);
        }
        console_puts("\n");
        return;
    }
    if (status != 0) {
        console_puts("zc failed: could not load source\n");
        return;
    }

    zero_memory(asm_output, EXEC_BUFFER_SIZE);
    zero_memory(exec_buffer, EXEC_BUFFER_SIZE);
    if (assembler_assemble_source_ex(zscript_output,
                                     asm_size,
                                     asm_output,
                                     EXEC_BUFFER_SIZE,
                                     (uint64_t)(uintptr_t)exec_buffer,
                                     &output_size,
                                     &assemble_error_line) != 0) {
        console_puts("zc failed: compiler emitted unsupported asm");
        if (assemble_error_line != 0) {
            console_puts(" on line ");
            console_put_dec64(assemble_error_line);
        }
        console_puts("\n");
        return;
    }

    status = lainfs_save_file_in_dir((char)('A' + drive),
                                     cwd_dirs[drive],
                                     output_name,
                                     (const char *)asm_output,
                                     output_size);
    if (status == -9) {
        console_puts("zc failed: disk is full\n");
        return;
    }
    if (status != 0) {
        console_puts("zc failed: could not save output\n");
        return;
    }

    console_puts("compiled ");
    console_puts(source_name);
    console_puts(" to ");
    console_puts(output_name);
    console_puts(" bytes=");
    console_put_dec64(output_size);
    console_puts("\n");
}

static void cmd_zco(const char *args, const boot_info_t *info) {
    (void)info;

    static char source[ASM_SOURCE_SIZE + 1];
    uint32_t source_size = 0;
    uint32_t asm_size = 0;
    uint32_t object_size = 0;
    uint32_t compile_error_line = 0;
    char entry_label[32];
    char label_prefix[8];
    char *mutable_args = (char *)args;
    char *source_name = 0;
    char *output_name = 0;
    int drive = active_drive();
    int status = 0;

    if (drive < 0) {
        console_puts("select a mounted drive first, for example S:\n");
        return;
    }

    split_first_arg(mutable_args, &source_name, &output_name);
    if (*source_name == '\0' || *output_name == '\0') {
        console_puts("usage: zco source.Z output.zo\n");
        return;
    }

    status = lainfs_load_file_in_dir((char)('A' + drive),
                                     cwd_dirs[drive],
                                     source_name,
                                     source,
                                     ASM_SOURCE_SIZE,
                                     &source_size);
    if (status == -3) {
        console_puts("drive is not formatted as lainfs\n");
        return;
    }
    if (status == -5) {
        console_puts(".Z source not found\n");
        return;
    }
    if (status != 0) {
        console_puts("zco failed: could not load source\n");
        return;
    }

    source[source_size] = '\0';
    zero_memory(zscript_output, sizeof(zscript_output));
    make_zobject_prefix(output_name, label_prefix, sizeof(label_prefix));
    if (zscript_compile_source_object(source,
                                      source_size,
                                      label_prefix,
                                      zscript_output,
                                      ASM_SOURCE_SIZE,
                                      &asm_size,
                                      &compile_error_line,
                                      entry_label,
                                      sizeof(entry_label)) != 0) {
        console_puts("zco failed: unsupported .Z syntax");
        if (compile_error_line != 0) {
            console_puts(" on line ");
            console_put_dec64(compile_error_line);
        }
        console_puts("\n");
        return;
    }
    zscript_output[asm_size] = '\0';

    zero_memory(asm_output, EXEC_BUFFER_SIZE);
    if (zobject_from_asm(zscript_output,
                         asm_size,
                         entry_label,
                         asm_output,
                         EXEC_BUFFER_SIZE,
                         &object_size) != 0) {
        console_puts("zco failed: object is too large\n");
        return;
    }

    status = lainfs_save_file_in_dir((char)('A' + drive),
                                     cwd_dirs[drive],
                                     output_name,
                                     (const char *)asm_output,
                                     object_size);
    if (status == -9) {
        console_puts("zco failed: disk is full\n");
        return;
    }
    if (status != 0) {
        console_puts("zco failed: could not save output\n");
        return;
    }

    console_puts("compiled object ");
    console_puts(output_name);
    console_puts(" bytes=");
    console_put_dec64(object_size);
    console_puts("\n");
}

static void cmd_zlink(const char *args, const boot_info_t *info) {
    (void)info;

    const unsigned char *objects[ZLINK_MAX_OBJECTS];
    uint32_t object_sizes[ZLINK_MAX_OBJECTS];
    uint32_t object_count = 0;
    uint32_t object_offset = 0;
    uint32_t output_size = 0;
    uint32_t error_line = 0;
    char *mutable_args = (char *)args;
    char *tokens[ZLINK_MAX_OBJECTS + 1u];
    uint32_t token_count = 0;
    char *output_name = 0;
    int drive = active_drive();
    int status = 0;

    if (drive < 0) {
        console_puts("select a mounted drive first, for example S:\n");
        return;
    }

    mutable_args = skip_spaces(mutable_args);
    while (*mutable_args != '\0') {
        if (token_count >= ZLINK_MAX_OBJECTS + 1u) {
            console_puts("usage: zlink input.zo [more.zo ...] output.bin\n");
            return;
        }

        tokens[token_count++] = mutable_args;
        while (*mutable_args && *mutable_args != ' ' && *mutable_args != '\t') {
            ++mutable_args;
        }
        if (*mutable_args != '\0') {
            *mutable_args++ = '\0';
            mutable_args = skip_spaces(mutable_args);
        }
    }

    if (token_count < 2u) {
        console_puts("usage: zlink input.zo [more.zo ...] output.bin\n");
        return;
    }

    output_name = tokens[token_count - 1u];
    zero_memory(asm_output, EXEC_BUFFER_SIZE);

    for (uint32_t i = 0; i + 1u < token_count; ++i) {
        uint32_t remaining = EXEC_BUFFER_SIZE - object_offset;

        objects[object_count] = asm_output + object_offset;
        status = lainfs_load_file_in_dir((char)('A' + drive),
                                         cwd_dirs[drive],
                                         tokens[i],
                                         (char *)(asm_output + object_offset),
                                         remaining,
                                         &object_sizes[object_count]);
        if (status == -3) {
            console_puts("drive is not formatted as lainfs\n");
            return;
        }
        if (status == -5) {
            console_puts(".zo object not found: ");
            console_puts(tokens[i]);
            console_puts("\n");
            return;
        }
        if (status != 0) {
            console_puts("zlink failed: could not load object\n");
            return;
        }

        object_offset += object_sizes[object_count];
        ++object_count;
    }

    zero_memory(exec_buffer, EXEC_BUFFER_SIZE);
    if (zobject_link_flat_many(objects,
                               object_sizes,
                               object_count,
                               exec_buffer,
                               EXEC_BUFFER_SIZE,
                               (uint64_t)(uintptr_t)exec_buffer,
                               &output_size,
                               &error_line) != 0) {
        console_puts("zlink failed: unsupported object");
        if (error_line != 0) {
            console_puts(" asm line ");
            console_put_dec64(error_line);
        }
        console_puts("\n");
        return;
    }

    status = lainfs_save_file_in_dir((char)('A' + drive),
                                     cwd_dirs[drive],
                                     output_name,
                                     (const char *)exec_buffer,
                                     output_size);
    if (status == -9) {
        console_puts("zlink failed: disk is full\n");
        return;
    }
    if (status != 0) {
        console_puts("zlink failed: could not save output\n");
        return;
    }

    console_puts("linked ");
    console_put_dec64(object_count);
    console_puts(" object(s)");
    console_puts(" to ");
    console_puts(output_name);
    console_puts(" bytes=");
    console_put_dec64(output_size);
    console_puts("\n");
}

static int zmodule_find_free_slot(void) {
    for (uint32_t i = 0; i < ZMODULE_MAX_MODULES; ++i) {
        if (!zmodule_slots[i].loaded) {
            return (int)i;
        }
    }

    return -1;
}

static int zmodule_collect_exports(zobject_resolved_symbol_t *symbols,
                                   uint32_t capacity,
                                   uint32_t *out_count) {
    uint32_t count = 0;

    if (symbols == 0 || out_count == 0) {
        return -1;
    }

    for (uint32_t i = 0; i < ZMODULE_MAX_MODULES; ++i) {
        if (!zmodule_slots[i].loaded) {
            continue;
        }

        for (uint32_t j = 0; j < zmodule_slots[i].export_count; ++j) {
            if (count >= capacity) {
                return -1;
            }

            symbols[count++] = zmodule_slots[i].exports[j];
        }
    }

    *out_count = count;
    return 0;
}

static void cmd_zmod(const char *args, const boot_info_t *info) {
    (void)info;

    const unsigned char *objects[ZLINK_MAX_OBJECTS];
    uint32_t object_sizes[ZLINK_MAX_OBJECTS];
    uint32_t object_count = 0;
    uint32_t object_offset = 0;
    uint32_t output_size = 0;
    uint32_t error_line = 0;
    uint32_t resident_symbol_count = 0;
    uint32_t export_symbol_count = 0;
    char *mutable_args = (char *)args;
    char *tokens[ZLINK_MAX_OBJECTS];
    uint32_t token_count = 0;
    int slot_index = -1;
    int drive = active_drive();
    int status = 0;
    zobject_resolved_symbol_t resident_symbols[ZOBJECT_MAX_RESOLVED_SYMBOLS];
    zobject_resolved_symbol_t export_symbols[ZMODULE_MAX_EXPORTS];

    static const exec_api_t api = {
        EXEC_API_MAGIC,
        1,
        console_puts,
        console_put_hex64,
        console_put_dec64,
        timer_ticks,
    };

    if (drive < 0) {
        console_puts("select a mounted drive first, for example S:\n");
        return;
    }

    mutable_args = skip_spaces(mutable_args);
    while (*mutable_args != '\0') {
        if (token_count >= ZLINK_MAX_OBJECTS) {
            console_puts("usage: zmod input.zo [more.zo ...]\n");
            return;
        }

        tokens[token_count++] = mutable_args;
        while (*mutable_args && *mutable_args != ' ' && *mutable_args != '\t') {
            ++mutable_args;
        }
        if (*mutable_args != '\0') {
            *mutable_args++ = '\0';
            mutable_args = skip_spaces(mutable_args);
        }
    }

    if (token_count == 0) {
        console_puts("usage: zmod input.zo [more.zo ...]\n");
        return;
    }

    slot_index = zmodule_find_free_slot();
    if (slot_index < 0) {
        console_puts("zmod failed: no free resident module slots\n");
        return;
    }

    if (zmodule_collect_exports(resident_symbols,
                                ZOBJECT_MAX_RESOLVED_SYMBOLS,
                                &resident_symbol_count) != 0) {
        console_puts("zmod failed: resident symbol table is full\n");
        return;
    }

    zero_memory(asm_output, EXEC_BUFFER_SIZE);
    for (uint32_t i = 0; i < token_count; ++i) {
        uint32_t remaining = EXEC_BUFFER_SIZE - object_offset;

        objects[object_count] = asm_output + object_offset;
        status = lainfs_load_file_in_dir((char)('A' + drive),
                                         cwd_dirs[drive],
                                         tokens[i],
                                         (char *)(asm_output + object_offset),
                                         remaining,
                                         &object_sizes[object_count]);
        if (status == -3) {
            console_puts("drive is not formatted as lainfs\n");
            return;
        }
        if (status == -5) {
            console_puts(".zo module not found: ");
            console_puts(tokens[i]);
            console_puts("\n");
            return;
        }
        if (status != 0) {
            console_puts("zmod failed: could not load module\n");
            return;
        }

        object_offset += object_sizes[object_count];
        ++object_count;
    }

    zero_memory(zmodule_slots[slot_index].image, EXEC_BUFFER_SIZE);
    if (zobject_link_flat_many_ex(objects,
                                  object_sizes,
                                  object_count,
                                  zmodule_slots[slot_index].image,
                                  EXEC_BUFFER_SIZE,
                                  (uint64_t)(uintptr_t)zmodule_slots[slot_index].image,
                                  &output_size,
                                  &error_line,
                                  resident_symbols,
                                  resident_symbol_count,
                                  export_symbols,
                                  ZMODULE_MAX_EXPORTS,
                                  &export_symbol_count) != 0) {
        console_puts("zmod failed: unresolved or unsupported module");
        if (error_line != 0) {
            console_puts(" asm line ");
            console_put_dec64(error_line);
        }
        console_puts("\n");
        return;
    }

    zmodule_slots[slot_index].loaded = 1;
    zmodule_slots[slot_index].image_size = output_size;
    zmodule_slots[slot_index].object_count = object_count;
    zmodule_slots[slot_index].export_count = export_symbol_count;
    copy_text_limited(zmodule_slots[slot_index].name,
                      sizeof(zmodule_slots[slot_index].name),
                      tokens[0]);
    for (uint32_t i = 0; i < export_symbol_count; ++i) {
        zmodule_slots[slot_index].exports[i] = export_symbols[i];
    }

    console_puts("loading ");
    console_put_dec64(object_count);
    console_puts(" module object(s) in slot ");
    console_put_dec64((uint32_t)slot_index);
    console_puts(" at 0x");
    console_put_hex64((uint64_t)(uintptr_t)zmodule_slots[slot_index].image);
    console_puts(" bytes=");
    console_put_dec64(output_size);
    console_puts(" exports=");
    console_put_dec64(export_symbol_count);
    console_puts("\n");

    ((exec_program_t)(uintptr_t)zmodule_slots[slot_index].image)(&api);

    console_puts("\nmodule resident\n");
}

static void cmd_zmods(const char *args, const boot_info_t *info) {
    (void)args;
    (void)info;

    int any = 0;

    for (uint32_t i = 0; i < ZMODULE_MAX_MODULES; ++i) {
        if (!zmodule_slots[i].loaded) {
            continue;
        }

        any = 1;
        console_puts("#");
        console_put_dec64(i);
        console_puts(" ");
        console_puts(zmodule_slots[i].name);
        console_puts(" base=0x");
        console_put_hex64((uint64_t)(uintptr_t)zmodule_slots[i].image);
        console_puts(" bytes=");
        console_put_dec64(zmodule_slots[i].image_size);
        console_puts(" objects=");
        console_put_dec64(zmodule_slots[i].object_count);
        console_puts(" exports=");
        console_put_dec64(zmodule_slots[i].export_count);
        console_puts("\n");

        for (uint32_t j = 0; j < zmodule_slots[i].export_count; ++j) {
            console_puts("  ");
            console_puts(zmodule_slots[i].exports[j].name);
            console_puts(" = 0x");
            console_put_hex64(zmodule_slots[i].exports[j].value);
            console_puts("\n");
        }
    }

    if (!any) {
        console_puts("no resident modules\n");
    }
}

static void cmd_zrun(const char *args, const boot_info_t *info) {
    (void)info;

    static char source[ASM_SOURCE_SIZE + 1];
    const char *name = skip_const_spaces(args);
    uint32_t source_size = 0;
    uint32_t asm_size = 0;
    uint32_t output_size = 0;
    uint32_t compile_error_line = 0;
    uint32_t assemble_error_line = 0;
    int drive = active_drive();
    int status = 0;

    static const exec_api_t api = {
        EXEC_API_MAGIC,
        1,
        console_puts,
        console_put_hex64,
        console_put_dec64,
        timer_ticks,
    };

    if (drive < 0) {
        console_puts("select a mounted drive first, for example S:\n");
        return;
    }

    if (*name == '\0') {
        console_puts("usage: zrun source.Z\n");
        return;
    }

    status = compile_z_source_file(name,
                                   source,
                                   &source_size,
                                   &compile_error_line,
                                   &asm_size,
                                   &drive);
    if (status == -3) {
        console_puts("drive is not formatted as lainfs\n");
        return;
    }
    if (status == -5) {
        console_puts(".Z source not found\n");
        return;
    }
    if (status == -20) {
        console_puts("zrun failed: unsupported .Z syntax");
        if (compile_error_line != 0) {
            console_puts(" on line ");
            console_put_dec64(compile_error_line);
        }
        console_puts("\n");
        return;
    }
    if (status != 0) {
        console_puts("zrun failed: could not load source\n");
        return;
    }

    zero_memory(exec_buffer, EXEC_BUFFER_SIZE);
    if (assembler_assemble_source_ex(zscript_output,
                                     asm_size,
                                     exec_buffer,
                                     EXEC_BUFFER_SIZE,
                                     (uint64_t)(uintptr_t)exec_buffer,
                                     &output_size,
                                     &assemble_error_line) != 0) {
        console_puts("zrun failed: compiler emitted unsupported asm");
        if (assemble_error_line != 0) {
            console_puts(" on line ");
            console_put_dec64(assemble_error_line);
        }
        console_puts("\n");
        return;
    }

    console_puts("running ");
    console_puts(name);
    console_puts(" at 0x");
    console_put_hex64((uint64_t)(uintptr_t)exec_buffer);
    console_puts("\n");

    ((exec_program_t)(uintptr_t)exec_buffer)(&api);
    zero_memory(exec_buffer, EXEC_BUFFER_SIZE);

    console_puts("\nprogram returned\n");
}

static void cmd_zasm(const char *args, const boot_info_t *info) {
    (void)info;

    static char source[ASM_SOURCE_SIZE + 1];
    uint32_t source_size = 0;
    uint32_t asm_size = 0;
    uint32_t compile_error_line = 0;
    char *mutable_args = (char *)args;
    char *source_name = 0;
    char *output_name = 0;
    int drive = active_drive();
    int status = 0;

    if (drive < 0) {
        console_puts("select a mounted drive first, for example S:\n");
        return;
    }

    split_first_arg(mutable_args, &source_name, &output_name);
    if (*source_name == '\0') {
        console_puts("usage: zasm source.Z [output.asm]\n");
        return;
    }

    status = compile_z_source_file(source_name,
                                   source,
                                   &source_size,
                                   &compile_error_line,
                                   &asm_size,
                                   &drive);
    if (status == -3) {
        console_puts("drive is not formatted as lainfs\n");
        return;
    }
    if (status == -5) {
        console_puts(".Z source not found\n");
        return;
    }
    if (status == -20) {
        console_puts("zasm failed: unsupported .Z syntax");
        if (compile_error_line != 0) {
            console_puts(" on line ");
            console_put_dec64(compile_error_line);
        }
        console_puts("\n");
        return;
    }
    if (status != 0) {
        console_puts("zasm failed: could not load source\n");
        return;
    }

    if (*output_name != '\0') {
        status = lainfs_save_file_in_dir((char)('A' + drive),
                                         cwd_dirs[drive],
                                         output_name,
                                         zscript_output,
                                         asm_size);
        if (status == -9) {
            console_puts("zasm failed: disk is full\n");
            return;
        }
        if (status != 0) {
            console_puts("zasm failed: could not save output\n");
            return;
        }

        console_puts("wrote generated asm to ");
        console_puts(output_name);
        console_puts("\n");
        return;
    }

    console_puts("generated asm for ");
    console_puts(source_name);
    console_puts(":\n");
    console_puts(zscript_output);
    if (asm_size == 0 || zscript_output[asm_size - 1] != '\n') {
        console_puts("\n");
    }
}

static void cmd_bgcolor(const char *args, const boot_info_t *info) {
    (void)info;

    unsigned int color = 0;

    if (parse_color_arg(args, &color) != 0) {
        console_puts("Invalid color, use 6 digit hex code!\n");
        return;
    }

    console_set_bg_color(color);
}

static void cmd_fgcolor(const char *args, const boot_info_t *info) {
    (void)info;

    unsigned int color = 0;

    if (parse_color_arg(args, &color) != 0) {
        console_puts("Invalid color, use 6 digit hex code!\n");
        return;
    }

    console_set_fg_color(color);
}

void shell_set_session(unsigned int session) {
    if (session >= SHELL_MAX_SESSIONS) {
        return;
    }

    ensure_session_initialized(session);
    active_session_index = session;
}

void shell_init(void) {
    zero_memory(shell_sessions, sizeof(shell_sessions));

    for (int i = 0; i < MAX_DRIVES; ++i) {
        drives[i].present = 0;
        drives[i].label[0] = '\0';
    }

    active_session_index = 0;
    init_session_blank(&shell_sessions[0]);
}

void shell_run_autoexec(const char *name, const boot_info_t *info) {
    int previous_drive = current_drive;

    if (active_drive() < 0) {
        return;
    }

    run_script_file(name, info, 1);
    current_drive = previous_drive;
}

void shell_print_prompt(void) {
    if (current_drive >= 0 && drives[current_drive].present) {
        print_drive_name(current_drive);
        console_puts(cwd_paths[current_drive]);
        console_puts("> ");
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
