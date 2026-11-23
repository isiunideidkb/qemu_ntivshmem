# 4. 挂载内存共享

```bash
# 什么路径都可以，但是/dev/shm/下面可以直接放到内存
truncate -s 16M /dev/shm/ivshmem

# 确认一次客户机确实看见了它
右键「我的电脑」→ 属性 → 硬件 → 打开设备管理器，
 System devices → PCI standard RAM Controller -> Details

PCI\VEN_1AF4&DEV_1110



```

# 5. install WDK

https://download.microsoft.com/download/4/A/2/4A25C7D5-EFBE-4182-B6A9-AE6850409A78/GRMWDK_EN_7600_1.ISO

hot load cd

```bash
telnet 127.0.0.1 4444

change cd0 /home/sucicada/Documents/PROGRAM/GRMWDK_EN_7600_1.ISO

```

xp里打开cd
安装
Full Development Environment → Build Environments
Full Development Environment → Tools

# 6. 挂载外部磁盘

```
  -drive "file=fat:rw:$host_driver_dir,format=raw,if=ide,index=3"
```

# 7. 开启外部控制

XP 专业版把 Telnet 放在服务里，不放在「添加 Windows 组件」里。打开：

Control Panel → Administrative Tools → Services

找 Telnet。启动类型改成 Automatic，再点 Start。

默认只允许 NTLM 登录，Linux 自带的 telnet 客户端连不上。在 XP 的命令行执行一次：

tlntadmn config sec=-NTLM +passwd

打开 Control Panel → Windows Firewall，先关掉

允许空密码从网络登录：
reg add HKLM\SYSTEM\CurrentControlSet\Control\Lsa /v LimitBlankPasswordUse /t REG_DWORD /d 0 /f

```bash
telnet 127.0.0.1 2323

Trying 127.0.0.1...
Connected to 127.0.0.1.
Escape character is '^]'.
Welcome to Microsoft Telnet Service 

login: Administartor
password: 


```

---------------------------------

```bash

call C:\WinDDK\7600.16385.1\bin\setenv.bat C:\WinDDK\7600.16385.1\ fre x86 WXP
build

mkdir C:\CusDriverDir
cd C:\driver
copy /Y cusdriver.inf C:\CusDriverDir
copy /Y C:\driver\objfre_wxp_x86\i386\cusdriver.sys C:\CusDriverDir

```

then:
手动方法：
设备管理器里找到 PCI 标准 RAM 控制器（硬件 ID 应是 PCI\VEN_1AF4&DEV_1110）
右键 → 更新驱动程序 → 从列表或指定位置安装 → 选 C:\CusDriverDir

必须先手动之后，才能cmd方法
 安装可以从宿主机发命令完成。XP 里已经有 WDK 自带的 devcon.exe，能用命令行装驱动。你只需要在 XP 里点一处设置：
右键「我的电脑」→ Properties → Hardware → Driver Signing。 选 Ignore，再点 OK。

```bash


C:\WinDDK\7600.16385.1\tools\devcon\i386\devcon.exe update C:\CusDriverDir\cusdriver.inf "PCI\VEN_1AF4&DEV_1110"

# copy /Y C:\CusDriverDir\cusdriver.sys C:\WINDOWS\system32\DRIVERS\cusdriver.sys
# C:\WinDDK\7600.16385.1\tools\devcon\i386\devcon.exe restart "PCI\VEN_1AF4&DEV_1110"


type C:\CusDriverLog.txt

```



