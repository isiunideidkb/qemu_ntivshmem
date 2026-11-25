#!/bin/bash
WDK=/usrspace/vmshare/CaseInsensetive/7600.16385.1
#_X86_ XAMD64_ _IA64_ 这几个是WDK预定义的
#Gz同clang选项 -mrtd


clang -target i686-pc-windows-msvc \
  -mrtd \
  -D_X86_ \
  -Wno-invalid-token-paste \
  -fms-compatibility-version=15 -fms-extensions -fms-compatibility  \
  -nostdlib -nostdinc -fno-builtin \
  -I "$WDK/inc/api" -I "$WDK/inc/crt" -I "$WDK/inc/ddk" \
  -std=gnu2y \
  ./cusdriver.c ./cusdriver_2.c \
  -fuse-ld=lld \
  -Wl,/PDB:./cusdriver.pdb,/DRIVER:WDM,/SUBSYSTEM:NATIVE,/ENTRY:DriverEntry,ntoskrnl.lib,hal.lib,wdm.lib,/OUT:cusdriver.sys,/RELEASE \
  -Wl,/LIBPATH:"$WDK/lib/wxp/i386" \
  -Wl,/LIBPATH:"$WDK/lib/Crt/i386"
