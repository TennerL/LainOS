#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZOBJECT_MAGIC 0x4F5A4E49u
#define ZOBJECT_VERSION 6u
#define ZOBJECT_SYMBOL_NAME_SIZE 128u
#define ZOBJECT_SYMBOL_RECORD_SIZE (12u + ZOBJECT_SYMBOL_NAME_SIZE)
#define ZOBJECT_RELOCATION_RECORD_SIZE (8u + ZOBJECT_SYMBOL_NAME_SIZE)
#define ZOBJECT_SECTION_NAME_SIZE 8u
#define ZOBJECT_SECTION_RECORD_SIZE (20u + ZOBJECT_SECTION_NAME_SIZE)
#define ZOBJECT_HEADER_SIZE 36u
#define ZOBJECT_SYMBOL_EXPORT 1u
#define ZOBJECT_SYMBOL_EXTERN 2u
#define ZOBJECT_RELOC_ABS64 1u
#define ZOBJECT_RELOC_RELATIVE64 2u
#define ZOBJECT_RELOC_RIP32 3u
#define ZOBJECT_SECTION_TEXT 1u
#define ZOBJECT_SECTION_DATA 2u
#define ZOBJECT_SECTION_BSS 3u
#define ZELF_MAX_SECTIONS 256u
#define ZELF_MAX_SYMBOLS 4096u
#define ZELF_MAX_RELOCS 16384u
#define ZELF_MAX_OBJECT_SIZE (16u * 1024u * 1024u)

#define EI_CLASS 4
#define EI_DATA 5
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define ET_REL 1
#define EM_X86_64 62
#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_RELA 4
#define SHT_NOBITS 8
#define SHT_NOTE 7
#define SHF_WRITE 1ull
#define SHF_ALLOC 2ull
#define SHF_EXECINSTR 4ull
#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STB_WEAK 2
#define STT_SECTION 3
#define SHN_UNDEF 0
#define R_X86_64_64 1
#define R_X86_64_PC32 2
#define R_X86_64_PLT32 4

typedef struct {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf64_ehdr_t;

typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} elf64_shdr_t;

typedef struct {
    uint32_t st_name;
    unsigned char st_info;
    unsigned char st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} elf64_sym_t;

typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t r_addend;
} elf64_rela_t;

typedef struct {
    uint32_t type;
    uint32_t file_offset;
    uint32_t load_offset;
    uint32_t file_size;
    uint32_t mem_size;
    char name[ZOBJECT_SECTION_NAME_SIZE];
} zout_section_t;

typedef struct {
    uint32_t type;
    uint64_t value;
    char name[ZOBJECT_SYMBOL_NAME_SIZE];
} zout_symbol_t;

typedef struct {
    uint32_t type;
    uint32_t offset;
    char name[ZOBJECT_SYMBOL_NAME_SIZE];
} zout_reloc_t;

typedef struct {
    uint32_t kind;
    uint32_t load_offset;
    uint32_t file_offset;
    uint32_t file_size;
    uint32_t mem_size;
    uint32_t out_file_offset;
} section_map_t;

static uint16_t rd16(const unsigned char *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const unsigned char *p) {
    uint64_t v = 0;
    for (uint32_t i = 0; i < 8u; ++i) {
        v |= (uint64_t)p[i] << (i * 8u);
    }
    return v;
}

static void wr32(unsigned char *p, uint32_t value) {
    p[0] = (unsigned char)(value & 0xffu);
    p[1] = (unsigned char)((value >> 8) & 0xffu);
    p[2] = (unsigned char)((value >> 16) & 0xffu);
    p[3] = (unsigned char)((value >> 24) & 0xffu);
}

static void wr64(unsigned char *p, uint64_t value) {
    for (uint32_t i = 0; i < 8u; ++i) {
        p[i] = (unsigned char)((value >> (i * 8u)) & 0xffu);
    }
}

static uint32_t align_up(uint32_t value, uint64_t align) {
    uint32_t a = (align > 16u || align == 0u) ? 16u : (uint32_t)align;
    return (value + a - 1u) & ~(a - 1u);
}

