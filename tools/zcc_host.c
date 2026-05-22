#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zscript.h"

#define ZCC_HOST_MAX_INCLUDE_DEPTH 16u
#define ZCC_HOST_MAX_SOURCE_SIZE (4u * 1024u * 1024u)

static int write_nasm_globals(FILE *out, const char *asm_output, uint32_t asm_size) {
    const char *cursor = asm_output;
    const char *end = asm_output + asm_size;
    const char prefix[] = "; zo_export ";
    const unsigned int prefix_len = (unsigned int)(sizeof(prefix) - 1u);

    while (cursor < end) {
        const char *line = cursor;
        const char *line_end = line;
        unsigned int i = 0;

        while (line_end < end && *line_end != '\n') {
            ++line_end;
        }

        while (i < prefix_len && line + i < line_end && line[i] == prefix[i]) {
            ++i;
        }

        if (i == prefix_len) {
            const char *name = line + prefix_len;
            const char *name_end = name;

            while (name_end < line_end && *name_end != ' ' && *name_end != '\t') {
                ++name_end;
            }

            if (name_end > name) {
                if (fputs("global ", out) < 0 ||
                    fwrite(name, 1, (size_t)(name_end - name), out) != (size_t)(name_end - name) ||
                    fputc('\n', out) == EOF) {
                    return -1;
                }
            }
        }

        cursor = line_end;
        if (cursor < end && *cursor == '\n') {
            ++cursor;
        }
    }

    return 0;
}

static int append_bytes(char *out, uint32_t capacity, uint32_t *size, const char *data, uint32_t data_size) {
    if (*size > capacity || data_size > capacity - *size) {
        return -1;
    }

    if (data_size != 0) {
        memcpy(out + *size, data, data_size);
        *size += data_size;
    }
    return 0;
}

static int append_char(char *out, uint32_t capacity, uint32_t *size, char ch) {
    return append_bytes(out, capacity, size, &ch, 1);
}

static int dirname_from_path(const char *path, char *out, uint32_t out_capacity) {
    const char *slash = strrchr(path, '/');
    uint32_t len;

    if (!slash) {
        if (out_capacity == 0) {
            return -1;
        }
        out[0] = '\0';
        return 0;
    }

    len = (uint32_t)(slash - path + 1);
    if (len + 1u > out_capacity) {
        return -1;
    }
    memcpy(out, path, len);
    out[len] = '\0';
    return 0;
}

static int join_path(const char *dir, const char *name, char *out, uint32_t out_capacity) {
    uint32_t dir_len = (uint32_t)strlen(dir);
    uint32_t name_len = (uint32_t)strlen(name);

    if (dir_len + name_len + 1u > out_capacity) {
        return -1;
    }
    memcpy(out, dir, dir_len);
    memcpy(out + dir_len, name, name_len);
    out[dir_len + name_len] = '\0';
    return 0;
}

static const char *skip_line_spaces(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r')) {
        ++p;
    }
    return p;
}

static int parse_include_line(const char *line, const char *line_end, char *include_name, uint32_t include_capacity) {
    const char include_word[] = "include";
    const char *p = skip_line_spaces(line, line_end);
    uint32_t i;

    if (p >= line_end || *p != '#') {
        return 0;
    }
    ++p;
    p = skip_line_spaces(p, line_end);

    for (i = 0; include_word[i] != '\0'; ++i) {
        if (p + i >= line_end || p[i] != include_word[i]) {
            return 0;
        }
    }
    p += i;
    p = skip_line_spaces(p, line_end);

    if (p >= line_end || *p != '"') {
        return -1;
    }
    ++p;

    i = 0;
    while (p < line_end && *p != '"') {
        if (*p == '\n' || *p == '\r' || i + 1u >= include_capacity) {
            return -1;
        }
        include_name[i++] = *p++;
    }
    if (p >= line_end || *p != '"') {
        return -1;
    }
    include_name[i] = '\0';
    return 1;
}

