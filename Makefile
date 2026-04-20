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
DATA_IMG := data.img
ISO_DIR := build/image
EFI_DIR := $(ISO_DIR)/EFI/BOOT
EFI_ARCH ?= x86_64
EFI_INC ?= /usr/include/efi
EFI_LIBDIR ?= /usr/lib
EFI_CRT0 ?= /usr/lib/crt0-efi-$(EFI_ARCH).o
EFI_LDS ?= /usr/lib/elf_$(EFI_ARCH)_efi.lds
MTOOLS_MCOPY ?= mcopy
MTOOLS_MMD ?= mmd
MKFS_FAT ?= mkfs.fat
SGDISK ?= sgdisk
ESP_SIZE_KB ?= 65536
BOOTDISK_SIZE_KB ?= 131072
DATA_SIZE_KB ?= 65536
NTFS_DRIVER ?=

PROJECT_CFLAGS := -Iboot/shared
NASMFLAGS := -Iboot/shared/
CFLAGS := $(PROJECT_CFLAGS) -I$(EFI_INC) -I$(EFI_INC)/$(EFI_ARCH) -fpic -ffreestanding -fno-stack-protector -fno-stack-check -fshort-wchar -mno-red-zone -Wall -Wextra -DEFI_FUNCTION_WRAPPER
KERNEL_CFLAGS := $(PROJECT_CFLAGS) -ffreestanding -fno-stack-protector -fno-stack-check -mno-red-zone -Wall -Wextra -std=c11
LDFLAGS_EFI := -nostdlib -znocombreloc -T $(EFI_LDS) -shared -Bsymbolic -L$(EFI_LIBDIR) $(EFI_CRT0)
LDLIBS_EFI := -lefi -lgnuefi
OBJCOPY_EFI_FLAGS := --target efi-app-$(EFI_ARCH) -j .text -j .sdata -j .data -j .dynamic -j .dynsym -j .rela -j .rel -j .reloc

all: build/$(BOOTLOADER) build/$(KERNEL_BIN) build/$(KERNEL_ELF) image build/$(ESP_IMG) build/$(BOOTDISK_IMG) build/$(DATA_IMG)

build:
	$(MKDIR_P) build

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

build/kernel/main.o: kernel/main.c kernel/kernel.h kernel/shell.h kernel/storage.h kernel/ahci.h boot/shared/bootinfo.h | build
	$(MKDIR_P) build/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

build/kernel/shell.o: kernel/shell.c kernel/shell.h kernel/kernel.h kernel/storage.h kernel/lainfs.h kernel/editor.h kernel/ahci.h boot/shared/bootinfo.h | build
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

build/kernel/timer.o: kernel/timer.c kernel/kernel.h | build
	$(MKDIR_P) build/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

build/kernel/cpu_c.o: kernel/cpu.c kernel/kernel.h boot/shared/bootinfo.h | build
	$(MKDIR_P) build/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

build/kernel/console_c.o: kernel/console.c kernel/kernel.h boot/shared/bootinfo.h | build
	$(MKDIR_P) build/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

build/kernel/keyboard.o: kernel/keyboard.c kernel/kernel.h kernel/keyboard.h boot/shared/bootinfo.h | build
	$(MKDIR_P) build/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

build/kernel/cpu_low.o: kernel/cpu_low.asm | build
	$(MKDIR_P) build/kernel
	$(NASM) $(NASMFLAGS) -f elf64 $< -o $@

build/kernel/interrupts.o: kernel/interrupts.asm | build
	$(MKDIR_P) build/kernel
	$(NASM) $(NASMFLAGS) -f elf64 $< -o $@

build/$(KERNEL_ELF): build/kernel/entry.o build/kernel/main.o build/kernel/shell.o build/kernel/storage.o build/kernel/pci.o build/kernel/ahci.o build/kernel/lainfs.o build/kernel/editor.o build/kernel/timer.o build/kernel/cpu_c.o build/kernel/cpu_low.o build/kernel/console_c.o build/kernel/keyboard.o build/kernel/interrupts.o kernel/linker.ld
	$(LD) -nostdlib -z max-page-size=0x1000 -T kernel/linker.ld -o build/$(KERNEL_ELF) build/kernel/entry.o build/kernel/main.o build/kernel/shell.o build/kernel/storage.o build/kernel/pci.o build/kernel/ahci.o build/kernel/lainfs.o build/kernel/editor.o build/kernel/timer.o build/kernel/cpu_c.o build/kernel/cpu_low.o build/kernel/console_c.o build/kernel/keyboard.o build/kernel/interrupts.o

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
	$(SGDISK) --clear --new=1:2048:+64M --typecode=1:EF00 --change-name=1:EFI build/$(BOOTDISK_IMG)
	dd if=build/$(ESP_IMG) of=build/$(BOOTDISK_IMG) bs=512 seek=2048 conv=notrunc

build/$(DATA_IMG): | build
	dd if=/dev/zero of=build/$(DATA_IMG) bs=1024 count=$(DATA_SIZE_KB)
	printf '\000\000\002\000\231\377\377\377\000\010\000\000\000\370\001\000' | dd of=build/$(DATA_IMG) bs=1 seek=446 conv=notrunc
	printf '\125\252' | dd of=build/$(DATA_IMG) bs=1 seek=510 conv=notrunc

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

.PHONY: all build image run run-ahci run-bootdisk clean inspect-efi print-efi-config
