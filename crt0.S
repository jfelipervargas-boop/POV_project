.syntax unified
.cpu cortex-m4
.fpu fpv4-sp-d16
.thumb

.global Reset_Handler
.global Default_Handler
.global _infinite_loop

/* ------------------------------------------------------------------
 * Tabla de vectores: el Cortex-M lee SP inicial de 0x08000000 y la
 * direccion de Reset de 0x08000004. Sin esto el micro no arranca.
 * ------------------------------------------------------------------ */
.section .isr_vector, "a", %progbits
.type g_pfnVectors, %object
g_pfnVectors:
    .word _estack              /*  0: SP inicial        */
    .word Reset_Handler        /*  1: Reset             */
    .word Default_Handler      /*  2: NMI               */
    .word Default_Handler      /*  3: HardFault         */
    .word Default_Handler      /*  4: MemManage         */
    .word Default_Handler      /*  5: BusFault          */
    .word Default_Handler      /*  6: UsageFault        */
    .word 0                    /*  7: reservado         */
    .word 0                    /*  8: reservado         */
    .word 0                    /*  9: reservado         */
    .word 0                    /* 10: reservado         */
    .word Default_Handler      /* 11: SVCall            */
    .word Default_Handler      /* 12: DebugMon          */
    .word 0                    /* 13: reservado         */
    .word Default_Handler      /* 14: PendSV            */
    .word Default_Handler      /* 15: SysTick           */

.section .text.Reset_Handler
.type Reset_Handler, %function
.thumb_func
Reset_Handler:
    LDR R0, =_estack
    MOV SP, R0

    /* Habilitar FPU (compilamos con -mfloat-abi=hard) */
    LDR R0, =0xE000ED88
    LDR R1, [R0]
    ORR R1, R1, #(0xF << 20)
    STR R1, [R0]
    DSB
    ISB

    /* Copiar .data de FLASH a RAM */
    LDR R0, =_sdata
    LDR R1, =_edata
    LDR R2, =_sidata
    B LoopCopyDataInit

CopyDataInit:
    LDR R3, [R2], #4
    STR R3, [R0], #4

LoopCopyDataInit:
    CMP R0, R1
    BLT CopyDataInit

    /* Limpiar .bss */
    LDR R0, =_sbss
    LDR R1, =_ebss
    MOV R2, #0
    B LoopFillZerobss

FillZerobss:
    STR R2, [R0], #4

LoopFillZerobss:
    CMP R0, R1
    BLT FillZerobss

    BL main

.type _infinite_loop, %function
.thumb_func
_infinite_loop:
    B _infinite_loop

.type Default_Handler, %function
.thumb_func
Default_Handler:
    B Default_Handler