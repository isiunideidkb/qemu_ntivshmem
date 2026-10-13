/*
  test 64 bit: cc -m64 -D_ENABLE_LOG -g -std=c23 -Werror -I.. ./atomic.c
  test 32bit: i686-linux-gnu-gcc -D_ENABLE_LOG -g -std=c23 -Werror -I.. ./atomic.c or clang -D__i686__ -target i686-pc-linux-gnu -D_ENABLE_LOG -g -std=c23 -Werror -I.. ./atomic.c
  note: clang target triple i686-* will not define the __i686__ macro,define it manually
*/

#include <common/atomicop.h>
#include <stdlib.h>
#include <stdio.h>

#if (!defined(__x86_64__)) && (!defined(__i686__))
#error "wrong platform,must be x8664 or i686(!i686, not i386!)"
#endif

#pragma message "please compile i686 and x86_64 respectively"

static void terminate_test(void){
  printf("===TERMINATE TEST===\n");
  __myatomic_read(nullptr,nullptr,100);
}

static void read_test(void) {
#ifdef __x86_64__
  printf("===READ TEST===\n");
  printf("==SINGLE BYTE==\n");
  volatile char _0_target_value = 100;
  volatile char _0_value = 0;
  __myatomic_read((void*)(&_0_target_value),(void*)(&_0_value),READ_BYTE);
  printf("result: read %d to %d\n",(int)_0_target_value,(int)_0_value);
  
  printf("==DOUBLE BYTE(WORD)==\n");
  volatile short _1_target_value = 100;
  volatile short _1_value = 0;
  __myatomic_read((void*)(&_1_target_value),(void*)(&_1_value),READ_2BYTE);
  printf("result: read %d to %d\n",(int)_1_target_value,(int)_1_value);

  printf("==DOUBLE WORD==\n");
  volatile int _2_target_value = 100;
  volatile int _2_value = 0;
  __myatomic_read((void*)(&_2_target_value),(void*)(&_2_value),READ_4BYTE);
  printf("result: read %d to %d\n",_2_target_value,_2_value);  

  printf("==QUAD WORD==\n");
  volatile long long _3_target_value = 100;
  volatile long long _3_value = 0;
  __myatomic_read((void*)(&_3_target_value),(void*)(&_3_value),READ_8BYTE);
  printf("result: read %lld to %lld\n",_3_target_value,_3_value);    
#elif defined(__i686__)
  printf("===READ TEST===\n");
  printf("==SINGLE BYTE==\n");
  volatile char _0_target_value = 100;
  volatile char _0_value = 0;
  __myatomic_read((void*)(&_0_target_value),(void*)(&_0_value),READ_BYTE);
  printf("result: read %d to %d\n",(int)_0_target_value,(int)_0_value);
  
  printf("==DOUBLE BYTE(WORD)==\n");
  volatile short _1_target_value = 100;
  volatile short _1_value = 0;
  __myatomic_read((void*)(&_1_target_value),(void*)(&_1_value),READ_2BYTE);
  printf("result: read %d to %d\n",(int)_1_target_value,(int)_1_value);

  printf("==DOUBLE WORD==\n");
  volatile long _2_target_value = 100;
  volatile long _2_value = 0;
  __myatomic_read((void*)(&_2_target_value),(void*)(&_2_value),READ_4BYTE);
  printf("result: read %ld to %ld\n",_2_target_value,_2_value);  

  printf("==QUAD WORD==\n");
  volatile long long _3_target_value = 100;
  volatile long long _3_value = 0;
  __myatomic_read((void*)(&_3_target_value),(void*)(&_3_value),READ_8BYTE);
  printf("result: read %lld to %lld\n",_3_target_value,_3_value);    
#endif
  return;
}

static void write_test(void) {
#ifdef __x86_64__
  printf("===WRITE TEST===\n");
  printf("==SINGLE BYTE==\n");
  volatile char _0_value = 100;
  volatile char _0_target_value = 0;
  __myatomic_write((void*)(&_0_target_value),(void*)(&_0_value),WRITE_BYTE);
  printf("WRITE %d to %d\n",(int)(_0_value),(int)(_0_target_value));
  printf("==DOUBLE BYTE(WORD)==\n");
  volatile short _1_value = 100;
  volatile short _1_target_value = 0;
  __myatomic_write((void*)(&_1_target_value),(void*)(&_1_value),WRITE_2BYTE);
  printf("WRITE %d to %d\n",(int)(_1_value),(int)(_1_target_value));  
  printf("==DOUBLE WORD==\n");
  volatile int _2_value = 100;
  volatile int _2_target_value = 0;
  __myatomic_write((void*)(&_2_target_value),(void*)(&_2_value),WRITE_4BYTE);
  printf("WRITE %d to %d\n",_2_value,_2_target_value);    
  printf("==QUAD WORD==\n");
  volatile long long _3_value = 100;
  volatile long long _3_target_value = 0;
  __myatomic_write((void*)(&_3_target_value),(void*)(&_3_value),WRITE_8BYTE);
  printf("WRITE %lld to %lld\n",_3_value,_3_target_value);      
#elif defined(__i686__)
  printf("===WRITE TEST===\n");
  printf("==SINGLE BYTE==\n");
  volatile char _0_value = 100;
  volatile char _0_target_value = 0;
  __myatomic_write((void*)(&_0_target_value),(void*)(&_0_value),WRITE_BYTE);
  printf("WRITE %d to %d\n",(int)(_0_value),(int)(_0_target_value));
  printf("==DOUBLE BYTE(WORD)==\n");
  volatile short _1_value = 100;
  volatile short _1_target_value = 0;
  __myatomic_write((void*)(&_1_target_value),(void*)(&_1_value),WRITE_2BYTE);
  printf("WRITE %d to %d\n",(int)(_1_value),(int)(_1_target_value));  
  printf("==DOUBLE WORD==\n");
  volatile long _2_value = 100;
  volatile long _2_target_value = 0;
  __myatomic_write((void*)(&_2_target_value),(void*)(&_2_value),WRITE_4BYTE);
  printf("WRITE %ld to %ld\n",_2_value,_2_target_value);    
  printf("==QUAD WORD==\n");
  volatile long long _3_value = 100;
  volatile long long _3_target_value = 0;
  __myatomic_write((void*)(&_3_target_value),(void*)(&_3_value),WRITE_8BYTE);
  printf("WRITE %lld to %lld \n",_3_value,_3_target_value);      
#endif
  return;
}



int main(){
  read_test();
  write_test();
  terminate_test();
  printf("completed!\n");
  return 0;
}
