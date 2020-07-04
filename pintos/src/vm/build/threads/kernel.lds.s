OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH("i386")
ENTRY(start)
SECTIONS
{
  _start = 0xc0000000 + 0x20000;
  . = _start + SIZEOF_HEADERS;
  .text : { *(.start) *(.text) } = 0x90
  .rodata : { *(.rodata) *(.rodata.*) *(.data.rel.ro) *(.data.rel.ro.local)
       . = ALIGN(0x1000);
       _end_kernel_text = .; }
  .data : { *(.data) *(.data.rel.local)
     _signature = .; LONG(0xaa55aa55) }
  _start_bss = .;
  .bss : { *(.bss) }
  _end_bss = .;
  _end = .;
  ASSERT (_end - _start <= 512K, "Kernel image is too big.")
}
