/* ============================================================================
   FIROZ OS Payload - Custom Linker Script
   Fixes RWE segment issue by separating code and data into different segments
   ============================================================================ */

ENTRY(_start)

PHDRS {
  text_segment PT_LOAD FLAGS(4);        /* R + E (Read + Execute) */
  rodata_segment PT_LOAD FLAGS(4);      /* R (Read-only) */
  data_segment PT_LOAD FLAGS(6);        /* RW (Read + Write) */
  dynamic_segment PT_DYNAMIC FLAGS(6);  /* RW (Read + Write) */
}

SECTIONS {
  /* ========================================================================
     TEXT SEGMENT - Code section (R + E only, no write)
     ======================================================================== */
  
  . = 0x0;
  
  .text : {
    *(.text*)
    *(.init)
    *(.fini)
    *(.plt)
    *(.got.plt)
  } : text_segment

  /* ========================================================================
     RODATA SEGMENT - Read-only data (R only, no write/execute)
     ======================================================================== */
  
  . = ALIGN(0x1000);
  
  .rodata : {
    *(.rodata*)
    *(.eh_frame_hdr)
    *(.eh_frame)
  } : rodata_segment
  
  .data.rel.ro : {
    *(.data.rel.ro*)
  } : rodata_segment
  
  .got : {
    *(.got)
  } : rodata_segment

  /* ========================================================================
     DYNAMIC SEGMENT - Dynamic linking information
     ======================================================================== */
  
  . = ALIGN(0x1000);
  
  .dynamic : {
    *(.dynamic)
  } : dynamic_segment : rodata_segment

  /* ========================================================================
     DATA SEGMENT - Initialized and uninitialized data (RW only)
     ======================================================================== */
  
  . = ALIGN(0x1000);
  
  .data : {
    *(.data*)
    *(.init_array*)
    *(.fini_array*)
  } : data_segment
  
  .bss : {
    *(.bss*)
    *(COMMON)
  } : data_segment

  /* ========================================================================
     DEBUG INFO - Stripped in production but useful for debugging
     ======================================================================== */
  
  .symtab : {
    *(.symtab)
  }
  
  .strtab : {
    *(.strtab)
  }
  
  .shstrtab : {
    *(.shstrtab)
  }

  /* ========================================================================
     OTHER SECTIONS - For compatibility
     ======================================================================== */
  
  .dynsym : {
    *(.dynsym)
  }
  
  .dynstr : {
    *(.dynstr)
  }
  
  .rela.dyn : {
    *(.rela.dyn)
    *(.rela.plt)
  }
  
  .gnu.hash : {
    *(.gnu.hash)
  }

  /* Remove unused sections */
  /DISCARD/ : {
    *(.note.GNU-stack)
    *(.gnu_debuglink)
    *(.gnu_debugaltlink)
  }
}
