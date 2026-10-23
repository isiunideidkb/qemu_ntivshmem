//please use cpp!


/*
  after write flag and bodies,in some rare cases,after write flag,the client will monitored and write the reply messages,PRECEDE the host's umonitor to be executed
  if&else comparsion is useless! this operation will also causes a spare time region so that the client will reply the message also PRECEDE the host's umonitor to be executed,use cmpxchg instead to guarantee the atomic-ity
*/


.arch .waitpkg
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
  movq -8(%rsp),%rdi
  sub $8,%rsp
  push %rbx
  add $16,%rsp

  movl %edx,%esi
  not %esi
  or %esi,%edx
  movl %eax,%esi
  not %esi
  or %esi,%eax

  xor %ecx,%ecx
  add $1,%ecx
0:
  umonitor %rdi
  movb $0,%bl
  movb $0,%al
  lock cmpxchgb %bl,(%rdi)
  jne 1f
  umwait %ecx
  jmp 0b
1:
  sub $16,%rsp
  pop %rbx
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
  movl %edi,-4(%esp)
  movl -4(%esp),%edi
  sub $4,%esp
  pushl %ebx
  add $8,%esp

  movl %edx,%esi
  not %esi
  or %esi,%edx
  movl %eax,%esi
  not %esi
  or %esi,%eax

  xor %ecx,%ecx
  add $1,%ecx
0:
  umonitor %edi
  movb $0,%bl
  movb $0,%al
  lock cmpxchgb %bl,(%edi)
  jne 1f
  umwait %ecx
  jmp 0b
1:
  sub $8,%rsp
  popl %ebx
  ret
#else
#error "invalid platform!"
#endif
