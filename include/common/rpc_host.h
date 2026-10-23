/*

重写:

为什么失败? 代码耦合性太高了 导致依赖循环 应该原生就内置context wrapper

这个系列不需要calling convention,因为需要指示的一般都是出现在编译器不知道的情况 例如跨编译器 或者汇编实现 而这个的函数定义和实现(.c .h)都是在同一个编译器内部 所以不需要指示 编译器会自动选择并且对应 不同的平台编译不同的就可以了

*/
#pragma once
#if !defined(__STDC_VERSION__) || !defined(__GNUC__) || (__STDC_VERSION__ < 2023L)
#error "wrong platform"
#endif


/*

  a abstract OS independent,rpc management API
  
  1. in host_hello_handshake stage the cap_mask is deprecated,set 1 for client_error,0 for ok
  2. the operations are 4-byte align guaranteed


*/


struct _host_raw_hh {
  union {
    struct {
      char flag;
      char _ignoreme1;
      char cap_mask; //deprecated,1 for client refuse, 0 for success
      char _ignoreme2;
    };
    unsigned int _ignoreme3;
  };
  unsigned int addr_length;
}; //hello handshake

struct _host_raw_dhh {
  alignas(4) union {
    struct {
      char flag;
      char error_code; //0 for ok
    };
    unsigned short _ignoreme;
  };
  unsigned short split_count;
  unsigned int bytes_per_split;
}; //dispatch handshake

static_assert(sizeof(struct _host_raw_hh) == 8);
static_assert(sizeof(struct _host_raw_dhh) == 8);

enum host_rpc_state {
  TERMINATE = -1,INIT = 0,HH,DHH,DH
};
enum host_dispatch_node_type {
  HUGE_SHM = 0x0,SHM = 0x1,FUNC = 0x2
};
//this is not RAW!
struct _host_dispatch_node;

typedef void(*rpc_routine_t)(struct _host_dispatch_node*);

struct _host_dispatch_node {
  struct _host_dispatch_node* next;
  void* start_addr;
  alignas(8) union {
    unsigned int length;
    rpc_routine_t func;
    //length and func is conflict
  };
  alignas(8) enum host_dispatch_node_type type;
};


/*
  Warning nullptr not checked
*/
extern struct _host_dispatch_node* host_rpc_create_node(
  enum host_dispatch_node_type type,
  void* start_addr,
  unsigned int length,
  rpc_routine_t* routine
);
 //malloc

inline void host_rpc_concat_node(struct _host_dispatch_node* prev,struct _host_dispatch_node* next)
{
  prev->next = next;
}

inline void host_rpc_del_node(struct _host_dispatch_node* node)
{
  free(node);
}

void host_rpc_remove_nodes(struct _host_dispatch_node* head); //remove a number of consequent nodes created by host_rpc_create_node
typedef struct _host_rpc_ctx host_rpc_ctx;
typedef bool(*host_rpc_waiter_t)(void* addr,host_rpc_ctx* rpc_ctx);
typedef bool(*host_rpc_notifier_t)(unsigned short number,host_rpc_ctx* rpc_ctx);
typedef struct _host_rpc_ctx {
  void* start_addr;
  unsigned int length;  
  alignas(8) union 
  {
    struct _host_raw_hh hello_handshake;
    struct _host_raw_dhh dispatch_handshake;
  };
  struct
  {
    struct _host_dispatch_node* disp_table;
  } dispatch_ctx;
  enum host_rpc_state cur_state;
  
  //enable 
  bool is_umonitor;
  //or enable
  host_rpc_waiter_t system_waiter; //not available while is_umonitor == true
  host_rpc_notifier_t system_notifier; //not available while is_umonitor == true
  
} host_rpc_ctx;

/*
  create a ctx build from params
  
  void* begin_addr : the base addr of context,not null(must be the mod of 64)

  uint length : the size of addr,not 0 (must be the mod of 64,power of 2)

  return: available ptr or nullptr
*/
extern host_rpc_ctx* host_rpc_new_ctx(void* begin_addr,unsigned int length);

/*
  delete a ctx created by host_rpc_new_ctx
  
  host_rpc_ctx* in_ptr nullable,if null will be ignored directly

  no return value,this function has no extra side-effect
*/
extern void host_rpc_del_ctx(host_rpc_ctx* in_ptr);

/*
  set rpcs system waiter,notifier
  ignored when ctx->is_umonitor == true

  see host_rpc.c,here's a example of introducing the system waiter,notifier
*/
extern bool host_rpc_set_system(host_rpc_ctx* ctx,host_rpc_waiter_t waiter,host_rpc_notifier_t notifier);

/*
  check the whether the host_rpc_ctx is ready(non-TERMINATE,handshake-able),do not check dispatch,failure when cur_state == DH or TERMINATE
*/
extern bool host_rpc_check(host_rpc_ctx* ctx);

/*
  check the whether the host_rpc_ctx is appropriate for different stage
*/
extern bool host_rpc_check_stage(host_rpc_ctx* ctx,enum host_rpc_state stage);


/*
  perform handshake,dispatch handshake,dispatch
  this routine blocks the program
  this routine changes the cur_state
*/
extern char host_rpc_handshake(host_rpc_ctx* ctx, unsigned short split_amount);

/*
  perform handshake,dispatch handshake,dispatch  asyncly
  this routine does NOT block the program
  this routine changes the cur_state
*/
//extern void host_rpc_handshake_async();

extern bool host_rpc_set_dispatch(
  host_rpc_ctx* ctx,
  struct _host_dispatch_node* attatched_node
);
/*
  perform dispatch,requires handshake before
  this routine does NOT block the program
  this routine changes the cur_state]
*/
extern bool host_rpc_dispatch(
  host_rpc_ctx* ctx
);
