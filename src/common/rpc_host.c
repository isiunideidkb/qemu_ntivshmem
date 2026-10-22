/*
  implementation and internals for include/common/rpc_host.h
  a abstract OS independent,rpc management API
*/

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
  } while(0);

#define WRITE_HANDSHAKE(ctx_ptr)                                  \
  do {                                                            \
    void* _value_ptr = &(ctx_ptr->hello_handshake)                \
    __myatomic_write(ctx_ptr->start_addr,_value_ptr,WRITE_8BYTE); \ //sharing the same address!
  while (0);
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
#if !defined(_ENABLE_POSIX) && !defined(_ENABLE_WIN32)
#pragma message "detect automaticly"

#if __has_include(<pthread.h>) && __has_include(<dlfcn.h>)
#pragma message "using POSIX API"
#define _ENABLE_POSIX
#elif __has_include(<windows.h>)
#pragma message "using Win32 API"
#define _ENABLE_WIN32
#endif

#if !defined(_ENABLE_POSIX) && !defined(_ENABLE_WIN32)
#error "can not detect the environment"
#endif

#endif

#endif
/*=============================*/

#include <stdlib.h>
#include <stdio.h>
#include <common/rpc_host.h>
#include <immintrin.h>
#include <common/atomicop.h>
#ifdef _ENABLE_POSIX
#include <pthread.h>
#include <dlfcn.h>
#endif

#ifdef _ENABLE_WIN32
#error "sorry, haven't implemented Win32 yet"
#include <windows.h>
#endif


[[gnu::sysv_abi]] extern __host_wait_umonitor(void* volatile addr);



extern struct _host_dispatch_node* host_rpc_create_node(enum host_dispatch_node_type type,
                                                        void* start_addr,
							unsigned int length,
							rpc_routine_t* routine)
{
  void * ptr = malloc(sizeof(struct _host_dispatch_node));
  if(!ptr)
    return ptr;
  ptr->type = type;
  ptr->start_addr = start_addr;
  if(type == FUNC)
    ptr->func = routine;
  else if (type == HUGE_SHM || type == SHM)
    ptr->length = length;
  else {
    WARN("invalid node type");
    free(ptr);
    return nullptr;
  }
}							
#pragma GCC push_options
#pragma GCC optimize("-O0")
void host_rpc_remove_nodes(struct _host_dispatch_node* head)
{
  struct _host_dispatch_node* cur_node = head->next;
  struct _host_dispatch_node* prev_node = head;
  //do not fucking eliminate this LOOP
  for(;cur_node != nullptr; host_rpc_del_node(prev_node), (prev_node = cur_node), (cur_node = cur_node->next)){}
  if(prev_node) //delete the one last omitted prev_node if exists
    host_rpc_del_node(prev_node);
}
#pragma GCC pop_options


static bool is_init = false;
static bool default_waiter(void* addr,host_rpc_ctx* rpc_ctx)
{
#ifdef _ENABLE_POSIX
  if (!is_init) {
    void* ptr = dlsym(RTLD_DEFAULT,"custom_request_init_monitor");
    if(!ptr)
      return false;
    bool(*func)(struct _host_dispatch_node*) = (__typeof__(func))ptr;
    bool ret = func(&(rpc_ctx->dispatch_ctx.disp_table));
    if(ret)
      is_init = true;
    return ret;
  }
  void* ptr = dlsym(RTLD_DEFAULT,"custom_request_inhabit_monitor");
  if(!ptr)
    return false;
  bool(*func)(void*) = (__typeof__(func))ptr;
  return func(addr);
#else
  return false;
#endif

}
static bool default_notifier(unsigned int number,host_rpc_ctx* rpc_ctx)
{
  return false;
}
//tood export wrapped default_notifier to custom_request_notify
//calling sequence  host_wait -> qemu_inside -> qemu_add_eventfd -> qemu caught the eventfd(guest wrote the data which host want) -> directly return -> host_wait_return
//need no check the flag
//these custom_request_* related routine does not defined by rpc_host.c/h


extern host_rpc_ctx* host_rpc_new_ctx(
  void* begin_addr,
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
  //todo add default waiter and notifier if is_umonitor == false : ok
  if(! (alloc_ctx->is_umonitor)) {
    alloc_ctx->system_waiter = default_waiter;
    alloc_ctx->system_notifier = default_notifier;
  }
  return alloc_ctx;
}

extern void host_rpc_del_ctx(host_rpc_ctx* in_ptr)
{
  if(in_ptr == nullptr)
    return;
  struct _host_dispatch_node* head = in_ptr->dispatch_ctx.disp_table
  if(head)
    host_rpc_remove_nodes(head);
  free(in_ptr);
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
  if(ctx->cur_state == INIT)
    return stat;
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
    return host_rpc_check(ctx) && (unsigned long)ctx ? ( ctx->cur_state == stage ) : false;
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
       (addr & (64 - 1)) != 0)
      goto failure;
    //addr本身就是length第1个 但是length并不包括0 从计数来看 想要转成addr对应的偏移只能-1
    if((addr - (initial_length - 1)) != (unsigned long)(ctx->start_addr))
      goto failure;
    // put behind length checking behind the particular type checking
    switch(cur_node->type) {
      case HUGE_SHM:
        if(len == 0 ||
	  (len & (len - 1)) != 0 ||
          (len & (64 - 1)) != 0 )
          goto failure;
        if( (len % (ctx->dispatch_handshake.bytes_per_split)) != 0)
	  goto failure;
        break;
      case SHM:
        if(len == 0 ||
	  (len & (len - 1)) != 0 ||
          (len & (64 - 1)) != 0 )
          goto failure;      
        if(len != (ctx->dispatch_handshake.bytes_per_split))
	  goto failure;
        break;
      case FUNC:
        if(!len)
	  goto failure;      
        break;
      default: //not anyone of host_dispatch_node_type
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


extern void host_rpc_set_system(host_rpc_ctx* ctx,host_rpc_waiter_t waiter,host_rpc_notifier_t notifier)
{
  //todo check the ctx for INIT stage
  if (!host_rpc_check(ctx))
    return false;
  if(ctx->is_umonitor) {
    //ignore
  }
  else {
    ctx->system_waiter = waiter;
    ctx->system_notifier = notifier;
  }
  return;
}

static bool host_hello_handshake(host_rpc_ctx* ctx)
{
  WRITE_HANDSHAKE(ctx);
  bool* cap_mask_addr = ((char*)(ctx->start_addr))+2;  
  if(ctx->is_umonitor) {
    __host_wait_umonitor(ctx->start_addr);
    return false;
  }
  else {
    if(!(ctx->system_waiter))
      return false;
    (ctx->system_waiter)(ctx->start_addr);
  }
  bool is_error = *cap_mask_addr;
  memset(
    &(ctx->hello_handshake),
    0,
    sizeof(struct _host_raw_hh)
  );
  WRITE_HANDSHAKE(ctx);
  return is_error;
}


static bool host_dispatch_handshake(host_rpc_ctx* ctx)
{
  WRITE_HANDSHAKE(ctx);
  bool* cap_mask_addr = ((char*)(ctx->start_addr))+1;  
  if(ctx->is_umonitor) {
    __host_wait_umonitor(ctx->start_addr);
  }
  else {
    LOG("using system waiter");
    if(!(ctx->system_waiter))
      return false;
    (ctx->system_waiter)(ctx->start_addr);
  }
  bool is_error = *cap_mask_addr;
  struct _host_raw_dhh* padded_sample = { 0 };
  WRITE_HANDSHAKE(padded_sample);
  //reserve the dhh for next stage checking(DH checking will access the field)
  return is_error;
}


/*

  exit code

  0 OTHER ERROR  bool compatible

  1 HH CLIENT ERROR 

  2 DH CLIENT ERROR

  3 OK

*/
char host_rpc_handshake(host_rpc_ctx* ctx, unsigned short split_amount)
{
  if (!host_rpc_check_stage(ctx,INIT)) {
    ((unsigned long)ctx ? (ctx->cur_state = TERMINATE) : 6767), (stat_code = 0);
    return (char)0;
  }
  ctx->cur_state = HH;
  ctx->hello_handshake.flag = (char)1;
  ctx->hello_handshake.cap_mask = (char)0;
  ctx->hello_handshake.addr_length = ctx->length;
  if (!host_rpc_check_stage(ctx,HH)) {
    (ctx->cur_state = TERMINATE), (stat_code = 0);
    return (char)1;
  }    
  if (host_hello_handshake(ctx)) { //err = 1,i.e. true
    (ctx->cur_state = TERMINATE), (stat_code = 0);
    return (char)1;
  }
  ctx->cur_state = DHH;
  ctx->dispatch_handshake.flag = (char)1;
  ctx->dispatch_handshake.error_code = (char)1;
  ctx->split_count = split_amount;
  ctx->bytes_per_split = (unsigned int)((ctx->length) / split_amount);
  if (!host_rpc_check_stage(ctx,DHH)) {
    (ctx->cur_state = TERMINATE), (stat_code = 0);
    return (char)2;
  }
  if (host_dispatch_handshake(ctx)) { //err = 1,i.e. true
    (ctx->cur_state = TERMINATE), (stat_code = 0);
    return (char)2;
  }  
  return (char)3;
}

extern bool host_rpc_set_dispatch(
  host_rpc_ctx* ctx,
  struct _host_dispatch_node* attatched_node
)
{
  if (!host_rpc_check_stage(ctx,DHH)) {
    (unsigned long)ctx ? (ctx->cur_state = TERMINATE) : 6767;
    return false;
  }
  struct _host_dispatch_node* head = ctx->dispatch_ctx.disp_table;
  host_rpc_concat_node(head,attatched_node);
  return true;
}

[[gnu::noreturn]] static void* host_dispatch_one_node_routine(void* ptr,bool is_umonitor)
{

  char** ptr_ptr = (char**)ptr;
  struct _host_dispatch_node* cur_node = (struct _host_dispatch_node*)*ptr_ptr;
  char** ptr_ptr2 = (ptr_ptr+(sizeof(void*)-1));
  [[gnu::unused]] host_rpc_waiter_t system_waiter = (host_rpc_waiter_t)(*ptr_ptr2);
  char flag_tobew[8] = {
    [1] = (char)0,
    [3] = (char)0,  //padded
    [0] = (char)1,
    [2] = (char)(cur_node->type),
    0,0,0,0 //padded if HUGE_SHM does not exist
  };
  //与前面的initial_length不同 C语言数组访问默认是从0开始的 所以就是第1+四个(5个) 正好是HUGE_SHM len的起始地址
  int* length_ptr_hshm = &(flag_tobew[4]);
  void* start_addr = cur_node->start_addr;
  void* write_value_addr = flag_tobew;
  if(cur_node->type == HUGE_SHM)
  {
    *length_ptr_hshm = cur_node->length;
    __myatomic_write(start_addr,write_value_addr,WRITE_8BYTE);
  }
  else {
    __myatomic_write(start_addr,write_value_addr,WRITE_4BYTE);
  }
LOOP:  
  if(is_umonitor) {}
    __host_wait_umonitor(start_addr);
  else
    system_waiter(start_addr);
  if(cur_node->type == FUNC && cur_node->func != nullptr)
    func(cur_node);
  goto LOOP;
  pthread_exit(nullptr);
  return nullptr;
}

//wrapper
static inline void* host_dispatch_one_node_routine_system(void* ptr)
{
  return host_dispatch_one_node_routine(ptr,false);
}
static inline void* host_dispatch_one_node_routine_umonitor(void* ptr)
{
  return host_dispatch_one_node_routine(ptr,true);
}

extern bool host_rpc_dispatch(
  host_rpc_ctx* ctx
)
{
  ctx->cur_state = DH;
  if (!host_rpc_check_stage(ctx,DH)) {
    (unsigned long)ctx ? (ctx->cur_state = TERMINATE) : 6767;
    return false;  
  }
  struct _host_dispatch_node* cur_node = ptr;
  while(true)
  {
#ifdef _ENABLE_POSIX
    if(cur_node == nullptr)
      break;
    pthread_t _placeholder;
    //malloc(sizeof(void*)*2); //specific waiter and
    if(ctx->is_umonitor)
      pthread_create(&_placeholder,nullptr,host_dispatch_one_node_routine_umonitor,(void*)cur_node);
    else {
      LOG("using system waiter for dispatching");
      char* base_ptr = malloc(sizeof(void*)*2); //specific waiter and
      char** node_ptr = (char**)base_ptr;
      //和上面同理 sizeof返回的是length不包括0 但是base_ptr包括0
      char** waiter_ptr = (char**)(base_ptr+(sizeof(void*)-1));
      *node_ptr = (char*)(cur_node);
      *waiter_ptr = (char*)(ctx->system_waiter);
      pthread_create(&_placeholder,nullptr,host_dispatch_one_node_routine_system,(void*)cur_node);
    }
    pthread_detach(_placeholder);
    cur_node = cur_node->next;
#else
    return false;
#endif  
  }
  return true;
}
