/*
  cross platform OS independent atomic utility
  read: write back memory is required,using memory bariier to guarantee read
  operation will not- reordered before last write operation

  write: write back memory is required as well,use mov or xchg?

  exactly since P6,lock prefix operations will use cache lock rather than bus
  lock in appropriate cases

  process order will also affect the order of read&write on shared
  memory,although the read&write are lock prefix operation(e.g. XCHG)

  !!note!! split the high,low and store then respectively,in ordinary
  concurrency cases are dangerous,because of the store order are interruptible
  - but in here,the order can be guarantee to un-interruptible as long as the
  proram obey the standard RPC protocol

  why not use GCC atomic intrinsics? emm... actually the GCC atomic intrinsics
  can not guarantee atomic-ity without OS participation,and gcc aotmic
  intrinsics will roll-back to software implementation in some cases notes as
  to read operations for greater than single byte: the low-level assembly
  implementation are ordinary MOV instructions for READ OP,because MOV
  instruction of x86 can guarantee atomic in most cases,but there's some
  exceptions: read op greater than single byte,if the addr is unaligned(see
  IA32 SDM) or cross cache line,the CPU may not provide atomic-ity for this
  operation,but the standard memory access sequence of RPC standard still
  guarantee that the read will not be raced by other processor although the CPU
  can not provide atoimc-ity for read operation.likewise,i used the "lfence"
  instruction to preventing the reordering of program as much as possible. but
  i'm still confused about whether the processor guarantees the operation read
  out old value see IA32SDM strategy of WB memory caching,memory order,bus
  locks

  https://stackoverflow.com/questions/80012673/in-exactly-what-situations-does-loads-may-be-reordered-with-earlier-stores-to-d

  macro options:
  _ENABLE_LOG : enables log for read option, recommended
  _USE_CMPXCHG: use cmpxchg for global atomic operation, enabled by default
  _USE_MOV: use mov for read for global atomic operation
  至少来说 write会有一个隐式的屏障 如果你是 write-read 你可以省去屏障 如果你读的地方期待的WRITE并没有使用LOCK指令换句话说也就是内建写的 那么你应该额外加上 屏障宏RPC_READ_BARRIER




  todo 完整的大页操作 因为默认的WB所以我们只支持目标 源NT的一些基本操作 分成两个版本 原子和不原子 并且都能编译

  <isa>_<dest_memtype>_<op>(void* src,void* dest,unsigned long length)
  note: 很遗憾 x86上的MOVNT 以及AVX SSE均不保证！不保证原子性 我们只需要保证不重排(no-reorder)即可
  不条件编译 尽量运行时候判断 或者不判断 丢给调用我的 其他操作也如此
  isa: 有
    AVX
    AVX2
    SSE3
    GPI(General Purpose Instruction 一般在Intel使用ERMS AMD使用rep movs/stos)
  memtype:
    ca(cached)
    nt(non-temporal)
    注 src 的memtype是固定ca 的 这里压根没有名字标注
  op:
    memswap
    memmove
*/

#include <stdio.h>
#include <stdlib.h>
#ifdef __gnu_linux__
#include <execinfo.h>
#endif

#if !defined(__STDC_VERSION__) || !defined(__GNUC__) || (__STDC_VERSION__ < 2023L)
#error "wrong stdc version"
#endif


#if !defined(_USE_MOV) && !defined(_USE_CMPXCHG)
#define _USE_CMPXCHG
#endif

#define CACHE_LINE_SIZE 64

static inline bool check_op(void* _src,void* _dest,unsigned long length) {
  unsigned long src = (unsigned long)_src;
  unsigned long dest = (unsigned long)_dest;
  if((!src) || (src & (64 - 1)) != 0)
    return false;
  if((!dest) || (dest & (64 - 1)) != 0)
    return false;
  if((!length) || (length & (64 - 1)) != 0)
    return false;
  return true;
}

