/*
  implementation and internals for include/common/rpc_host.h
  a abstract OS independent,rpc management API
*/

#incude <stdio.h>
#include <stdlib.h>
#include <common/rpc_host.h>
#include <immintrin.h>

#if !defined(__STDC_VERSION__) || !defined(__GNUC__) || (__STDC_VERSION__ < 2023L)
#error "wrong stdc version"
#endif

//todo macro options check
#ifdef _ENABLE_LOG
#define LOG(ct,...) printf("[INFO]" ct "\n" __VA_OPT__(,) __VA_ARGS__);
#define WARN(ct,...) printf("[WARN]" ct "\n" __VA_OPT__(,) __VA_ARGS__);
#else
#define LOG(ct)
#define WARN(ct)
#endif

#define assert(cond,content)                      \
  do {                                            \
    if(cond) { /* pass */}                        \
    else { printf("[ASSERT]" content);exit(-1); } \
  } while(0)

#ifdef _ENABLE_WAITPKG
//check waitpkg
#if !__has_builtin(__builtin_ia32_umonitor)
#error "you defined _ENABLE_WAITPKG but the there's no support,please pass -mwaitpkg"
#endif
#endif

#ifdef _ENABLE_POSIX
#if !__has_include(<pthread.h>) && !__has_include(<unistd.h>)
#error "you defined _ENABLE_POSIX but there's no POSIX api header"
#endif
#endif

#ifdef _ENABLE_WIN32
#if !__has_include(<windows.h>)
#error "you defined _ENABLE_WIN32 but there's no windows.h"
#endif
#endif

#if !defined(_ENABLE_WAITPKG) && !defined(_ENABLE_POSIX) && !defined(_ENABLE_WIN32)
#pragma message "detect automaticly"

#if __has_include(<pthread.h>)
#define _ENABLE_POSIX
#elif __has_include(<windows.h>)
#define _ENABLE_WIN32
#endif
#if __has_builtin(__builtin_ia32_umonitor)
#define _ENABLE_WAITPKG
#else
#error "invalid platform"
#endif

//static sometype_t internal_default_SemaphoreOrFutex = null;
static char default_waiter(void* addr,void* rpc)
{
  host_rpc_ctx* ctx = rpc;
  //todo check the ctx for stage HH or DHH or DH
  //addr cannot be nullptr
  bool is_find = false;
  if((unsigned long)addr == ctx->start_addr)
    is_find = true;
  else {
    for(struct _host_dispatch_node cur_node = ctx->dispatch_ctx->disp_table->next; cur_node != nullptr; cur_node = p->next)
    {
      //check p
      if((unsigned long)addr == p->start_addr)
      {
        is_find = true;
      }
    }
  }
  if(is_find) {
    //todo block using internal_default_SemaphoreOrFutex
    //todo call qemu enter_monitor wait the notifier back
  }
}
static char default_notifier(unsigned int length,void* rpc)
{
  host_rpc_ctx* ctx = rpc;
  //todo check ctx
  //todo check the internal_default_SemaphoreOrFutex and unblock if exists
}
//tood export wrapped default_notifier to custom_request_notify
//calling sequence  host_wait -> qemu_inside -> qemu_add_eventfd -> qemu call notify -> host_notify -> host_wait wake up
//these custom_request_* related routine does not defined by rpc_host.c/h 



#endif
extern host_rpc_ctx* host_rpc_new_ctx(void* begin_addr,
   	     	                      unsigned int length)
{  
  assert(begin_addr != nullptr &&
         length != 0 &&
	 ((unsigned long)begin_addr & (64 - 1)) == 0 &&
	 (length & (64 - 1)) == 0 &&
	 (length & (length - 1)) == 0,"invalid param");
  host_rpc_ctx* alloc_ctx = malloc(sizeof(host_rpc_ctx));
  if(alloc_ctx == nullptr) {
    WARN("host failed to create ctx");
    return nullptr;
  }
  memset(alloc_ctx,0,sizeof(host_rpc_ctx));
  alloc_ctx->length = length;
  alloc_ctx->cur_state = INIT;
  alloc_ctx->is_umonitor = is_suppport_umonitor();
  alloc_ctx->start_addr = begin_addr;
  //todo add default waiter and notifier if is_umonitor == false
  return alloc_ctx;
}

extern void host_rpc_del_ctx(host_rpc_ctx* in_ptr)
{
  if(in_ptr == nullptr)
    return;
  free(in_ptr);
  return;
}

extern void host_rpc_set_system(host_rpc_ctx* ctx,host_rpc_waiter_t waiter,host_rpc_notifier_t notifier)
{
  //todo check the ctx for INIT stage
  if(ctx->is_umonitor) {
    //ignore
  }
  else {
    ctx->system_waiter = waiter;
    ctx->system_notifier = notifier;
  }
  return;
}
