/*
  coding style C23 + GCC(MinGW)
  clang-format --style=LLVM
  platform independent rpc protocol for host
  compile with -mwaitpkg
*/
#ifundef __GNU__
#error "wrong platform"
#endif

#if __STDC_VERSION__ < 2023L
#error "wrong stdc version"
#endif

#include <string.h>
#include <immintrin.h>

#ifdef _ENABLE_LOG
#define LOG(content, ...) printf(content __VA_OPT__(,) __VA_ARGS__);
#else
#define LOG(content, ...)
#endif

typedef struct [[gnu::packed]] _function_info_t {
  unsigned short flag;
  unsigned short split_number;
  unsigned short _1_bytes_per_split;
  unsigned short _2_bytes_per_split;
} function_info_t;

typedef struct [[gnu::packed]] _host_handshake_t {
  char hello_message;
  char cap_mask;
  char _1_length;
  char _2_length;
  char _3_length;
  char _4_length;
} host_handshake_t;

enum dispatch_type {
  SHM,
  HUGE_SHM,
  FUNC
}

//private struct
struct [[gnu::packed]] __disp_list {
  void(*)(struct __disp_list* self) dispatch_function; // NULL if SHM/HUGE_SHM
  unsigned int length; //include flag
  void* base_addr; //include flag
  dispatch_type d_t;
  struct __disp_list* next;
}
typedef struct [[gnu::packed]] _host_dispatch_t {
  struct __disp_list head;
  unsigned int total_length;
  void* start_addr;
} host_dispatch_t;


void host_handshake_new(host_handshake_t* out_ptr,bool is_phys,bool is_umonitor,unsigned int length) {
  memset(out_ptr,0,sizeof(host_handshake_t));
  out_ptr->hello_message = 1;
  if(is_phys && (!is_umonitor))
    out_ptr->mask = 00000010_00000001;
  if((!is_phys) && is_umonitor)
    out_ptr->mask = 00000001_00000001;
  if(is_phys && is_umonitor)
    out_ptr->mask = 00000011_00000001;
  unsigned int* length_ptr = (unsigned int*)(&(out_ptr->_1_length));
  *length_ptr = length;
}

/*
write hello msg for host
atomicly(using gcc intrinsics) & block(using umonitor)
do not call not asyncly
*/
[[gnu::target("waitpkg")]] void hello_handshake_umonitor(host_handshake_t* out_ptr,void* addr){
  host_handshake_t* dest = addr;
  __atomic_store_n(&dest,&out_ptr,__ATOMIC_RELEASE);
  //wait client
  char* flag = addr;
waiting:  
  _umonitor(flag);
  _umwait(1,0);
  if (__atomic_load(flag,__ATOMIC_ACQUIRE))
    goto waiting;
  return;
}

void hello_function_dispatch_umonitor(){
  //
}


