clear
qemu-system-x86_64 \
  -m 256M \
  -drive if=pflash,format=raw,readonly=on,file=./OVMF_CODE.fd \
  -drive if=pflash,format=raw,file=./OVMF_VARS.fd \
  -drive format=raw,file=build/esp.img,if=ide,index=0 \
  -drive format=raw,file=build/data.img,if=ide,index=1
