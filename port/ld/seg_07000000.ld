/* N64 data segment 0x07: everything in one .data output section at the
   segment's base, so symbol addresses are segmented addresses (0x07000000+offset)
   and "objcopy -O binary --only-section=.data" gives the segment image. */
SECTIONS
{
  . = 0x07000000;
  .data : {
    *(.text .text.*)
    *(.rodata .rodata.*)
    *(.data .data.*)
    *(.sdata .sdata.*)
    *(.bss .bss.* .sbss .sbss.* COMMON)
  }
  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) *(.note*) }
}
