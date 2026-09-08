/*
 * Custom crt0 for 3DSX with configurable heap sizes.
 * Overrides the default 3dsx_crt0.o to set __heap_size and __linear_heap_size.
 */
.section .crt0, "ax"
.global __stacksize__
.global __heap_size__
.global __linear_heap_size__

@ Stack size: 512 KB
__stacksize__:
    .word 524288

@ Standard heap (APPMEM): 64 MB - needed for particle definitions
__heap_size__:
    .word 67108864

@ Linear heap (GPU textures): 8 MB - enough for downscaled textures
__linear_heap_size__:
    .word 8388608

@ Jump to the real entry point
.global _start
_start:
    bl main
    mov r0, #0
    bl exit
    b .