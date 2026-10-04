#ifndef __GNUC__
#error "you must use GCC!"
#endif

#ifndef __i686__
#error "please use i686 target gcc"
#endif

#if defined(__has_builtin) && __has_builtin(__builtin_ia32_monitor) && __has_builtin(__builtin_ia32_mwait)
#error "please disable -msse3 option"
#else
#endif

#if __STDC_VERSION__ < 2023L
#error "wrong stdc version"
#endif

//do not link CRT use -nodefaults


//use sse3 compatible
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

extern void CusLog();
static_assert(sizeof(unsigned int) == 4);
bool IsSupportMonitor() {
  unsigned int eax,ebx,ecx,edx;
  __get_cpuid_count(0x01,0x00,&eax,&ebx,&ecx,&edx);
  unsigned int ecx_tmp = ecx << ((sizeof(unsigned int)*8) - (3 + 1));
  unsigned int is_support_umonitor = ((sizeof(unsigned int)*8) - 1);
  if(is_support_umonitor)
    return true;
  return false;
}

bool MonitorInit() {
monitor_init:
  unsigned int eax,edx;
  unsigned int out_eax,out_edx;
  if(IsSuportMonitor()) {
    __asm__ volatile ("movl $416,%ecx"
                      "rdmsr"
		      "movl %eax,%0"
		      "movl %edx,%0");
   unsigned int eax_tmp = eax << ((sizeof(unsigned int)*8) - (19 + 1)); //fetch 19 bit of eax
   unsigned int eax_final = eax_tmp >> ((sizeof(unsigned int)*8) - 1);
   //todo log the operation
   if(eax_final) {
     return false;
   }
   else {
     out_eax = eax + 0b00000000'00000100'00000000'00000000;
     out_edx = edx;    
     __asm__ volatile ("movl $416,%ecx"
                       "movl %0,%eax" //todo %0 is the out_eax
		       "movl %1,%edx"); //todo %1 is the out_edx
     //todo log the operation		       
   }
  }
  if(!IsSupportMonitor()) {
    //todo log failure
  }
  //if supported call __get_cpuid_count(0x05,0x00,&eax,&ebx,&ecx,&edx); print the SMALLEST_MONITOR_LINE_SIZE
  
  
}
void MonitorAddr(void* addr) {
  //todo advvance extension support see IA32SDM
  __builtin_ia32_monitor (addr,0,0);
}

void WaitAddr(void* addr) {
  //todo advvance extension support see IA32SDM
  __builtin_ia32_mwait (0,0);
}