[[noreturn]]
void _backtrace_and_exit() {
#ifdef __gnu_linux__
  void* stack_ptrs[10];
  char** strings;
  int avail_size;
  avail_size = backtrace(stack_ptrs, 10);
  strings = backtrace_symbols(stack_ptrs, avail_size);
  if (strings != nullptr) {
    printf("=====STACK TRACE INFO=====\n");
    for (int i = 0; i < avail_size; i++)
      printf("stack NO. %d: %s \n", i, strings[i]);
  }
#endif
  exit(-1);
}


void __myatomic_erms_ca_memmove(void* src,void* dest,unsigned long length) {
  if(!check_op(src,dest,length))
    _backtrace_and_exit();
  asm volatile(
    "movq %[src],%%rsi \n\t"
    "movq %[dest],%%rdi \n\t"
    "mov %[length],%%rcx \n\t"
    "rep movsb \n\t"
    "mfence \n\t" //为什么不sfence 因为在这里我们要求的不仅是写入全部完毕 我们还要求不会重排 也就是写入一定先于所有读不重排 sfence无法保证
    : /* nothing */
    : [src]"rm"(src),[dest]"rm"(dest),[length]"rm"(length)
    : "rdi","rsi","rcx"
  );    
}
//__myatomic_erms_ca_memswap(void* src,void* dest,unsigned long length);

//__myatomic_sse_ca_memmove(void* src,void* dest,unsigned long length);
//__myatomic_sse_ca_memswap(void* src,void* dest,unsigned long length);
//__myatomic_sse_nt_memmove(void* src,void* dest,unsigned long length);
//__myatomic_sse_nt_memswap(void* src,void* dest,unsigned long length);

//__myatomic_avx_ca_memmove(void* src,void* dest,unsigned long length);
//__myatomic_avx_ca_memswap(void* src,void* dest,unsigned long length);
//__myatomic_avx_nt_memmove(void* src,void* dest,unsigned long length);
//__myatomic_avx_nt_memswap(void* src,void* dest,unsigned long length);





//ERMS
static void __myatomic_memcpy_cached_rep(void* src,void* dest,unsigned long length) {
  bool src_err = ((unsigned long)src & (64 - 1));
  bool dest_err = ((unsigned long)dest & (64 - 1));
  bool len_err = (length & (64 - 1));
  if(src_err || dest_err || len_err)
    _backtrace_and_exit();
  asm volatile(
    "movq %[src],%%rsi \n\t"
    "movq %[dest],%%rdi \n\t"
    "mov %[length],%%rcx \n\t"
    "rep movsb \n\t"
    "mfence \n\t" //为什么不sfence 因为在这里我们要求的不仅是写入全部完毕 我们还要求不会重排 也就是写入一定先于所有读不重排 sfence无法保证
    : /* nothing */
    : [src]"rm"(src),[dest]"rm"(dest),[length]"rm"(length)
    : "rdi","rsi","rcx"
  );
  return;
}


#pragma GCC push_options
#pragma GCC target("avx")
//使用ymm0 ymm1
static void __myatomic_memcpy_nt_avx(void* src,void* dest,unsigned long length) {
  bool src_err = ((unsigned long)src & (64 - 1));
  bool dest_err = ((unsigned long)dest & (64 - 1));
  bool len_err = (length & (64 - 1));
  if(src_err || dest_err || len_err)
    _backtrace_and_exit();  
  for(unsigned long offset = 0; offset < length; offset += 64) {
    asm volatile(
      "vmovdqa (%[base],%[off],1),%%ymm0 \n\t"
      "vmovdqa 32(%[base],%[off],1),%%ymm1 \n\t"
      "vmovntdq %%ymm0,(%[dest],%[off],1) \n\t"
      "vmovntdq %%ymm1,32(%[dest],%[off],1) \n\t"
      "sfence \n\t"
      "clflushopt (%[dest],%[off],1)\n\t"
      :
      : [base] "r"(src), [dest] "r"(dest), [off] "r"(offset)
      : "ymm0", "ymm1"
    );
  }
}
#pragma GCC pop_options

