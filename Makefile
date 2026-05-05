CC := gcc
LD := ld
NASM := nasm
OBJCOPY := objcopy
MKDIR_P := mkdir -p

BOOTLOADER := BOOTX64.EFI
KERNEL_BIN := kernel.bin
KERNEL_ELF := kernel.elf
ESP_IMG := esp.img
BOOTDISK_IMG := bootdisk.img
BOOTDISK_GPT_IMG := bootdisk-gpt.img
DATA_IMG := data.img
ISO_IMG := boot.iso
ISO_DIR := build/image
EFI_DIR := $(ISO_DIR)/EFI/BOOT
ISO_STAGING_DIR := build/iso-root
ISO_BOOT_IMG := efiboot.img
ISO_STARTUP_NSH := build/startup.nsh
BUILD_VERSION_H := build/version.h
RAMDISK_SEED_H := build/ramdisk_seed.h
RAMDISK_SEED_FILES := $(shell find examples -type f | sort)
EFI_ARCH ?= x86_64
EFI_INC ?= /usr/include/efi
EFI_LIBDIR ?= /usr/lib
EFI_CRT0 ?= /usr/lib/crt0-efi-$(EFI_ARCH).o
EFI_LDS ?= /usr/lib/elf_$(EFI_ARCH)_efi.lds
MTOOLS_MCOPY ?= mcopy
MTOOLS_MMD ?= mmd
MKFS_FAT ?= mkfs.fat
SGDISK ?= sgdisk
XORRISO ?= xorriso
MKISOFS ?= mkisofs
GENISOIMAGE ?= genisoimage
ESP_SIZE_KB ?= 65536
ISO_ESP_SIZE_KB ?= 16384
BOOTDISK_SIZE_KB ?= 131072
DATA_SIZE_KB ?= 65536
NTFS_DRIVER ?=
BOOT_RES_WIDTH ?= 0
BOOT_RES_HEIGHT ?= 0

PROJECT_CFLAGS := -Iboot/shared -Ibuild
NASMFLAGS := -Iboot/shared/
CFLAGS := $(PROJECT_CFLAGS) -I$(EFI_INC) -I$(EFI_INC)/$(EFI_ARCH) -fpic -ffreestanding -fno-stack-protector -fno-stack-check -fshort-wchar -mno-red-zone -Wall -Wextra -DEFI_FUNCTION_WRAPPER -DBOOT_RES_WIDTH=$(BOOT_RES_WIDTH) -DBOOT_RES_HEIGHT=$(BOOT_RES_HEIGHT)
KERNEL_CFLAGS := $(PROJECT_CFLAGS) -ffreestanding -fno-stack-protector -fno-stack-check -mno-red-zone -Wall -Wextra -std=c11
HOST_CFLAGS := -Iboot/shared -Ikernel -std=c11 -Wall -Wextra -Wno-unused-function
LDFLAGS_EFI := -nostdlib -znocombreloc -T $(EFI_LDS) -shared -Bsymbolic -L$(EFI_LIBDIR) $(EFI_CRT0)
LDLIBS_EFI := -lefi -lgnuefi
OBJCOPY_EFI_FLAGS := --target efi-app-$(EFI_ARCH) -j .text -j .sdata -j .data -j .dynamic -j .dynsym -j .rela -j .rel -j .reloc

all: build/$(BOOTLOADER) build/$(KERNEL_BIN) build/$(KERNEL_ELF) image build/$(ESP_IMG) build/$(BOOTDISK_IMG) build/$(BOOTDISK_GPT_IMG) build/$(DATA_IMG) build/$(ISO_IMG)

build:
	$(MKDIR_P) build

FORCE:

