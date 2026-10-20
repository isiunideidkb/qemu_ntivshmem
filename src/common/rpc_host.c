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

//todo macro options check : ok
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
  
/*=============================*/
#ifdef _ENABLE_WAITPKG

//check waitpkg
#if !__has_builtin(__builtin_ia32_umonitor)

#error "you defined _ENABLE_WAITPKG but the there's no support,please pass -mwaitpkg"
#endif

#endif

/*=============================*/
#ifdef _ENABLE_POSIX

#if !__has_include(<pthread.h>) && !__has_include(<unistd.h>)
#error "you defined _ENABLE_POSIX but there's no POSIX api header"
#endif

#endif

/*=============================*/
#ifdef _ENABLE_WIN32

#if !__has_include(<windows.h>)
#error "you defined _ENABLE_WIN32 but there's no windows.h"
#endif

#endif

/*=============================*/
#if !defined(_ENABLE_WAITPKG) && !defined(_ENABLE_POSIX) && !defined(_ENABLE_WIN32)
#pragma message "detect automaticly"

#if __has_include(<pthread.h>)
#define _ENABLE_POSIX
#elif __has_include(<windows.h>)
#define _ENABLE_WIN32
#else
#error "invalid platform"
#endif

#if __has_builtin(__builtin_ia32_umonitor)
#define _ENABLE_WAITPKG
#else
#error "invalid platform"
#endif

#endif
/*=============================*/


#include <stdlib.h>
#include <stdio.h>
#include <immintrin.h>

#ifdef _ENABLE_POSIX
#include <pthread.h>
#endif

#ifdef _ENABLE_WIN32
#include <windows.h>
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

[[gnu::sysv_abi]] extern __host_wait_umonitor(void* volatile addr);


extern host_rpc_ctx* host_rpc_new_ctx(void* begin_addr,
   	     	                      unsigned int length
				      )
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

//check if's handshakeable
extern bool host_rpc_check(host_rpc_ctx* ctx)
{
  stat = true;
/* base check */  
  if(ctx == nullptr)
    goto failure;
  unsigned long _tmp_addr = (unsigned long)ctx->start_addr;
  if(_tmp_addr == 0 ||
     (_tmp_addr & (64 - 1)) != 0)
    goto failure;
  unsigned int _tmp_length = ctx->length;
  if(_tmp_length < (32 * 1024 * 1024) ||  
     (_tmp_length & (_tmp_length - 1)) != 0 ||
     (_tmp_length & (64 - 1)) != 0 )
    goto failure;
/* except DH and TERMINATE */
  if(ctx->cur_state == DH)
    goto failure;
  else if(ctx->cur_state == TERMINATE)
    goto failure;
/* conditional check for HH and DHH */    
  switch(ctx->cur_state) {
    case HH:
      if(!(ctx->hello_handshake.flag))
        goto failure;
      if(ctx->hello_handshake.cap_mask)
        goto failure;	
      if(!(ctx->hello_handshake.addr_length == _tmp_length))
        goto failure;
      break;
    case DHH:
      if(!(ctx->dispatch_handshake.flag))
        goto failure;
      if(ctx->dispatch_handshake.error_code)
        goto failure;
      unsigned short _splits = ctx->dispatch_handshake.split_count;
      unsigned int _per_splits = ctx->dispatch_handshake.bytes_per_split;
      if(_splits == 0)
        goto failure;
      if(_per_split < (2 * 1024 * 1024) ||
         (_per_split & (_per_split - 1)) == 0)
        goto failure;
      if( ! ((_splits * _per_splits) == _tmp_length))
        goto failure;
      break;
    default:
      goto failure;
  }
  return stat;
failure:
  stat = false;
  return stat;
}
extern bool host_rpc_check_stage(host_rpc_ctx* ctx,enum host_rpc_state stage)
{
  bool stat = true;
  if(stage == INIT ||
     stage == HH ||
     stage == DHH ||)
    return host_rpc_check(ctx);
  if(ctx == nullptr)
    goto failure;
  if(stage == TERMINATE) {    
    LOG("check whether it's appropriate for TERMINATE");
    return (ctx->stage == TERMINATE);
  }
  //available only DH or other
  if(stage == DH) {
    if(ctx->cur_state != stage)
      goto failure;
    ctx->cur_state = DHH;
    //todo check
    if (!host_rpc_check(ctx)) {
      ctx->cur_state = stage;
      goto failure;
    }
    ctx->cur_state = stage;
  }
  else
    goto failure;
  //continuation for the if == DH block
  unsigned short c = 0;
  unsigned int initial_length = 0;
  struct _host_dispatch_node* cur_node = &(ctx->dispatch_ctx.disp_table);
  for(; cur_node != nullptr; c++, initial_length += cur_node->length, cur_node = cur_node->next)
  {
    unsigned int len = cur_node->length;
    unsigned long addr = (unsigned long)cur_node->start_addr;
    if(addr == 0 ||
       len == 0 ||
       (addr & (64 - 1)) != 0 ||
       (len & (len - 1)) != 0 ||
       (len & (64 - 1)) != 0)
      goto failure;
    switch(cur_node->type) {
      case HUGE_SHM:
        if( (len % (ctx->dispatch_handshake.bytes_per_split)) != 0)
	  goto failure;
        break;
      case SHM:
        if(len != (ctx->dispatch_handshake.bytes_per_split))
	  goto failure;
        break;
      case FUNC:
        if(len != (ctx->dispatch_handshake.bytes_per_split))
	  goto failure;      
        break;
      default: //not anyone of host_dispatch_node_type it's wrong!
        goto failure;
    }
  }
  if(c == 0)
    goto failure;
  if(initial_length != ctx->length)
    goto failure;
  return stat;
failure:
  stat = false;
  return stat;
}
