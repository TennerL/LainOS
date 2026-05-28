HOST_CC ?= cc
ifeq ($(origin CC),default)
CC := $(shell if command -v x86_64-elf-gcc >/dev/null 2>&1; then printf 'x86_64-elf-gcc'; elif command -v clang >/dev/null 2>&1 && ! ld --version >/dev/null 2>&1; then printf 'clang --target=x86_64-unknown-elf'; else printf 'gcc'; fi)
endif
LD := $(shell if ld --version >/dev/null 2>&1; then printf 'ld'; elif command -v gld >/dev/null 2>&1; then printf 'gld'; elif command -v x86_64-elf-ld >/dev/null 2>&1; then printf 'x86_64-elf-ld'; else printf 'ld'; fi)
NASM := nasm
OBJCOPY := $(shell if objcopy --version >/dev/null 2>&1; then printf 'objcopy'; elif command -v gobjcopy >/dev/null 2>&1; then printf 'gobjcopy'; elif command -v x86_64-elf-objcopy >/dev/null 2>&1; then printf 'x86_64-elf-objcopy'; else printf 'objcopy'; fi)
MKDIR_P := mkdir -p
SHA1SUM := $(shell if command -v sha1sum >/dev/null 2>&1; then printf 'sha1sum'; elif command -v gsha1sum >/dev/null 2>&1; then printf 'gsha1sum'; elif command -v shasum >/dev/null 2>&1; then printf 'shasum -a 1'; else printf 'sha1sum'; fi)

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
PXE_DIR := build/pxe
PXE_BOOTLOADER := BOOTX64.EFI
BUILD_VERSION_H := build/version.h
RAMDISK_SEED_H := build/ramdisk_seed.h
RAMDISK_SEED_FILES := $(shell find examples -type f | sort)
RAMDISK_SEED_ARGS := $(RAMDISK_SEED_FILES)
KERNEL_FONT_TTF := third_party/fonts/DejaVuSans.ttf
KERNEL_FONT_TTF_H := build/dejavu_sans_ttf.h
NETSURF_DEFAULT_CSS := third_party/netsurf/src/netsurf/resources/default.css
NETSURF_QUIRKS_CSS := third_party/netsurf/src/netsurf/resources/quirks.css
NETSURF_RESOURCE_CSS_H := build/netsurf_resource_css.h
EXPAT_CONFIG_H := build/third_party/expat/expat_config.h
NETSURF_LIBDOM_XML_HEADER := build/third_party/netsurf/libdom/include/dom/bindings/xml/xmlparser.h
EFI_ARCH ?= x86_64
EFI_PREFIX ?= $(shell prefix=''; if command -v brew >/dev/null 2>&1; then prefix=$$(brew --prefix gnu-efi 2>/dev/null || true); fi; if [ -n "$$prefix" ] && [ -d "$$prefix" ]; then printf '%s' "$$prefix"; elif [ -d /opt/homebrew/opt/gnu-efi ]; then printf '/opt/homebrew/opt/gnu-efi'; elif [ -d /usr/local/opt/gnu-efi ]; then printf '/usr/local/opt/gnu-efi'; else printf '/usr'; fi)
EFI_INC ?= $(if $(filter /usr,$(EFI_PREFIX)),$(shell for dir in "$(EFI_PREFIX)/include/efi" "$(EFI_PREFIX)/include" /usr/include/efi; do if [ -f "$$dir/efi.h" ]; then printf '%s' "$$dir"; exit; fi; done; printf '%s/include/efi' "$(EFI_PREFIX)"),$(EFI_PREFIX)/include/efi)
EFI_LIBDIR ?= $(if $(filter /usr,$(EFI_PREFIX)),$(shell for dir in "$(EFI_PREFIX)/lib" "$(EFI_PREFIX)/lib64" /usr/lib /usr/lib64; do if [ -f "$$dir/crt0-efi-$(EFI_ARCH).o" ] || [ -f "$$dir/elf_$(EFI_ARCH)_efi.lds" ] || [ -f "$$dir/libefi.a" ]; then printf '%s' "$$dir"; exit; fi; done; printf '%s/lib' "$(EFI_PREFIX)"),$(EFI_PREFIX)/lib)
EFI_CRT0 ?= $(EFI_LIBDIR)/crt0-efi-$(EFI_ARCH).o
EFI_LDS ?= $(EFI_LIBDIR)/elf_$(EFI_ARCH)_efi.lds
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
DATA_SIZE_KB ?= 131072
NTFS_DRIVER ?=
BOOT_RES_WIDTH ?= 0
BOOT_RES_HEIGHT ?= 0
QEMU_VIDEO_WIDTH ?= $(if $(filter 0,$(BOOT_RES_WIDTH)),1280,$(BOOT_RES_WIDTH))
QEMU_VIDEO_HEIGHT ?= $(if $(filter 0,$(BOOT_RES_HEIGHT)),720,$(BOOT_RES_HEIGHT))
QEMU_VGAMEM_MB ?= 64
QEMU_VIDEO_ARGS ?= -vga none -device VGA,vgamem_mb=$(QEMU_VGAMEM_MB),xres=$(QEMU_VIDEO_WIDTH),yres=$(QEMU_VIDEO_HEIGHT),xmax=$(QEMU_VIDEO_WIDTH),ymax=$(QEMU_VIDEO_HEIGHT)

PROJECT_CFLAGS := -Iboot/shared -Ibuild
KERNEL_INC := -Ikernel/include
BEARSSL_INC := -Ithird_party/bearssl/inc -Ithird_party/bearssl/src
STB_INC := -Ithird_party/stb
EXPAT_INC := -Ibuild/third_party/expat -Ithird_party/expat/expat/lib
NETSURF_INC := \
	-Ibuild/third_party/netsurf/libdom/include \
	-Ithird_party/netsurf/src/libsvgtiny/include \
	-Ithird_party/netsurf/src/libwapcaplet/include \
	-Ithird_party/netsurf/src/libparserutils/include \
	-Ithird_party/netsurf/src/libhubbub/include \
	-Ithird_party/netsurf/src/libcss/include \
	-Ithird_party/netsurf/src/libdom/include \
	-Ithird_party/netsurf/src/libdom \
	-Ithird_party/netsurf/src/libdom/bindings
