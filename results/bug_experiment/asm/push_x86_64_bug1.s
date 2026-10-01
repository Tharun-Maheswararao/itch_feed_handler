movq	64(%rdi), %rax
movq	%rax, %rcx
subq	80(%rdi), %rcx
cmpq	$64, %rcx
jne	LBB0_6
cmpq	%rax, 72(%rdi)
je	LBB0_3
movq	%rax, (%rdi)
movq	%rax, 72(%rdi)
movq	128(%rdi), %rcx
movq	%rcx, 80(%rdi)
movq	64(%rdi), %rax
movq	%rax, %rdx
subq	%rcx, %rdx
cmpq	$64, %rdx
jne	LBB0_6
xorl	%eax, %eax
retq
pushq	%rbp
movq	%rsp, %rbp
movq	256(%rdi), %rcx
andl	$63, %eax
leaq	(%rax,%rax,2), %rax
shll	$4, %eax
movaps	(%rsi), %xmm0
movaps	16(%rsi), %xmm1
movaps	32(%rsi), %xmm2
movaps	%xmm2, 32(%rcx,%rax)
movaps	%xmm1, 16(%rcx,%rax)
movaps	%xmm0, (%rcx,%rax)
movq	64(%rdi), %rcx
movq	264(%rdi), %rdx
incq	%rcx
movq	%rcx, 64(%rdi)
movq	%rcx, %rsi
movb	$1, %al
subq	72(%rdi), %rsi
je	LBB0_9
cmpq	%rdx, %rsi
jb	LBB0_9
movq	%rcx, (%rdi)
movq	%rcx, 72(%rdi)
popq	%rbp
retq
