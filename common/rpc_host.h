/*
  coding style C23 + GCC(MinGW)
  clang-format --style=LLVM
  common rpc protocol for host
  use nullptr instead of NULL
*/

/* ===== PLATFORM CHECK ===== */
#pragma once
#ifndef __GNU__
#error "wrong platform"
#endif

#if __STDC_VERSION__ < 2023L
#error "wrong stdc version"
#endif

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

//ENABLE_* manually selected macro,must be platform suited(throw error if it's not matched)
//note: WAITPKG is optionally,but can not appeared as single option
#if defined(ENABLE_WINDOWS)
/* ===== cond1 ===== */
#if __has_include(<windows.h>) && defined(__MINGW32__)
#define __USE_WINDOWS__
#pragma message "using win32 api"
#else
#error "there's no windows sdk,making sure you're using mingw"
#endif
/* ===== cond1 ===== */
#elif defined(ENABLE_LINUX)
/* ===== cond2 ===== */
#if __has_include(<linux/futex.h>) && defined(__gnu_linux__)
#define __USE_LINUX__
#pragma message "using linux futex api"
#else
#error "there's no futex headers,making sure you're using gcc"
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
#if __has_include(<linux/futex.h>) && defined(__gnu_linux__)
#pragma message "using linux futex api"
#define __USE_LINUX__
#elif __has_include(<windows.h>) && defined(__MINGW32__)
#pragma message "using win32 api"
#define __USE_WINDOWS__
#elifndef __USE_WAITPKG__
#error "can not select the platform api"
#endif
/* ===== cond4 ===== */
#endif

/* ===== HEADERS ===== */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#ifdef __USE_WINDOWS__
#include <windows.h>
#endif
#ifdef __USE_LINUX__
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#endif
#ifdef __USE_WAITPKG__
#include <immintrin.h>
#endif

/* ===== UTILITY MACROS ===== */
#ifdef _ENABLE_LOG
#define LOG(content, ...) printf(content"\n"  __VA_OPT__(,) __VA_ARGS__);
#else
#define LOG(content, ...)
#endif
#ifundef assert
#define assert(cond,content) if(cond) {/* do nothing*/} else { printf(content"\n"); exit(-1); }
#else
#error "please remove #include <assert.h>"
#endif

/* ========== protocol abstraction routine ========== */

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


typedef struct [[gnu::packed]] _host_dispatch_handshake_t {
  char flag_lowaddr;
  char flag_highaddr;
  unsigned short split_number;
  unsigned short bytes_per_split;
} host_dispatch_handshake_t;

enum dispatch_type {
  SHM,
  HUGE_SHM,
  FUNC
}

typedef void(*dispatch_func_t)(struct __disp_list* self);
//private struct
struct [[gnu::packed]] __disp_list {
  dispatch_func_t dispatch_function; // nullptr if SHM/HUGE_SHM
  unsigned int length; //include flag
  void* base_addr; //include flag
  dispatch_type d_t;
  struct __disp_list* next;
}
typedef struct [[gnu::packed]] _host_dispatch_t {
  struct __disp_list head;
  struct __disp_list end; //works only head >= 1,
  unsigned int total_length;
  void* start_addr;
} host_dispatch_t;


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
if dispatch_type == HUEG_SHM
  assert that the len % <bytes per split> == 0
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
void host_run_hello_handshake(host_handshake_t* out_ptr,void* addr) {
//todo roll back hello_handshake_system if it's not support waitpkg ISA
  if (is_support_umonitor()) {
    LOG("rolling back system");
    host_run_hello_handshake_system(out_ptr,addr);
    return;
  }
  host_handshake_t* dest = addr;
  __atomic_store_n(&dest,&out_ptr,__ATOMIC_RELEASE);
  //wait client
  char* flag = addr;
waiting:  
  _umonitor(flag);
  _umwait(1,0);
  if (__atomic_load_n(flag,__ATOMIC_ACQUIRE))
    goto waiting;
  return;
}
#endif

#ifdef __USE_WINDOWS__
//provids host_run_*_system function
#endif

ifdef __USE_LINUX__
//provids host_run_*_system function
#endif
