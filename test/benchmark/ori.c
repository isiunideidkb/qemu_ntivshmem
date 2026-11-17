#include <time.h>
#include <stdio.h>
#include <stdlib.h>
//测试五次 建议使用clock_gettime

//警告 大小没有文件对齐 会读到错误数据 这里少了 (size % 64) 字节 这种特殊情况 暂时不考虑数据完整性
//实际上使用文件的size必须要64byte对齐!
char data[] __attribute__((section(".custom_data"),aligned(64))) = {
#embed "/mount_points/internalHDD/Serial Experiments Lain (Japan) (Disc 1).chd"
};

//临时存储配合ERMSB优化(Intel Only since IvyBridge),cached rep movsb copy
void rep_cpy(void* src,void* dest,unsigned long length) {
  asm volatile(
    "movq %[src],%%rsi \n\t"
    "movq %[dest],%%rdi \n\t"
    "mov %[length],%%rcx \n\t"
    "rep movsb \n\t"
    : /* nothing */
    : [src]"rm"(src),[dest]"rm"(dest),[length]"rm"(length)
    : "rdi","rsi","rcx"
  );
  return;
}

//SIMD cached copy

//不计入cache line filling的开销,uncached simd nt copy
void simd_nt_cpy(void* src, void* dest, unsigned long length) {
    for(unsigned long offset = 0; offset < length; offset += 64) {
        asm volatile(
            "vmovdqa   (%[s], %[off]), %%ymm0\n\t"
            "vmovdqa 32(%[s], %[off]), %%ymm1\n\t"
            "clflushopt (%[d], %[off])\n\t"	    
            "vmovntdq %%ymm0, (%[d], %[off])\n\t"
            "vmovntdq %%ymm1, 32(%[d], %[off])\n\t"
	    "sfence \n\t"
            :
            : [s]   "r" (src),
              [d]   "r" (dest),
              [off] "r" (offset)
            : "ymm0", "ymm1", "memory"
        );
    }
}
int main() {
  unsigned long size = _Countof(data);
  size -= ( (size % 64) == 0 ) ? 0 : (size % 64); //hack 因为不能pad 64byte对齐 所以这里剔除掉不对齐的
  void* dest = aligned_alloc(64,size);
  auto beg = clock();
  rep_cpy(data,dest,size);
  auto end = clock();
  long double rep_spent = (long double)(end - beg) / CLOCKS_PER_SEC;
  printf("ERMS  copy spent %.6Lf sec\n", rep_spent);
  auto beg1 = clock();
  simd_nt_cpy(data,dest,size);
  auto end1 = clock();
  long double simd_spent = (long double)(end1 - beg1) / CLOCKS_PER_SEC;
  printf("SIMD copy spent %.6Lf sec\n", simd_spent);
  return 0;
}
