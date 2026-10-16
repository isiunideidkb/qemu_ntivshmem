#ifndef __GNUC__
#error "you must use GCC!"
#endif

#if (!defined(__i686__)) && (!defined(__MINGW32__))
#error "please use i686 target gcc-mingw"
#endif

#if defined(__has_builtin) && __has_builtin(__builtin_ia32_monitor) && \
    __has_builtin(__builtin_ia32_mwait)
#error "please disable -msse3 option"
#else
#endif

#if __STDC_VERSION__ < 2023L
#error "wrong stdc version"
#endif

// note : do not linked with CRT, use -nostdlib,this is a part of
// WindowsKernelDriverRoutine with using different compiler

// note
/*
The address range provided to the MONITOR instruction must be of write-back
caching type. Only write-back memory type stores to the monitored address range
will trigger the monitor hardware. If the address range is not in memory of
write-back type, the address monitor hardware may not be set up properly or the
monitor hardware may not be armed. Software is also responsible for ensuring
that


The address range provided to the MONITOR instruction must be of write-back
caching type. Only write-back memory type stores to the monitored address range
will trigger the monitor hardware. If the address range is not in memory of
write-back type, the address monitor hardware may not be set up properly or the
monitor hardware may not be armed. Software is also responsible for ensuring
that • Writes that are not intended to cause the exit of a busy loop do not
write to a location within the address region being monitored by the monitor
hardware, • Writes intended to cause the exit of a busy loop are written to
locations within the monitored address region.

MONITOR/MWAIT不支持Virtual-8086模式
See IA33 SDM Volume 3, Chapter 11.10.4
*/
#include <cpuid.h>
#include <immintrin.h>
extern void CusLog(const char* fmt, ...);
static_assert(sizeof(unsigned int) == 4);
[[gnu::cdecl]] bool IsSupportMonitor() {
  // MONITOR is of SSE3 ISA,making sure OS enabled the CR4.OSFXSR
  unsigned int cr4 = 0;
  __asm__ volatile("movl %%cr4,%[out_cr4]"
                   : [out_cr4] "=r"(cr4)
                   : /* nothing read */
                   :);
  unsigned int tmp_cr4 = cr4 << (32 - (10 + 1));
  unsigned int is_osfxsr = tmp_cr4 >> (32 - 1);
  if (!is_osfxsr) return false;
  unsigned int eax, ebx, ecx, edx;
  __get_cpuid_count(0x01, 0x00, &eax, &ebx, &ecx, &edx);
  unsigned int ecx_tmp = ecx << (32 - (3 + 1));
  unsigned int is_support_umonitor = ecx_tmp >> (32 - 1);
  if (is_support_umonitor) return true;
  return false;
}

[[gnu::cdecl]] bool MonitorInit() {
  unsigned int eax, ebx, ecx, edx;
  unsigned int out_eax, out_edx;
monitor_init:
  if (!IsSupportMonitor()) {
    CusLog("===INITIALIZING MONITOR ISA===");
    __asm__ volatile(
        "movl $416,%%ecx \n\t"
        "rdmsr \n\t"
        "movl %%eax,%0 \n\t"
        "movl %%edx,%1 \n\t"
        : "=rm"(eax), "=rm"(edx)
        : /* 没有要读的喵 */
        : "eax", "edx");
    unsigned int eax_tmp = eax << (32 - (19 + 1));  // fetch the 19th bit of eax
    unsigned int eax_final = eax_tmp >> (32 - 1);
    if (eax_final) {  // MSR is already enabled,but there's no support
      CusLog("[MONITOR] monitor is unsupported on your cpu");
      return false;
    } else {  // MSR is not ready,init it and try it again,just need once
      out_eax = eax + 0b00000000'00000100'00000000'00000000;
      out_edx = edx;
      __asm__ volatile(
          "movl $416,%%ecx \n\t"
          "movl %0,%%eax \n\t"
          "movl %1,%%edx \n\t"
          : /* nothing */
          : "rm"(out_eax), "rm"(out_edx)
          : "eax", "edx");
      CusLog("[MONITOR] enabled MSR");
      goto monitor_init;
    }
  }
  CusLog("[MONITOR] monitor ISA initialize successfully");
  // if supported call __get_cpuid_count(0x05,0x00,&eax,&ebx,&ecx,&edx); print
  // the SMALLEST_MONITOR_LINE_SIZE : ok
  __get_cpuid_count(0x05, 0x00, &eax, &ebx, &ecx, &edx);
  unsigned short* SmallestMonitorLineSize = (unsigned short*)&eax;
  unsigned int SmallestMonitorLineSizeLog = *SmallestMonitorLineSize;
  unsigned short* LargestMonitorLineSize = (unsigned short*)&ebx;
  unsigned int LargestMonitorLineSizeLog = *LargestMonitorLineSize;
  CusLog("[MONITOR] SmallestMonitorLineSize: %d", SmallestMonitorLineSizeLog);
  CusLog("[MONITOR] LargestMonitorLineSize: %d", LargestMonitorLineSizeLog);
  return true;
}
[[gnu::target("sse3")]] void MonitorAddr(void* addr) {
  // todo advance extension support see IA32SDM, do not using stack addr space
  // for 1th param
  __builtin_ia32_monitor(addr, 0, 0);
  return;
}

[[gnu::target("sse3"),gnu::cdecl]] void WaitAddr() {
  // todo advance extension support see IA32SDM
  __builtin_ia32_mwait(0, 0);
  return;
}

// todo monitor addr with ext
// todo wait with ext
// todo CR0 check to avoid non-WB cache access : ok

[[gnu::cdecl]] void CorrectCache() {
  CusLog("=====Correct Cache=====");
  unsigned int old_cr0;
  __asm__ volatile("movl %%cr0,%[out_parm] \n\t"
                   : [out_parm] "=rm"(old_cr0)
                   :
                   :);
  unsigned int _tmp = old_cr0 << (32 - (29 + 1));  // 不包括0啊! 还要额外加一
  unsigned int is_nw = _tmp >> (32 - 1);
  _tmp = old_cr0 << (32 - (30 + 1));
  unsigned int is_cd = _tmp >> (32 - 1);
  unsigned int new_cr0 = old_cr0;
  if (is_nw) {
    CusLog("[CORRECT] invalid nw bit");
    new_cr0 -= 0b00100000'00000000'00000000'00000000;
  }
  if (is_cd) {
    new_cr0 -= 0b01000000'00000000'00000000'00000000;
    CusLog("[CORRECT] invalid cd bit");
  }
  if (new_cr0 != old_cr0) {
    __asm__ volatile(
        "movl %[in_param],%%edi \n\t"
        "movl %%edi,%%cr0 \n\t"
        "wbinvd \n\t"
        :
        : [in_param] "rm"(new_cr0)
        : "edi");
  }
  return;
}