$(BUILD_VERSION_H): FORCE | build
	@old=0; \
	if [ -f build/build_number.txt ]; then old=$$(cat build/build_number.txt); fi; \
	new=$$((old + 1)); \
	printf '%s\n' $$new > build/build_number.txt; \
	git_rev=$$(git rev-parse --short HEAD 2>/dev/null || printf unknown); \
	build_time=$$(date -u '+%Y-%m-%dT%H:%M:%SZ'); \
	printf '#ifndef BUILD_VERSION_H\n#define BUILD_VERSION_H\n\n' > $@; \
	printf '#define BUILD_NUMBER %s\n' "$$new" >> $@; \
	printf '#define BUILD_GIT_REV "%s"\n' "$$git_rev" >> $@; \
	printf '#define BUILD_TIMESTAMP "%s"\n' "$$build_time" >> $@; \
	printf '#define BUILD_VERSION_STRING "build %s (%s, %s)"\n\n' "$$new" "$$build_time" "$$git_rev" >> $@; \
	printf '#endif\n' >> $@

build/bootloader/main.o: bootloader/main.c | build
	$(MKDIR_P) build/bootloader
	$(CC) $(CFLAGS) -c $< -o $@

build/bootloader.so: build/bootloader/main.o
	$(LD) $(LDFLAGS_EFI) -o $@ $< $(LDLIBS_EFI)

build/$(BOOTLOADER): build/bootloader.so
	$(OBJCOPY) $(OBJCOPY_EFI_FLAGS) $< $@

build/kernel/entry.o: kernel/entry.asm | build
	$(MKDIR_P) build/kernel
	$(NASM) $(NASMFLAGS) -f elf64 $< -o $@

build/kernel/main.o: kernel/main.c kernel/kernel.h kernel/graphics.h kernel/shell.h kernel/storage.h kernel/ahci.h kernel/mouse.h boot/shared/bootinfo.h $(BUILD_VERSION_H) | build
	$(MKDIR_P) build/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

# build/kernel/shell.o: kernel/shell.c kernel/shell.h kernel/kernel.h kernel/storage.h kernel/lainfs.h kernel/editor.h kernel/ahci.h kernel/zobject.h boot/shared/bootinfo.h $(BUILD_VERSION_H) | build
# 	$(MKDIR_P) build/kernel
# 	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

build/kernel/shell.o: kernel/shell.c kernel/shell.h kernel/kernel.h kernel/storage.h kernel/lainfs.h kernel/editor.h kernel/browser.h kernel/ahci.h kernel/zobject.h boot/shared/bootinfo.h $(BUILD_VERSION_H) $(RAMDISK_SEED_H) | build
	$(MKDIR_P) build/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

build/kernel/kernel_exports.o: kernel/kernel_exports.c kernel/kernel_exports.h kernel/kernel.h kernel/graphics.h kernel/shell.h boot/shared/bootinfo.h | build
	$(MKDIR_P) build/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

build/kernel/assembler.o: kernel/assembler.c kernel/assembler.h kernel/kernel.h kernel/kernel_exports.h boot/shared/bootinfo.h | build
	$(MKDIR_P) build/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

build/kernel/zscript.o: kernel/zscript.c kernel/zscript.h | build
	$(MKDIR_P) build/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

build/tools/zcc_host: tools/zcc_host.c kernel/zscript.c kernel/zscript.h | build
	$(MKDIR_P) build/tools
	$(CC) $(HOST_CFLAGS) tools/zcc_host.c kernel/zscript.c -o $@

build/tools/lainfs_seed: tools/lainfs_seed.c | build
	$(MKDIR_P) build/tools
	$(CC) $(HOST_CFLAGS) tools/lainfs_seed.c -o $@

build/tools/ramdisk_seed_gen: tools/ramdisk_seed_gen.c | build
	$(MKDIR_P) build/tools
	$(CC) $(HOST_CFLAGS) tools/ramdisk_seed_gen.c -o $@

$(RAMDISK_SEED_H): build/tools/ramdisk_seed_gen $(RAMDISK_SEED_FILES) | build
	build/tools/ramdisk_seed_gen $@ $(RAMDISK_SEED_FILES)

build/kernel/zlink_probe.asm: kernel/zlink_probe.Z build/tools/zcc_host | build
	$(MKDIR_P) build/kernel
	build/tools/zcc_host $< $@

