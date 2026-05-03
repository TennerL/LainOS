#include <efi.h>
#include <efilib.h>
#include <elf.h>
#include "bootinfo.h"

#define KERNEL_PATH L"\\kernel.elf"
#define NTFS_DRIVER_PATH L"\\EFI\\BOOT\\drivers\\ntfs_x64.efi"
#define BOOT_RES_CONFIG_NAME "bootres.cfg"
#define LAINFS_BLOCK_SIZE 512u
#define LAINFS_DIR_BLOCKS 64u
#define LAINFS_MAX_FILES ((LAINFS_DIR_BLOCKS * LAINFS_BLOCK_SIZE) / LAINFS_ENTRY_SIZE)
#define LAINFS_ENTRY_SIZE 64u
#define LAINFS_MAGIC0 0x4E49414Cu
#define LAINFS_MAGIC1 0x315346u
#define LAINFS_ENTRY_FILE 1u
#define MBR_PARTITION_TABLE_OFFSET 446u
#define MBR_PARTITION_ENTRY_SIZE 16u
#define MBR_SIGNATURE_OFFSET 510u

#ifndef BOOT_RES_WIDTH
#define BOOT_RES_WIDTH 0
#endif

#ifndef BOOT_RES_HEIGHT
#define BOOT_RES_HEIGHT 0
#endif

typedef void (*kernel_entry_t)(boot_info_t *boot_info);

__attribute__((noreturn)) static void jump_to_kernel(void *entry_point, boot_info_t *boot_info) {
    __asm__ __volatile__(
        "mov %0, %%rdi\n\t"
        "jmp *%1\n\t"
        :
        : "r"(boot_info), "r"(entry_point)
        : "rdi", "memory"
    );

    __builtin_unreachable();
}

typedef struct {
    EFI_FILE_PROTOCOL *handle;
    UINTN size;
} kernel_file_t;

typedef struct __attribute__((packed)) {
    UINT32 magic0;
    UINT32 magic1;
    UINT32 version;
    UINT32 block_size;
    UINT32 dir_start_lba;
    UINT32 dir_blocks;
    UINT32 data_start_lba;
    UINT32 max_files;
    UINT8 reserved[LAINFS_BLOCK_SIZE - 32u];
} boot_lainfs_superblock_t;

typedef struct __attribute__((packed)) {
    UINT8 used;
    char name[31];
    UINT32 start_lba;
    UINT32 byte_size;
    UINT8 reserved[24];
} boot_lainfs_dirent_t;

static EFI_STATUS get_file_size(EFI_SYSTEM_TABLE *SystemTable, EFI_FILE_PROTOCOL *file, UINTN *size) {
    EFI_STATUS status;
    EFI_FILE_INFO *file_info = NULL;
    UINTN file_info_size = 0;

    status = uefi_call_wrapper(file->GetInfo, 4, file, &gEfiFileInfoGuid, &file_info_size, NULL);
    if (status != EFI_BUFFER_TOO_SMALL) return status;

    status = uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3,
        EfiLoaderData, file_info_size, (void**)&file_info);
    if (EFI_ERROR(status)) return status;

    status = uefi_call_wrapper(file->GetInfo, 4, file, &gEfiFileInfoGuid, &file_info_size, file_info);
    if (EFI_ERROR(status)) {
        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, file_info);
        return status;
    }

    *size = file_info->FileSize;
    uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, file_info);
    return EFI_SUCCESS;
}

static EFI_STATUS open_file_on_fs(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs, CHAR16 *path, EFI_FILE_PROTOCOL **out) {
    EFI_STATUS status;
    EFI_FILE_PROTOCOL *root = NULL;

    status = uefi_call_wrapper(fs->OpenVolume, 2, fs, &root);
    if (EFI_ERROR(status)) return status;

    status = uefi_call_wrapper(root->Open, 5, root, out, path, EFI_FILE_MODE_READ, 0);
    uefi_call_wrapper(root->Close, 1, root);
    return status;
}

static void connect_all_controllers(EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS status;
    EFI_BOOT_SERVICES *bs = SystemTable->BootServices;
    EFI_HANDLE *handles = NULL;
    UINTN handle_count = 0;

    status = uefi_call_wrapper(bs->LocateHandleBuffer, 5,
        AllHandles, NULL, NULL, &handle_count, &handles);
    if (EFI_ERROR(status)) return;

    for (UINTN i = 0; i < handle_count; ++i) {
        uefi_call_wrapper(bs->ConnectController, 4, handles[i], NULL, NULL, TRUE);
    }

    uefi_call_wrapper(bs->FreePool, 1, handles);
}

