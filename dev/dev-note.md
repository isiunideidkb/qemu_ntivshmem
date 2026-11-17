project dependency:

- mingw13

---

# 1. install mingw13

## At ubuntu 22.04

if your pc is ubuntu 22.04, not support mingw13, so i need to use docker to build the mingw13 environment.

```bash
docker build -t mingw13 - <<'EOF'
FROM ubuntu:26.04
RUN apt-get update && apt-get install -y gcc-mingw-w64-i686 && rm -rf /var/lib/apt/lists/*
EOF


# add to ~/.zshrc (use a function, not an alias: docker has no TTY, so gcc
# disables colored diagnostics unless we force it when stderr is a terminal)
i686-w64-mingw32-gcc() {
  local color=()
  [[ -t 2 ]] && color=(-fdiagnostics-color=always)
  docker run --rm -u $(id -u):$(id -g) -v "$PWD":"$PWD" -w "$PWD" \
    mingw13 i686-w64-mingw32-gcc "${color[@]}" "$@"
}

source ~/.zshrc

i686-w64-mingw32-gcc --version        # should show 13-win32

```

## At manjaro

```bash
yay -S mingw-w64-gcc

i686-w64-mingw32-gcc --version        # should show 16.2.0

```

# 2. mingw compile  cusdriver_2.c  ->  cusdriver_2.obj

```bash
cd src/guest/i386nt/driver
# -std=gnu2x: code needs C23 (bool keyword, 0b0000'0100, [[gnu::...]]); GCC 13 defaults to gnu17 and calls C23 "2x"
i686-w64-mingw32-gcc -std=gnu2x -c cusdriver_2.c -o cusdriver_2.obj
ls -l cusdriver_2.obj

```

# 3. install xp

download from [https://massgrave.dev/windows_xp_links](https://massgrave.dev/windows_xp_links)

eg:  xp iso  at :  
/home/peng/Downloads/en_windows_xp_professional_with_service_pack_3_x86_cd_vl_x14-73974.iso  
XCYBK-2B3KV-G8T8F-WXJM7-WCTYT

```bash
mkdir vm && cd vm

# 1. Create a virtual disk (10GB is plenty; qcow2 grows as it's used, so it starts at a few hundred KB)
qemu-img create -f qcow2 xp.qcow2 10G

Boot from the ISO and install


qemu-system-i386 -machine pc,accel=kvm -m 1G \
  -drive file=xp.qcow2,if=ide \
  -cdrom /home/peng/Downloads/en_windows_xp_professional_with_service_pack_3_x86_cd_vl_x14-73974.iso \
  -boot d \
  -vga cirrus \
  -nic user,model=rtl8139 \
  -rtc base=localtime \
  -usb -device usb-tablet \
 -display none -vnc 0.0.0.0:0 \
-device AC97,audiodev=audio0 \
-spice addr=0.0.0.0,port=5930,disable-ticketing=on
#  -audiodev pa,id=audio0 \

# install xp
pipx install vncdotool
for i in {1..25}; do    # press bsp 25 times  
vncdo -s localhost::5900 key bsp
done
vncdo -s localhost::5900 type XCYBK2B3KVG8T8FWXJM7WCTYT
```

What each parameter does:

| Parameter             | Why                                                                                                                     |
| --------------------- | ----------------------------------------------------------------------------------------------------------------------- |
| `if=ide`              | XP doesn't recognize newer disk controllers, but it does recognize IDE. Otherwise setup says it can't find a hard disk. |
| `-boot d`             | Boot from the CD                                                                                                        |
| `-vga cirrus`         | XP ships with a driver for this graphics card, so you get normal resolution right after install                         |
| `model=rtl8139`       | XP also ships with a driver for this network card                                                                       |
| `-rtc base=localtime` | Windows assumes the hardware clock is local time                                                                        |
| `usb-tablet`          | Keeps the mouse in sync so it doesn't drift                                                                             |

vncdo -s [localhost](http://localhost)::5900 type 'XXXXX-XXXXX-XXXXX-XXXXX-XXXXX'