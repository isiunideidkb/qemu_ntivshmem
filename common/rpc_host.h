/*
  coding style C23 + GCC(MinGW)
  clang-format --style=LLVM
  common rpc protocol for host
  use nullptr instead of NULL

  todo list:
  1. hello_handshake host did not clean(zero-filled back) the space
  2. dispatch_handshake host did not clean(zero-filled back) the space
  3. complete system specific routines
  4. improves this bullshit "robustness"
  5. fix identifier errors,UB
*/

/* ===== PLATFORM CHECK ===== */
#pragma once
#ifndef __GNU__
#error "wrong platform"
#endif

#if __STDC_VERSION__ < 2023L
#error "wrong stdc version"
#endif

//todo atomic instrincs check: using __has_attrubute(target) or related builtin
//see GCC documentation for details

/* ===== PLATFORM SWITCH ===== */
//clean predefs
#if defined(__USE_WINDOWS__)
#undef __USE_WINDOWS__
#endif
#if defined(__USE_WAITPKG__)
#undef __USE_WAITPKG__
#endif
#if defined(__USE_LINUX__)
#undef __USE_LINUX__
#endif
#if defined(__USE_UNIX__)
#undef __USE_UNIX__
#endif

//ENABLE_* manually selected macro,must be platform suited(throw error if it's not matched by given target config)
//note: WAITPKG is optionally,but can not appeared as single option,paralleled with system api
#if defined(ENABLE_WINDOWS) //system api
/* ===== cond1 ===== */
#if __has_include(<windows.h>) && defined(__MINGW32__)
#define __USE_WINDOWS__
#pragma message "using win32 api"
#else
#error "there's no windows sdk"
#endif
/* ===== cond1 ===== */
#elif defined(ENABLE_UNIX)
#if __has_include(<unistd.h>) && defined(__unix__)
#define __USE_UNIX__
#pragma message "using unix api"
#else
#error "there's no POSIX headers"
#endif
#elif defined(ENABLE_LINUX)
/* ===== cond2 ===== */
#if __has_include(<linux/futex.h>) && defined(__gnu_linux__)
#define __USE_LINUX__
#pragma message "using linux api"
#else
#error "there's no linux specific headers"
#endif
/* ===== cond2 ===== */
#endif

#if defined(ENABLE_WAITPKG) //note : waitpkg is paralled with system specific api
/* ===== cond3 ===== */
#if __has_builtin(__builtin_ia32_umwait) && __has_builtin(__builtin_ia32_umonitor)
#define __USE_WAITPKG__
#pragma message "using waitpkg intrinsics"
#else
#error "making sure you specified -mwaitpkg option"
#endif
/* ===== cond3 ===== */
#endif

#if !defined(ENABLE_WINDOWS) && !defined(ENABLE_WAITPKG) && !defined(ENABLE_LINUX)
/* ===== cond4 ===== */
#pragma message "mathing automaticly"
#if __has_builtin(__builtin_ia32_umwait) && __has_builtin(__builtin_ia32_umonitor)
#pragma message "using waitpkg"
#define __USE_WAITPKG__
#endif
//note : waitpkg is paralled with system specific api
#if defined(__unix__)
#pragma message "using unix api"
#define __USE_UNIX__
#elif __has_include(<linux/futex.h>) && defined(__gnu_linux__)
#pragma message "using linux api"
#define __USE_LINUX__
#elif __has_include(<windows.h>) && defined(__MINGW32__)
#pragma message "using win32 api"
#define __USE_WINDOWS__
#else //note: waitpkg cannot appeared as a sinlge option
#error "can not select the platform api"
#endif
/* ===== cond4 ===== */
#endif

/* ===== HEADERS ===== */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "umonitor_check.h"
#include <immintrin.h>
#ifdef __USE_WINDOWS__
#include <windows.h>
#endif
#ifdef __USE_LINUX__
#endif
#ifdef __USE_UNIX__
#include <unistd.h>
#endif
#ifdef __USE_WAITPKG__
#endif