static EFI_STATUS load_optional_driver_from_boot_volume(EFI_HANDLE image, EFI_SYSTEM_TABLE *SystemTable, CHAR16 *path) {
    EFI_STATUS status;
    EFI_BOOT_SERVICES *bs = SystemTable->BootServices;
    EFI_LOADED_IMAGE *loaded_image = NULL;
    EFI_DEVICE_PATH *driver_path = NULL;
    EFI_HANDLE driver_image = NULL;
    EFI_GUID loaded_image_protocol = EFI_LOADED_IMAGE_PROTOCOL_GUID;

    status = uefi_call_wrapper(bs->HandleProtocol, 3,
        image, &loaded_image_protocol, (void**)&loaded_image);
    if (EFI_ERROR(status)) return status;

    driver_path = FileDevicePath(loaded_image->DeviceHandle, path);
    if (driver_path == NULL) return EFI_NOT_FOUND;

    status = uefi_call_wrapper(bs->LoadImage, 6, FALSE, image, driver_path, NULL, 0, &driver_image);
    uefi_call_wrapper(bs->FreePool, 1, driver_path);
    if (EFI_ERROR(status)) return status;

    status = uefi_call_wrapper(bs->StartImage, 3, driver_image, NULL, NULL);
    if (EFI_ERROR(status)) {
        uefi_call_wrapper(bs->UnloadImage, 1, driver_image);
    } else {
        connect_all_controllers(SystemTable);
    }

    return status;
}

static EFI_STATUS try_open_kernel_on_handle(EFI_BOOT_SERVICES *bs, EFI_HANDLE handle, EFI_GUID *fs_protocol, EFI_SYSTEM_TABLE *SystemTable, kernel_file_t *out) {
    EFI_STATUS status;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    EFI_FILE_PROTOCOL *kernel = NULL;
    UINTN kernel_size = 0;

    status = uefi_call_wrapper(bs->HandleProtocol, 3, handle, fs_protocol, (void**)&fs);
    if (EFI_ERROR(status)) return status;

    status = open_file_on_fs(fs, KERNEL_PATH, &kernel);
    if (EFI_ERROR(status)) return status;

    status = get_file_size(SystemTable, kernel, &kernel_size);
    if (EFI_ERROR(status)) {
        uefi_call_wrapper(kernel->Close, 1, kernel);
        return status;
    }

    out->handle = kernel;
    out->size = kernel_size;
    return EFI_SUCCESS;
}

static EFI_STATUS open_kernel(EFI_HANDLE image, EFI_SYSTEM_TABLE *SystemTable, kernel_file_t *out) {
    EFI_STATUS status;
    EFI_BOOT_SERVICES *bs = SystemTable->BootServices;
    EFI_GUID fs_protocol = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_GUID loaded_image_protocol = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE *loaded_image = NULL;
    EFI_HANDLE *fs_handles = NULL;
    UINTN fs_handle_count = 0;
    EFI_STATUS last_error = EFI_NOT_FOUND;

    status = uefi_call_wrapper(bs->HandleProtocol, 3, image, &loaded_image_protocol, (void**)&loaded_image);
    if (!EFI_ERROR(status) && loaded_image != NULL) {
        status = try_open_kernel_on_handle(bs, loaded_image->DeviceHandle, &fs_protocol, SystemTable, out);
        if (!EFI_ERROR(status)) {
            return EFI_SUCCESS;
        }
        last_error = status;
    }

    status = uefi_call_wrapper(bs->LocateHandleBuffer, 5,
        ByProtocol, &fs_protocol, NULL, &fs_handle_count, &fs_handles);
    if (EFI_ERROR(status)) return status;

    for (UINTN i = 0; i < fs_handle_count; ++i) {
        status = try_open_kernel_on_handle(bs, fs_handles[i], &fs_protocol, SystemTable, out);
        if (!EFI_ERROR(status)) {
            uefi_call_wrapper(bs->FreePool, 1, fs_handles);
            return EFI_SUCCESS;
        }
        last_error = status;
    }

    uefi_call_wrapper(bs->FreePool, 1, fs_handles);
    return last_error;
}

