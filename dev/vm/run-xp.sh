#!/bin/bash
# -cdrom /home/peng/Downloads/en_windows_xp_professional_with_service_pack_3_x86_cd_vl_x14-73974.iso \
current_dir=$(dirname $(realpath $0))
cmd=(
  qemu-system-i386
  -machine pc,accel=kvm
  -m 1G
  -drive "file=$current_dir/xp.qcow2,if=ide"
  -boot c
  -vga cirrus
  -rtc base=localtime
  -usb -device usb-tablet
  -display none
  -vnc 0.0.0.0:0,audiodev=snd0
  -device virtio-serial-pci,disable-modern=on
  -chardev spicevmc,id=vdagent,name=vdagent
  -device virtserialport,chardev=vdagent,name=com.redhat.spice.0
  -audiodev spice,id=snd0
  -device AC97,audiodev=snd0
  -spice addr=0.0.0.0,port=5930,disable-ticketing=on
)

# 内存共享
cmd+=(
  -object memory-backend-file,id=hostmem,share=on,mem-path=/dev/shm/ivshmem,size=16M
  -device ivshmem-plain,memdev=hostmem
)

# 光驱
cmd+=(
  -drive if=ide,index=1,media=cdrom,id=cd0
  -monitor telnet:127.0.0.1:4444,server,nowait
)

# -cdrom /home/sucicada/Downloads/en_windows_xp_professional_with_service_pack_3_x86_cd_vl_x14-73974.iso \
cmd+=(
-cdrom $current_dir/GRMWDK_EN_7600_1.ISO 
)

# 转发 telnet。宿主机连 127.0.0.1:2323，转到 XP 的 23 端口
cmd+=(
  -nic user,model=rtl8139,hostfwd=tcp:0.0.0.0:2323-:23
)

echo "${cmd[@]}"«
echo
exec "${cmd[@]}"

# -device AC97,audiodev=audio0
# -audiodev pa,id=audio0