build/kernel/zlink_probe.o: build/kernel/zlink_probe.asm | build
	$(MKDIR_P) build/kernel
	$(NASM) $(NASMFLAGS) -f elf64 $< -o $@

build/kernel/zobject.o: kernel/zobject.c kernel/zobject.h kernel/assembler.h kernel/kernel_exports.h | build
	$(MKDIR_P) build/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

build/kernel/storage.o: kernel/storage.c kernel/storage.h kernel/ahci.h | build
	$(MKDIR_P) build/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

build/kernel/pci.o: kernel/pci.c kernel/pci.h | build
	$(MKDIR_P) build/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

build/kernel/ahci.o: kernel/ahci.c kernel/ahci.h kernel/pci.h kernel/storage.h | build
	$(MKDIR_P) build/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

build/kernel/lainfs.o: kernel/lainfs.c kernel/lainfs.h kernel/kernel.h kernel/storage.h | build
	$(MKDIR_P) build/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

build/kernel/editor.o: kernel/editor.c kernel/editor.h kernel/kernel.h kernel/keyboard.h kernel/lainfs.h boot/shared/bootinfo.h | build
	$(MKDIR_P) build/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

build/kernel/browser.o: kernel/browser.c kernel/browser.h kernel/kernel.h kernel/keyboard.h kernel/lainfs.h boot/shared/bootinfo.h | build
	$(MKDIR_P) build/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

build/kernel/timer.o: kernel/timer.c kernel/kernel.h | build
	$(MKDIR_P) build/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

build/kernel/cpu_c.o: kernel/cpu.c kernel/kernel.h boot/shared/bootinfo.h | build
	$(MKDIR_P) build/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

build/kernel/graphics.o: kernel/graphics.c kernel/graphics.h | build
	$(MKDIR_P) build/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

build/kernel/console_c.o: kernel/console.c kernel/kernel.h kernel/graphics.h boot/shared/bootinfo.h | build
	$(MKDIR_P) build/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

build/kernel/keyboard.o: kernel/keyboard.c kernel/kernel.h kernel/keyboard.h kernel/mouse.h boot/shared/bootinfo.h | build
	$(MKDIR_P) build/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

build/kernel/mouse.o: kernel/mouse.c kernel/mouse.h kernel/graphics.h | build
	$(MKDIR_P) build/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

build/kernel/cpu_low.o: kernel/cpu_low.asm | build
	$(MKDIR_P) build/kernel
	$(NASM) $(NASMFLAGS) -f elf64 $< -o $@

build/kernel/interrupts.o: kernel/interrupts.asm | build
	$(MKDIR_P) build/kernel
	$(NASM) $(NASMFLAGS) -f elf64 $< -o $@

# build/$(KERNEL_ELF): build/kernel/entry.o build/kernel/main.o build/kernel/shell.o build/kernel/kernel_exports.o build/kernel/assembler.o build/kernel/zscript.o build/kernel/zobject.o build/kernel/storage.o build/kernel/pci.o build/kernel/ahci.o build/kernel/lainfs.o build/kernel/editor.o build/kernel/timer.o build/kernel/cpu_c.o build/kernel/cpu_low.o build/kernel/graphics.o build/kernel/console_c.o build/kernel/keyboard.o build/kernel/mouse.o build/kernel/zlink_probe.o build/kernel/interrupts.o kernel/linker.ld
# 	$(LD) -nostdlib -z max-page-size=0x1000 -T kernel/linker.ld -o build/$(KERNEL_ELF) build/kernel/entry.o build/kernel/main.o build/kernel/shell.o build/kernel/kernel_exports.o build/kernel/assembler.o build/kernel/zscript.o build/kernel/zobject.o build/kernel/storage.o build/kernel/pci.o build/kernel/ahci.o build/kernel/lainfs.o build/kernel/editor.o build/kernel/timer.o build/kernel/cpu_c.o build/kernel/cpu_low.o build/kernel/graphics.o build/kernel/console_c.o build/kernel/keyboard.o build/kernel/mouse.o build/kernel/zlink_probe.o build/kernel/interrupts.o

