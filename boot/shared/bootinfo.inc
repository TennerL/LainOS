%ifndef BOOTINFO_INC
%define BOOTINFO_INC

BOOTINFO_MAGIC equ 0x4C41494E424F4F54

struc boot_info
    .magic                           resq 1
    .kernel_base                     resq 1
    .kernel_size                     resq 1
    .memory_map                      resq 1
    .memory_map_size                 resq 1
    .memory_map_descriptor_size      resq 1
    .memory_map_descriptor_version   resq 1
    .framebuffer_base                resq 1
    .framebuffer_width               resd 1
    .framebuffer_height              resd 1
    .framebuffer_pixels_per_scanline resd 1
    .framebuffer_format              resd 1
    .rsdp                            resq 1
endstruc

%endif
