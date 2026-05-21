#ifndef REGISTRY_H
#define REGISTRY_H

#include <stdint.h>

#define REGISTRY_OK 0
#define REGISTRY_ERR_INPUT -1
#define REGISTRY_ERR_NOT_FOUND -2
#define REGISTRY_ERR_FULL -3
#define REGISTRY_ERR_TOO_LONG -4

void registry_init(void);
void registry_set_save_hook(void (*hook)(void));
void registry_suspend_save(int suspend);
int registry_set(const char *key, const char *value);
const char *registry_get(const char *key);
int registry_get_u32(const char *key, uint32_t default_value, uint32_t *out_value);
uint32_t registry_count(void);
const char *registry_key_at(uint32_t index);
const char *registry_value_at(uint32_t index);

#endif
