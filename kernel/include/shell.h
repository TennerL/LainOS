#ifndef SHELL_H
#define SHELL_H

#include "bootinfo.h"

void shell_init(void);
int shell_mount_first_lainfs(char drive_letter);
void shell_set_session(unsigned int session);
void shell_run_autoexec(const char *name, const boot_info_t *info);
void shell_print_prompt(void);
void shell_run_command(char *line, const boot_info_t *info);
void shell_registry_load(void);
void shell_registry_save(void);
void shell_modules_tick(void);
uint32_t shell_module_count(void);
const char *shell_module_name(uint32_t index);
int shell_module_has_export(uint32_t index, const char *export_name);
int shell_module_is_ui_app(uint32_t index);
int shell_module_tick(uint32_t index);
int shell_module_call(uint32_t index, const char *export_name);
int shell_module_key(uint32_t index, uint32_t key_type, uint32_t ch);
int shell_module_mouse(uint32_t index, uint32_t x, uint32_t y, uint32_t buttons, int32_t wheel);
int shell_api_mkdir(const char *path);
int shell_api_delete(const char *path);
int shell_api_write_file(const char *path, const char *text);
int shell_api_cat_file(const char *path);
int shell_api_file_size(const char *path);
int shell_api_read_file(const char *path, char *buffer, uint32_t capacity);
int shell_api_load_file_shared(const char *path);
uint8_t *shell_api_file_buffer(void);
int shell_api_http_get(const char *url, char *buffer, uint32_t capacity);
int shell_api_rename(const char *old_path, const char *new_path);
int shell_api_copy_file(const char *src_path, const char *dst_path);
int shell_api_strlen(const char *text);
int shell_api_strcmp(const char *a, const char *b);
int shell_api_starts_with(const char *text, const char *prefix);
int shell_api_atoi(const char *text);
int shell_api_list_dir(const char *path);
int shell_api_chdir(const char *path);
int shell_api_dir_count(const char *path);
int shell_api_dir_name(const char *path, uint32_t index, char *buffer, uint32_t capacity);
int shell_api_dir_type(const char *path, uint32_t index);
int shell_api_dir_size(const char *path, uint32_t index);
int shell_api_zbuild(const char *target);
int shell_api_ztest(const char *target);
int shell_api_zinstall(const char *target);
int shell_api_zmod(const char *target);
int shell_api_zunload(const char *target);
int shell_api_zreload(const char *target);

#endif
