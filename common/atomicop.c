/*
  read: write back memory is required,using memory bariier to guarantee read operation will not-
  reordered before last write operation

  write: write back memory is required as well,use mov or xchg?
  
  exactly since P6,lock prefix operations will use cache lock rather than bus lock in appropriate cases
  
  process order will also affect the order of read&write on shared memory,although the read&write are lock prefix operation(e.g. XCHG)
  
  !!note!! split the high,low and store then respectively,in ordinary concurrency cases are dangerous,because of the store order are interruptible -
  but in here,the order can be guarantee to un-interruptible as long as the proram obey the standard RPC protocol

  why not use GCC atomic intrinsics? emm... actually the GCC atomic intrinsics can not guarantee atomic-ity without OS participation,and gcc aotmic intrinsics will roll-back to software implementation in some cases
  notes as to read operations for greater than single byte:
    the low-level assembly implementation are ordinary MOV instructions for READ OP,because MOV instruction of x86 can guarantee atomic in most cases,but there's some exceptions:
      read op greater than single byte,if the addr is unaligned(see IA32 SDM) or cross cache line,the CPU may not provide atomic-ity for this operation,but the standard memory access sequence of RPC standard still guarantee that the read will not be raced by other processor although the CPU can not provide atoimc-ity for read operation.likewise,i used the "lfence" instruction to preventing the reordering of program as much as possible. but i'm still confused about whether the processor guarantees the operation read out old value
      see IA32SDM strategy of WB memory caching,memory order,bus locks
*/

#include <stdio.h>
#include <stdlib.h>
#ifdef __gnu_linux__
  #include <execinfo.h>
#endif
static void _backtrace_and_exit() {
#ifdef __gnu_linux__
  void* stack_ptrs[10];
  char** strings;
  int avail_size;
  avail_size = backtrace(stack_ptrs,10);
  strings = backtrace_symbols(stack_ptrs,avail_size);
  if(strings != nullptr) {
    printf("=====STACK TRACE INFO=====\n");
    for (int i = 0; i < avail_size; i++)
      printf("stack NO. %d: %s \n",i,strings[i]);
  }
#endif
  exit(-1);
}

bool __myatomic_write(void* volatile _in_addr,
                      void* volatile _in_value_addr,
		      unsigned int number)
{
  if((_in_addr == nullptr) || (_in_value_addr == nullptr))
    goto error;
  void* in_addr = _in_addr;
  register void* in_value_addr = _in_value_addr; //must be a register variable
  switch (number) {
   case 0:
    __asm__ volatile("nop \n\t"
#ifdef __x86_64__
                     "movq %[addr],%%rax \n\t"
		     "movb (%[value_addr]),%%dl \n\t" //load from value_addr
		     "lock xchg %%dl,(%%rax) \n\t"
		     "mfence \n\t"
		     //old value is discarded
		     : 
		     : [addr]"rm"(in_addr),[value_addr]"r"(in_value_addr)
		     : "memory","rax","dl");
#elif defined(__i686__)
		     "movl %[addr],%%eax \n\t"
		     "movb (%[value_addr]),%%dl \n\t" //load from value_addr
		     "lock xchg %%dl,(%%eax) \n\t"
		     "mfence \n\t" 		     //old value is discarded
		     : 
		     : [addr]"rm"(in_addr),[value_addr]"r"(in_value_addr)
		     : "memory","eax","dl");
#else
#error "invalid platform"
#endif
    return true;
   case 1:
    __asm__ volatile("nop \n\t"
#ifdef __x86_64__
                     "movq %[addr],%%rax \n\t"
		     "movw (%[value_addr]),%%dx \n\t" //load from value_addr
		     "lock xchg %%dx,(%%rax) \n\t"
		     "mfence \n\t" 		     //old value is discarded
		     : 
		     : [addr]"rm"(in_addr),[value_addr]"r"(in_value_addr)
		     : "memory","rax","dx");
#elif defined(__i686__)
		     "movl %[addr],%%eax \n\t"
		     "movw (%[value_addr]),%%dx \n\t" //load from value_addr
		     "lock xchg %%dx,(%%eax) \n\t"
		     "mfence \n\t"		     //old value is discarded
		     : 
		     : [addr]"rm"(in_addr),[value_addr]"r"(in_value_addr)
		     : "memory","eax","dx");
#else
#error "invalid platform"
#endif
    return true;		     
   case 2:
    __asm__ volatile("nop \n\t"
#ifdef __x86_64__
                     "movq %[addr],%%rax \n\t"
		     "movl (%[value_addr]),%%edx \n\t" //load from value_addr
		     "lock xchg %%edx,(%%rax) \n\t"
		     "mfence \n\t" 		     //old value is discarded
		     : 
		     : [addr]"rm"(in_addr),[value_addr]"r"(in_value_addr)
		     : "memory","rax","edx");
#elif defined(__i686__)
		     "movl %[addr],%%eax \n\t"
		     "movl (%[value_addr]),%%edx \n\t" //load from value_addr
		     "lock xchg %%edx,(%%eax) \n\t"
		     "mfence \n\t" 		     //old value is discarded
		     :
		     : [addr]"rm"(in_addr),[value_addr]"r"(in_value_addr)
		     : "memory","eax","edx");
#else
#error "invalid platform"
#endif
    return true;		     
   case 3:
    __asm__ volatile("nop \n\t"
#ifdef __x86_64__
                     "movq %[addr],%%rax \n\t"
		     "movq (%[value_addr]),%%rdx \n\t" //load from value_addr
		     "lock xchg %%edx,(%%rax) \n\t"
		     "mfence \n\t"		     //old value is discarded
		     : 
		     : [addr]"rm"(in_addr),[value_addr]"r"(in_value_addr)
		     : "memory","rax","rdx");
