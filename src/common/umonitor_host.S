//please use cpp!


/*
  after write flag and bodies,in some rare cases,after write flag,the client will monitored and write the reply messages,PRECEDE the host's umonitor to be executed
  if&else comparsion is useless! this operation will also causes a spare time region so that the client will reply the message also PRECEDE the host's umonitor to be executed,use cmpxchg instead to guarantee the atomic-ity
*/



.global __host_wait_umonitor


.text
/* sysv_abi */
#if defined(__x86_64__)

/* 
  %rdi address to be monitored
  %al whether it client returns true or false
*/
__host_wait_umonitor:
  movq %rdi,-8(%rsp)
  movq -8(%rsp),%rax
0:
  umonitor %rax
  movb $0,%bl
  movb $0,%al
  lock cmpxchgb %bl,(%rax)
  jne 1f
  umwait
  jmp 0b
1:
  ret
#  movb 2(%rax),%sil
#  sub $1,%sil
#  jc 3f
#2:
#  movb $0,%al
#  ret
#3:
#  movb $1,%al
#  ret

#elif defined(__i686__) || defined(__i386__)
__host_wait_umonitor:
  movl %edi,-8(%esp)
  movl -8(%esp),%eax
0:
  umonitor %eax
  movb $0,%bl
  movb $0,%al
  lock cmpxchgb %bl,(%eax)
  jne 1f
  umwait
  jmp 0b
1:
  ret
#else
#error "invalid platform!"
#endif
