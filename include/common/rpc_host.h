/*

重写:

为什么失败? 代码耦合性太高了 导致依赖循环 应该原生就内置context wrapper

*/
#pragma once
#if !defined(__STDC_VERSION__) || !defined(__GNUC__)
#error "wrong platform"
#endif

#if (__STDC_VERSION__ < 2023L) && (__STDC_VERSION__ > 1999L)
#define nullptr NULL
#define bool _Bool
#define true (char)1
#define false (char)0
#endif

//todo compiation option selector

//todo utility macro

alignas(4) struct _host_raw_hh {
  union {
    struct {
      char flag;
      char _ignoreme1;
      char cap_mask;
      char _ignoreme2;
    };
    unsigned int _ignoreme3;
  };
  unsigned int addr_length;
}; //hello handshake

alignas(4) struct _host_raw_dhh {
  alignas(4) union {
    struct {
      char flag;
      char error_code;
    };
    unsigned short _ignoreme;
  };
  unsigned short split_count;
  unsigned int bytes_per_split;
}; //dispatch handshake

static_assert(sizeof(struct _host_raw_hh) == 8);
static_assert(sizeof(struct _host_raw_dhh) == 8);

extern bool host_rpc_check_hh();
extern bool host_rpc_check_dhh();

enum host_rpc_state {
  NONE = 0,HH,DHH,DH
};
struct _host_dispatch_node {
  struct _host_dispatch_table* next;
  void* begin_addr;
  unsigned int length;
};
typedef struct _host_rpc_ctx {
  void* start_addr;
  alignas(8) union handshake_area {
    struct _host_raw_hh hello_handshake;
    struct _host_raw_dhh hello_dispatch_handshake;
  };
  struct {
    void* begin_addr;
    unsigned int length;
    struct _host_dispatch_node* disp_table;
    struct _host_dispatch_node* disp_table_end;
  } dispatch_ctx;
  enum host_rpc_state cur_state;
} host_rpc_ctx;

extern void host_rpc_check_ctx();
extern void host_rpc_new_ctx();
extern void host_rpc_del_ctx();
extern void host_rpc_startup();
