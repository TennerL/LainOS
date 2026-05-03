#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void emit_c_string(FILE *out, const char *text) {
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p == '\\' || *p == '"') {
            fputc('\\', out);
            fputc(*p, out);
        } else if (*p >= 32u && *p <= 126u) {
            fputc(*p, out);
        } else {
            fprintf(out, "\\x%02x", *p);
        }
    }
    fputc('"', out);
}

static int emit_file_array(FILE *out, const char *path, unsigned int index) {
    FILE *in = fopen(path, "rb");
    int ch;
    unsigned int column = 0;

    if (!in) {
        fprintf(stderr, "ramdisk_seed_gen: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }

    fprintf(out, "static const unsigned char ramdisk_seed_file_%u[] = {", index);
    while ((ch = fgetc(in)) != EOF) {
        if (column == 0) {
            fprintf(out, "\n    ");
        }
        fprintf(out, "0x%02x,", (unsigned int)(unsigned char)ch);
        ++column;
        if (column == 12u) {
            column = 0;
        } else {
            fputc(' ', out);
        }
    }
    if (ferror(in)) {
        fclose(in);
        fprintf(stderr, "ramdisk_seed_gen: cannot read %s\n", path);
        return -1;
    }
    fclose(in);

    if (column != 0) {
        fputc('\n', out);
    }
    fprintf(out, "};\n\n");
    return 0;
}

int main(int argc, char **argv) {
    FILE *out;

    if (argc < 3) {
        fprintf(stderr, "usage: %s output.h file...\n", argv[0]);
        return 2;
    }

    out = fopen(argv[1], "wb");
    if (!out) {
        fprintf(stderr, "ramdisk_seed_gen: cannot create %s: %s\n", argv[1], strerror(errno));
        return 1;
    }

    fprintf(out, "#ifndef RAMDISK_SEED_H\n");
    fprintf(out, "#define RAMDISK_SEED_H\n\n");
    fprintf(out, "#include <stdint.h>\n\n");
    fprintf(out, "typedef struct {\n");
    fprintf(out, "    const char *path;\n");
    fprintf(out, "    const unsigned char *data;\n");
    fprintf(out, "    uint32_t size;\n");
    fprintf(out, "} ramdisk_seed_entry_t;\n\n");

    for (int i = 2; i < argc; ++i) {
        if (emit_file_array(out, argv[i], (unsigned int)(i - 2)) != 0) {
            fclose(out);
            return 1;
        }
    }

    fprintf(out, "static const ramdisk_seed_entry_t ramdisk_seed_entries[] = {\n");
    for (int i = 2; i < argc; ++i) {
        fprintf(out, "    { ");
        emit_c_string(out, argv[i]);
        fprintf(out, ", ramdisk_seed_file_%u, (uint32_t)sizeof(ramdisk_seed_file_%u) },\n",
                (unsigned int)(i - 2),
                (unsigned int)(i - 2));
    }
    fprintf(out, "};\n\n");
    fprintf(out, "#define RAMDISK_SEED_ENTRY_COUNT %u\n\n", (unsigned int)(argc - 2));
    fprintf(out, "#endif\n");

    if (fclose(out) != 0) {
        fprintf(stderr, "ramdisk_seed_gen: cannot finish %s: %s\n", argv[1], strerror(errno));
        return 1;
    }

    return 0;
}
