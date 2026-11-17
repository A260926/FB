.text
.globl dispatch
.type dispatch,@function
dispatch:
 movl %edi,%edi
 cmpl $1, %edi
 ja .Ldefault
 jmp *.Ltable(,%rdi,8)
.Lcase0:
 movl $10,%eax
 ret
.Lcase1:
 movl $20,%eax
 ret
.Ldefault:
 movl $-1,%eax
 ret
.Ltail:
 movl $99,%eax
 ret
.size dispatch,.-dispatch
.section .rodata
.p2align 3
.Ltable:
 .quad .Lcase0
 .quad .Lcase1
 .quad .Ltail
 .quad 0
