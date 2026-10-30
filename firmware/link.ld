ENTRY(reset_handler)

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 64K
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 16K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS
{
  .text :
  {
    KEEP(*(.vectors))
    *(.text*)
    *(.rodata*)
    . = ALIGN(4);
  } > FLASH

  _sidata = LOADADDR(.data);

  .data :
  {
    _sdata = .;
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } > RAM AT > FLASH

  .bss (NOLOAD) :
  {
    _sbss = .;
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
  } > RAM

  /DISCARD/ :
  {
    *(.ARM.exidx*)
    *(.comment)
  }
}