/* ===== UTILITY MACROS ===== */
#ifdef _ENABLE_LOG
#define LOG(content, ...) printf(content"\n"  __VA_OPT__(,) __VA_ARGS__);
#else
#define LOG(content, ...)
#endif
#ifndef assert
#define assert(cond,content) if(cond) {/* do nothing*/} else { printf(content"\n"); exit(-1); }
#else
#error "please remove #include <assert.h>"
#endif
#define PRIVATE_FUNC static


//1byte!
typedef void(*ssw_t)(void* addr)
//quickdoc: system_specific_waiter used by host_run_*_system routines,if this variable is not null,system_specific_waiter is prefered
//this waiter receives a addr,listens the modification in 1byte region
//this variable is private linkage(i.e. static)
static ssw_t system_specific_waiter = nullptr;
void set_system_specific_waiter(void* target) {
  system_specific_waiter = (ssw_t)(target);
  return;
}


/* ========== protocol abstraction routine ========== */
//quickdoc: abstract hello handshake memlayout structure, in stage 1
typedef struct [[gnu::packed]] _host_handshake_t {
  char hello_message;
  char cap_mask;
  char _1_length;
  char _2_length;
  char _3_length;
  char _4_length;
} host_handshake_t;

//quickdoc: abstract dispatch handshake memlayout structure, in stage 2
typedef struct [[gnu::packed]] _host_dispatch_handshake_t {
  char flag_lowaddr;
  char flag_highaddr;
  unsigned short split_number;
  unsigned short bytes_per_split;
} host_dispatch_handshake_t;

//quickdoc: dispatch_type used by dispatch handshake protocol
enum dispatch_type : char {
  SHM = (char)0x0,
  HUGE_SHM = (char)0x1,
  FUNC = (char)0x2
}

typedef void(*dispatch_func_t)(struct __disp_list* self);
//quickdoc: this is private struct,stores every function info node in RPC protocol,in stage 3
struct [[gnu::packed]] __disp_list {
  dispatch_func_t dispatch_function; // nullptr if SHM/HUGE_SHM
  unsigned int length; //include flag
  void* base_addr; //include flag
  dispatch_type d_t;
  struct __disp_list* next; //represent as linked list which node represents single function,linear layout,
}
//quickdoc: this structure represents all concrete function in RPC protocol,in stage 3
typedef struct [[gnu::packed]] _host_dispatch_t {
  struct __disp_list head;
  struct __disp_list end; //works only head >= 1,
  unsigned int total_length;
  void* start_addr;
} host_dispatch_t;

//quickdoc: note: all of these functions's out parameter ptr, the lifetime of these ptr owned by CALLER!

//quickdoc: this requires  hello handshake init, init a dispatch handshake structure, starge 2
void host_dispatch_handshake_init(host_dispatch_handshake_t* hdh_out, host_handshake_t* hht_out,
                                 unsigned short psplit_number,unsigned short pbytes_per_split) {
  assert(hdh_out != nullptr && hht_out != nullptr,"[DISSHAKE-ASSERT] wrong param, nullptr");
  unsigned int* length_ptr = (unsigned int*)(&(hht_out->_1_length));
  assert(psplit_number * pbytes_per_split == *length_ptr,"[DISSHAKE-ASSERT] wrong param, incorrect length");
  assert(psplit_number >= 1,"[DISSHAKE-ASSERT] wrong param")
  assert ((pbytes_per_split) > 0 && ((n & (n-1)) == 0),"[DISSHAKE-ASSERT] wrong param, incorrect bytes_per_split")
  memset(hdh_ptr,0,sizeof(host_dispatch_handshake_t));
  hdh_ptr->flag_lowaddr = 1;
  hdh_ptr->flag_highaddr = 0;
  hdh_ptr->split_number = psplit_number;
  hdh_ptr->split_bytes_per_split = pbytes_per_split
}

