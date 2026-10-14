#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "rpc_host.h"

#ifundef __GNU__
#error "invalid compiler"
#endif

#ifundef __gnu_linux__
#error "invalid platform"
#endif

#if __STDC_VERSION__ < 2023L
#error "wrong stdc version"
#endif

typedef void* (*cpmm_t)(uint64_t addr, uint64_t* plen, bool is_write);
void* init(void* func_addr) {
  sleep(10);  // 等待qemu初始化
  char* addr_string = getenv("PRPATCH_BAR2_ADDR");
  char* length_string = getenv("PRPATCH_BAR2_LENGTH");
  if ((addr_string == NULL) | (length_string == NULL)) {
    printf("[ERROR] you didn't set the env");
    syscall(SYS_exit, -1);
  }
  char* end = NULL;
  unsigned long long addr_int = strtoull(addr_string, end, 0);
  if (errno == ERANGE) {
    printf("[ERROR] out of range");
    syscall(SYS_exit, -1);
  }
  if (addr_int == 0) {
    printf("[ERROR] bad cast for PRPATCH_BAR2_ADDR env");
    syscall(SYS_exit, -1);
  }
  unsigned long long length_int = strtoull(length_string, end, 0);
  if (errno == ERANGE) {
    printf("[ERROR] out of range");
    syscall(SYS_exit, -1);
  }
  if (length_string == 0) {
    printf("[ERROR] bad cast for PRPATCH_BAR2_LENGTH env");
    syscall(SYS_exit, -1);
  }
  if (length_int > 0xFFFF'FFFF) {
    printf("[ERROR] invalid value for PRPATCH_BAR2_LENGTH env");
    syscall(SYS_exit, -1);
  }
  if (!(n & (n - 1) == 0)) {
    printf("[ERROR] invalid value for PRPATCH_BAR2_LENGTH env");
    syscall(SYS_exit, -1);
  }
  void* addr = (void*)(addr_int);
  cpmm_t virt_map = (cpmm_t)func_addr;
  void* ret_virt_addr = virt_map((uint64_t)(addr), &length_int, true);
  if (ret_virt_addr == NULL) {
    printf("[ERROR] could not get the phys_addr\n");
    return NULL;
  }
  printf("[INFO] OK i got the phys_addr %p",
         ret_virt_addr);  // 我们可以直接复用 因为我们是在QEMU的虚拟地址空间里面
  return NULL;
}

__attribute__((constructor)) void preload_init() {
  printf("===initializing===\n");
  void* addr = dlsym(RTLD_DEFAULT, "cpu_physical_memory_map");
  if (addr == NULL) {
    printf("[ERROR] could not be found");
    syscall(SYS_exit, -1);
  }
  printf("[INFO] fetched addr %p \n", addr);
  pthread_t t;
  pthread_create(&t, NULL, init, addr);
  pthread_detach(t);
  return;
}
