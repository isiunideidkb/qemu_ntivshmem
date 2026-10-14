#include <common/rpc_host.h>
#include <stdlib.h>
#include <stdio.h>
int main(){
  printf("size: %d %d",sizeof(host_handshake_t),sizeof(host_dispatch_handshake_t));
  return 0;
}