static int read_file_raw(const char *path, char **out_data, uint32_t *out_size) {
    FILE *in;
    long input_size;
    char *data;

    in = fopen(path, "rb");
    if (!in) {
        return -1;
    }

    if (fseek(in, 0, SEEK_END) != 0) {
        fclose(in);
        return -1;
    }
    input_size = ftell(in);
    if (input_size < 0 ||
        input_size > (long)ZCC_HOST_MAX_SOURCE_SIZE ||
        fseek(in, 0, SEEK_SET) != 0) {
        fclose(in);
        return -1;
    }

    data = (char *)malloc((size_t)input_size + 1u);
    if (!data) {
        fclose(in);
        return -1;
    }

    if (fread(data, 1, (size_t)input_size, in) != (size_t)input_size) {
        fclose(in);
        free(data);
        return -1;
    }
    fclose(in);
    data[input_size] = '\0';

    *out_data = data;
    *out_size = (uint32_t)input_size;
    return 0;
}

static int expand_source_file(const char *path,
                              char *out,
                              uint32_t out_capacity,
                              uint32_t *out_size,
                              uint32_t depth) {
    char *source = 0;
    uint32_t source_size = 0;
    char dir[512];
    uint32_t pos = 0;

    if (depth >= ZCC_HOST_MAX_INCLUDE_DEPTH ||
        dirname_from_path(path, dir, sizeof(dir)) != 0 ||
        read_file_raw(path, &source, &source_size) != 0) {
        return -1;
    }

    while (pos < source_size) {
        const char *line = source + pos;
        const char *line_end = line;
        char include_name[256];
        int include_status;

        while ((uint32_t)(line_end - source) < source_size && *line_end != '\n') {
            ++line_end;
        }

        include_status = parse_include_line(line, line_end, include_name, sizeof(include_name));
        if (include_status < 0) {
            free(source);
            return -1;
        }

        if (include_status > 0) {
            char include_path[768];
            if (join_path(dir, include_name, include_path, sizeof(include_path)) != 0 ||
                expand_source_file(include_path, out, out_capacity, out_size, depth + 1u) != 0 ||
                append_char(out, out_capacity, out_size, '\n') != 0) {
                free(source);
                return -1;
            }
        } else if (append_bytes(out,
                                out_capacity,
                                out_size,
                                line,
                                (uint32_t)(line_end - line)) != 0 ||
                   append_char(out, out_capacity, out_size, '\n') != 0) {
            free(source);
            return -1;
        }

        pos = (uint32_t)(line_end - source);
        if (pos < source_size && source[pos] == '\n') {
            ++pos;
        }
    }

    free(source);
    return 0;
}

int main(int argc, char **argv) {
    FILE *out;
    char *source;
    char *asm_output;
    uint32_t source_capacity = ZCC_HOST_MAX_SOURCE_SIZE;
    uint32_t source_size = 0;
    uint32_t asm_capacity = 8u * 1024u * 1024u;
    uint32_t asm_size = 0;
    uint32_t error_line = 0;
    char entry_label[64];
    const char *label_prefix = "kz";

    if (argc != 3) {
        fprintf(stderr, "usage: zcc_host input.Z output.asm\n");
        return 2;
    }

    source = (char *)malloc(source_capacity + 1u);
    asm_output = (char *)malloc(asm_capacity);
    if (!source || !asm_output) {
        free(source);
        free(asm_output);
        return 1;
    }

    if (expand_source_file(argv[1], source, source_capacity, &source_size, 0) != 0) {
        fprintf(stderr, "%s: failed to load expanded .Z source\n", argv[1]);
        free(source);
        free(asm_output);
        return 1;
    }
    source[source_size] = '\0';

    if (zscript_compile_source_object(source,
                                      source_size,
                                      label_prefix,
                                      asm_output,
                                      asm_capacity,
                                      &asm_size,
                                      &error_line,
                                      entry_label,
                                      sizeof(entry_label)) != 0) {
        if (zscript_last_error() == ZSCRIPT_ERROR_OUTPUT_FULL) {
            fprintf(stderr, "%s:%u: generated asm exceeded host build buffer\n", argv[1], error_line);
        } else {
            fprintf(stderr, "%s:%u: unsupported .Z syntax\n", argv[1], error_line);
        }
        free(source);
        free(asm_output);
        return 1;
    }

    out = fopen(argv[2], "wb");
    if (!out) {
        perror(argv[2]);
        free(source);
        free(asm_output);
        return 1;
    }

    if (write_nasm_globals(out, asm_output, asm_size) != 0 ||
        fwrite(asm_output, 1, asm_size, out) != asm_size) {
        fclose(out);
        free(source);
        free(asm_output);
        return 1;
    }

    fclose(out);
    free(source);
    free(asm_output);
    return 0;
}