//quickdoc: this requires dispatch handshake init,init a host_dispatch_t(i.e. represents concrete dispatch functions type)
void host_dispatch_init(host_dispatch_t* hdt_out, host_dispatch_handshake_t* hdh_out,
                       host_handshake_t* hht_out, void* addr) {
  assert(hdt_out != nullptr && hdh_out != nullptr && hht_out != nullptr && addr != nullptr,"[DIS-ASSERT] wrong param, nullptr");
  unsigned int* length_ptr = (unsigned int*)(&(hht_out->_1_length));
  memset(hdt_out,0,sizeof(host_dispatch_t));
  hdt_out->total_length = *length_ptr;
  hdt_out->start_addr = addr;
  hdt_out->end = nullptr;
  hdt_out->head = nullptr;
  return;
}

//quickdoc: clean the resource created by append_node, WARNING THIS FUNCTION DO NOT CLEAN hdt_out,the ptr's lifetime owned by caller
void host_dispatch_destory_node(host_dispatch_t* hdt_out) {
  assert(hdt_out != nullptr,"[DISDES-ASSERT] wrong param, nullptr");
  if(hdt_out->head == nullptr) {
    LOG("[DESTORY] invalid HEAD");
    return;
  }
  struct __disp_list* cur_node = hdt_out->head;
  struct __disp_list* tmp_prev_node = nullptr;
  while(true) {
    if(cur_node == nullptr) {
      break;
    }
    tmp_prev_node = cur_node;
    cur_node = cur_node->next;
    free(tmp_prev_node);
  }
  hdt_out->head = nullptr;
  assert(tmp_prev_node != nullptr,"[DESTORY-ASSERT] tmp_prev_node MUST-NOT be nullptr");
  return;
}

/*
todo: assert
if dispatch_type == SHM
  assert that the len == <bytes per split>
if dispatch_type == HUGE_SHM
  assert that the len % <bytes per split> == 0


quickdoc: append node,which node represents single function,linear layout,represent as linked list  
*/
void host_dispatch_append_node(host_dispatch_t* hdt_out, void* addr,
                               host_dispatch_handshake_t* hdh_out, dispatch_func_t df,
			       unsigned int len,dispatch_type dt) {
  assert(hdt_out != nullptr && addr != nullptr && hdh_out != nullptr,"{DISAPP-ASSERT} wrong param, nullptr");
  assert(len != 0,"[DISAPP-ASSERT] wrong param, incorrect len");
  if (dt == SHM)
    assert(len == hdh_out->bytes_per_split,"[DISAPP-ASSERT] wrong param, incorrect len");
  if (dt == HUGE_SHM)
    assert((len % hdh_out->bytes_per_split == 0),"[DISAPP-ASSERT] wrong param, incorrect len");
  struct __disp_list* node = malloc(sizeof(struct __disp_list));
  node->length = len;
  node->base_addr = addr;
  node->d_t = dt;
  node->next = nullptr;
  if(dt == SHM || dt == HUGE_SHM) {
    node->dispatch_function = nullptr;
  }
  else {
    node->dispatch_function = df;
  }

  if(hdt_out->head == nullptr & hdt_out->end == nullptr) {
    hdt_out->head = node;
    hdt_out->end = node; 
  }
  else {
    assert(hdt_out->end != nullptr,"[DISAPP-ASSERT] invalid field");
    hdt_out->end->next = node;
    hdt_out->end = node;
  }
}

/*
  ret: is_error,the details will print on the stdio
*/
bool host_check_dispatches(host_dispatch_t* hdt){
  assert(hdt != nullptr,"[DISCHECK] wrong param, nullptr");
  bool stat = true; //only write false,in the following context
  unsigned int initial_length = 0;
  struct __disp_list* cur_node = hdt->head;
  while(true) {
    if(cur_node == nullptr) {
      LOG("[CHECK] end");
      break;
    }
    if(cur_node->length == 0 | cur_node->base_addr == nullptr) {
      LOG("[CHECK] invalid fields"); 
      stat = false;
    }
    cur_node += length;
    cur_node = cur_node->next;
  }
  if(initial_length != hdt->total_length) {
    stat = false;
  }
  return stat;
}
void host_handshake_init(host_handshake_t* out_ptr, bool is_phys,
                        bool is_umonitor, unsigned int length) {
  assert(out_ptr != nullptr,"[HANDINIT-ASSERT] wrong param, nullptr");
  memset(out_ptr,0,sizeof(host_handshake_t));
  out_ptr->hello_message = 1;
  if(is_phys && (!is_umonitor))
    out_ptr->mask = 00000010'00000001;
  if((!is_phys) && is_umonitor)
    out_ptr->mask = 00000001'00000001;
  if(is_phys && is_umonitor)
    out_ptr->mask = 00000011'00000001;
  unsigned int* length_ptr = (unsigned int*)(&(out_ptr->_1_length));
  *length_ptr = length;
}


