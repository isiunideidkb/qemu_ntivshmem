.global host_read_head
.global host_write_head

#note 虽然默认在WriteBack内存下 并且现代处理器也能保证非aligned cache line的MOV一致性 但是对于某些特殊情况会失败并且退化到无法保证原子性 所以应该使用lock xchg
#note 由于协商只是普通的 我们只需要xchg 即便有性能开销也不管 反正不是核心的部分
#note 使用sfence主要是为了防止"Loads May Be Reordered with Earlier Stores to Different Locations" See IA32SDM Volume 3

//the arguments are volatile void* volatile ptr;
host_write_head:
  movq %rdi,(%rsp) //addr 
  movb $1,%al
  movq (%rsp),%rcx
  lock xchg %al,(%rcx) #现在al是之前的值了
  sfence
  #todo compare if the %al is zero? if it's zero ok,not zero crached    