//如果不支持AVX 可以试试SSE2 xmm(128) 16字节
#pragma GCC push_options
#pragma GCC target("sse2")
static void __myatomic_memcpy_nt_sse(void* src,void* dest,unsigned long length) {
  bool src_err = ((unsigned long)src & (64 - 1));
  bool dest_err = ((unsigned long)dest & (64 - 1));
  bool len_err = (length & (64 - 1));
  if(src_err || dest_err || len_err)
    _backtrace_and_exit();  
  //单次step 64字节 xmm0 xmm1 xmm2 xmm3
  for(unsigned long offset = 0; offset < length; offset += 64) {
    asm volatile(
      "movdqa (%[base],%[off],1),%%xmm0 \n\t"
      "movdqa 16(%[base],%[off],1),%%xmm1 \n\t"
      "movdqa 32(%[base],%[off],1),%%xmm2 \n\t"
      "movdqa 48(%[base],%[off],1),%%xmm3 \n\t"
      "movntdq %%xmm0,(%[dest],%[off],1) \n\t" //注意这是提示不是真的 所以后面尽量需要sfence和clflushopt
      "movntdq %%xmm1,16(%[dest],%[off],1) \n\t"
      "movntdq %%xmm2,32(%[dest],%[off],1) \n\t"
      "movntdq %%xmm3,48(%[dest],%[off],1) \n\t"
      "sfence \n\t"
      "clflushopt (%[dest],%[off],1)\n\t"
      :
      : [base] "r"(src), [dest] "r"(dest), [off] "r"(offset)
      : "xmm0", "xmm1", "xmm2", "xmm3"
    );
  }
}
#pragma GCC pop_options



[[gnu::sysv_abi]] bool __myatomic_write(void* volatile _in_addr, void* volatile _in_value_addr,
                      unsigned int number) {
  if ((_in_addr == nullptr) || (_in_value_addr == nullptr)) goto error;
  register void* in_addr = _in_addr;
  register void* in_value_addr = _in_value_addr;  // must be a register variable
  switch (number) {
    case 0:
      __asm__ volatile(
          "nop \n\t"
#ifdef __x86_64__
          "movq %[addr],%%rax \n\t"
          "movb (%[value_addr]),%%dl \n\t"  // load from value_addr
          "lock xchg %%dl,(%%rax) \n\t"
          // old value is discarded
          :
          : [addr] "rm"(in_addr), [value_addr] "r"(in_value_addr)
          : "memory", "rax", "dl");
#elif defined(__i686__)
          "movl %[addr],%%eax \n\t"
          "movb (%[value_addr]),%%dl \n\t"  // load from value_addr
          "lock xchg %%dl,(%%eax) \n\t"
          :
          : [addr] "rm"(in_addr), [value_addr] "r"(in_value_addr)
          : "memory", "eax", "dl");
#else
#error "invalid platform"
#endif
      return true;
    case 1:
      __asm__ volatile(
          "nop \n\t"
#ifdef __x86_64__
          "movq %[addr],%%rax \n\t"
          "movw (%[value_addr]),%%dx \n\t"  // load from value_addr
          "lock xchg %%dx,(%%rax) \n\t"
          :
          : [addr] "rm"(in_addr), [value_addr] "r"(in_value_addr)
          : "memory", "rax", "dx");
#elif defined(__i686__)
          "movl %[addr],%%eax \n\t"
          "movw (%[value_addr]),%%dx \n\t"  // load from value_addr
          "lock xchg %%dx,(%%eax) \n\t"
          :
          : [addr] "rm"(in_addr), [value_addr] "r"(in_value_addr)
          : "memory", "eax", "dx");
#else
#error "invalid platform"
#endif
      return true;
    case 2:
      __asm__ volatile(
          "nop \n\t"
#ifdef __x86_64__
          "movq %[addr],%%rax \n\t"
          "movl (%[value_addr]),%%edx \n\t"  // load from value_addr
          "lock xchg %%edx,(%%rax) \n\t"
          :
          : [addr] "rm"(in_addr), [value_addr] "r"(in_value_addr)
          : "memory", "rax", "edx");
