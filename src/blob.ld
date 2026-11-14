/* Injected blob for the 8BitDo Ultimate 2C (fw 1.09), loaded at flash 0x18500.
   RAM scratch (.bss/.data) is NOLOAD at 0x8432C0: the blob is written to flash
   as-is, so nothing initialises it — keep every global zero-initialised.
   The .hook* sections exist only to encode the tjl bytes; they are extracted
   separately into hook_*.bin and are NOT part of the blob. */
SECTIONS
{
  . = 0x18500;
  .text : { *(.text*) *(.rodata*) }
  /* Scratch RAM. Both 0x842A00 and 0x842600 (below) turned out to be trampled:
     the whole un-referenced 0x842600-0x842E00 gap is in use by something at
     runtime (the values written by the ISR hook were gone by the next frame).
     0x8432C0 sits in the largest literal-free hole inside the stock .bss
     (0x843178 and 0x8432B8 are referenced, the next use is 0x8433B8), which is
     the same "dead bss" area the 2C custom-firmware toolkit uses. */
  . = 0x8432C0;
  .bss (NOLOAD) : { *(.bss*) *(.data*) *(COMMON) }
  .hookaa68 0xAA68 : { *(.hookaa68) }
  .hookaa2e 0xAA2E : { *(.hookaa2e) }
  .hooka45c 0xA45C : { *(.hooka45c) }
  /DISCARD/ : { *(.comment) *(.ARM*) }
}