#elif defined(__i686__)
		     "movl %[addr],%%eax \n\t"
		     "movl (%[value_addr]),%%edx \n\t" //load from value_addr
		     "movl 4(%[value_addr]),%%esi \n\t" //load from value high addr  ..
		     "lock xchg %%edx,(%%eax) \n\t"
		     "lock xchg %%esi,4(%%eax) \n\t"
		     "mfence \n\t" 		     //old value is discarded
		     : 
		     : [addr]"rm"(in_addr),[value_addr]"r"(in_value_addr)
		     : "memory","eax","edx","esi");
#else
#error "invalid platform"
#endif
    return true;		     
   default:
    break;
  }
error:  
  printf("[FATAL ERROR] invaild write operation,the program NEED TO FIX THIS BUG\n");
  _backtrace_and_exit();
  return false;
}

bool __myatomic_read(void* volatile in_addr,
                     void* volatile out_addr,
                     unsigned int number)
{
  if((in_addr == nullptr) || (out_addr == nullptr))
    goto error;    
  register void* out_ptr = out_addr;
  switch(number) {
   case 0: {
    //byte
    __asm__ volatile ("nop \n\t"
#ifdef __x86_64__
                      "movq %[addr],%%rax \n\t"
		      "movb (%%rax),%%dl \n\t"
		      "lfence \n\t"		      
		      "movb %%dl,(%[out_ptr]) \n\t"
		      : 
		      : [addr]"rm"(in_addr),[out_ptr]"r"(out_ptr)
		      : "memory","rax","dl");
#elif defined(__i686__)
                      "movl %[addr],%%eax \n\t"
		      "movb (%%eax),%%dl \n\t"
		      "lfence \n\t"		      
		      "movb %%dl,(%[out_ptr]) \n\t"
		      : 
		      : [addr]"rm"(in_addr),[out_ptr]"r"(out_ptr)
		      : "memory","eax","dl");
#else
#error "invalid platform"
#endif
    //byte end
    return true;
   }
   case 1: {
#ifdef _ENABLE_LOG
    //todo mem align check for read op(op byte greater than 1 only)
#endif   
    //word(double byte)
    __asm__ volatile ("nop \n\t"
#ifdef __x86_64__
                      "movq %[addr],%%rax \n\t"
		      "movw (%%rax),%%dx \n\t"
		      "lfence \n\t"		      
		      "movw %%dx,(%[out_ptr]) \n\t"
		      : 
		      : [addr]"rm"(in_addr),[out_ptr]"r"(out_ptr)
		      : "memory","rax","dx");
#elif defined(__i686__)
                      "movl %[addr],%%eax \n\t"
		      "movw (%%eax),%%dx \n\t"
		      "lfence \n\t"		      
		      "movw %%dx,(%[out_ptr]) \n\t"
		      : 
		      : [addr]"rm"(in_addr),[out_ptr]"r"(out_ptr)
		      : "memory","eax","dx");
#else
#error "invalid platform"
#endif
    //word end
    return true;    
   }
   case 2: {
#ifdef _ENABLE_LOG
    //todo mem align check for read op(op byte greater than 1 only)
#endif
    //dword(double word)
    __asm__ volatile ("nop \n\t"
#ifdef __x86_64__
                      "movq %[addr],%%rax \n\t"
		      "movl (%%rax),%%edx \n\t"
		      "lfence \n\t"
		      "movl %%edx,(%[out_ptr]) \n\t"
		      : 
		      : [addr]"rm"(in_addr),[out_ptr]"r"(out_ptr)
		      : "memory","rax","edx");
#elif defined(__i686__)
                      "movl %[addr],%%eax \n\t"
		      "movl (%%eax),%%edx \n\t"
		      "lfence \n\t"		      
		      "movl %%edx,(%[out_ptr]) \n\t"
		      : 
		      : [addr]"rm"(in_addr),[out_ptr]"r"(out_ptr)
		      : "memory","eax","edx");
#else
#error "invalid platform"
#endif
    //dword end
    return true;   
   }
   case 3: {
#ifdef _ENABLE_LOG
    //todo mem alignment check for read op(op byte greater than 1 only)
#endif   
    //qword(quad word)
    __asm__ volatile ("nop \n\t"
#ifdef __x86_64__
                      "movq %[addr],%%rax \n\t"
		      "movq (%%rax),%%rdx \n\t"
		      "lfence \n\t"		      
		      "movq %%rdx,(%[out_ptr]) \n\t"
		      : 
		      : [addr]"rm"(in_addr),[out_ptr]"r"(out_ptr)
		      : "memory","rax","rdx");
#elif defined(__i686__)
                      "movl %[addr],%%eax \n\t"
		      "movl (%%eax),%%edx \n\t" //low addr
		      "movl 4(%%eax),%%esi \n\t" //high addr
		      "lfence \n\t"
		      "movl %%edx,(%[out_ptr]) \n\t"
		      "movl %%esi,4(%[out_ptr]) \n\t"
		      : 
		      : [addr]"rm"(in_addr),[out_ptr]"r"(out_ptr)
		      : "memory","eax","edx", "esi");
#else
#error "invalid platform"
#endif
    //qword end
    return true;       
   }
   default:
    break;
  }
error:  
  printf("[FATAL ERROR] invaild write operation,the program NEED TO FIX THIS BUG\n");
  _backtrace_and_exit();
  return false;
}