KERNEL_NO_SSE_CFLAGS := -mno-mmx -mno-sse -mno-sse2 -msoft-float
NETSURF_COMMON_CFLAGS = $(KERNEL_CFLAGS) $(KERNEL_NO_SSE_CFLAGS) -DNDEBUG -DWITHOUT_ICONV_FILTER -Wno-unused-parameter -Wno-unused-function
KERNEL_STACK_CFLAGS := -mstackrealign -mincoming-stack-boundary=3
NETSURF_STACK_CFLAGS := $(KERNEL_STACK_CFLAGS)
NETSURF_CORE_CFLAGS = $(KERNEL_CFLAGS) $(NETSURF_STACK_CFLAGS) -Ithird_party/netsurf/src/netsurf -Ithird_party/netsurf/src/netsurf/include -Ithird_party/netsurf/src/netsurf/content/handlers -Ithird_party/netsurf/src/netsurf/content/handlers/javascript/duktape -Ithird_party/netsurf/src/libnsutils/include -DNDEBUG -DWITHOUT_ICONV_FILTER -Wno-unused-parameter -Wno-unused-function -D_BSD_SOURCE -D_DEFAULT_SOURCE -D_GNU_SOURCE -D_ALIGNED= -DNETSURF_BUILTIN_LOG_FILTER=\"\" -DNETSURF_BUILTIN_VERBOSE_FILTER=\"\" -DNETSURF_HOMEPAGE=\"about:blank\"
NETSURF_PARSERUTILS_CFLAGS = $(NETSURF_COMMON_CFLAGS) -Ithird_party/netsurf/src/libparserutils/src
NETSURF_HUBBUB_CFLAGS = $(NETSURF_COMMON_CFLAGS) -D_BSD_SOURCE -D_DEFAULT_SOURCE -Ithird_party/netsurf/src/libhubbub/src
NETSURF_LIBCSS_CFLAGS = $(NETSURF_COMMON_CFLAGS) -D_GNU_SOURCE -D_ALIGNED= -Ithird_party/netsurf/src/libcss/src
NETSURF_LIBDOM_CFLAGS = $(NETSURF_COMMON_CFLAGS) -D_BSD_SOURCE -D_DEFAULT_SOURCE -Ithird_party/netsurf/src/libdom/src -Ithird_party/netsurf/src/libdom/bindings
NETSURF_LIBSVGTINY_CFLAGS = $(KERNEL_CFLAGS) $(NETSURF_STACK_CFLAGS) -DNDEBUG -DWITHOUT_ICONV_FILTER -Wno-unused-parameter -Wno-unused-function -D_BSD_SOURCE -D_DEFAULT_SOURCE -D_GNU_SOURCE -Ibuild/third_party/netsurf/libsvgtiny/src -Ithird_party/netsurf/src/libsvgtiny/src
NASMFLAGS := -Iboot/shared/ -Ikernel/include/
CFLAGS := $(PROJECT_CFLAGS) -I$(EFI_INC) -I$(EFI_INC)/$(EFI_ARCH) -fpic -ffreestanding -fno-stack-protector -fno-stack-check -fshort-wchar -mno-red-zone -Wall -Wextra -DEFI_FUNCTION_WRAPPER -DBOOT_RES_WIDTH=$(BOOT_RES_WIDTH) -DBOOT_RES_HEIGHT=$(BOOT_RES_HEIGHT)
KERNEL_CFLAGS := $(PROJECT_CFLAGS) $(KERNEL_INC) $(BEARSSL_INC) $(STB_INC) $(EXPAT_INC) $(NETSURF_INC) -DBR_USE_URANDOM=0 -DBR_USE_UNIX_TIME=0 -DBR_RDRAND=0 -DBR_AES_X86NI=0 -DBR_SSE2=0 -DBR_POWER8=0 -ffreestanding -fno-stack-protector -fno-stack-check -mno-red-zone -Wall -Wextra -std=c11
HOST_CFLAGS := -Iboot/shared $(KERNEL_INC) -Ikernel -std=c11 -Wall -Wextra -Wno-unused-function
KERNEL_CFLAGS_TAG := $(shell printf '%s' '$(CC) $(KERNEL_CFLAGS)' | $(SHA1SUM) | cut -c1-12)
NETSURF_PARSERUTILS_CFLAGS_TAG := $(shell printf '%s' '$(NETSURF_PARSERUTILS_CFLAGS)' | $(SHA1SUM) | cut -c1-12)
NETSURF_HUBBUB_CFLAGS_TAG := $(shell printf '%s' '$(NETSURF_HUBBUB_CFLAGS)' | $(SHA1SUM) | cut -c1-12)
NETSURF_LIBCSS_CFLAGS_TAG := $(shell printf '%s' '$(NETSURF_LIBCSS_CFLAGS)' | $(SHA1SUM) | cut -c1-12)
NETSURF_LIBDOM_CFLAGS_TAG := $(shell printf '%s' '$(NETSURF_LIBDOM_CFLAGS)' | $(SHA1SUM) | cut -c1-12)
NETSURF_COMMON_CFLAGS_TAG := $(shell printf '%s' '$(NETSURF_COMMON_CFLAGS)' | $(SHA1SUM) | cut -c1-12)
NETSURF_CORE_CFLAGS_TAG := $(shell printf '%s' '$(NETSURF_CORE_CFLAGS)' | $(SHA1SUM) | cut -c1-12)
BOOT_CFLAGS_TAG := $(shell printf '%s' '$(CC) $(CFLAGS)' | $(SHA1SUM) | cut -c1-12)
KERNEL_CFLAGS_STAMP := build/.kernel-cflags-$(KERNEL_CFLAGS_TAG)
NETSURF_PARSERUTILS_CFLAGS_STAMP := build/.netsurf-parserutils-cflags-$(NETSURF_PARSERUTILS_CFLAGS_TAG)
NETSURF_HUBBUB_CFLAGS_STAMP := build/.netsurf-hubbub-cflags-$(NETSURF_HUBBUB_CFLAGS_TAG)
NETSURF_LIBCSS_CFLAGS_STAMP := build/.netsurf-libcss-cflags-$(NETSURF_LIBCSS_CFLAGS_TAG)
NETSURF_LIBDOM_CFLAGS_STAMP := build/.netsurf-libdom-cflags-$(NETSURF_LIBDOM_CFLAGS_TAG)
NETSURF_COMMON_CFLAGS_STAMP := build/.netsurf-common-cflags-$(NETSURF_COMMON_CFLAGS_TAG)
NETSURF_CORE_CFLAGS_STAMP := build/.netsurf-core-cflags-$(NETSURF_CORE_CFLAGS_TAG)
BOOT_CFLAGS_STAMP := build/.boot-cflags-$(BOOT_CFLAGS_TAG)
LDFLAGS_EFI := -nostdlib -znocombreloc -T $(EFI_LDS) -shared -Bsymbolic -L$(EFI_LIBDIR) $(EFI_CRT0)
LDLIBS_EFI := -lefi -lgnuefi
OBJCOPY_EFI_FLAGS := -O efi-app-$(EFI_ARCH) -j .text -j .sdata -j .data -j .rodata -j .dynamic -j .dynsym -j .rela -j .rel -j .reloc
KERNEL_HEADERS := $(wildcard kernel/include/*.h) boot/shared/bootinfo.h
KERNEL_C_SOURCES := \
	kernel/core/main.c \
	kernel/core/kernel_exports.c \
	kernel/core/timer.c \
	kernel/core/clock.c \
	kernel/core/cpu.c \
	kernel/core/dma.c \
	kernel/core/kmem.c \
	kernel/core/js_runtime.c \
	kernel/core/webcompat.c \
	kernel/core/registry.c \
	kernel/core/libc.c \
	kernel/core/power.c \
	kernel/drivers/graphics.c \
	kernel/drivers/image.c \
	kernel/drivers/console.c \
	kernel/drivers/keyboard.c \
	kernel/drivers/mouse.c \
	kernel/drivers/pci.c \
	kernel/drivers/net.c \
	kernel/drivers/tls.c \
	kernel/drivers/tls_roots.c \
	kernel/drivers/e1000.c \
	kernel/drivers/ahci.c \
	kernel/drivers/usb.c \
	kernel/drivers/storage.c \
	kernel/fs/lainfs.c \
	kernel/ui/shell.c \
	kernel/ui/editor.c \
	kernel/ui/browser.c \
	kernel/ui/weblayout.c \
	kernel/ui/netsurf_port.c \
	kernel/ui/netsurf_frontend.c \
	kernel/ui/netsurf_core_bridge.c \
	kernel/ui/netsurf_js_duktape.c \
	kernel/ui/netsurf_browser.c \
	kernel/ui/desktop.c \
	kernel/z/assembler.c \
	kernel/z/zscript.c \
	kernel/z/zobject.c
KERNEL_ASM_SOURCES := \
	kernel/arch/x86_64/entry.asm \
	kernel/arch/x86_64/cpu_low.asm \
	kernel/arch/x86_64/interrupts.asm \
	kernel/arch/x86_64/setjmp.asm
BEARSSL_C_SOURCES_ALL := $(shell find third_party/bearssl/src -type f -name '*.c' | sort)
BEARSSL_C_SOURCES := $(filter-out %/rand/sysrng.c %x86ni%.c %pwr8%.c %sse2%.c %pclmul%.c,$(BEARSSL_C_SOURCES_ALL))
EXPAT_C_SOURCES := \
	third_party/expat/expat/lib/xmlparse.c \
	third_party/expat/expat/lib/xmlrole.c \
	third_party/expat/expat/lib/xmltok.c
NETSURF_PARSERUTILS_C_SOURCES := $(shell find third_party/netsurf/src/libparserutils/src -type f -name '*.c' | sort)
NETSURF_HUBBUB_C_SOURCES := $(filter-out %/autogenerated-element-type.c,$(shell find third_party/netsurf/src/libhubbub/src -type f -name '*.c' | sort))
NETSURF_LIBCSS_PARSE_PROPERTY_SOURCES := $(filter-out %/css_property_parser_gen.c,$(shell find third_party/netsurf/src/libcss/src/parse/properties -type f -name '*.c' | sort))
NETSURF_LIBCSS_SELECT_SOURCES := $(shell find third_party/netsurf/src/libcss/src/select -maxdepth 1 -type f -name '*.c' | sort)
NETSURF_LIBCSS_SELECT_PROPERTY_SOURCES := $(shell find third_party/netsurf/src/libcss/src/select/properties -type f -name '*.c' | sort)
NETSURF_LIBDOM_C_SOURCES := \
	$(shell find third_party/netsurf/src/libdom/src -type f -name '*.c' | sort) \
	third_party/netsurf/src/libdom/bindings/hubbub/parser.c \
	third_party/netsurf/src/libdom/bindings/xml/expat_xmlparser.c
NETSURF_LIBCSS_C_SOURCES := \
	third_party/netsurf/src/libcss/src/stylesheet.c \
	third_party/netsurf/src/libcss/src/charset/detect.c \
	third_party/netsurf/src/libcss/src/lex/lex.c \
	third_party/netsurf/src/libcss/src/parse/parse.c \
	third_party/netsurf/src/libcss/src/parse/language.c \
	third_party/netsurf/src/libcss/src/parse/important.c \
	third_party/netsurf/src/libcss/src/parse/propstrings.c \
	third_party/netsurf/src/libcss/src/parse/font_face.c \
	third_party/netsurf/src/libcss/src/parse/mq.c \
	$(NETSURF_LIBCSS_PARSE_PROPERTY_SOURCES) \
	$(NETSURF_LIBCSS_SELECT_SOURCES) \
	$(NETSURF_LIBCSS_SELECT_PROPERTY_SOURCES) \
	third_party/netsurf/src/libcss/src/utils/errors.c \
	third_party/netsurf/src/libcss/src/utils/utils.c
NETSURF_C_SOURCES := \
	$(NETSURF_PARSERUTILS_C_SOURCES) \
	third_party/netsurf/src/libwapcaplet/src/libwapcaplet.c \
	$(NETSURF_HUBBUB_C_SOURCES) \
	$(NETSURF_LIBCSS_C_SOURCES)
NETSURF_C_SOURCES += $(NETSURF_LIBDOM_C_SOURCES)
NETSURF_LIBSVGTINY_C_SOURCES := \
	third_party/netsurf/src/libsvgtiny/src/svgtiny.c \
	third_party/netsurf/src/libsvgtiny/src/svgtiny_gradient.c \
	third_party/netsurf/src/libsvgtiny/src/svgtiny_list.c \
	third_party/netsurf/src/libsvgtiny/src/svgtiny_parse.c \
	third_party/netsurf/src/libsvgtiny/src/svgtiny_path.c
NETSURF_NSUTILS_C_SOURCES := \
	third_party/netsurf/src/libnsutils/src/base64.c
NETSURF_CORE_UTILS_PROBE_C_SOURCES := \
	third_party/netsurf/src/netsurf/utils/bloom.c \
	third_party/netsurf/src/netsurf/utils/corestrings.c \
	third_party/netsurf/src/netsurf/utils/file.c \
	third_party/netsurf/src/netsurf/utils/filepath.c \
	third_party/netsurf/src/netsurf/utils/hashmap.c \
	third_party/netsurf/src/netsurf/utils/hashtable.c \
	third_party/netsurf/src/netsurf/utils/idna.c \
	third_party/netsurf/src/netsurf/utils/libdom.c \
	third_party/netsurf/src/netsurf/utils/log.c \
	third_party/netsurf/src/netsurf/utils/messages.c \
	third_party/netsurf/src/netsurf/utils/nscolour.c \
	third_party/netsurf/src/netsurf/utils/nsoption.c \
	third_party/netsurf/src/netsurf/utils/punycode.c \
	third_party/netsurf/src/netsurf/utils/ssl_certs.c \
	third_party/netsurf/src/netsurf/utils/talloc.c \
	third_party/netsurf/src/netsurf/utils/time.c \
	third_party/netsurf/src/netsurf/utils/url.c \
	third_party/netsurf/src/netsurf/utils/useragent.c \
	third_party/netsurf/src/netsurf/utils/utf8.c \
	third_party/netsurf/src/netsurf/utils/utils.c \
	third_party/netsurf/src/netsurf/utils/nsurl/nsurl.c \
	third_party/netsurf/src/netsurf/utils/nsurl/parse.c \
	third_party/netsurf/src/netsurf/utils/http/challenge.c \
	third_party/netsurf/src/netsurf/utils/http/generics.c \
	third_party/netsurf/src/netsurf/utils/http/primitives.c \
	third_party/netsurf/src/netsurf/utils/http/parameter.c \
	third_party/netsurf/src/netsurf/utils/http/cache-control.c \
	third_party/netsurf/src/netsurf/utils/http/content-disposition.c \
	third_party/netsurf/src/netsurf/utils/http/content-type.c \
	third_party/netsurf/src/netsurf/utils/http/strict-transport-security.c \
	third_party/netsurf/src/netsurf/utils/http/www-authenticate.c
NETSURF_CORE_CONTENT_PROBE_C_SOURCES := \
	third_party/netsurf/src/netsurf/content/content.c \
	third_party/netsurf/src/netsurf/content/content_factory.c \
	third_party/netsurf/src/netsurf/content/fetch.c \
	third_party/netsurf/src/netsurf/content/hlcache.c \
	third_party/netsurf/src/netsurf/content/llcache.c \
	third_party/netsurf/src/netsurf/content/mimesniff.c \
	third_party/netsurf/src/netsurf/content/textsearch.c \
	third_party/netsurf/src/netsurf/content/urldb.c \
	third_party/netsurf/src/netsurf/content/no_backing_store.c \
	third_party/netsurf/src/netsurf/content/fetchers/data.c \
	third_party/netsurf/src/netsurf/content/fetchers/resource.c \
	third_party/netsurf/src/netsurf/content/handlers/css/css.c \
	third_party/netsurf/src/netsurf/content/handlers/css/dump.c \
	third_party/netsurf/src/netsurf/content/handlers/css/internal.c \
	third_party/netsurf/src/netsurf/content/handlers/css/hints.c \
	third_party/netsurf/src/netsurf/content/handlers/css/select.c \
	third_party/netsurf/src/netsurf/content/handlers/image/image.c \
	third_party/netsurf/src/netsurf/content/handlers/image/image_cache.c \
	third_party/netsurf/src/netsurf/content/handlers/javascript/content.c \
	third_party/netsurf/src/netsurf/content/handlers/javascript/duktape/duktape.c \
	third_party/netsurf/src/netsurf/content/handlers/javascript/fetcher.c \
	third_party/netsurf/src/netsurf/content/handlers/text/textplain.c \
	third_party/netsurf/src/netsurf/content/handlers/html/box_construct.c \
	third_party/netsurf/src/netsurf/content/handlers/html/box_inspect.c \
	third_party/netsurf/src/netsurf/content/handlers/html/box_manipulate.c \
	third_party/netsurf/src/netsurf/content/handlers/html/box_normalise.c \
	third_party/netsurf/src/netsurf/content/handlers/html/box_special.c \
	third_party/netsurf/src/netsurf/content/handlers/html/box_textarea.c \
	third_party/netsurf/src/netsurf/content/handlers/html/css.c \
	third_party/netsurf/src/netsurf/content/handlers/html/css_fetcher.c \
	third_party/netsurf/src/netsurf/content/handlers/html/dom_event.c \
	third_party/netsurf/src/netsurf/content/handlers/html/font.c \
	third_party/netsurf/src/netsurf/content/handlers/html/form.c \
	third_party/netsurf/src/netsurf/content/handlers/html/forms.c \
	third_party/netsurf/src/netsurf/content/handlers/html/html.c \
	third_party/netsurf/src/netsurf/content/handlers/html/imagemap.c \
	third_party/netsurf/src/netsurf/content/handlers/html/interaction.c \
	third_party/netsurf/src/netsurf/content/handlers/html/layout.c \
	third_party/netsurf/src/netsurf/content/handlers/html/layout_flex.c \
	third_party/netsurf/src/netsurf/content/handlers/html/object.c \
	third_party/netsurf/src/netsurf/content/handlers/html/redraw.c \
	third_party/netsurf/src/netsurf/content/handlers/html/redraw_border.c \
	third_party/netsurf/src/netsurf/content/handlers/html/script.c \
	third_party/netsurf/src/netsurf/content/handlers/html/table.c \
	third_party/netsurf/src/netsurf/content/handlers/html/textselection.c
NETSURF_CORE_DESKTOP_PROBE_C_SOURCES := \
	third_party/netsurf/src/netsurf/desktop/bitmap.c \
	third_party/netsurf/src/netsurf/desktop/browser.c \
	third_party/netsurf/src/netsurf/desktop/browser_window.c \
	third_party/netsurf/src/netsurf/desktop/browser_history.c \
	third_party/netsurf/src/netsurf/desktop/cw_helper.c \
	third_party/netsurf/src/netsurf/desktop/download.c \
	third_party/netsurf/src/netsurf/desktop/frames.c \
	third_party/netsurf/src/netsurf/desktop/gui_factory.c \
	third_party/netsurf/src/netsurf/desktop/knockout.c \
	third_party/netsurf/src/netsurf/desktop/local_history.c \
	third_party/netsurf/src/netsurf/desktop/mouse.c \
	third_party/netsurf/src/netsurf/desktop/netsurf.c \
	third_party/netsurf/src/netsurf/desktop/plot_style.c \
	third_party/netsurf/src/netsurf/desktop/print.c \
	third_party/netsurf/src/netsurf/desktop/scrollbar.c \
	third_party/netsurf/src/netsurf/desktop/search.c \
	third_party/netsurf/src/netsurf/desktop/searchweb.c \
	third_party/netsurf/src/netsurf/desktop/selection.c \
	third_party/netsurf/src/netsurf/desktop/system_colour.c \
	third_party/netsurf/src/netsurf/desktop/textarea.c \
	third_party/netsurf/src/netsurf/desktop/textinput.c
NETSURF_CORE_PROBE_C_SOURCES := \
	$(NETSURF_CORE_UTILS_PROBE_C_SOURCES) \
	$(NETSURF_CORE_CONTENT_PROBE_C_SOURCES) \
	$(NETSURF_CORE_DESKTOP_PROBE_C_SOURCES)
NETSURF_CORE_KERNEL_C_SOURCES := \
	third_party/netsurf/src/netsurf/utils/bloom.c \
	third_party/netsurf/src/netsurf/utils/corestrings.c \
	third_party/netsurf/src/netsurf/utils/hashmap.c \
	third_party/netsurf/src/netsurf/utils/hashtable.c \
	third_party/netsurf/src/netsurf/utils/idna.c \
	third_party/netsurf/src/netsurf/utils/libdom.c \
	third_party/netsurf/src/netsurf/utils/log.c \
	third_party/netsurf/src/netsurf/utils/messages.c \
	third_party/netsurf/src/netsurf/utils/nscolour.c \
	third_party/netsurf/src/netsurf/utils/nsoption.c \
	third_party/netsurf/src/netsurf/utils/punycode.c \
	third_party/netsurf/src/netsurf/utils/talloc.c \
	third_party/netsurf/src/netsurf/utils/time.c \
	third_party/netsurf/src/netsurf/utils/url.c \
	third_party/netsurf/src/netsurf/utils/useragent.c \
	third_party/netsurf/src/netsurf/utils/utf8.c \
	third_party/netsurf/src/netsurf/utils/utils.c \
	third_party/netsurf/src/netsurf/utils/nsurl/nsurl.c \
	third_party/netsurf/src/netsurf/utils/nsurl/parse.c \
	third_party/netsurf/src/netsurf/utils/http/challenge.c \
	third_party/netsurf/src/netsurf/utils/http/generics.c \
	third_party/netsurf/src/netsurf/utils/http/primitives.c \
	third_party/netsurf/src/netsurf/utils/http/parameter.c \
	third_party/netsurf/src/netsurf/utils/http/cache-control.c \
	third_party/netsurf/src/netsurf/utils/http/content-disposition.c \
	third_party/netsurf/src/netsurf/utils/http/content-type.c \
	third_party/netsurf/src/netsurf/utils/http/strict-transport-security.c \
	third_party/netsurf/src/netsurf/utils/http/www-authenticate.c \
	third_party/netsurf/src/netsurf/content/content.c \
	third_party/netsurf/src/netsurf/content/content_factory.c \
	third_party/netsurf/src/netsurf/content/fetch.c \
	third_party/netsurf/src/netsurf/content/hlcache.c \
	third_party/netsurf/src/netsurf/content/llcache.c \
	third_party/netsurf/src/netsurf/content/mimesniff.c \
	third_party/netsurf/src/netsurf/content/textsearch.c \
	third_party/netsurf/src/netsurf/content/no_backing_store.c \
	third_party/netsurf/src/netsurf/content/fetchers/data.c \
	third_party/netsurf/src/netsurf/content/fetchers/resource.c \
	third_party/netsurf/src/netsurf/content/handlers/css/css.c \
	third_party/netsurf/src/netsurf/content/handlers/css/internal.c \
	third_party/netsurf/src/netsurf/content/handlers/css/hints.c \
	third_party/netsurf/src/netsurf/content/handlers/css/select.c \
	third_party/netsurf/src/netsurf/content/handlers/image/svg.c \
	third_party/netsurf/src/netsurf/content/handlers/javascript/content.c \
	third_party/netsurf/src/netsurf/content/handlers/javascript/duktape/duktape.c \
	third_party/netsurf/src/netsurf/content/handlers/javascript/fetcher.c \
	third_party/netsurf/src/netsurf/content/handlers/html/box_construct.c \
	third_party/netsurf/src/netsurf/content/handlers/html/box_inspect.c \
	third_party/netsurf/src/netsurf/content/handlers/html/box_manipulate.c \
	third_party/netsurf/src/netsurf/content/handlers/html/box_normalise.c \
	third_party/netsurf/src/netsurf/content/handlers/html/box_special.c \
	third_party/netsurf/src/netsurf/content/handlers/html/box_textarea.c \
	third_party/netsurf/src/netsurf/content/handlers/html/css.c \
	third_party/netsurf/src/netsurf/content/handlers/html/css_fetcher.c \
	third_party/netsurf/src/netsurf/content/handlers/html/dom_event.c \
	third_party/netsurf/src/netsurf/content/handlers/html/font.c \
	third_party/netsurf/src/netsurf/content/handlers/html/form.c \
	third_party/netsurf/src/netsurf/content/handlers/html/forms.c \
	third_party/netsurf/src/netsurf/content/handlers/html/html.c \
	third_party/netsurf/src/netsurf/content/handlers/html/imagemap.c \
	third_party/netsurf/src/netsurf/content/handlers/html/interaction.c \
	third_party/netsurf/src/netsurf/content/handlers/html/layout.c \
	third_party/netsurf/src/netsurf/content/handlers/html/layout_flex.c \
	third_party/netsurf/src/netsurf/content/handlers/html/object.c \
	third_party/netsurf/src/netsurf/content/handlers/html/redraw.c \
	third_party/netsurf/src/netsurf/content/handlers/html/redraw_border.c \
	third_party/netsurf/src/netsurf/content/handlers/html/script.c \
	third_party/netsurf/src/netsurf/content/handlers/html/table.c \
	third_party/netsurf/src/netsurf/content/handlers/html/textselection.c \
	third_party/netsurf/src/netsurf/desktop/bitmap.c \
	third_party/netsurf/src/netsurf/desktop/plot_style.c \
	third_party/netsurf/src/netsurf/desktop/scrollbar.c \
	third_party/netsurf/src/netsurf/desktop/system_colour.c \
	third_party/netsurf/src/netsurf/desktop/textarea.c
KERNEL_C_OBJECTS := $(patsubst kernel/%.c,build/kernel/%.o,$(KERNEL_C_SOURCES))
BEARSSL_C_OBJECTS := $(patsubst third_party/bearssl/src/%.c,build/third_party/bearssl/%.o,$(BEARSSL_C_SOURCES))
EXPAT_C_OBJECTS := $(patsubst third_party/expat/expat/lib/%.c,build/third_party/expat/%.o,$(EXPAT_C_SOURCES))
NETSURF_C_OBJECTS := $(patsubst third_party/netsurf/src/%.c,build/third_party/netsurf/%.o,$(NETSURF_C_SOURCES))
NETSURF_LIBSVGTINY_C_OBJECTS := $(patsubst third_party/netsurf/src/libsvgtiny/%.c,build/third_party/netsurf/libsvgtiny/%.o,$(NETSURF_LIBSVGTINY_C_SOURCES))
NETSURF_NSUTILS_C_OBJECTS := $(patsubst third_party/netsurf/src/libnsutils/src/%.c,build/third_party/netsurf/libnsutils/%.o,$(NETSURF_NSUTILS_C_SOURCES))
NETSURF_CORE_PROBE_C_OBJECTS := $(patsubst third_party/netsurf/src/netsurf/%.c,build/third_party/netsurf/netsurf-core-probe/%.o,$(NETSURF_CORE_PROBE_C_SOURCES))
NETSURF_CORE_C_OBJECTS := $(patsubst third_party/netsurf/src/netsurf/%.c,build/third_party/netsurf/netsurf-core/%.o,$(NETSURF_CORE_KERNEL_C_SOURCES))
KERNEL_ASM_OBJECTS := $(patsubst kernel/%.asm,build/kernel/%.o,$(KERNEL_ASM_SOURCES))
KERNEL_OBJECTS := $(KERNEL_ASM_OBJECTS) $(KERNEL_C_OBJECTS) $(BEARSSL_C_OBJECTS) $(EXPAT_C_OBJECTS) $(NETSURF_C_OBJECTS) $(NETSURF_LIBSVGTINY_C_OBJECTS) $(NETSURF_NSUTILS_C_OBJECTS) $(NETSURF_CORE_C_OBJECTS) build/kernel/z/zlink_probe.o

all: build/$(BOOTLOADER) build/$(KERNEL_BIN) build/$(KERNEL_ELF) image build/$(ESP_IMG) build/$(BOOTDISK_IMG) build/$(BOOTDISK_GPT_IMG) build/$(DATA_IMG) build/$(ISO_IMG)

build:
	$(MKDIR_P) build

FORCE:

check-efi-linker:
	@if ! $(LD) --version >/dev/null 2>&1; then \
		echo "GNU ld is required for the EFI/kernel link step; Apple ld cannot link this project." >&2; \
		echo "On macOS, install GNU binutils and rerun with LD=gld, or put gld ahead of ld in PATH." >&2; \
		exit 1; \
	fi

check-objcopy:
	@if ! $(OBJCOPY) --version >/dev/null 2>&1; then \
		echo "GNU objcopy is required for EFI/kernel images." >&2; \
		echo "On macOS, install GNU binutils and rerun with OBJCOPY=gobjcopy, or put gobjcopy ahead of objcopy in PATH." >&2; \
		exit 1; \
	fi

check-gnu-efi:
	@if [ ! -f "$(EFI_INC)/efi.h" ]; then \
		echo "GNU-EFI headers not found: $(EFI_INC)/efi.h" >&2; \
		echo "Install gnu-efi, or set EFI_INC to its include/efi directory." >&2; \
		exit 1; \
	fi
	@if [ ! -f "$(EFI_INC)/$(EFI_ARCH)/efibind.h" ]; then \
		echo "GNU-EFI architecture headers not found: $(EFI_INC)/$(EFI_ARCH)/efibind.h" >&2; \
		echo "Set EFI_INC to a GNU-EFI include/efi directory with an $(EFI_ARCH) subdirectory." >&2; \
		exit 1; \
	fi
	@if [ ! -f "$(EFI_CRT0)" ]; then \
		echo "GNU-EFI crt object not found: $(EFI_CRT0)" >&2; \
		echo "Install gnu-efi, or set EFI_CRT0 and EFI_LIBDIR." >&2; \
		exit 1; \
	fi
	@if [ ! -f "$(EFI_LDS)" ]; then \
		echo "GNU-EFI linker script not found: $(EFI_LDS)" >&2; \
		echo "Install gnu-efi, or set EFI_LDS and EFI_LIBDIR." >&2; \
		exit 1; \
	fi
	@if [ ! -f "$(EFI_LIBDIR)/libefi.a" ] || [ ! -f "$(EFI_LIBDIR)/libgnuefi.a" ]; then \
		echo "GNU-EFI static libraries not found in EFI_LIBDIR=$(EFI_LIBDIR)" >&2; \
		echo "Install gnu-efi, or set EFI_LIBDIR to the directory containing libefi.a and libgnuefi.a." >&2; \
		exit 1; \
	fi

$(KERNEL_CFLAGS_STAMP) $(NETSURF_PARSERUTILS_CFLAGS_STAMP) $(NETSURF_HUBBUB_CFLAGS_STAMP) $(NETSURF_LIBCSS_CFLAGS_STAMP) $(NETSURF_LIBDOM_CFLAGS_STAMP) $(NETSURF_COMMON_CFLAGS_STAMP) $(NETSURF_CORE_CFLAGS_STAMP) $(BOOT_CFLAGS_STAMP): | build
	@touch $@

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

build/bootloader/main.o: bootloader/main.c $(BOOT_CFLAGS_STAMP) check-gnu-efi | build
	$(MKDIR_P) build/bootloader
	$(CC) $(CFLAGS) -c $< -o $@

build/bootloader.so: build/bootloader/main.o check-efi-linker check-gnu-efi
	$(LD) $(LDFLAGS_EFI) -o $@ $< $(LDLIBS_EFI)

build/$(BOOTLOADER): build/bootloader.so check-objcopy
	$(OBJCOPY) $(OBJCOPY_EFI_FLAGS) $< $@

build/kernel_elf.o: build/$(KERNEL_ELF) check-efi-linker
	$(LD) -r -b binary -o $@ $<

build/bootloader-pxe/main.o: bootloader/main.c build/$(KERNEL_ELF) $(BOOT_CFLAGS_STAMP) check-gnu-efi | build
	$(MKDIR_P) build/bootloader-pxe
	$(CC) $(CFLAGS) -DEMBED_KERNEL -c bootloader/main.c -o $@

build/bootloader-pxe.so: build/bootloader-pxe/main.o build/kernel_elf.o check-efi-linker check-gnu-efi
	$(LD) $(LDFLAGS_EFI) -o $@ build/bootloader-pxe/main.o build/kernel_elf.o $(LDLIBS_EFI)

build/pxe/$(PXE_BOOTLOADER): build/bootloader-pxe.so check-objcopy
	$(MKDIR_P) $(PXE_DIR)
	$(OBJCOPY) $(OBJCOPY_EFI_FLAGS) $< $@

refresh-ramdisk: build/tools/ramdisk_seed_gen | build
	build/tools/ramdisk_seed_gen $(RAMDISK_SEED_H) $(RAMDISK_SEED_ARGS)

pxe: refresh-ramdisk
	$(MAKE) build/pxe/$(PXE_BOOTLOADER)

NETSURF_FRONTEND_CFLAGS := -Ithird_party/netsurf/src/netsurf -Ithird_party/netsurf/src/netsurf/include -Ithird_party/netsurf/src/netsurf/content/handlers -Ithird_party/netsurf/src/libnsutils/include -DNDEBUG -DWITHOUT_ICONV_FILTER -Wno-unused-parameter -Wno-unused-function -D_BSD_SOURCE -D_DEFAULT_SOURCE -D_GNU_SOURCE -D_ALIGNED=

build/kernel/ui/weblayout.o build/kernel/ui/netsurf_port.o: KERNEL_CFLAGS += $(KERNEL_NO_SSE_CFLAGS)
build/kernel/core/libc.o: KERNEL_CFLAGS += $(KERNEL_STACK_CFLAGS)
build/kernel/ui/netsurf_frontend.o: KERNEL_CFLAGS += $(NETSURF_STACK_CFLAGS) $(NETSURF_FRONTEND_CFLAGS)
build/kernel/ui/netsurf_frontend.o: $(NETSURF_CORE_CFLAGS_STAMP) $(KERNEL_FONT_TTF_H)
build/kernel/ui/netsurf_core_bridge.o: KERNEL_CFLAGS += $(NETSURF_STACK_CFLAGS) $(NETSURF_FRONTEND_CFLAGS)
build/kernel/ui/netsurf_core_bridge.o: $(NETSURF_CORE_CFLAGS_STAMP) $(NETSURF_RESOURCE_CSS_H)
build/kernel/ui/netsurf_js_duktape.o: KERNEL_CFLAGS += $(NETSURF_STACK_CFLAGS) $(NETSURF_FRONTEND_CFLAGS) -Ithird_party/netsurf/src/netsurf/content/handlers/javascript/duktape
build/kernel/ui/netsurf_js_duktape.o: $(NETSURF_CORE_CFLAGS_STAMP)
build/kernel/ui/netsurf_browser.o: KERNEL_CFLAGS += $(NETSURF_STACK_CFLAGS) $(NETSURF_FRONTEND_CFLAGS)
build/kernel/ui/netsurf_browser.o: $(NETSURF_CORE_CFLAGS_STAMP)

build/kernel/%.o: kernel/%.c $(KERNEL_HEADERS) $(BUILD_VERSION_H) $(RAMDISK_SEED_H) $(KERNEL_CFLAGS_STAMP) | build
	$(MKDIR_P) $(@D)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_FONT_TTF_H): $(KERNEL_FONT_TTF) | build
	{ \
		printf '#ifndef DEJAVU_SANS_TTF_H\n#define DEJAVU_SANS_TTF_H\n#include <stdint.h>\n'; \
		printf 'static const uint8_t dejavu_sans_ttf[] = {\n'; \
		od -An -v -tx1 $< | tr -s ' ' '\n' | sed '/^$$/d;s/^/0x/;s/$$/,/'; \
		printf '};\nstatic const uint32_t dejavu_sans_ttf_len = sizeof(dejavu_sans_ttf);\n#endif\n'; \
	} > $@

$(NETSURF_RESOURCE_CSS_H): $(NETSURF_DEFAULT_CSS) $(NETSURF_QUIRKS_CSS) | build
	{ \
		printf '#ifndef NETSURF_RESOURCE_CSS_H\n#define NETSURF_RESOURCE_CSS_H\n#include <stdint.h>\n'; \
		printf 'static const uint8_t netsurf_resource_default_css[] = {\n'; \
		od -An -v -tx1 $(NETSURF_DEFAULT_CSS) | tr -s ' ' '\n' | sed '/^$$/d;s/^/0x/;s/$$/,/'; \
		printf '};\nstatic const uint32_t netsurf_resource_default_css_len = sizeof(netsurf_resource_default_css);\n'; \
		printf 'static const uint8_t netsurf_resource_quirks_css[] = {\n'; \
		od -An -v -tx1 $(NETSURF_QUIRKS_CSS) | tr -s ' ' '\n' | sed '/^$$/d;s/^/0x/;s/$$/,/'; \
		printf '};\nstatic const uint32_t netsurf_resource_quirks_css_len = sizeof(netsurf_resource_quirks_css);\n#endif\n'; \
	} > $@

build/third_party/bearssl/%.o: third_party/bearssl/src/%.c | build
	$(MKDIR_P) $(@D)
	$(CC) $(KERNEL_CFLAGS) -Wno-unused-parameter -Wno-unused-function -c $< -o $@

$(EXPAT_CONFIG_H): Makefile | build
	$(MKDIR_P) $(@D)
	{ \
		printf '#ifndef EXPAT_CONFIG_H\n#define EXPAT_CONFIG_H 1\n\n'; \
		printf '#define BYTEORDER 1234\n'; \
		printf '#define HAVE_INTTYPES_H 1\n'; \
		printf '#define HAVE_STDINT_H 1\n'; \
		printf '#define HAVE_STDLIB_H 1\n'; \
		printf '#define HAVE_STRING_H 1\n'; \
		printf '#define PACKAGE "expat"\n'; \
		printf '#define PACKAGE_NAME "expat"\n'; \
		printf '#define PACKAGE_VERSION "2"\n'; \
		printf '#define STDC_HEADERS 1\n'; \
		printf '#define XML_CONTEXT_BYTES 0\n'; \
		printf '#define XML_GE 0\n'; \
		printf '#define XML_NS 1\n\n'; \
		printf '#endif\n'; \
	} > $@

$(NETSURF_LIBDOM_XML_HEADER): Makefile | build
	$(MKDIR_P) $(@D)
	{ \
		printf '#ifndef dom_bindings_xml_xmlparser_public_h_\n'; \
		printf '#define dom_bindings_xml_xmlparser_public_h_\n\n'; \
		printf '#include "../../../../../../../../third_party/netsurf/src/libdom/bindings/xml/xmlparser.h"\n\n'; \
		printf '#endif\n'; \
	} > $@

build/third_party/expat/%.o: third_party/expat/expat/lib/%.c $(EXPAT_CONFIG_H) | build
	$(MKDIR_P) $(@D)
	$(CC) $(KERNEL_CFLAGS) $(KERNEL_NO_SSE_CFLAGS) -DXML_POOR_ENTROPY -Wno-unused-parameter -Wno-unused-function -c $< -o $@

build/third_party/netsurf/libsvgtiny/src/autogenerated_colors.c: third_party/netsurf/src/libsvgtiny/src/colors.gperf Makefile | build
	$(MKDIR_P) $(@D)
	{ \
		printf 'static const struct svgtiny_named_color svgtiny_named_colors[] = {\n'; \
		awk 'BEGIN { emit = 0 } /^%%/ { emit = 1; next } emit && NF > 0 { split($$0, fields, ","); name = fields[1]; sub(/[ \t]+$$/, "", name); value = substr($$0, index($$0, ",") + 1); sub(/^[ \t]+/, "", value); printf "    { \"%s\", %s },\n", name, value }' $<; \
		printf '};\n'; \
		printf 'static const struct svgtiny_named_color *svgtiny_color_lookup(const char *str, size_t len) {\n'; \
		printf '    for (size_t i = 0; i < sizeof(svgtiny_named_colors) / sizeof(svgtiny_named_colors[0]); ++i) {\n'; \
		printf '        const char *name = svgtiny_named_colors[i].name;\n'; \
		printf '        if (strlen(name) == len && memcmp(name, str, len) == 0) return &svgtiny_named_colors[i];\n'; \
		printf '    }\n'; \
		printf '    return 0;\n'; \
		printf '}\n'; \
	} > $@

build/third_party/netsurf/libsvgtiny/src/svgtiny_parse.o: build/third_party/netsurf/libsvgtiny/src/autogenerated_colors.c

build/third_party/netsurf/libsvgtiny/%.o: third_party/netsurf/src/libsvgtiny/%.c $(NETSURF_COMMON_CFLAGS_STAMP) $(NETSURF_LIBDOM_XML_HEADER) | build
	$(MKDIR_P) $(@D)
	$(CC) $(NETSURF_LIBSVGTINY_CFLAGS) -c $< -o $@

build/third_party/netsurf/libparserutils/%.o: third_party/netsurf/src/libparserutils/%.c $(NETSURF_PARSERUTILS_CFLAGS_STAMP) | build
	$(MKDIR_P) $(@D)
	$(CC) $(NETSURF_PARSERUTILS_CFLAGS) -c $< -o $@

build/third_party/netsurf/libcss/%.o: third_party/netsurf/src/libcss/%.c $(NETSURF_LIBCSS_CFLAGS_STAMP) | build
	$(MKDIR_P) $(@D)
	$(CC) $(NETSURF_LIBCSS_CFLAGS) -c $< -o $@

build/third_party/netsurf/libhubbub/%.o: third_party/netsurf/src/libhubbub/%.c $(NETSURF_HUBBUB_CFLAGS_STAMP) | build
	$(MKDIR_P) $(@D)
	$(CC) $(NETSURF_HUBBUB_CFLAGS) -c $< -o $@

build/third_party/netsurf/libdom/%.o: third_party/netsurf/src/libdom/%.c $(NETSURF_LIBDOM_CFLAGS_STAMP) | build
	$(MKDIR_P) $(@D)
	$(CC) $(NETSURF_LIBDOM_CFLAGS) -c $< -o $@

build/third_party/netsurf/libdom/bindings/xml/expat_xmlparser.o: $(NETSURF_LIBDOM_XML_HEADER)

build/third_party/netsurf/libwapcaplet/%.o: third_party/netsurf/src/libwapcaplet/%.c $(NETSURF_COMMON_CFLAGS_STAMP) | build
	$(MKDIR_P) $(@D)
	$(CC) $(NETSURF_COMMON_CFLAGS) -c $< -o $@

build/third_party/netsurf/netsurf-core-probe/%.o: third_party/netsurf/src/netsurf/%.c $(NETSURF_CORE_CFLAGS_STAMP) | build
	$(MKDIR_P) $(@D)
	$(CC) $(NETSURF_CORE_CFLAGS) -c $< -o $@

build/third_party/netsurf/netsurf-core/%.o: third_party/netsurf/src/netsurf/%.c $(NETSURF_CORE_CFLAGS_STAMP) | build
	$(MKDIR_P) $(@D)
	$(CC) $(NETSURF_CORE_CFLAGS) -c $< -o $@

build/third_party/netsurf/libnsutils/%.o: third_party/netsurf/src/libnsutils/src/%.c $(NETSURF_CORE_CFLAGS_STAMP) | build
	$(MKDIR_P) $(@D)
	$(CC) $(NETSURF_CORE_CFLAGS) -c $< -o $@

build/kernel/%.o: kernel/%.asm | build
	$(MKDIR_P) $(@D)
	$(NASM) $(NASMFLAGS) -f elf64 $< -o $@

build/tools/zcc_host: tools/zcc_host.c kernel/z/zscript.c kernel/include/zscript.h | build
	$(MKDIR_P) build/tools
	$(HOST_CC) $(HOST_CFLAGS) tools/zcc_host.c kernel/z/zscript.c -o $@

build/tools/zmod_link_host: tools/zmod_link_host.c kernel/z/zscript.c kernel/z/assembler.c kernel/z/zobject.c kernel/include/zscript.h kernel/include/assembler.h kernel/include/zobject.h | build
	$(MKDIR_P) build/tools
	$(HOST_CC) $(HOST_CFLAGS) tools/zmod_link_host.c kernel/z/zscript.c kernel/z/assembler.c kernel/z/zobject.c -o $@

zcc-smoke: build/tools/zcc_host | build
	$(MKDIR_P) build/zcc-smoke
	@set -e; \
	for f in examples/*.Z examples/zlang/*.Z; do \
		case "$$f" in \
			examples/kernel_api.Z|examples/jpg_decoder_api.Z|examples/libc_api.Z|examples/mini_zlib_api.Z|examples/zbrowser_html_api.Z|examples/zlang/kernel_api.Z|examples/zlang/mouse_api.Z|examples/zlang/header_controls.Z) continue ;; \
		esac; \
		out="build/zcc-smoke/$$(basename "$$f" .Z).asm"; \
		printf 'ZCC %s\n' "$$f"; \
		build/tools/zcc_host "$$f" "$$out"; \
	done

build/tools/lainfs_seed: tools/lainfs_seed.c | build
	$(MKDIR_P) build/tools
	$(HOST_CC) $(HOST_CFLAGS) tools/lainfs_seed.c -o $@

build/tools/lainfs_check_host: tools/lainfs_check_host.c | build
	$(MKDIR_P) build/tools
	$(HOST_CC) $(HOST_CFLAGS) tools/lainfs_check_host.c -o $@

lainfs-smoke: build/tools/lainfs_check_host | build
	build/tools/lainfs_check_host smoke

netsurf-core-probe: $(NETSURF_CORE_PROBE_C_OBJECTS)

build/tools/ramdisk_seed_gen: tools/ramdisk_seed_gen.c | build
	$(MKDIR_P) build/tools
	$(HOST_CC) $(HOST_CFLAGS) tools/ramdisk_seed_gen.c -o $@

$(RAMDISK_SEED_H): build/tools/ramdisk_seed_gen $(RAMDISK_SEED_FILES) | build
	build/tools/ramdisk_seed_gen $@ $(RAMDISK_SEED_ARGS)

build/kernel/z/zlink_probe.asm: kernel/z/zlink_probe.Z build/tools/zcc_host | build
	$(MKDIR_P) $(@D)
	build/tools/zcc_host $< $@
	printf '\nsection .note.GNU-stack noalloc noexec nowrite progbits\n' >> $@

build/kernel/z/zlink_probe.o: build/kernel/z/zlink_probe.asm | build
	$(MKDIR_P) $(@D)
	$(NASM) $(NASMFLAGS) -f elf64 $< -o $@

build/$(KERNEL_ELF): $(KERNEL_OBJECTS) kernel/linker.ld check-efi-linker
	$(LD) -nostdlib -z max-page-size=0x1000 -T kernel/linker.ld -o build/$(KERNEL_ELF) $(KERNEL_OBJECTS)
# 	@bss=$$(size build/$(KERNEL_ELF) | awk 'NR == 2 { print $$3 }'); \
# 	if [ "$$bss" -gt 8388608 ]; then \
# 		echo "kernel bss is too large for reliable ISO boot: $$bss bytes" >&2; \
# 		exit 1; \
# 	fi

build/$(KERNEL_BIN): build/$(KERNEL_ELF) check-objcopy
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
			-eltorito-platform efi -eltorito-alt-boot -e $(ISO_BOOT_IMG) -no-emul-boot \
			-isohybrid-gpt-basdat \
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

run:
	$(MAKE) BOOT_RES_WIDTH=$(QEMU_VIDEO_WIDTH) BOOT_RES_HEIGHT=$(QEMU_VIDEO_HEIGHT) all
	qemu-system-x86_64 \
		-m 256M \
		$(QEMU_VIDEO_ARGS) \
		-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
		-drive if=pflash,format=raw,file=build/OVMF_VARS.iso.fd \
		-drive format=raw,file=build/$(ESP_IMG),if=ide,index=0 \
		-drive format=raw,file=build/$(DATA_IMG),if=ide,index=1 
run-ahci:
	$(MAKE) BOOT_RES_WIDTH=$(QEMU_VIDEO_WIDTH) BOOT_RES_HEIGHT=$(QEMU_VIDEO_HEIGHT) all
	qemu-system-x86_64 \
		-enable-kvm \
		-m 256M \
		$(QEMU_VIDEO_ARGS) \
		-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
		-drive if=pflash,format=raw,file=/usr/share/OVMF/OVMF_VARS_4M.fd \
		-drive format=raw,file=build/$(ESP_IMG),if=ide,index=0 \
		-device ich9-ahci,id=ahci \
		-drive if=none,id=data,format=raw,file=build/$(DATA_IMG) \
		-device ide-hd,drive=data,bus=ahci.0

run-usb:
	$(MAKE) BOOT_RES_WIDTH=$(QEMU_VIDEO_WIDTH) BOOT_RES_HEIGHT=$(QEMU_VIDEO_HEIGHT) all
	qemu-system-x86_64 \
		-enable-kvm \
		-m 256M \
		$(QEMU_VIDEO_ARGS) \
		-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
		-drive if=pflash,format=raw,file=/usr/share/OVMF/OVMF_VARS_4M.fd \
		-drive format=raw,file=build/$(ESP_IMG),if=ide,index=0 \
		-drive format=raw,file=build/$(DATA_IMG),if=ide,index=1 \
		-device qemu-xhci,id=xhci \
		-device usb-kbd,bus=xhci.0 \
		-device usb-mouse,bus=xhci.0

run-net:
	$(MAKE) BOOT_RES_WIDTH=$(QEMU_VIDEO_WIDTH) BOOT_RES_HEIGHT=$(QEMU_VIDEO_HEIGHT) all
	qemu-system-x86_64 \
		-enable-kvm \
		-m 256M \
		$(QEMU_VIDEO_ARGS) \
		-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
		-drive if=pflash,format=raw,file=/usr/share/OVMF/OVMF_VARS_4M.fd \
		-drive format=raw,file=build/$(ESP_IMG),if=ide,index=0 \
		-drive format=raw,file=build/$(DATA_IMG),if=ide,index=1 \
		-netdev user,id=net0 \
		-device e1000,netdev=net0

run-bootdisk:
	$(MAKE) BOOT_RES_WIDTH=$(QEMU_VIDEO_WIDTH) BOOT_RES_HEIGHT=$(QEMU_VIDEO_HEIGHT) all
	qemu-system-x86_64 \
		-enable-kvm \
		-m 256M \
		$(QEMU_VIDEO_ARGS) \
		-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
		-drive if=pflash,format=raw,file=/usr/share/OVMF/OVMF_VARS_4M.fd \
		-drive format=raw,file=build/$(BOOTDISK_IMG),if=ide,index=0 \
		-device ich9-ahci,id=ahci \
		-drive if=none,id=data,format=raw,file=build/$(DATA_IMG) \
		-device ide-hd,drive=data,bus=ahci.0

run-bootdisk-gpt:
	$(MAKE) BOOT_RES_WIDTH=$(QEMU_VIDEO_WIDTH) BOOT_RES_HEIGHT=$(QEMU_VIDEO_HEIGHT) all
	qemu-system-x86_64 \
		-enable-kvm \
		-m 256M \
		$(QEMU_VIDEO_ARGS) \
		-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
		-drive if=pflash,format=raw,file=/usr/share/OVMF/OVMF_VARS_4M.fd \
		-drive format=raw,file=build/$(BOOTDISK_GPT_IMG),if=ide,index=0 \
		-device ich9-ahci,id=ahci \
		-drive if=none,id=data,format=raw,file=build/$(DATA_IMG) \
		-device ide-hd,drive=data,bus=ahci.0

run-iso:
	$(MAKE) BOOT_RES_WIDTH=$(QEMU_VIDEO_WIDTH) BOOT_RES_HEIGHT=$(QEMU_VIDEO_HEIGHT) build/$(ISO_IMG) build/$(DATA_IMG)
	cp /usr/share/OVMF/OVMF_VARS_4M.fd build/OVMF_VARS.iso.fd
	qemu-system-x86_64 \
		-enable-kvm \
		-smp 8 \
		-m 8G \
		-boot d \
		$(QEMU_VIDEO_ARGS) \
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
	@echo HOST_CC=$(HOST_CC)
	@echo CC=$(CC)
	@echo LD=$(LD)
	@echo OBJCOPY=$(OBJCOPY)
	@echo EFI_PREFIX=$(EFI_PREFIX)
	@echo EFI_INC=$(EFI_INC)
	@echo EFI_LIBDIR=$(EFI_LIBDIR)
	@echo EFI_CRT0=$(EFI_CRT0)
	@echo EFI_LDS=$(EFI_LDS)
	@echo EFI_ARCH=$(EFI_ARCH)

.PHONY: all build image refresh-ramdisk pxe run run-ahci run-usb run-net run-bootdisk run-bootdisk-gpt run-iso reseed-data zcc-smoke lainfs-smoke netsurf-core-probe clean inspect-efi print-efi-config check-efi-linker check-objcopy check-gnu-efi FORCE
