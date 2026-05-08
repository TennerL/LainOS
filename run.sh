clear
qemu-system-x86_64 \
  -m 256M \
  -smp 5 \
  -drive if=pflash,format=raw,readonly=on,file=./OVMF_CODE.fd \
  -drive if=pflash,format=raw,file=./OVMF_VARS.fd \
  -cdrom build/boot.iso \
  -drive format=raw,file=build/data.img,if=ide,index=1
