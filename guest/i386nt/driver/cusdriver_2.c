#ifndef __GNUC__
#error "you must use GCC!"
#endif

#if (!defined(__i686__)) && (!defined(__MINGW32__))
#error "please use i686 target gcc-mingw"
#endif

#if defined(__has_builtin) && __has_builtin(__builtin_ia32_monitor) && __has_builtin(__builtin_ia32_mwait)
#error "please disable -msse3 option"
#else
#endif

#if __STDC_VERSION__ < 2023L
#error "wrong stdc version"
#endif

//note : do not linked with CRT, use -nostdlib,this is a part of WindowsKernelDriverRoutine with using different compiler

//note
/*
The address range provided to the MONITOR instruction must be of write-back caching type. Only write-back
memory type stores to the monitored address range will trigger the monitor hardware. If the address range is not
in memory of write-back type, the address monitor hardware may not be set up properly or the monitor hardware
may not be armed. Software is also responsible for ensuring that


The address range provided to the MONITOR instruction must be of write-back caching type. Only write-back
memory type stores to the monitored address range will trigger the monitor hardware. If the address range is not
in memory of write-back type, the address monitor hardware may not be set up properly or the monitor hardware
may not be armed. Software is also responsible for ensuring that
•
 Writes that are not intended to cause the exit of a busy loop do not write to a location within the address region
being monitored by the monitor hardware,
•
 Writes intended to cause the exit of a busy loop are written to locations within the monitored address region.

MONITOR/MWAIT不支持Virtual-8086模式
See IA33 SDM Volume 3, Chapter 11.10.4
*/
#include <immintrin.h>
#include <cpuid.h>
extern void CusLog(const char* fmt,...);
static_assert(sizeof(unsigned int) == 4);
bool IsSupportMonitor() {
  unsigned int eax,ebx,ecx,edx;
  __get_cpuid_count(0x01,0x00,&eax,&ebx,&ecx,&edx);
  unsigned int ecx_tmp = ecx << ((sizeof(unsigned int)*8) - (3 + 1));
  unsigned int is_support_umonitor = ecx_tmp >> ((sizeof(unsigned int)*8) - 1);
  if(is_support_umonitor)
    return true;
  return false;
}

bool MonitorInit() {
  unsigned int eax,ebx,ecx,edx;
  unsigned int out_eax,out_edx;
monitor_init:  
  if(!IsSupportMonitor()) {
    CusLog("===INITIALIZING MONITOR ISA===");
    __asm__ volatile ("movl $416,%%ecx \n\t"
                      "rdmsr \n\t"
		      "movl %%eax,%0 \n\t"
		      "movl %%edx,%1 \n\t" : "=m"(eax),"=m"(edx)
		                           : /* 没有要读的喵 */
					   : "eax","edx");
   unsigned int eax_tmp = eax << ((sizeof(unsigned int)*8) - (19 + 1)); //fetch the 19th bit of eax
   unsigned int eax_final = eax_tmp >> ((sizeof(unsigned int)*8) - 1);
   if(eax_final) { //MSR is already enabled,but there's no support
     CusLog("[MONITOR] monitor is unsupported on your cpu");
     return false;
   }
   else { //MSR is not ready,init it and try it again,just need once
     out_eax = eax + 0b00000000'00000100'00000000'00000000;
     out_edx = edx;    
     __asm__ volatile ("movl $416,%%ecx \n\t"
                       "movl %0,%%eax \n\t"
		       "movl %1,%%edx \n\t" : /* nothing */
		                       : "m"(out_eax),"m"(out_edx)
				       : "eax","edx");
     CusLog("[MONITOR] enabled MSR");
     goto monitor_init;
   }
  }
  CusLog("[MONITOR] monitor ISA initialize successfully");
  //if supported call __get_cpuid_count(0x05,0x00,&eax,&ebx,&ecx,&edx); print the SMALLEST_MONITOR_LINE_SIZE : ok
  __get_cpuid_count(0x05,0x00,&eax,&ebx,&ecx,&edx);
  unsigned short* SmallestMonitorLineSize = (unsigned short*)&eax;
  unsigned int SmallestMonitorLineSizeLog = *SmallestMonitorLineSize;
  CusLog("[MONITOR] SmallestMonitorLineSize: %d",SmallestMonitorLineSizeLog);
  return true;
}
[[gnu::target("sse3")]] void MonitorAddr(void* addr) {
  //todo advance extension support see IA32SDM, do not using stack addr space for 1th param
  __builtin_ia32_monitor (addr,0,0);
}

[[gnu::target("sse3")]] void WaitAddr() {
  //todo advance extension support see IA32SDM
  __builtin_ia32_mwait (0,0);
}


//todo monitor addr with ext
//todo wait with ext
//todo CR check to avoid non-WB cache access
//todo disable split-lock uc-lock see IA32_MEMORY_CTRL MSR and IA32_CORE_CAPABILITIES MSR,Volume 3, Chapter 11.1.2.3