#elif defined(__i686__)
          "movl %[addr],%%eax \n\t"
          "movl (%[value_addr]),%%edx \n\t"  // load from value_addr
          "lock xchg %%edx,(%%eax) \n\t"
          :
          : [addr] "rm"(in_addr), [value_addr] "r"(in_value_addr)
          : "memory", "eax", "edx");
#else
#error "invalid platform"
#endif
      return true;
    case 3:
      __asm__ volatile(
          "nop \n\t"
#ifdef __x86_64__
          "movq %[addr],%%rax \n\t"
          "movq (%[value_addr]),%%rdx \n\t"  // load from value_addr
          "lock xchg %%rdx,(%%rax) \n\t"
          :
          : [addr] "rm"(in_addr), [value_addr] "r"(in_value_addr)
          : "memory", "rax", "rdx");
#elif defined(__i686__)
          "movl %[addr],%%esi \n\t"
          "movl %[value_addr],%%edi \n\t"
          "push %%eax \n\t"
          "push %%ebx \n\t"
          "push %%ecx \n\t"
          "push %%edx \n\t"
          "movl $0,%%eax \n\t"
          "movl (%%edi),%%ebx \n\t"
          "movl 4(%%edi),%%ecx \n\t"
          "movl $0,%%edx \n\t"
          "0: \n\t"
          "lock cmpxchg8b (%%esi) \n\t"
          "jz 1f \n\t"  // if euqal directly pass,if not just retry
          "jmp 0b \n\t"
          "1: \n\t"  // if equal directly write
          "pop %%edx \n\t"
          "pop %%ecx \n\t"
          "pop %%ebx \n\t"
          "pop %%eax \n\t"
          :
          : [addr] "rm"(in_addr), [value_addr] "r"(in_value_addr)
          : "memory", "esi", "edi");
#else
#error "invalid platform"
#endif
      return true;
    default:
      break;
  }
error:
  printf(
      "[FATAL ERROR] invaild write operation,the program NEED TO FIX THIS "
      "BUG\n");
  _backtrace_and_exit();
  return false;
}

[[gnu::sysv_abi]] bool __myatomic_read(void* volatile in_addr, void* volatile out_addr,
                     unsigned int number) {
  // todo CMPXCHG for read instead of MOV
  if ((in_addr == nullptr) || (out_addr == nullptr)) goto error;

  register void* out_ptr = out_addr;
  switch (number) {
    case 0: {
      // byte
      __asm__ volatile(
          "nop \n\t"
#ifdef __x86_64__
          "movq %[addr],%%rax \n\t"
          "movb (%%rax),%%dl \n\t"
          "movb %%dl,(%[out_ptr]) \n\t"
          :
          : [addr] "rm"(in_addr), [out_ptr] "r"(out_ptr)
          : "memory", "rax", "dl");
#elif defined(__i686__)
          "movl %[addr],%%eax \n\t"
          "movb (%%eax),%%dl \n\t"
          "movb %%dl,(%[out_ptr]) \n\t"
          :
          : [addr] "rm"(in_addr), [out_ptr] "r"(out_ptr)
          : "memory", "eax", "dl");
#else
#error "invalid platform"
#endif
      // byte end
      return true;
    }
    case 1: {
#ifdef _ENABLE_LOG
      bool is_in = (((unsigned long)in_addr & (64 - 1)) + 2) > CACHE_LINE_SIZE;
      bool is_out = (((unsigned long)out_ptr & (64 - 1)) + 2) > CACHE_LINE_SIZE;
      if (is_in || is_out) {
        printf(
            "[WARN] cross cache line access,performance & program correctness "
            "may be affacted\n");
      }
#endif
      // word(double byte)
      __asm__ volatile(
          "nop \n\t"
#ifdef __x86_64__
          "movq %[addr],%%rax \n\t"
          "movw (%%rax),%%dx \n\t"
          "movw %%dx,(%[out_ptr]) \n\t"
          :
          : [addr] "rm"(in_addr), [out_ptr] "r"(out_ptr)
          : "memory", "rax", "dx");
#elif defined(__i686__)
          "movl %[addr],%%eax \n\t"
          "movw (%%eax),%%dx \n\t"
          "movw %%dx,(%[out_ptr]) \n\t"
          :
          : [addr] "rm"(in_addr), [out_ptr] "r"(out_ptr)
          : "memory", "eax", "dx");
#else
#error "invalid platform"
#endif
      // word end
      return true;
    }
    case 2: {
#ifdef _ENABLE_LOG
      bool is_in = (((unsigned long)in_addr & (64 - 1)) + 4) > CACHE_LINE_SIZE;
      bool is_out = (((unsigned long)out_ptr & (64 - 1)) + 4) > CACHE_LINE_SIZE;
      if (is_in || is_out) {
        printf(
            "[WARN] cross cache line access,performance & program correctness "
            "may be affacted\n");
      }
#endif
      // dword(double word)
      __asm__ volatile(
          "nop \n\t"
#ifdef __x86_64__
          "movq %[addr],%%rax \n\t"
          "movl (%%rax),%%edx \n\t"
          "movl %%edx,(%[out_ptr]) \n\t"
          :
          : [addr] "rm"(in_addr), [out_ptr] "r"(out_ptr)
          : "memory", "rax", "edx");
#elif defined(__i686__)
          "movl %[addr],%%eax \n\t"
          "movl (%%eax),%%edx \n\t"
          "movl %%edx,(%[out_ptr]) \n\t"
          :
          : [addr] "rm"(in_addr), [out_ptr] "r"(out_ptr)
          : "memory", "eax", "edx");