build/$(KERNEL_ELF): build/kernel/entry.o build/kernel/main.o build/kernel/shell.o build/kernel/kernel_exports.o build/kernel/assembler.o build/kernel/zscript.o build/kernel/zobject.o build/kernel/storage.o build/kernel/pci.o build/kernel/ahci.o build/kernel/lainfs.o build/kernel/editor.o build/kernel/browser.o build/kernel/timer.o build/kernel/cpu_c.o build/kernel/cpu_low.o build/kernel/graphics.o build/kernel/console_c.o build/kernel/keyboard.o build/kernel/mouse.o build/kernel/zlink_probe.o build/kernel/interrupts.o kernel/linker.ld
	$(LD) -nostdlib -z max-page-size=0x1000 -T kernel/linker.ld -o build/$(KERNEL_ELF) build/kernel/entry.o build/kernel/main.o build/kernel/shell.o build/kernel/kernel_exports.o build/kernel/assembler.o build/kernel/zscript.o build/kernel/zobject.o build/kernel/storage.o build/kernel/pci.o build/kernel/ahci.o build/kernel/lainfs.o build/kernel/editor.o build/kernel/browser.o build/kernel/timer.o build/kernel/cpu_c.o build/kernel/cpu_low.o build/kernel/graphics.o build/kernel/console_c.o build/kernel/keyboard.o build/kernel/mouse.o build/kernel/zlink_probe.o build/kernel/interrupts.o
	@bss=$$(size build/$(KERNEL_ELF) | awk 'NR == 2 { print $$3 }'); \
	if [ "$$bss" -gt 8388608 ]; then \
		echo "kernel bss is too large for reliable ISO boot: $$bss bytes" >&2; \
		exit 1; \
	fi

build/$(KERNEL_BIN): build/$(KERNEL_ELF)
	$(OBJCOPY) -O binary build/$(KERNEL_ELF) $@

image: build/$(BOOTLOADER) build/$(KERNEL_ELF)
	$(MKDIR_P) $(EFI_DIR)
	cp build/$(BOOTLOADER) $(EFI_DIR)/BOOTX64.EFI
	cp build/$(KERNEL_ELF) $(ISO_DIR)/kernel.elf
	if [ -n "$(NTFS_DRIVER)" ]; then $(MKDIR_P) $(EFI_DIR)/drivers && cp "$(NTFS_DRIVER)" $(EFI_DIR)/drivers/ntfs_x64.efi; fi
	cp /bin/true $(ISO_DIR)/.image-stamp >/dev/null 2>&1 || true

build/$(ESP_IMG): image
	dd if=/dev/zero of=build/$(ESP_IMG) bs=1024 count=$(ESP_SIZE_KB)
	$(MKFS_FAT) -F 32 build/$(ESP_IMG)
	$(MTOOLS_MMD) -i build/$(ESP_IMG) ::/EFI ::/EFI/BOOT
	$(MTOOLS_MCOPY) -i build/$(ESP_IMG) build/$(BOOTLOADER) ::/EFI/BOOT/BOOTX64.EFI
	$(MTOOLS_MCOPY) -i build/$(ESP_IMG) build/$(KERNEL_ELF) ::/kernel.elf
	if [ -n "$(NTFS_DRIVER)" ]; then $(MTOOLS_MMD) -i build/$(ESP_IMG) ::/EFI/BOOT/drivers && $(MTOOLS_MCOPY) -i build/$(ESP_IMG) "$(NTFS_DRIVER)" ::/EFI/BOOT/drivers/ntfs_x64.efi; fi

