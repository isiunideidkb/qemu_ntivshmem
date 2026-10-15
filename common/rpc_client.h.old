/*
  coding style C23 + GCC(MinGW)
  clang-format --style=LLVM
  common rpc protocol for host
  use nullptr instead of NULL
*/

/* ===== PLATFORM CHECK ===== */
#pragma once
#ifndef __GNUC__
#error "wrong platform"
#endif
#if __STDC_VERSION__ < 2023L
#error "wrong stdc version"
#endif

/* ===== PLATFORM SWITCH ===== */
// clean predefs
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

// ENABLE_* manually selected macro,must be platform suited(throw error if it's
// not matched by given target config) note: WAITPKG is optionally,but can not
// appeared as single option,paralleled with system api
#if defined(ENABLE_WINDOWS)  // system api
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

// note : !! ENABLE_WAITPKG does not mean is_support_umonitor == true, in the
// same vein is_support_umonitor == true does not mean WAIT_PKG routines is not
// empty!
#if defined(ENABLE_WAITPKG)  // note : waitpkg is paralled with system specific
                             // api
/* ===== cond3 ===== */
#if __has_builtin(__builtin_ia32_umwait) && \
    __has_builtin(__builtin_ia32_umonitor)
#define __USE_WAITPKG__
#pragma message "using waitpkg intrinsics"
#else
#error "making sure you specified -mwaitpkg option"
#endif
/* ===== cond3 ===== */
#endif

#if !defined(ENABLE_WINDOWS) && !defined(ENABLE_WAITPKG) && \
    !defined(ENABLE_LINUX)
/* ===== cond4 ===== */
#pragma message "mathing automaticly"
#if __has_builtin(__builtin_ia32_umwait) && \
    __has_builtin(__builtin_ia32_umonitor)
#pragma message "using waitpkg"
#define __USE_WAITPKG__
#endif
// note : waitpkg is paralled with system specific api
#if defined(__unix__)
#pragma message "using unix api"
#define __USE_UNIX__
#elif __has_include(<linux/futex.h>) && defined(__gnu_linux__)
#pragma message "using linux api"
#define __USE_LINUX__
#elif __has_include(<windows.h>) && defined(__MINGW32__)
#pragma message "using win32 api"
#define __USE_WINDOWS__
#else  // note: waitpkg cannot appeared as a sinlge option
#error "can not select the platform api"
#endif
/* ===== cond4 ===== */
#endif

/* ===== HEADERS ===== */
#include <common/atomicop.h>
#include <common/umonitor_check.h>
#include <immintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __USE_WINDOWS__
#include <windows.h>
#endif
#ifdef __USE_LINUX__
#endif
#ifdef __USE_UNIX__
#include <pthread.h>
#include <unistd.h>
#endif
#ifdef __USE_WAITPKG__
#endif

/* ===== UTILITY MACROS ===== */
#ifdef _ENABLE_LOG
#define LOG(content, ...) printf(content "\n" __VA_OPT__(, ) __VA_ARGS__);
#else
#define LOG(content, ...)
#endif
#ifndef assert
#define assert(cond, content) \
  if (cond) { /* do nothing*/ \
  } else {                    \
    printf(content "\n");     \
    exit(-1);                 \
  }
#else
#error "please remove #include <assert.h>"
#endif
#define PRIVATE_FUNC static
#define READ_HELLO_HANDSHAKE(addr, hh_out)                                    \
  do {                                                                        \
    __myatomic_read((void*)(((char*)addr) + 1), (void*)(&(hh_out->cap_mask)), \
                    READ_BYTE);                                               \
    __myatomic_read((void*)(((char*)(addr)) + 2),                             \
                    (void*)(&(hh_out->_1_length)), READ_4BYTE);               \
    __myatomic_read(addr, (void*)(&(hh_out->hello_message)), READ_BYTE);      \
  } while (0)
#define READ_DISPATCH_HANDSHAKE(addr, hdh_out)                           \
  do {                                                                   \
    __myatomic_read((void*)(((char*)addr) + 1),                          \
                    (void*)(&(hdh_out->flag_highaddr)), READ_BYTE);      \
    __myatomic_read((void*)(((char*)addr) + 2),                          \
                    (void*)(&(hdh_out->split_number)), READ_2BYTE);      \
    __myatomic_read((void*)(((char*)addr) + 4),                          \
                    (void*)(&(hdh_out->bytes_per_split)), READ_4BYTE);   \
    __myatomic_read(addr, (void*)(&(hdh_out->flag_lowaddr)), READ_BYTE); \
  } while (0)

// 1byte!
typedef void (*ssw_t)(void* addr);
// quickdoc: system_specific_waiter used by host_run_*_system routines,if this
// variable is not null,system_specific_waiter is prefered this waiter receives
// a addr,listens the modification in 1byte region this variable is private
// linkage(i.e. static)
static ssw_t system_specific_waiter = nullptr;
void set_client_system_specific_waiter(void* target) {
  system_specific_waiter = (ssw_t)(target);
  return;
}
