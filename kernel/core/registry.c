#include <stdint.h>

#include "registry.h"

#define REGISTRY_MAX_ENTRIES 96u
#define REGISTRY_KEY_SIZE 48u
#define REGISTRY_VALUE_SIZE 96u

typedef struct {
    int used;
    char key[REGISTRY_KEY_SIZE];
    char value[REGISTRY_VALUE_SIZE];
} registry_entry_t;

static registry_entry_t registry_entries[REGISTRY_MAX_ENTRIES];
static void (*registry_save_hook)(void);
static int registry_save_suspended;

static int registry_streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

static uint32_t registry_strlen(const char *s) {
    uint32_t len = 0;

    if (s == 0) {
        return 0;
    }
    while (s[len]) {
        ++len;
    }
    return len;
}

static void registry_copy(char *dst, uint32_t dst_size, const char *src) {
    uint32_t i = 0;

    if (dst_size == 0u) {
        return;
    }
    while (src != 0 && src[i] != '\0' && i + 1u < dst_size) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

static int registry_find(const char *key) {
    for (uint32_t i = 0; i < REGISTRY_MAX_ENTRIES; ++i) {
        if (registry_entries[i].used && registry_streq(registry_entries[i].key, key)) {
            return (int)i;
        }
    }
    return -1;
}

static int registry_hex_digit(char ch, uint32_t *out) {
    if (ch >= '0' && ch <= '9') {
        *out = (uint32_t)(ch - '0');
        return 0;
    }
    if (ch >= 'a' && ch <= 'f') {
        *out = 10u + (uint32_t)(ch - 'a');
        return 0;
    }
    if (ch >= 'A' && ch <= 'F') {
        *out = 10u + (uint32_t)(ch - 'A');
        return 0;
    }
    return -1;
}

static int registry_parse_u32(const char *text, uint32_t *out_value) {
    uint32_t value = 0;
    uint32_t digit = 0;
    int hex = 0;

    if (text == 0 || out_value == 0) {
        return -1;
    }
    while (*text == ' ' || *text == '\t') {
        ++text;
    }
    if (*text == '#') {
        hex = 1;
        ++text;
    } else if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        hex = 1;
        text += 2;
    }

    if (*text == '\0') {
        return -1;
    }

    while (*text != '\0' && *text != ' ' && *text != '\t') {
        if (hex) {
            if (registry_hex_digit(*text, &digit) != 0) {
                return -1;
            }
            value = (value << 4) | digit;
        } else {
            if (*text < '0' || *text > '9') {
                return -1;
            }
            value = value * 10u + (uint32_t)(*text - '0');
        }
        ++text;
    }

    *out_value = value;
    return 0;
}

void registry_init(void) {
    (void)registry_set("desktop.theme", "lain");
    (void)registry_set("desktop.bg.top", "0x35063e");
    (void)registry_set("desktop.bg.bottom", "0x2b1d3d");
    (void)registry_set("desktop.bg.image", "");
    (void)registry_set("desktop.topbar", "0x100b18");
    (void)registry_set("desktop.taskbar", "0x171020");
    (void)registry_set("desktop.panel", "0x221a2d");
    (void)registry_set("desktop.panel.inner", "0x3a2f49");
    (void)registry_set("desktop.accent", "0xe05f4f");
    (void)registry_set("desktop.accent.soft", "0xb98556");
    (void)registry_set("desktop.text", "0xf6eadb");
    (void)registry_set("desktop.button", "0x8f3f62");
    (void)registry_set("desktop.task.active", "0x4b2347");
    (void)registry_set("desktop.task.inactive", "0x2d2038");
    (void)registry_set("desktop.title.left", "0x7c3a78");
    (void)registry_set("desktop.title.right", "0xe05f4f");
}

void registry_set_save_hook(void (*hook)(void)) {
    registry_save_hook = hook;
}

void registry_suspend_save(int suspend) {
    registry_save_suspended = suspend != 0;
}

int registry_set(const char *key, const char *value) {
    int index;

    if (key == 0 || value == 0 || *key == '\0') {
        return REGISTRY_ERR_INPUT;
    }
    if (registry_strlen(key) >= REGISTRY_KEY_SIZE ||
        registry_strlen(value) >= REGISTRY_VALUE_SIZE) {
        return REGISTRY_ERR_TOO_LONG;
    }

    index = registry_find(key);
    if (index < 0) {
        for (uint32_t i = 0; i < REGISTRY_MAX_ENTRIES; ++i) {
            if (!registry_entries[i].used) {
                index = (int)i;
                registry_entries[i].used = 1;
                registry_copy(registry_entries[i].key, REGISTRY_KEY_SIZE, key);
                break;
            }
        }
    }
    if (index < 0) {
        return REGISTRY_ERR_FULL;
    }

    registry_copy(registry_entries[index].value, REGISTRY_VALUE_SIZE, value);
    if (!registry_save_suspended && registry_save_hook != 0) {
        registry_save_hook();
    }
    return REGISTRY_OK;
}

const char *registry_get(const char *key) {
    int index;

    if (key == 0) {
        return 0;
    }
    index = registry_find(key);
    return index < 0 ? 0 : registry_entries[index].value;
}

int registry_get_u32(const char *key, uint32_t default_value, uint32_t *out_value) {
    const char *value;

    if (out_value == 0) {
        return REGISTRY_ERR_INPUT;
    }

    value = registry_get(key);
    if (value == 0 || registry_parse_u32(value, out_value) != 0) {
        *out_value = default_value;
        return REGISTRY_ERR_NOT_FOUND;
    }
    return REGISTRY_OK;
}

uint32_t registry_count(void) {
    uint32_t count = 0;

    for (uint32_t i = 0; i < REGISTRY_MAX_ENTRIES; ++i) {
        if (registry_entries[i].used) {
            ++count;
        }
    }
    return count;
}

const char *registry_key_at(uint32_t index) {
    uint32_t seen = 0;

    for (uint32_t i = 0; i < REGISTRY_MAX_ENTRIES; ++i) {
        if (!registry_entries[i].used) {
            continue;
        }
        if (seen == index) {
            return registry_entries[i].key;
        }
        ++seen;
    }
    return 0;
}

const char *registry_value_at(uint32_t index) {
    uint32_t seen = 0;

    for (uint32_t i = 0; i < REGISTRY_MAX_ENTRIES; ++i) {
        if (!registry_entries[i].used) {
            continue;
        }
        if (seen == index) {
            return registry_entries[i].value;
        }
        ++seen;
    }
    return 0;
}