static EFI_STATUS read_kernel_file(EFI_SYSTEM_TABLE *SystemTable, kernel_file_t *kernel, void **buffer) {
    EFI_STATUS status;
    void *file_buffer = NULL;
    UINTN file_size = kernel->size;

    status = uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3,
        EfiLoaderData, file_size, &file_buffer);
    if (EFI_ERROR(status)) return status;

    status = uefi_call_wrapper(kernel->handle->SetPosition, 2, kernel->handle, 0);
    if (EFI_ERROR(status)) return status;

    status = uefi_call_wrapper(kernel->handle->Read, 3, kernel->handle, &file_size, file_buffer);
    if (EFI_ERROR(status)) return status;

    *buffer = file_buffer;
    return EFI_SUCCESS;
}

static EFI_STATUS load_elf_kernel(EFI_SYSTEM_TABLE *SystemTable, void *file_buffer, UINTN file_size, void **entry_point, UINT64 *kernel_base, UINT64 *kernel_end) {
    if (file_size < sizeof(Elf64_Ehdr)) return EFI_LOAD_ERROR;

    Elf64_Ehdr *ehdr = (Elf64_Ehdr*)file_buffer;
    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr->e_ident[EI_MAG3] != ELFMAG3) {
        return EFI_LOAD_ERROR;
    }

    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64 || ehdr->e_machine != EM_X86_64) {
        return EFI_UNSUPPORTED;
    }

    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) {
        return EFI_LOAD_ERROR;
    }

    Elf64_Phdr *phdrs = (Elf64_Phdr*)((UINT8*)file_buffer + ehdr->e_phoff);
    UINT64 min_addr = ~0ULL;
    UINT64 max_addr = 0;

    for (UINT16 i = 0; i < ehdr->e_phnum; ++i) {
        Elf64_Phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_memsz == 0) continue;

        if (ph->p_paddr < min_addr) min_addr = ph->p_paddr;
        if (ph->p_paddr + ph->p_memsz > max_addr) max_addr = ph->p_paddr + ph->p_memsz;
    }

    if (min_addr == ~0ULL || max_addr <= min_addr) {
        return EFI_LOAD_ERROR;
    }

    UINT64 alloc_base = min_addr & ~0xFFFULL;
    UINT64 alloc_end = (max_addr + 0xFFFULL) & ~0xFFFULL;
    EFI_PHYSICAL_ADDRESS load_addr = alloc_base;
    UINTN pages = EFI_SIZE_TO_PAGES(alloc_end - alloc_base);
    EFI_STATUS status = uefi_call_wrapper(SystemTable->BootServices->AllocatePages, 4,
        AllocateAddress, EfiLoaderData, pages, &load_addr);
    if (EFI_ERROR(status)) return status;

    for (UINT64 p = alloc_base; p < alloc_end; ++p) {
        ((volatile UINT8*)p)[0] = 0;
    }

    for (UINT16 i = 0; i < ehdr->e_phnum; ++i) {
        Elf64_Phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_offset + ph->p_filesz > file_size) return EFI_LOAD_ERROR;

        CopyMem((void*)(UINTN)ph->p_paddr, (UINT8*)file_buffer + ph->p_offset, ph->p_filesz);
        if (ph->p_memsz > ph->p_filesz) {
            SetMem((UINT8*)(UINTN)ph->p_paddr + ph->p_filesz, ph->p_memsz - ph->p_filesz, 0);
        }
    }

    *entry_point = (void*)(UINTN)ehdr->e_entry;
    *kernel_base = min_addr;
    *kernel_end = max_addr;
    return EFI_SUCCESS;
}

static void *find_rsdp(EFI_SYSTEM_TABLE *SystemTable) {
    EFI_GUID acpi20 = ACPI_20_TABLE_GUID;
    EFI_GUID acpi10 = ACPI_TABLE_GUID;

    for (UINTN i = 0; i < SystemTable->NumberOfTableEntries; ++i) {
        EFI_CONFIGURATION_TABLE *table = &SystemTable->ConfigurationTable[i];
        if (!CompareGuid(&table->VendorGuid, &acpi20) || !CompareGuid(&table->VendorGuid, &acpi10)) {
            return table->VendorTable;
        }
    }

    return NULL;
}

