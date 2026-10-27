    .syntax unified
    .arm
    .fpu neon-vfpv4
    .text
    .global prog_basic
    .global prog_loop
    .global prog_fp
    .global prog_syscall
@ r3 = writable data page, r4 = unmapped address. 7*6 stored to [r3]; svc 0x42; then a read from [r4] must fault.
prog_basic:
    mov r0, #7
    mov r1, #6
    mul r2, r0, r1
    str r2, [r3]
    svc #0x42
    ldr r0, [r4]
    bx lr
@ 100M iterations of a two-instruction loop, then svc 0x43.
prog_loop:
    movw r0, #0xE100
    movt r0, #0x05F5
1:  subs r0, r0, #1
    bne 1b
    svc #0x43
@ 2.0f * 3.0f in VFP, then the same through Neon (vmla.f32 q) into memory at [r3]; svc 0x44.
prog_fp:
    mov r0, #0x40000000
    movw r1, #0
    movt r1, #0x4040
    vmov s0, r0
    vmov s1, r1
    vmul.f32 s2, s0, s1
    vmov r2, s2
    vdup.32 q0, r0
    vdup.32 q1, r1
    vmov.i32 q2, #0
    vmla.f32 q2, q0, q1
    vst1.32 {d4-d5}, [r3]
    svc #0x44
@ Linux EABI style system call: r7 = number, svc 0; the host sees imm 0 and r7.
prog_syscall:
    mov r7, #20
    svc #0
    svc #0x45
