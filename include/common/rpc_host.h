/*

重写:

为什么失败? 代码耦合性太高了 导致依赖循环 应该原生就内置context wrapper

这个系列不需要calling convention,因为需要指示的一般都是出现在编译器不知道的情况 例如跨编译器 或者汇编实现 而这个的函数定义和实现(.c .h)都是在同一个编译器内部 所以不需要指示 编译器会自动选择并且对应 不同的平台编译不同的就可以了

*/
#pragma once
#if !defined(__STDC_VERSION__) || !defined(__GNUC__) || (__STDC_VERSION__ < 2023L)
#error "wrong platform"
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
