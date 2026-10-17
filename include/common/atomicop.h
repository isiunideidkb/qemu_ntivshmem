#pragma once

#if !defined(__STDC_VERSION__) || !defined(__GNUC__) || (__STDC_VERSION__ < 2023L)
#error "wrong stdc version"
#endif


#define WRITE_BYTE 0
#define READ_BYTE 0
#define WRITE_2BYTE 1
#define READ_2BYTE 1
#define WRITE_4BYTE 2
#define READ_4BYTE 2
#define WRITE_8BYTE 3
#define READ_8BYTE 3

//todo switch to assebmly implementation
[[gnu::sysv_abi]] bool __myatomic_write(void* volatile _in_addr, void* volatile _in_value_addr,
                      unsigned int number);

[[gnu::sysv_abi]] bool __myatomic_read(void* volatile in_addr, void* volatile out_addr,
                     unsigned int number);