static int ascii_is_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

static int parse_u32_ascii(const char **text, UINT32 *out) {
    UINT32 value = 0;
    const char *s = *text;

    while (ascii_is_space(*s)) {
        ++s;
    }

    if (*s < '0' || *s > '9') {
        return 0;
    }

    while (*s >= '0' && *s <= '9') {
        value = value * 10u + (UINT32)(*s - '0');
        ++s;
    }

    *text = s;
    *out = value;
    return 1;
}

static int parse_resolution_text(const char *text, UINT32 *out_width, UINT32 *out_height) {
    UINT32 width = 0;
    UINT32 height = 0;

    if (!parse_u32_ascii(&text, &width) || !parse_u32_ascii(&text, &height) ||
        width == 0 || height == 0) {
        return 0;
    }

    *out_width = width;
    *out_height = height;
    return 1;
}

static UINT32 read_le32(const UINT8 *p) {
    return ((UINT32)p[0]) |
           ((UINT32)p[1] << 8) |
           ((UINT32)p[2] << 16) |
           ((UINT32)p[3] << 24);
}

static int boot_lainfs_name_equals(const char *entry_name, const char *name) {
    UINTN i = 0;

    while (i < 31u && entry_name[i] && name[i] && entry_name[i] == name[i]) {
        ++i;
    }

    return (i == 31u || entry_name[i] == '\0') && name[i] == '\0';
}

static EFI_STATUS read_lainfs_boot_resolution_from_block(EFI_BLOCK_IO_PROTOCOL *block,
                                                         EFI_SYSTEM_TABLE *SystemTable,
                                                         EFI_LBA base_lba,
                                                         UINT32 *out_width,
                                                         UINT32 *out_height) {
    EFI_STATUS status;
    UINT8 *sector = NULL;
    UINT8 *directory = NULL;
    UINT8 *file = NULL;
    boot_lainfs_superblock_t *super;

    if (block == NULL || block->Media == NULL || block->Media->BlockSize != LAINFS_BLOCK_SIZE) {
        return EFI_NOT_FOUND;
    }

    status = uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3,
        EfiLoaderData, LAINFS_BLOCK_SIZE, (void **)&sector);
    if (EFI_ERROR(status)) {
        return status;
    }

    status = uefi_call_wrapper(block->ReadBlocks, 5,
        block, block->Media->MediaId, base_lba, LAINFS_BLOCK_SIZE, sector);
    if (EFI_ERROR(status)) {
        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, sector);
        return status;
    }

    super = (boot_lainfs_superblock_t *)(void *)sector;
    if (super->magic0 != LAINFS_MAGIC0 ||
        super->magic1 != LAINFS_MAGIC1 ||
        super->version != 1 ||
        super->block_size != LAINFS_BLOCK_SIZE ||
        super->dir_blocks != LAINFS_DIR_BLOCKS ||
        super->max_files != LAINFS_MAX_FILES) {
        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, sector);
        return EFI_NOT_FOUND;
    }

    status = uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3,
        EfiLoaderData, LAINFS_DIR_BLOCKS * LAINFS_BLOCK_SIZE, (void **)&directory);
    if (EFI_ERROR(status)) {
        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, sector);
        return status;
    }

    status = uefi_call_wrapper(block->ReadBlocks, 5,
        block,
        block->Media->MediaId,
        base_lba + super->dir_start_lba,
        LAINFS_DIR_BLOCKS * LAINFS_BLOCK_SIZE,
        directory);
    if (EFI_ERROR(status)) {
        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, directory);
        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, sector);
        return status;
    }

    for (UINT32 i = 0; i < LAINFS_MAX_FILES; ++i) {
        boot_lainfs_dirent_t *entry = (boot_lainfs_dirent_t *)(void *)(directory + i * LAINFS_ENTRY_SIZE);
        UINT32 parent_id = ((UINT32)entry->reserved[0]) |
                           ((UINT32)entry->reserved[1] << 8) |
                           ((UINT32)entry->reserved[2] << 16) |
                           ((UINT32)entry->reserved[3] << 24);

        if (entry->used != LAINFS_ENTRY_FILE ||
            parent_id != 0 ||
            entry->byte_size == 0 ||
            entry->byte_size > LAINFS_BLOCK_SIZE ||
            !boot_lainfs_name_equals(entry->name, BOOT_RES_CONFIG_NAME)) {
            continue;
        }

        status = uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3,
            EfiLoaderData, LAINFS_BLOCK_SIZE, (void **)&file);
        if (EFI_ERROR(status)) {
            break;
        }

        status = uefi_call_wrapper(block->ReadBlocks, 5,
            block,
            block->Media->MediaId,
            base_lba + entry->start_lba,
            LAINFS_BLOCK_SIZE,
            file);
        if (!EFI_ERROR(status)) {
            file[entry->byte_size < LAINFS_BLOCK_SIZE ? entry->byte_size : LAINFS_BLOCK_SIZE - 1u] = '\0';
            if (parse_resolution_text((const char *)file, out_width, out_height)) {
                uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, file);
                uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, directory);
                uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, sector);
                return EFI_SUCCESS;
            }
        }

        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, file);
        break;
    }

    uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, directory);
    uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, sector);
    return EFI_NOT_FOUND;
}

