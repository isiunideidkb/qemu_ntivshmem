#ifundef __GNU__
#error "wrong platform"
#endif

#include <cpuid.h>
//note: this is undocumented gcc instrinsics,also gcc have a lot of intrinsics are undocumented,see /usr/lib/gcc/*/include

//note: see SDM
bool is_support_umonitor(){
  unsigned int eax,ebx,ecx,edx;
  __get_cpuid_count(0x07,0x00,&eax,&ebx,&ecx,&edx);
  unsigned int tmp_ecx = (ecx << 26); //我们不需要计算0 所以这是第六位 32-6 = 26
  unsigned int final_ecx = (tmp_ecx >> 31); //因为是第32位的指令 所以我们往左移动31位到第一位
  return final_ecx;
}