static int read_file(const char *path, unsigned char **out, uint32_t *out_size) {
    FILE *f = fopen(path, "rb");
    long size;
    unsigned char *data;
    if (!f) {
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 ||
        size > (long)ZELF_MAX_OBJECT_SIZE || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    data = (unsigned char *)malloc((size_t)size);
    if (!data) {
        fclose(f);
        return -1;
    }
    if (size != 0 && fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return -1;
    }
    fclose(f);
    *out = data;
    *out_size = (uint32_t)size;
    return 0;
}

static int write_file(const char *path, const unsigned char *data, uint32_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        return -1;
    }
    if (size != 0 && fwrite(data, 1, size, f) != size) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

static void copy_name(char *dst, uint32_t capacity, const char *src) {
    uint32_t i = 0;
    if (capacity == 0) {
        return;
    }
    while (src && src[i] && i + 1u < capacity) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

static int parse_ehdr(const unsigned char *data, uint32_t size, elf64_ehdr_t *out) {
    if (size < 64u || data[0] != 0x7fu || data[1] != 'E' || data[2] != 'L' || data[3] != 'F' ||
        data[EI_CLASS] != ELFCLASS64 || data[EI_DATA] != ELFDATA2LSB) {
        return -1;
    }
    memcpy(out->e_ident, data, 16u);
    out->e_type = rd16(data + 16);
    out->e_machine = rd16(data + 18);
    out->e_version = rd32(data + 20);
    out->e_entry = rd64(data + 24);
    out->e_phoff = rd64(data + 32);
    out->e_shoff = rd64(data + 40);
    out->e_flags = rd32(data + 48);
    out->e_ehsize = rd16(data + 52);
    out->e_phentsize = rd16(data + 54);
    out->e_phnum = rd16(data + 56);
    out->e_shentsize = rd16(data + 58);
    out->e_shnum = rd16(data + 60);
    out->e_shstrndx = rd16(data + 62);
    return out->e_type == ET_REL && out->e_machine == EM_X86_64 &&
           out->e_shentsize == 64u && out->e_shnum < ZELF_MAX_SECTIONS ? 0 : -1;
}

static int parse_shdr(const unsigned char *data, uint32_t size, const elf64_ehdr_t *eh, uint32_t index, elf64_shdr_t *out) {
    uint64_t off = eh->e_shoff + (uint64_t)index * eh->e_shentsize;
    if (off > size || 64u > size - off) {
        return -1;
    }
    data += off;
    out->sh_name = rd32(data + 0);
    out->sh_type = rd32(data + 4);
    out->sh_flags = rd64(data + 8);
    out->sh_addr = rd64(data + 16);
    out->sh_offset = rd64(data + 24);
    out->sh_size = rd64(data + 32);
    out->sh_link = rd32(data + 40);
    out->sh_info = rd32(data + 44);
    out->sh_addralign = rd64(data + 48);
    out->sh_entsize = rd64(data + 56);
    return 0;
}

static int parse_sym(const unsigned char *base, uint32_t size, const elf64_shdr_t *symtab, uint32_t index, elf64_sym_t *out) {
    uint64_t off = symtab->sh_offset + (uint64_t)index * symtab->sh_entsize;
    if (symtab->sh_entsize != 24u || off > size || 24u > size - off) {
        return -1;
    }
    base += off;
    out->st_name = rd32(base + 0);
    out->st_info = base[4];
    out->st_other = base[5];
    out->st_shndx = rd16(base + 6);
    out->st_value = rd64(base + 8);
    out->st_size = rd64(base + 16);
    return 0;
}

static int map_offset(const section_map_t *maps, uint32_t section_index, uint64_t offset, uint32_t *out) {
    if (section_index >= ZELF_MAX_SECTIONS || maps[section_index].kind == 0 ||
        offset > maps[section_index].mem_size) {
        return -1;
    }
    *out = maps[section_index].load_offset + (uint32_t)offset;
    return 0;
}

static int symbol_target(const elf64_sym_t *symbols,
                         const section_map_t *maps,
                         const char *strtab,
                         uint32_t strtab_size,
                         uint32_t sym_index,
                         int64_t addend,
                         uint32_t *local_target,
                         const char **external_name) {
    const elf64_sym_t *sym = &symbols[sym_index];
    *external_name = 0;
    if (sym->st_shndx == SHN_UNDEF) {
        if (sym->st_name >= strtab_size || strtab[sym->st_name] == '\0') {
            return -1;
        }
        *external_name = strtab + sym->st_name;
        *local_target = (uint32_t)addend;
        return 0;
    }
    if (sym->st_shndx >= ZELF_MAX_SECTIONS) {
        return -1;
    }
    return map_offset(maps, sym->st_shndx, (uint64_t)((int64_t)sym->st_value + addend), local_target);
}

static void write_section_record(unsigned char *out,
                                 uint32_t type,
                                 const char *name,
                                 uint32_t file_offset,
                                 uint32_t load_offset,
                                 uint32_t file_size,
                                 uint32_t mem_size) {
    wr32(out + 0, type);
    wr32(out + 4, file_offset);
    wr32(out + 8, load_offset);
    wr32(out + 12, file_size);
    wr32(out + 16, mem_size);
    memset(out + 20, 0, ZOBJECT_SECTION_NAME_SIZE);
    for (uint32_t i = 0; name[i] && i + 1u < ZOBJECT_SECTION_NAME_SIZE; ++i) {
        out[20 + i] = (unsigned char)name[i];
    }
}

int main(int argc, char **argv) {
    unsigned char *elf = 0;
    unsigned char *zo = 0;
    uint32_t elf_size = 0;
    elf64_ehdr_t eh;
    elf64_shdr_t shdrs[ZELF_MAX_SECTIONS];
    section_map_t maps[ZELF_MAX_SECTIONS];
    elf64_sym_t symbols[ZELF_MAX_SYMBOLS];
    zout_symbol_t out_symbols[ZELF_MAX_SYMBOLS];
    zout_reloc_t out_relocs[ZELF_MAX_RELOCS];
    uint32_t sym_count = 0;
    uint32_t out_symbol_count = 0;
    uint32_t out_reloc_count = 0;
    uint32_t symtab_index = 0;
    uint32_t strtab_index = 0;
    uint32_t text_size = 1u;
    uint32_t data_size = 0;
    uint32_t bss_size = 0;
    const char *strtab = 0;
    uint32_t strtab_size = 0;
    uint32_t image_size;
    uint32_t symbol_bytes;
    uint32_t reloc_bytes;
    uint32_t section_bytes = 3u * ZOBJECT_SECTION_RECORD_SIZE;
    uint32_t zo_size;

    if (argc != 3) {
        fprintf(stderr, "usage: zelf_to_zobject input.o output.zo\n");
        return 2;
    }
    if (read_file(argv[1], &elf, &elf_size) != 0 || parse_ehdr(elf, elf_size, &eh) != 0) {
        fprintf(stderr, "%s: unsupported ELF64 relocatable\n", argv[1]);
        free(elf);
        return 1;
    }

    memset(shdrs, 0, sizeof(shdrs));
    memset(maps, 0, sizeof(maps));
    for (uint32_t i = 0; i < eh.e_shnum; ++i) {
        if (parse_shdr(elf, elf_size, &eh, i, &shdrs[i]) != 0) {
            fprintf(stderr, "%s: bad section table\n", argv[1]);
            free(elf);
            return 1;
        }
        if (shdrs[i].sh_offset > elf_size ||
            (shdrs[i].sh_type != SHT_NOBITS && shdrs[i].sh_size > elf_size - shdrs[i].sh_offset)) {
            fprintf(stderr, "%s: bad section bounds\n", argv[1]);
            free(elf);
            return 1;
        }
        if (shdrs[i].sh_type == SHT_SYMTAB) {
            symtab_index = i;
            strtab_index = shdrs[i].sh_link;
        }
    }
    if (symtab_index == 0 || strtab_index >= eh.e_shnum || shdrs[strtab_index].sh_type != SHT_STRTAB) {
        fprintf(stderr, "%s: missing symbol table\n", argv[1]);
        free(elf);
        return 1;
    }
    strtab = (const char *)(elf + shdrs[strtab_index].sh_offset);
    strtab_size = (uint32_t)shdrs[strtab_index].sh_size;
    sym_count = (uint32_t)(shdrs[symtab_index].sh_size / shdrs[symtab_index].sh_entsize);
    if (sym_count > ZELF_MAX_SYMBOLS) {
        fprintf(stderr, "%s: too many symbols\n", argv[1]);
        free(elf);
        return 1;
    }
    for (uint32_t i = 0; i < sym_count; ++i) {
        if (parse_sym(elf, elf_size, &shdrs[symtab_index], i, &symbols[i]) != 0) {
            fprintf(stderr, "%s: bad symbol table\n", argv[1]);
            free(elf);
            return 1;
        }
    }

    for (uint32_t i = 0; i < eh.e_shnum; ++i) {
        if ((shdrs[i].sh_flags & SHF_ALLOC) == 0) {
            continue;
        }
        if (shdrs[i].sh_type == SHT_PROGBITS && (shdrs[i].sh_flags & SHF_EXECINSTR) != 0) {
            text_size = align_up(text_size, shdrs[i].sh_addralign);
            maps[i].kind = ZOBJECT_SECTION_TEXT;
            maps[i].load_offset = text_size;
            maps[i].file_offset = (uint32_t)shdrs[i].sh_offset;
            maps[i].file_size = (uint32_t)shdrs[i].sh_size;
            maps[i].mem_size = (uint32_t)shdrs[i].sh_size;
            text_size += maps[i].mem_size;
        }
    }
    for (uint32_t i = 0; i < eh.e_shnum; ++i) {
        if ((shdrs[i].sh_flags & SHF_ALLOC) == 0 ||
            (shdrs[i].sh_type == SHT_PROGBITS && (shdrs[i].sh_flags & SHF_EXECINSTR) != 0)) {
            continue;
        }
        if (shdrs[i].sh_type == SHT_PROGBITS) {
            data_size = align_up(data_size, shdrs[i].sh_addralign);
            maps[i].kind = ZOBJECT_SECTION_DATA;
            maps[i].load_offset = text_size + data_size;
            maps[i].file_offset = (uint32_t)shdrs[i].sh_offset;
            maps[i].file_size = (uint32_t)shdrs[i].sh_size;
            maps[i].mem_size = (uint32_t)shdrs[i].sh_size;
            maps[i].out_file_offset = data_size;
            data_size += maps[i].mem_size;
        } else if (shdrs[i].sh_type == SHT_NOBITS || shdrs[i].sh_type == SHT_NOTE) {
            continue;
        } else {
            fprintf(stderr, "%s: unsupported alloc section type %u\n", argv[1], shdrs[i].sh_type);
            free(elf);
            return 1;
        }
    }
    for (uint32_t i = 0; i < eh.e_shnum; ++i) {
        if ((shdrs[i].sh_flags & SHF_ALLOC) == 0 ||
            shdrs[i].sh_type != SHT_NOBITS) {
            continue;
        }
        bss_size = align_up(bss_size, shdrs[i].sh_addralign);
        maps[i].kind = ZOBJECT_SECTION_BSS;
        maps[i].load_offset = text_size + data_size + bss_size;
        maps[i].mem_size = (uint32_t)shdrs[i].sh_size;
        bss_size += maps[i].mem_size;
    }

    for (uint32_t i = 1; i < sym_count; ++i) {
        uint32_t bind = symbols[i].st_info >> 4;
        uint32_t type = symbols[i].st_info & 0xfu;
        uint32_t mapped = 0;
        const char *name;
        if ((bind != STB_GLOBAL && bind != STB_WEAK) || type == STT_SECTION ||
            symbols[i].st_name >= strtab_size || strtab[symbols[i].st_name] == '\0') {
            continue;
        }
        name = strtab + symbols[i].st_name;
        if (symbols[i].st_shndx == SHN_UNDEF) {
            out_symbols[out_symbol_count].type = ZOBJECT_SYMBOL_EXTERN;
            out_symbols[out_symbol_count].value = 0;
        } else {
            if (map_offset(maps, symbols[i].st_shndx, symbols[i].st_value, &mapped) != 0) {
                continue;
            }
            out_symbols[out_symbol_count].type = ZOBJECT_SYMBOL_EXPORT;
            out_symbols[out_symbol_count].value = mapped;
        }
        copy_name(out_symbols[out_symbol_count].name, sizeof(out_symbols[out_symbol_count].name), name);
        ++out_symbol_count;
    }

    image_size = text_size + data_size;
    zo = (unsigned char *)calloc(1u, ZELF_MAX_OBJECT_SIZE);
    if (!zo) {
        free(elf);
        return 1;
    }
    zo[0] = 0xc3u;
    for (uint32_t i = 0; i < eh.e_shnum; ++i) {
        if (maps[i].kind == ZOBJECT_SECTION_TEXT) {
            memcpy(zo + maps[i].load_offset, elf + maps[i].file_offset, maps[i].file_size);
        } else if (maps[i].kind == ZOBJECT_SECTION_DATA) {
            memcpy(zo + maps[i].load_offset, elf + maps[i].file_offset, maps[i].file_size);
        }
    }

    for (uint32_t i = 0; i < eh.e_shnum; ++i) {
        if (shdrs[i].sh_type != SHT_RELA || shdrs[i].sh_info >= eh.e_shnum ||
            maps[shdrs[i].sh_info].kind == 0) {
            continue;
        }
        if (shdrs[i].sh_entsize != 24u) {
            fprintf(stderr, "%s: unsupported relocation entry size\n", argv[1]);
            free(zo);
            free(elf);
            return 1;
        }
        for (uint32_t j = 0; j < shdrs[i].sh_size / shdrs[i].sh_entsize; ++j) {
            uint64_t off = shdrs[i].sh_offset + (uint64_t)j * shdrs[i].sh_entsize;
            elf64_rela_t rela;
            uint32_t reloc_type;
            uint32_t sym_index;
            uint32_t patch_offset;
            uint32_t local_target = 0;
            const char *external_name = 0;
            rela.r_offset = rd64(elf + off);
            rela.r_info = rd64(elf + off + 8);
            rela.r_addend = (int64_t)rd64(elf + off + 16);
            reloc_type = (uint32_t)(rela.r_info & 0xffffffffu);
            sym_index = (uint32_t)(rela.r_info >> 32);
            if (sym_index >= sym_count ||
                map_offset(maps, shdrs[i].sh_info, rela.r_offset, &patch_offset) != 0 ||
                symbol_target(symbols, maps, strtab, strtab_size, sym_index, rela.r_addend, &local_target, &external_name) != 0) {
                fprintf(stderr, "%s: bad relocation\n", argv[1]);
                free(zo);
                free(elf);
                return 1;
            }
            if (out_reloc_count >= ZELF_MAX_RELOCS) {
                fprintf(stderr, "%s: too many relocations\n", argv[1]);
                free(zo);
                free(elf);
                return 1;
            }
            if (reloc_type == R_X86_64_64) {
                if (external_name) {
                    out_relocs[out_reloc_count].type = ZOBJECT_RELOC_ABS64;
                    wr64(zo + patch_offset, (uint64_t)rela.r_addend);
                    copy_name(out_relocs[out_reloc_count].name, sizeof(out_relocs[out_reloc_count].name), external_name);
                } else {
                    out_relocs[out_reloc_count].type = ZOBJECT_RELOC_RELATIVE64;
                    wr64(zo + patch_offset, local_target);
                    out_relocs[out_reloc_count].name[0] = '\0';
                }
            } else if (reloc_type == R_X86_64_PC32 || reloc_type == R_X86_64_PLT32) {
                out_relocs[out_reloc_count].type = ZOBJECT_RELOC_RIP32;
                if (external_name) {
                    wr32(zo + patch_offset, (uint32_t)rela.r_addend);
                    copy_name(out_relocs[out_reloc_count].name, sizeof(out_relocs[out_reloc_count].name), external_name);
                } else {
                    int64_t disp = (int64_t)local_target - ((int64_t)patch_offset + 4ll);
                    if (disp < -2147483648ll || disp > 2147483647ll) {
                        fprintf(stderr, "%s: local PC32 relocation out of range\n", argv[1]);
                        free(zo);
                        free(elf);
                        return 1;
                    }
                    wr32(zo + patch_offset, (uint32_t)disp);
                    out_relocs[out_reloc_count].name[0] = '\0';
                }
            } else {
                fprintf(stderr, "%s: unsupported relocation type %u\n", argv[1], reloc_type);
                free(zo);
                free(elf);
                return 1;
            }
            out_relocs[out_reloc_count].offset = patch_offset;
            ++out_reloc_count;
        }
    }

    symbol_bytes = out_symbol_count * ZOBJECT_SYMBOL_RECORD_SIZE;
    reloc_bytes = out_reloc_count * ZOBJECT_RELOCATION_RECORD_SIZE;
    zo_size = ZOBJECT_HEADER_SIZE + symbol_bytes + reloc_bytes + section_bytes + image_size;
    if (zo_size > ZELF_MAX_OBJECT_SIZE) {
        fprintf(stderr, "%s: output too large\n", argv[1]);
        free(zo);
        free(elf);
        return 1;
    }
    memmove(zo + ZOBJECT_HEADER_SIZE + symbol_bytes + reloc_bytes + section_bytes, zo, image_size);
    memset(zo, 0, ZOBJECT_HEADER_SIZE + symbol_bytes + reloc_bytes + section_bytes);
    wr32(zo + 0, ZOBJECT_MAGIC);
    wr32(zo + 4, ZOBJECT_VERSION);
    wr32(zo + 8, image_size);
    wr32(zo + 12, out_symbol_count);
    wr32(zo + 16, out_reloc_count);
    wr32(zo + 20, 0);
    wr32(zo + 24, 3);
    for (uint32_t i = 0; i < out_symbol_count; ++i) {
        unsigned char *r = zo + ZOBJECT_HEADER_SIZE + i * ZOBJECT_SYMBOL_RECORD_SIZE;
        wr32(r, out_symbols[i].type);
        wr64(r + 4, out_symbols[i].value);
        copy_name((char *)(r + 12), ZOBJECT_SYMBOL_NAME_SIZE, out_symbols[i].name);
    }
    for (uint32_t i = 0; i < out_reloc_count; ++i) {
        unsigned char *r = zo + ZOBJECT_HEADER_SIZE + symbol_bytes + i * ZOBJECT_RELOCATION_RECORD_SIZE;
        wr32(r, out_relocs[i].type);
        wr32(r + 4, out_relocs[i].offset);
        copy_name((char *)(r + 8), ZOBJECT_SYMBOL_NAME_SIZE, out_relocs[i].name);
    }
    write_section_record(zo + ZOBJECT_HEADER_SIZE + symbol_bytes + reloc_bytes,
                         ZOBJECT_SECTION_TEXT, ".text", 0, 0, text_size, text_size);
    write_section_record(zo + ZOBJECT_HEADER_SIZE + symbol_bytes + reloc_bytes + ZOBJECT_SECTION_RECORD_SIZE,
                         ZOBJECT_SECTION_DATA, ".data", text_size, text_size, data_size, data_size);
    write_section_record(zo + ZOBJECT_HEADER_SIZE + symbol_bytes + reloc_bytes + 2u * ZOBJECT_SECTION_RECORD_SIZE,
                         ZOBJECT_SECTION_BSS, ".bss", image_size, image_size, 0, bss_size);

    if (write_file(argv[2], zo, zo_size) != 0) {
        fprintf(stderr, "%s: failed to write output\n", argv[2]);
        free(zo);
        free(elf);
        return 1;
    }
    fprintf(stderr, "converted %s -> %s symbols=%u relocations=%u image=%u\n",
            argv[1], argv[2], out_symbol_count, out_reloc_count, image_size);
    free(zo);
    free(elf);
    return 0;
}
