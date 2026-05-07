#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "zscript.h"

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

int main(int argc, char **argv) {
    FILE *in;
    FILE *out;
    long input_size;
    char *source;
    char *asm_output;
    uint32_t asm_capacity = 1024u * 1024u;
    uint32_t asm_size = 0;
    uint32_t error_line = 0;
    char entry_label[64];
    const char *label_prefix = "kz";

    if (argc != 3) {
        fprintf(stderr, "usage: zcc_host input.Z output.asm\n");
        return 2;
    }

    in = fopen(argv[1], "rb");
    if (!in) {
        perror(argv[1]);
        return 1;
    }

    if (fseek(in, 0, SEEK_END) != 0) {
        fclose(in);
        return 1;
    }
    input_size = ftell(in);
    if (input_size < 0 ||
        fseek(in, 0, SEEK_SET) != 0) {
        fclose(in);
        return 1;
    }

    source = (char *)malloc((size_t)input_size + 1u);
    asm_output = (char *)malloc(asm_capacity);
    if (!source || !asm_output) {
        fclose(in);
        free(source);
        free(asm_output);
        return 1;
    }

    if (fread(source, 1, (size_t)input_size, in) != (size_t)input_size) {
        fclose(in);
        free(source);
        free(asm_output);
        return 1;
    }
    fclose(in);
    source[input_size] = '\0';

    if (zscript_compile_source_object(source,
                                      (uint32_t)input_size,
                                      label_prefix,
                                      asm_output,
                                      asm_capacity,
                                      &asm_size,
                                      &error_line,
                                      entry_label,
                                      sizeof(entry_label)) != 0) {
        fprintf(stderr, "%s:%u: unsupported .Z syntax\n", argv[1], error_line);
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