/* ========== protocol processing routine ========== */
//note: the following routines are platform dependent

/*
 write hello msg for host
 atomicly(using gcc intrinsics) & block(using umonitor)
 do not call not asyncly
*/

#ifdef __USE_WAITPKG__


//todo: only add umonitor instruction cpuid check on alternative entry for rolling back

//quick doc hello handshake, alternative explicit entry
//return 1 OK
//return 0 error
//note.protocol bit 1 UMONITOR is regardless of client,it doesn't need to be symmetirc
int host_run_hello_handshake_umonitor(host_handshake_t* hh_out,void* addr) {
  assert(hh_out != nullptr && addr != nullptr,"[HSH-ASSERT] wrong param, nullptr");
  host_handshake_t* dest = addr;
  assert(dest->hello_message == 1,"[HSH-ASSERT] incorrect hello message");  
  __atomic_store_n(dest,hh_out,__ATOMIC_RELEASE);
  //wait client
  char* flag_addr = addr;
waiting:  
  _umonitor(flag);
  _umwait(1,0);
  if (__atomic_load_n(flag_addr,__ATOMIC_ACQUIRE))
    goto waiting;
  char* ret_cap_mask_addr = addr + 1;
  char ret_cap_mask = __atomic_load_n(ret_cap_mask_addr,__ATOMIC_ACQUIRE);
  //ret_cap_mask  XXXXX_XX which "_" is what i want
  //ret_cap_mask left shirt 5(8-3) bit
  //we got _XX00000 which "_" is what i want
  char tmp_cap_mask = (ret_cap_mask << 5);
  //right shirt 7(8-1) bit
  //we got 0000000_ which _ is what i want
  char result_client_bit = (tmp_cap_mask >> 7);
  //clean
  host_handshake_t _clean_example = {0};
  _clean_example
  if (result_client_bit) {
    return 0;
  }
  return 1;
}

//quick doc dispatch handshake with client, alternative explicit entry
//return 1 for ok
//return 0 for error ILLEGAL FUNC COUNT
//rturn -1 for ILLEGAL SPACE COUNT
//return -2 for RESERVED ERROR
int host_run_dispatch_handshake_umonitor(host_dispatch_handshake_t* hdh_out,void* addr) {
  assert(hdh_out != nullptr && addr != nullptr,"[HDSH-ASSERT] wrong param, nullptr");
  host_dispatch_handshake_t* dest = addr;
  assert(dest->hello_message == 1,"[HDSH-ASSERT] incorrect hello message");
  //wait client
  __atomic_store(dest,hdh_out,__ATOMIC_RELEASE);
waiting:  
  _umonitor(flag);
  _umwait(1,0);
  if (__atomic_load_n(flag_addr,__ATOMIC_ACQUIRE))
    goto waiting;
  char error_code = __atomic_load_n(flag_highaddr,__ATOMIC_ACQUIRE);
  //clean aftar all memory accessing
  host_dispatch_handshake_t _clean_example = {0};
  __atomic_store(dest,&_clean_example,__ATOMIC_RELEASE);
  if (error_code == 0x0) {
    return 1;
  }
  else if (error_code == 0x1) {
    return 0;
  }
  else if (error_code == 0x2) {
    return -1;
  }
  else {
    LOG("[WARN] receives an incorrect reserved code");
    return -2;
  }
}

