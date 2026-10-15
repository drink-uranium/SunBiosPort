ENTRY(_start)
SECTIONS { . = 0x00200000; .text : { *(.mbheader) *(.text*) } .rodata : { *(.rodata*) } .data : { *(.data*) } .bss : { *(.bss*) *(COMMON) } /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame) } }
