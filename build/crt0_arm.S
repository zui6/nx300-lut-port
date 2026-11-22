/* Minimal ARM EABI entry point for the NX300 LUT live loader.
 *
 * The camera runs glibc 2.13 (libc-2.13.so).  We link the target libc
 * directly and therefore cannot use the toolchain's crt1.o, whose startup
 * helpers reference newer glibc symbols.  This stub performs the same
 * call the standard startup makes:
 *
 *   __libc_start_main(main, argc, argv, init, fini, rtld_fini, stack_end)
 *
 * Arguments 1-4 travel in r0-r3; the remaining three are pushed on the
 * stack in order (fini, rtld_fini, stack_end), which keeps the 8-byte
 * stack alignment required by the AAPCS.
 */
    .syntax unified
    .arm
    .text
    .global _start
    .type _start, %function
    .align 2

_start:
    mov     fp, #0                  @ terminate the frame-pointer chain
    ldr     r0, =main               @ main
    ldr     r1, [sp]                @ argc
    add     r2, sp, #4              @ argv
    mov     r3, #0                  @ init   = NULL
    add     r4, sp, #8              @ envp   (unused here, kept readable)
    mov     r5, #0                  @ fini   = NULL
    mov     r6, #0                  @ rtld_fini = NULL
    mov     r7, sp                  @ stack_end
    push    {r5, r6, r7}            @ 5th, 6th, 7th arguments
    bl      __libc_start_main
    @ __libc_start_main never returns on success; exiting here is a
    @ defensive fallback so a failed start does not run into garbage.
    mov     r0, #1
    mov     r7, #1                  @ __NR_exit
    svc     #0
    .size _start, .-_start