static EFI_STATUS read_lainfs_boot_resolution_from_mbr(EFI_BLOCK_IO_PROTOCOL *block,
                                                       EFI_SYSTEM_TABLE *SystemTable,
                                                       UINT32 *out_width,
                                                       UINT32 *out_height) {
    EFI_STATUS status;
    UINT8 *mbr = NULL;

    if (block == NULL || block->Media == NULL || block->Media->BlockSize != LAINFS_BLOCK_SIZE) {
        return EFI_NOT_FOUND;
    }

    status = uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3,
        EfiLoaderData, LAINFS_BLOCK_SIZE, (void **)&mbr);
    if (EFI_ERROR(status)) {
        return status;
    }

    status = uefi_call_wrapper(block->ReadBlocks, 5,
        block, block->Media->MediaId, 0, LAINFS_BLOCK_SIZE, mbr);
    if (EFI_ERROR(status)) {
        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, mbr);
        return status;
    }

    if (mbr[MBR_SIGNATURE_OFFSET] != 0x55 || mbr[MBR_SIGNATURE_OFFSET + 1u] != 0xAA) {
        uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, mbr);
        return EFI_NOT_FOUND;
    }

    for (UINT32 i = 0; i < 4u; ++i) {
        UINT8 *entry = mbr + MBR_PARTITION_TABLE_OFFSET + i * MBR_PARTITION_ENTRY_SIZE;
        UINT32 first_lba = read_le32(entry + 8u);
        UINT32 sectors = read_le32(entry + 12u);

        if (entry[4] == 0 || first_lba == 0 || sectors == 0) {
            continue;
        }

        status = read_lainfs_boot_resolution_from_block(block,
                                                        SystemTable,
                                                        (EFI_LBA)first_lba,
                                                        out_width,
                                                        out_height);
        if (!EFI_ERROR(status)) {
            uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, mbr);
            return EFI_SUCCESS;
        }
    }

    uefi_call_wrapper(SystemTable->BootServices->FreePool, 1, mbr);
    return EFI_NOT_FOUND;
}

static EFI_STATUS read_lainfs_boot_resolution(EFI_SYSTEM_TABLE *SystemTable,
                                              UINT32 *out_width,
                                              UINT32 *out_height) {
    EFI_STATUS status;
    EFI_BOOT_SERVICES *bs = SystemTable->BootServices;
    EFI_GUID block_io_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_HANDLE *handles = NULL;
    UINTN handle_count = 0;
    EFI_STATUS last_status = EFI_NOT_FOUND;

    status = uefi_call_wrapper(bs->LocateHandleBuffer, 5,
        ByProtocol, &block_io_guid, NULL, &handle_count, &handles);
    if (EFI_ERROR(status)) {
        return status;
    }

    for (UINTN i = 0; i < handle_count; ++i) {
        EFI_BLOCK_IO_PROTOCOL *block = NULL;
        status = uefi_call_wrapper(bs->HandleProtocol, 3,
            handles[i], &block_io_guid, (void **)&block);
        if (EFI_ERROR(status) || block == NULL || block->Media == NULL) {
            continue;
        }

        status = read_lainfs_boot_resolution_from_block(block, SystemTable, 0, out_width, out_height);
        if (EFI_ERROR(status)) {
            status = read_lainfs_boot_resolution_from_mbr(block, SystemTable, out_width, out_height);
        }
        if (!EFI_ERROR(status)) {
            uefi_call_wrapper(bs->FreePool, 1, handles);
            return EFI_SUCCESS;
        }
        last_status = status;
    }

    uefi_call_wrapper(bs->FreePool, 1, handles);
    return last_status;
}