//quickdoc: private function,alternative function of host_internal_dispatch_runner_system,this is not entry,see host_internal_dispatch_runner_system
PRIVATE_FUNC void* host_internal_dispatch_runner_system_umonitor(void* _param) {
//note: actually we need no pthread_exit & return,this thread is expected as never_return
  struct __disp_list* node = _param;
  assert(node != nullptr,"[ASSERT] wrong param, nullptr");
  char* flag = node->base_addr;
  char* function_code = (node->base_addr) + 1;
  __atomic_store_n(function_code,node->d_t,__ATOMIC_RELEASE);
startup:
//setup the flag
  __atomic_store_n(flag,1,__ATOMIC_RELEASE);
  //wait for client pull down the flag
  _umonitor(flag);
  _umwait(0,0);
  if(__atomic_load_n(flag,__ATOMIC_ACQUIRE))
    goto startup;
  //if client pull down
  LOG("[INFO] host received a request");
  if(node->d_t == FUNC) {
    assert(node->dispatch_function != nullptr,"[ASSERT] incorrect dispatch function, nullptr");
    (node->dispatch_function)(node);
  }
  else {
    LOG("[INFO] find a empty SHM");
    //pass if type is SHM/HUGE_SHM
  }
  goto startup;
  return nullptr;
}
#endif

#ifdef __USE_WINDOWS__
#error "do not compile on windows SDK env"
#endif

#ifdef __USE_UNIX__
//provids host_run_*_system function


//todo host_run_hello_handshake_system umonitor check then forward to it

//todo host_run_dispatch_handshake_system umonitor check then forward to it
int host_run_dispatch_handshake_system(host_dispatch_handshake_t* hdh_out,void* addr);


//todo host_run_dispatch_in_threads_system
//quickdoc: private function,be used by host_run_dispatch_in_threads_system
PRIVATE_FUNC void* host_internal_dispatch_runner_system(void* _param) {
//note: actually we need no pthread_exit & return,this thread is expected as never_return
  struct __disp_list* node = _param;
#ifdef __USE_WAITPKG__
  if(is_support_umonitor()) {
    host_internal_dispatch_runner_system_umonitor(_param);
    return nullptr;
  }
#endif
  assert(node != nullptr,"[ASSERT]wrong param,nullptr")
  char* flag = node->base_addr;
  char* function_code = (node->base_addr) + 1;
  __atomic_store_n(function_code,node->d_t,__ATOMIC_RELEASE);  
  __atomic_store_n(flag,1,__ATOMIC_RELEASE);
  if(system_specific_waiter) {
    goto system_specific;
  }
  else {
    ASSERT(false,"[BUG]" __LINE__ __FILE__ "HAVE NOT IMPLEMENTED YET");
  }
  
  return nullptr;
system_specific:
  __atomic_store_n(flag,1,__ATOMIC_RELEASE);
  system_specific_waiter(node->base_addr); //wait
  if(__atomic_load_n(flag,__ATOMIC_ACQUIRE))
    goto system_specific;
  if(node->d_t == FUNC) {
    (node->dispatch_function)(node);
  }
  else {
    LOG("[INFO] passed for non FUNC function");
  }
  goto system_specific;
  return nullptr;
}

//quickdoc: this is function dispatching bootstrapper entry,provided by system-specific function,DO NOT CALL host_run_* ! it does not even exist
void host_run_dispatch_in_threads_system(host_dispatch_t* hd_out) {
  assert(hd_out != nullptr,"[ASSERT] wrong param, nullptr");
  if (hd_out->head == nullptr) {
    LOG("[INFO] dispatch list is empty");
  }
  struct __disp_list* cur_node = hd_out->head;
  while(true) {
    if(cur_node == nullptr)
      break;
    pthread_t _placeholder;
    pthread_create(&_placeholder,NULL,host_internal_dispatch_runner_system,cur_node);
    pthread_detach(_placeholder);
    cur_node = cur_node->next;
  }
  return;
}
#endif


//todo auto route function ,prefer waitpkg
