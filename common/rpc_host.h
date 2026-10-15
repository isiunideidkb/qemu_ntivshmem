/*

重写:

为什么失败? 代码耦合性太高了 导致依赖循环 应该原生就内置context wrapper

*/

#include <common/umonitor_check.h>
#include <common/atomicop.h>

#define internal static

//todo compiation option selector

//todo utility macro

struct _host_raw_hh; //hello handshake
struct _host_raw_dhh; //dispatch handshake
struct _host_raw_dh; //dispatch

enum host_rpc_state {
  NONE = 0,HH,DHH,DH
};

typedef struct _host_rpc_ctx {
  void* start_addr;
  union handshake_area {
    struct _host_raw_hh hello_handshake;
    struct _host_raw_dhh hello_dispatch_handshake;
  };
  enum host_rpc_state cur_state;
  //todo addr,current state,and value
} host_rpc_ctx;