build/$(BOOTDISK_IMG): build/$(ESP_IMG)
	dd if=/dev/zero of=build/$(BOOTDISK_IMG) bs=1024 count=$(BOOTDISK_SIZE_KB)
	dd if=build/$(ESP_IMG) of=build/$(BOOTDISK_IMG) bs=512 seek=2048 conv=notrunc
	printf '\200\000\002\000\014\377\377\377\000\010\000\000\000\000\002\000' | dd of=build/$(BOOTDISK_IMG) bs=1 seek=446 conv=notrunc
	printf '\125\252' | dd of=build/$(BOOTDISK_IMG) bs=1 seek=510 conv=notrunc

build/$(BOOTDISK_GPT_IMG): build/$(ESP_IMG)
	dd if=/dev/zero of=build/$(BOOTDISK_GPT_IMG) bs=1024 count=$(BOOTDISK_SIZE_KB)
	$(SGDISK) --clear --new=1:2048:+64M --typecode=1:EF00 --change-name=1:EFI build/$(BOOTDISK_GPT_IMG)
	dd if=build/$(ESP_IMG) of=build/$(BOOTDISK_GPT_IMG) bs=512 seek=2048 conv=notrunc

build/$(DATA_IMG): | build build/tools/lainfs_seed
	dd if=/dev/zero of=build/$(DATA_IMG) bs=1024 count=$(DATA_SIZE_KB)
	build/tools/lainfs_seed build/$(DATA_IMG)

reseed-data: build/tools/lainfs_seed | build
	dd if=/dev/zero of=build/$(DATA_IMG) bs=1024 count=$(DATA_SIZE_KB)
	build/tools/lainfs_seed build/$(DATA_IMG)

$(ISO_STARTUP_NSH): | build
	printf 'FS0:\\EFI\\BOOT\\BOOTX64.EFI\r\nFS1:\\EFI\\BOOT\\BOOTX64.EFI\r\nFS2:\\EFI\\BOOT\\BOOTX64.EFI\r\n' > $@

build/$(ISO_IMG): image $(ISO_STARTUP_NSH) | build
	rm -rf $(ISO_STAGING_DIR)
	$(MKDIR_P) $(ISO_STAGING_DIR)
	dd if=/dev/zero of=$(ISO_STAGING_DIR)/$(ISO_BOOT_IMG) bs=1024 count=$(ISO_ESP_SIZE_KB)
	$(MKFS_FAT) -F 16 $(ISO_STAGING_DIR)/$(ISO_BOOT_IMG)
	$(MTOOLS_MMD) -i $(ISO_STAGING_DIR)/$(ISO_BOOT_IMG) ::/EFI ::/EFI/BOOT
	$(MTOOLS_MCOPY) -i $(ISO_STAGING_DIR)/$(ISO_BOOT_IMG) build/$(BOOTLOADER) ::/EFI/BOOT/BOOTX64.EFI
	$(MTOOLS_MCOPY) -i $(ISO_STAGING_DIR)/$(ISO_BOOT_IMG) build/$(KERNEL_ELF) ::/kernel.elf
	$(MTOOLS_MCOPY) -i $(ISO_STAGING_DIR)/$(ISO_BOOT_IMG) $(ISO_STARTUP_NSH) ::/startup.nsh
	if [ -n "$(NTFS_DRIVER)" ]; then $(MTOOLS_MMD) -i $(ISO_STAGING_DIR)/$(ISO_BOOT_IMG) ::/EFI/BOOT/drivers && $(MTOOLS_MCOPY) -i $(ISO_STAGING_DIR)/$(ISO_BOOT_IMG) "$(NTFS_DRIVER)" ::/EFI/BOOT/drivers/ntfs_x64.efi; fi
	if [ -d "$(ISO_DIR)" ]; then cp -R $(ISO_DIR)/. $(ISO_STAGING_DIR)/; fi
	cp $(ISO_STARTUP_NSH) $(ISO_STAGING_DIR)/startup.nsh
	if command -v $(XORRISO) >/dev/null 2>&1; then \
		$(XORRISO) -as mkisofs -R -J -V LAINOS \
			-eltorito-alt-boot -e $(ISO_BOOT_IMG) -no-emul-boot \
			-o $@ $(ISO_STAGING_DIR); \
	elif command -v $(MKISOFS) >/dev/null 2>&1; then \
		$(MKISOFS) -R -J -V LAINOS \
			-eltorito-alt-boot -e $(ISO_BOOT_IMG) -no-emul-boot \
			-o $@ $(ISO_STAGING_DIR); \
	elif command -v $(GENISOIMAGE) >/dev/null 2>&1; then \
		$(GENISOIMAGE) -R -J -V LAINOS \
			-eltorito-alt-boot -e $(ISO_BOOT_IMG) -no-emul-boot \
			-o $@ $(ISO_STAGING_DIR); \
	else \
		echo "No supported ISO builder found. Install xorriso, mkisofs, or genisoimage." >&2; \
		exit 1; \
	fi