#else
#error "invalid platform"
#endif
      // dword end
      return true;
    }
    case 3: {
#if defined(_ENABLE_LOG) && !defined(__i686__)
      bool is_in = (((unsigned long)in_addr & (64 - 1)) + 8) > CACHE_LINE_SIZE;
      bool is_out = (((unsigned long)out_ptr & (64 - 1)) + 8) > CACHE_LINE_SIZE;
      if (is_in || is_out) {
        printf(
            "[WARN] cross cache line access,performance & program "
            "correctness may be affacted\n");
      }
#endif
      // qword(quad word)
      __asm__ volatile(
          "nop \n\t"
#ifdef __x86_64__
          "movq %[addr],%%rax \n\t"
          "movq (%%rax),%%rdx \n\t"
          "movq %%rdx,(%[out_ptr]) \n\t"
          :
          : [addr] "rm"(in_addr), [out_ptr] "r"(out_ptr)
          : "memory", "rax", "rdx");
#elif defined(__i686__)
          "movl %[addr],%%esi \n\t"
          "movl %[out_ptr],%%edi \n\t"  // avoid out_ptr was
                                        // modified by the
                                        // context
          "push %%eax \n\t"
          "push %%ebx \n\t"
          "push %%ecx \n\t"
          "push %%edx \n\t"
          "movl $0,%%eax \n\t"
          "movl $0,%%ebx \n\t"
          "movl $0,%%ecx \n\t"
          "movl $0,%%edx \n\t"
          "lock cmpxchg8b (%%esi) \n\t"
          "movl %%eax,(%%edi) \n\t"   // low 4byte
          "movl %%edx,4(%%edi) \n\t"  // high 4byte
          "pop %%edx \n\t"
          "pop %%ecx \n\t"
          "pop %%ebx \n\t"
          "pop %%eax \n\t"
          :
          : [addr] "rm"(in_addr), [out_ptr] "rm"(out_ptr)
          : "memory", "esi", "edi");
#else
#error "invalid platform"
#endif
      // qword end
      return true;
    }
    default:
      break;
  }
error:
  printf(
      "[FATAL ERROR] invaild write operation,the program NEED TO FIX THIS "
      "BUG\n");
  _backtrace_and_exit();
  return false;
}
