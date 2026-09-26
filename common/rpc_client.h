/*
  coding style c23
  clang-format --style=LLVM
  platform independent rpc protocol for client
  compile with -mwaitpkg
*/

#ifundef __GNU__
#error "wrong platform"
#endif

#if __STDC_VERSION__ < 2023L
#error "wrong stdc version"
#endif