static int gop_pixel_format_supported(EFI_GRAPHICS_PIXEL_FORMAT format) {
    return format == PixelRedGreenBlueReserved8BitPerColor ||
           format == PixelBlueGreenRedReserved8BitPerColor;
}

static EFI_STATUS set_requested_graphics_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
                                              UINT32 requested_width,
                                              UINT32 requested_height) {
    EFI_STATUS status;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
    UINTN info_size = 0;

    if (gop == NULL || gop->Mode == NULL || requested_width == 0 || requested_height == 0) {
        return EFI_SUCCESS;
    }

    for (UINT32 mode = 0; mode < gop->Mode->MaxMode; ++mode) {
        status = uefi_call_wrapper(gop->QueryMode, 4, gop, mode, &info_size, &info);
        if (EFI_ERROR(status) || info == NULL) {
            continue;
        }

        if (info->HorizontalResolution == requested_width &&
            info->VerticalResolution == requested_height &&
            gop_pixel_format_supported(info->PixelFormat)) {
            status = uefi_call_wrapper(gop->SetMode, 2, gop, mode);
            if (EFI_ERROR(status)) {
                Print(L"Failed to set requested GOP mode %ux%u: %r\r\n",
                      requested_width,
                      requested_height,
                      status);
            } else {
                Print(L"Set GOP mode %ux%u\r\n", requested_width, requested_height);
            }
            return status;
        }
    }

    Print(L"Requested GOP mode %ux%u not found; using firmware default.\r\n",
          requested_width,
          requested_height);
    return EFI_NOT_FOUND;
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *SystemTable) {
    InitializeLib(image, SystemTable);

    EFI_STATUS status;
    EFI_BOOT_SERVICES *bs = SystemTable->BootServices;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    UINTN memory_map_size = 0;
    EFI_MEMORY_DESCRIPTOR *memory_map = NULL;
    UINTN map_key = 0;
    UINTN descriptor_size = 0;
    UINT32 descriptor_version = 0;
    EFI_PHYSICAL_ADDRESS bootinfo_addr = 0;
    boot_info_t *boot_info = NULL;
    UINT32 requested_width = (UINT32)BOOT_RES_WIDTH;
    UINT32 requested_height = (UINT32)BOOT_RES_HEIGHT;

    Print(L"Lain UEFI loader starting...\r\n");

    kernel_file_t kernel_file = {0};
    void *kernel_file_buffer = NULL;
    void *kernel_entry_addr = NULL;
    UINT64 kernel_end = 0;

    status = load_optional_driver_from_boot_volume(image, SystemTable, NTFS_DRIVER_PATH);
    if (EFI_ERROR(status)) {
        Print(L"Optional NTFS driver not started: %r\r\n", status);
    } else {
        Print(L"Optional NTFS driver started.\r\n");
    }

    status = read_lainfs_boot_resolution(SystemTable, &requested_width, &requested_height);
    if (!EFI_ERROR(status)) {
        Print(L"Loaded lainfs boot resolution request: %ux%u\r\n", requested_width, requested_height);
    }

    status = open_kernel(image, SystemTable, &kernel_file);
    if (EFI_ERROR(status)) {
        Print(L"Failed to open kernel.elf: %r\r\n", status);
        return status;
    }

    status = read_kernel_file(SystemTable, &kernel_file, &kernel_file_buffer);
    if (EFI_ERROR(status)) {
        Print(L"Failed to read kernel.elf: %r\r\n", status);
        return status;
    }

    UINT64 kernel_base = 0;
    status = load_elf_kernel(SystemTable, kernel_file_buffer, kernel_file.size, &kernel_entry_addr, &kernel_base, &kernel_end);
    if (EFI_ERROR(status)) {
        Print(L"Failed to load ELF kernel: %r\r\n", status);
        return status;
    }

    status = uefi_call_wrapper(bs->AllocatePages, 4, AllocateAnyPages, EfiLoaderData,
        EFI_SIZE_TO_PAGES(sizeof(boot_info_t)), &bootinfo_addr);
    if (EFI_ERROR(status)) {
        Print(L"Failed to allocate boot info page: %r\r\n", status);
        return status;
    }

    boot_info = (boot_info_t*)(UINTN)bootinfo_addr;
    SetMem(boot_info, sizeof(*boot_info), 0);
    boot_info->magic = BOOTINFO_MAGIC;
    boot_info->kernel_base = kernel_base;
    boot_info->kernel_size = kernel_end - kernel_base;
    boot_info->rsdp = (UINT64)find_rsdp(SystemTable);
    boot_info->framebuffer_base = 0;
    boot_info->framebuffer_width = 0;
    boot_info->framebuffer_height = 0;
    boot_info->framebuffer_pixels_per_scanline = 0;
    boot_info->framebuffer_format = 0;

    status = uefi_call_wrapper(bs->LocateProtocol, 3, &gop_guid, NULL, (void**)&gop);
    if (!EFI_ERROR(status) && gop != NULL && gop->Mode != NULL && gop->Mode->Info != NULL) {
        set_requested_graphics_mode(gop, requested_width, requested_height);
        boot_info->framebuffer_base = gop->Mode->FrameBufferBase;
        boot_info->framebuffer_width = gop->Mode->Info->HorizontalResolution;
        boot_info->framebuffer_height = gop->Mode->Info->VerticalResolution;
        boot_info->framebuffer_pixels_per_scanline = gop->Mode->Info->PixelsPerScanLine;
        boot_info->framebuffer_format = gop->Mode->Info->PixelFormat;
    }

    status = uefi_call_wrapper(bs->GetMemoryMap, 5, &memory_map_size, memory_map, &map_key, &descriptor_size, &descriptor_version);
    if (status != EFI_BUFFER_TOO_SMALL) {
        Print(L"GetMemoryMap sizing failed: %r\r\n", status);
        return status;
    }

    memory_map_size += descriptor_size * 16;
    status = uefi_call_wrapper(bs->AllocatePool, 3, EfiLoaderData, memory_map_size, (void**)&memory_map);
    if (EFI_ERROR(status)) {
        Print(L"Failed to allocate memory map buffer: %r\r\n", status);
        return status;
    }

    Print(L"Loaded ELF kernel: base=0x%lx entry=0x%lx size=%lu bytes\r\n", kernel_base, kernel_entry_addr, kernel_end - kernel_base);
    Print(L"Exiting boot services...\r\n");

    for (UINTN attempt = 0; attempt < 8; ++attempt) {
        UINTN current_map_size = memory_map_size;
        status = uefi_call_wrapper(bs->GetMemoryMap, 5, &current_map_size, memory_map, &map_key, &descriptor_size, &descriptor_version);
        if (status == EFI_BUFFER_TOO_SMALL) {
            memory_map_size = current_map_size + descriptor_size * 16;
            status = uefi_call_wrapper(bs->FreePool, 1, memory_map);
            if (EFI_ERROR(status)) {
                Print(L"Failed to grow memory map buffer: %r\r\n", status);
                return status;
            }

            status = uefi_call_wrapper(bs->AllocatePool, 3, EfiLoaderData, memory_map_size, (void**)&memory_map);
            if (EFI_ERROR(status)) {
                Print(L"Failed to reallocate memory map buffer: %r\r\n", status);
                return status;
            }
            continue;
        }

        if (EFI_ERROR(status)) {
            Print(L"GetMemoryMap failed: %r\r\n", status);
            return status;
        }

        boot_info->memory_map = (UINT64)memory_map;
        boot_info->memory_map_size = current_map_size;
        boot_info->memory_map_descriptor_size = descriptor_size;
        boot_info->memory_map_descriptor_version = descriptor_version;

        status = uefi_call_wrapper(bs->ExitBootServices, 2, image, map_key);
        if (!EFI_ERROR(status)) {
            break;
        }

        if (status != EFI_INVALID_PARAMETER) {
            Print(L"ExitBootServices failed: %r\r\n", status);
            return status;
        }
    }

    if (EFI_ERROR(status)) {
        Print(L"ExitBootServices did not succeed after retries: %r\r\n", status);
        return status;
    }

    jump_to_kernel(kernel_entry_addr, boot_info);
}