run: all
	qemu-system-x86_64 \
		-enable-kvm \
		-m 256M \
		-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
		-drive if=pflash,format=raw,file=/usr/share/OVMF/OVMF_VARS_4M.fd \
		-drive format=raw,file=build/$(ESP_IMG),if=ide,index=0 \
		-drive format=raw,file=build/$(DATA_IMG),if=ide,index=1 
run-ahci: all
	qemu-system-x86_64 \
		-enable-kvm \
		-m 256M \
		-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
		-drive if=pflash,format=raw,file=/usr/share/OVMF/OVMF_VARS_4M.fd \
		-drive format=raw,file=build/$(ESP_IMG),if=ide,index=0 \
		-device ich9-ahci,id=ahci \
		-drive if=none,id=data,format=raw,file=build/$(DATA_IMG) \
		-device ide-hd,drive=data,bus=ahci.0

run-bootdisk: all
	qemu-system-x86_64 \
		-enable-kvm \
		-m 256M \
		-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
		-drive if=pflash,format=raw,file=/usr/share/OVMF/OVMF_VARS_4M.fd \
		-drive format=raw,file=build/$(BOOTDISK_IMG),if=ide,index=0 \
		-device ich9-ahci,id=ahci \
		-drive if=none,id=data,format=raw,file=build/$(DATA_IMG) \
		-device ide-hd,drive=data,bus=ahci.0

run-bootdisk-gpt: all
	qemu-system-x86_64 \
		-enable-kvm \
		-m 256M \
		-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
		-drive if=pflash,format=raw,file=/usr/share/OVMF/OVMF_VARS_4M.fd \
		-drive format=raw,file=build/$(BOOTDISK_GPT_IMG),if=ide,index=0 \
		-device ich9-ahci,id=ahci \
		-drive if=none,id=data,format=raw,file=build/$(DATA_IMG) \
		-device ide-hd,drive=data,bus=ahci.0

run-iso: build/$(ISO_IMG) build/$(DATA_IMG)
	cp /usr/share/OVMF/OVMF_VARS_4M.fd build/OVMF_VARS.iso.fd
	qemu-system-x86_64 \
		-m 256M \
		-boot d \
		-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
		-drive if=pflash,format=raw,file=build/OVMF_VARS.iso.fd \
		-cdrom build/$(ISO_IMG) \
		-drive format=raw,file=build/$(DATA_IMG),if=ide,index=1

clean:
	rm -rf build BOOTX64.EFI

inspect-efi: build/$(BOOTLOADER)
	file build/$(BOOTLOADER)
	objdump -p build/$(BOOTLOADER) | grep -E "Subsystem|ImageBase|SectionAlignment|FileAlignment"
	objdump -h build/$(BOOTLOADER)

print-efi-config:
	@echo EFI_INC=$(EFI_INC)
	@echo EFI_LIBDIR=$(EFI_LIBDIR)
	@echo EFI_CRT0=$(EFI_CRT0)
	@echo EFI_LDS=$(EFI_LDS)
	@echo EFI_ARCH=$(EFI_ARCH)

.PHONY: all build image run run-ahci run-bootdisk run-bootdisk-gpt run-iso reseed-data clean inspect-efi print-efi-config FORCE
