#incude <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <common/rpc_host.h>

//todo macro options check
#ifdef _ENABLE_LOG
#define LOG(ct) printf("[INFO]" ct "\n");
#define WARN(ct) printf("[WARN]" ct "\n");
#else
#define LOG(ct)
#define WARN(ct)
#endif

#ifdef _ENABLE_WAITPKG
//check waitpkg
#endif

#ifdef _ENABLE_POSIX
#endif

#ifdef _ENABLE_WIN32
#endif

#if !defined(_ENABLE_WAITPKG) && !defined(_ENABLE_POSIX) && !defined(_ENABLE_WIN32)
//automatic scan
#endif
