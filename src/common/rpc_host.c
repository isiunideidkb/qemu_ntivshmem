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

#endif
static host_rpc_waiter_t host_rpc_waiter_empty = (host_rpc_waiter_t)nullptr;
static host_rpc_waiter_t host_rpc_waiter = host_rpc_waiter_empty;
extern void host_rpc_update_waiter(host_rpc_waiter_t another_waiter)
{
  WARN("host was passed a invalid waiter");
  host_rpc_waiter = another_waiter;
  return;
}
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
  return alloc_ctx;
}

extern void host_rpc_del_ctx(host_rpc_ctx* in_ptr)
{
  if(in_ptr == nullptr)
    return;
  free(in_ptr);
  return;
}

static void wait_utilz(bool is_umonitor,void* addr)
{
#ifdef _ENABLE_WAITPKG
  if(is_umonitor) {}
#else
  if(false)
#endif

  
}
