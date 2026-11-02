/* Purplebootcode i386 kernel linker script. Intended for Multiboot2 loaders. */
OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)
ENTRY(_start)
SECTIONS
{
  . = 1M;
  .multiboot ALIGN(8) : { KEEP(*(.multiboot)) }
  .text ALIGN(16) : { *(.text.boot) *(.text .text.*) }
  .rodata ALIGN(16) : { *(.rodata .rodata.*) }
  .data ALIGN(16) : { *(.data .data.*) }
  .bss ALIGN(16) : { *(COMMON) *(.bss .bss.*) }
  /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}
