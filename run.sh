#!/bin/sh
set -eu

clear 2>/dev/null || true

if [ ! -f build/boot.iso ] || [ ! -f build/data.img ]; then
  make all
fi

OVMF_CODE=${OVMF_CODE:-./OVMF_CODE.fd}
OVMF_VARS_TEMPLATE=${OVMF_VARS_TEMPLATE:-./OVMF_VARS.fd}

mkdir -p build
cp "$OVMF_VARS_TEMPLATE" build/OVMF_VARS.run.fd

qemu-system-x86_64 \
  -m 256M \
  -smp 4 \
  -boot order=d,menu=on \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file=build/OVMF_VARS.run.fd \
  -cdrom build/boot.iso \
  -drive format=raw,file=build/data.img,if=ide,index=1
