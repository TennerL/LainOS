#!/bin/sh
set -eu

clear 2>/dev/null || true

FB_WIDTH=${FB_WIDTH:-${BOOT_RES_WIDTH:-1280}}
FB_HEIGHT=${FB_HEIGHT:-${BOOT_RES_HEIGHT:-720}}
QEMU_VGAMEM_MB=${QEMU_VGAMEM_MB:-512}
QEMU_MEMORY=${QEMU_MEMORY:-8096M}
QEMU_SMP=${QEMU_SMP:-8}
QEMU=${QEMU:-qemu-system-x86_64}
QEMU_ACCEL=${QEMU_ACCEL:-auto}
RUN_BUILD=${RUN_BUILD:-auto}
DRY_RUN=${DRY_RUN:-0}
HOST_OS=$(uname -s 2>/dev/null || echo unknown)
HOST_ARCH=$(uname -m 2>/dev/null || echo unknown)

case "$FB_WIDTH:$FB_HEIGHT:$QEMU_VGAMEM_MB" in
  *[!0-9:]*|:*|*:|*::*)
    echo "run.sh: FB_WIDTH, FB_HEIGHT, and QEMU_VGAMEM_MB must be positive integers" >&2
    exit 2
    ;;
esac

if [ "$FB_WIDTH" -eq 0 ] || [ "$FB_HEIGHT" -eq 0 ] || [ "$QEMU_VGAMEM_MB" -eq 0 ]; then
  echo "run.sh: FB_WIDTH, FB_HEIGHT, and QEMU_VGAMEM_MB must be positive integers" >&2
  exit 2
fi

bootres_stamp="build/.run-bootres-${FB_WIDTH}x${FB_HEIGHT}"
needs_build=0
if [ ! -f build/boot.iso ] || [ ! -f build/data.img ] || [ ! -f "$bootres_stamp" ]; then
  needs_build=1
fi

if [ "$RUN_BUILD" = auto ]; then
  case "$HOST_OS" in
    Darwin) RUN_BUILD=0 ;;
    *) RUN_BUILD=$needs_build ;;
  esac
fi

if [ "$RUN_BUILD" = 1 ]; then
  make BOOT_RES_WIDTH="$FB_WIDTH" BOOT_RES_HEIGHT="$FB_HEIGHT" all
  rm -f build/.run-bootres-*
  : > "$bootres_stamp"
elif [ ! -f build/boot.iso ] || [ ! -f build/data.img ]; then
  echo "run.sh: missing build/boot.iso or build/data.img" >&2
  echo "run.sh: build them in the Linux container first, or rerun with RUN_BUILD=1." >&2
  exit 1
elif [ "$needs_build" -eq 1 ]; then
  echo "run.sh: using existing build artifacts without rebuilding on $HOST_OS." >&2
  echo "run.sh: for an exact ${FB_WIDTH}x${FB_HEIGHT} GOP request, rebuild in the Linux container with:" >&2
  echo "        make BOOT_RES_WIDTH=$FB_WIDTH BOOT_RES_HEIGHT=$FB_HEIGHT all" >&2
fi

OVMF_CODE=${OVMF_CODE:-./OVMF_CODE.fd}
OVMF_VARS_TEMPLATE=${OVMF_VARS_TEMPLATE:-./OVMF_VARS.fd}
QEMU_ACCEL_ARGS=

if [ "$QEMU_ACCEL" = auto ]; then
  case "$HOST_OS:$HOST_ARCH" in
    Darwin:x86_64|Darwin:amd64)
      QEMU_ACCEL_ARGS="-accel hvf -cpu host"
      ;;
    Darwin:arm64|Darwin:aarch64)
      QEMU_ACCEL_ARGS="-accel tcg,thread=multi -cpu max"
      echo "run.sh: Apple Silicon cannot hardware-accelerate an x86_64 guest with qemu-system-x86_64." >&2
      echo "run.sh: using TCG; this will be much slower than a real PC, especially at high framebuffer resolutions." >&2
      ;;
    Linux:*)
      if [ -e /dev/kvm ] && [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
        QEMU_ACCEL_ARGS="-accel kvm -cpu host"
      else
        QEMU_ACCEL_ARGS="-accel tcg,thread=multi -cpu max"
        echo "run.sh: /dev/kvm is not usable; using slower TCG emulation." >&2
      fi
      ;;
    *)
      QEMU_ACCEL_ARGS="-accel tcg,thread=multi -cpu max"
      ;;
  esac
elif [ "$QEMU_ACCEL" != none ]; then
  case "$QEMU_ACCEL" in
    hvf|kvm) QEMU_ACCEL_ARGS="-accel $QEMU_ACCEL -cpu host" ;;
    tcg) QEMU_ACCEL_ARGS="-accel tcg,thread=multi -cpu max" ;;
    *) QEMU_ACCEL_ARGS="$QEMU_ACCEL" ;;
  esac
fi

mkdir -p build
cp "$OVMF_VARS_TEMPLATE" build/OVMF_VARS.run.fd

if [ "$DRY_RUN" = 1 ]; then
  printf '%s\n' \
    "$QEMU \\" \
    "  $QEMU_ACCEL_ARGS \\" \
    "  -m \"$QEMU_MEMORY\" \\" \
    "  -smp \"$QEMU_SMP\" \\" \
    "  -boot order=d,menu=on \\" \
    "  -vga none \\" \
    "  -device VGA,vgamem_mb=\"$QEMU_VGAMEM_MB\",xres=\"$FB_WIDTH\",yres=\"$FB_HEIGHT\",xmax=\"$FB_WIDTH\",ymax=\"$FB_HEIGHT\" \\" \
    "  -drive if=pflash,format=raw,readonly=on,file=\"$OVMF_CODE\" \\" \
    "  -drive if=pflash,format=raw,file=build/OVMF_VARS.run.fd \\" \
    "  -cdrom build/boot.iso \\" \
    "  -drive format=raw,file=build/data.img,if=ide,index=1 \\" \
    "  -netdev user,id=net0 \\" \
    "  -device e1000,netdev=net0"
  exit 0
fi

$QEMU \
  -m "$QEMU_MEMORY" \
  -smp "$QEMU_SMP" \
  -boot order=d,menu=on \
  -vga none \
  -device VGA,vgamem_mb="$QEMU_VGAMEM_MB",xres="$FB_WIDTH",yres="$FB_HEIGHT",xmax="$FB_WIDTH",ymax="$FB_HEIGHT" \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file=build/OVMF_VARS.run.fd \
  -cdrom build/boot.iso \
  -drive format=raw,file=build/data.img,if=ide,index=1 \
  -netdev user,id=net0 \
  -device e1000,netdev=net0
