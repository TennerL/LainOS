#include <efi.h>
#include <efilib.h>
#include <elf.h>
#include "bootinfo.h"

#define KERNEL_PATH L"\\kernel.elf"
#define NTFS_DRIVER_PATH L"\\EFI\\BOOT\\drivers\\ntfs_x64.efi"
#define BOOTINFO_LOAD_ADDRESS 0x90000

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

static EFI_STATUS open_kernel(EFI_SYSTEM_TABLE *SystemTable, kernel_file_t *out) {
    EFI_STATUS status;
    EFI_BOOT_SERVICES *bs = SystemTable->BootServices;
    EFI_GUID fs_protocol = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_HANDLE *fs_handles = NULL;
    UINTN fs_handle_count = 0;
    EFI_STATUS last_error = EFI_NOT_FOUND;

    status = uefi_call_wrapper(bs->LocateHandleBuffer, 5,
        ByProtocol, &fs_protocol, NULL, &fs_handle_count, &fs_handles);
    if (EFI_ERROR(status)) return status;

    for (UINTN i = 0; i < fs_handle_count; ++i) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
        EFI_FILE_PROTOCOL *kernel = NULL;
        UINTN kernel_size = 0;

        status = uefi_call_wrapper(bs->HandleProtocol, 3,
            fs_handles[i], &fs_protocol, (void**)&fs);
        if (EFI_ERROR(status)) {
            last_error = status;
            continue;
        }

        status = open_file_on_fs(fs, KERNEL_PATH, &kernel);
        if (EFI_ERROR(status)) {
            last_error = status;
            continue;
        }

        status = get_file_size(SystemTable, kernel, &kernel_size);
        if (EFI_ERROR(status)) {
            uefi_call_wrapper(kernel->Close, 1, kernel);
            last_error = status;
            continue;
        }

        out->handle = kernel;
        out->size = kernel_size;
        uefi_call_wrapper(bs->FreePool, 1, fs_handles);
        return EFI_SUCCESS;
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
    EFI_PHYSICAL_ADDRESS bootinfo_addr = BOOTINFO_LOAD_ADDRESS;
    boot_info_t *boot_info = NULL;

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

    status = open_kernel(SystemTable, &kernel_file);
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

    status = uefi_call_wrapper(bs->AllocatePages, 4, AllocateAddress, EfiLoaderData,
        EFI_SIZE_TO_PAGES(sizeof(boot_info_t)), &bootinfo_addr);
    if (EFI_ERROR(status)) {
        Print(L"Failed to allocate boot info page: %r\r\n", status);
        return status;
    }

    boot_info = (boot_info_t*)BOOTINFO_LOAD_ADDRESS;
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
