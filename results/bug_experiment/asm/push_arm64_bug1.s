ldr	x8, [x0, #128]
ldr	x9, [x0, #144]
sub	x9, x8, x9
cmp	x9, #64
b.ne	LBB0_6
ldr	x9, [x0, #136]
cmp	x9, x8
b.eq	LBB0_3
str	x8, [x0]
str	x8, [x0, #136]
add	x8, x0, #256
ldapr	x9, [x8]
str	x9, [x0, #144]
ldr	x8, [x0, #128]
sub	x9, x8, x9
cmp	x9, #64
b.ne	LBB0_6
mov	w8, #0                          ; =0x0
mov	x0, x8
ret
ldr	x9, [x0, #512]
and	x8, x8, #0x3f
mov	w10, #48                        ; =0x30
umaddl	x8, w8, w10, x9
ldp	q0, q1, [x1]
ldr	q2, [x1, #32]
stp	q1, q2, [x8, #16]
str	q0, [x8]
ldp	x8, x10, [x0, #128]
add	x9, x8, #1
str	x9, [x0, #128]
mov	w8, #1                          ; =0x1
subs	x10, x9, x10
b.eq	LBB0_5
ldr	x11, [x0, #520]
cmp	x10, x11
b.lo	LBB0_5
str	x9, [x0]
str	x9, [x0, #136]
mov	x0, x8
ret
