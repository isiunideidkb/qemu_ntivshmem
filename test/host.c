#include <common/rpc_host.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
int main() {
  size_t size = 64 * 1024 * 1024;  // 64 MB

  // mmap 匿名映射，返回的地址自动页对齐（4K），同时也是 64 字节对齐
  void *region = mmap(NULL, size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  printf("test: %p",region);
  auto ctx = host_rpc_new_ctx(region,64*1024*1024);
  host_rpc_handshake(ctx,64);
  return 0;
}
