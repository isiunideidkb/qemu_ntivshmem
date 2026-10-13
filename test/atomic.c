#include <common/rpc_host.h>
#include <common/atomicop.h>

void func_test(){
  __myatomic_write(nullptr,nullptr,100);
}


static volatile int v_01 = 100;
int main() {
  int _expected_value = 200;
  printf("[TEST] %p\n",(&_expected_value));
  printf("[TEST] %p\n",(&v_01));  
  __myatomic_write((int* volatile)(&v_01),&_expected_value,WRITE_4BYTE);
  printf("%d \n",(v_01 == _expected_value));
  return 0;
}
