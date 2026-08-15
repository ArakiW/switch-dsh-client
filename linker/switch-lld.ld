OUTPUT_ARCH(aarch64)
ENTRY(_start)

PHDRS
{
    code   PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data   PT_LOAD FLAGS(6);
    dyn    PT_DYNAMIC;
    tls    PT_TLS FLAGS(4);
}

SECTIONS
{
    PROVIDE_HIDDEN(__start__ = 0x0);
    . = __start__;
    PROVIDE_HIDDEN(__code_start = .);
    .text : { KEEP(*(.crt0)) *(.text.unlikely .text.*_unlikely .text.unlikely.*) *(.text.exit .text.exit.*) *(.text.startup .text.startup.*) *(.text.hot .text.hot.*) *(.text .stub .text.* .gnu.linkonce.t.*) . = ALIGN(8); } :code
    .init : { KEEP(*(.init)) . = ALIGN(8); } :code
    .plt : { *(.plt) *(.iplt) . = ALIGN(8); } :code
    .fini : { KEEP(*(.fini)) . = ALIGN(8); } :code

    . = ALIGN(0x1000);
    PROVIDE_HIDDEN(__rodata_start = .);
    .nx-module-name : { KEEP(*(.nx-module-name)) } :rodata
    .rela.dyn : { *(.rela.*) } :rodata
    .relr.dyn : { *(.relr.*) } :rodata
    .hash : { *(.hash) } :rodata
    .gnu.hash : { *(.gnu.hash) } :rodata
    .dynsym : { *(.dynsym) } :rodata
    .dynstr : { *(.dynstr) } :rodata
    .rodata : { *(.rodata .rodata.* .gnu.linkonce.r.*) . = ALIGN(8); } :rodata
    .tls.align : { QUAD(MAX(ALIGNOF(.tdata), ALIGNOF(.tbss))) } :rodata
    PROVIDE_HIDDEN(__tls_align = ADDR(.tls.align));
    .gcc_except_table : { *(.gcc_except_table .gcc_except_table.*) } :rodata
    .eh_frame_hdr : { *(.eh_frame_hdr) *(.eh_frame_entry .eh_frame_entry.*) } :rodata
    .eh_frame : { KEEP(*(.eh_frame)) *(.eh_frame.*) } :rodata
    .gnu_extab : { *(.gnu_extab*) } :rodata
    .exception_ranges : { *(.exception_ranges .exception_ranges*) } :rodata
    PROVIDE_HIDDEN(__eh_frame_hdr_start = ADDR(.eh_frame_hdr));
    PROVIDE_HIDDEN(__eh_frame_hdr_end = ADDR(.eh_frame_hdr) + SIZEOF(.eh_frame_hdr));
    .gnu.version : { *(.gnu.version) } :rodata
    .gnu.version_d : { *(.gnu.version_d) } :rodata
    .gnu.version_r : { *(.gnu.version_r) } :rodata
    .note.gnu.build-id : { *(.note.gnu.build-id) } :rodata

    . = ALIGN(0x1000);
    PROVIDE_HIDDEN(__relro_start = .);
    .preinit_array : { PROVIDE_HIDDEN(__preinit_array_start = .); KEEP(*(.preinit_array)) PROVIDE_HIDDEN(__preinit_array_end = .); } :data
    .init_array : { PROVIDE_HIDDEN(__init_array_start = .); KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*))) KEEP(*(.init_array .ctors)) PROVIDE_HIDDEN(__init_array_end = .); } :data
    .fini_array : { PROVIDE_HIDDEN(__fini_array_start = .); KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*))) KEEP(*(.fini_array .dtors)) PROVIDE_HIDDEN(__fini_array_end = .); } :data
    .data.rel.ro : { *(.data.rel.ro.local* .gnu.linkonce.d.rel.ro.local.*) *(.data.rel.ro .data.rel.ro.* .gnu.linkonce.d.rel.ro.*) . = ALIGN(8); } :data
    .dynamic : { *(.dynamic) } :data :dyn
    .got : { *(.got) *(.igot) } :data
    .got.plt : { *(.got.plt) *(.igot.plt) } :data
    PROVIDE_HIDDEN(__got_start__ = ADDR(.got));
    PROVIDE_HIDDEN(__got_end__ = ADDR(.got.plt) + SIZEOF(.got.plt));

    . = ALIGN(0x1000);
    PROVIDE_HIDDEN(__data_start = .);
    .data : { *(.data .data.* .gnu.linkonce.d.*) SORT(CONSTRUCTORS) . = ALIGN(8); } :data
    .tdata : { *(.tdata .tdata.* .gnu.linkonce.td.*) . = ALIGN(8); } :data :tls
    PROVIDE_HIDDEN(__tdata_lma = ADDR(.tdata));
    PROVIDE_HIDDEN(__tdata_lma_end = ADDR(.tdata) + SIZEOF(.tdata));
    .tbss : { *(.tbss .tbss.* .gnu.linkonce.tb.*) *(.tcommon) . = ALIGN(8); } :data :tls
    .bss : { *(.dynbss) *(.bss .bss.* .gnu.linkonce.b.*) *(COMMON) . = ALIGN(8); } :data
    .main.tls ALIGN(MAX(ALIGNOF(.tdata), ALIGNOF(.tbss))) : { . += SIZEOF(.tdata); . = ALIGN(ALIGNOF(.tbss)); . += SIZEOF(.tbss); } :data
    PROVIDE_HIDDEN(__tls_start = ADDR(.main.tls));
    PROVIDE_HIDDEN(__tls_end = ADDR(.main.tls) + SIZEOF(.main.tls));
    PROVIDE_HIDDEN(__bss_start__ = ADDR(.bss));
    PROVIDE_HIDDEN(__bss_end__ = __tls_end);
    PROVIDE_HIDDEN(__end__ = .);
    . = ALIGN(0x1000);
    PROVIDE_HIDDEN(__argdata__ = .);
    /DISCARD/ : { *(.group .comment .note) }
    .debug 0 : { *(.debug) }
    .line 0 : { *(.line) }
    .debug_aranges 0 : { *(.debug_aranges) }
    .debug_info 0 : { *(.debug_info .gnu.linkonce.wi.*) }
    .debug_abbrev 0 : { *(.debug_abbrev) }
    .debug_line 0 : { *(.debug_line .debug_line.* .debug_line_end) }
    .debug_frame 0 : { *(.debug_frame) }
    .debug_str 0 : { *(.debug_str) }
    .debug_line_str 0 : { *(.debug_line_str) }
    .debug_loc 0 : { *(.debug_loc) }
    .debug_ranges 0 : { *(.debug_ranges) }
    .debug_loclists 0 : { *(.debug_loclists) }
    .debug_rnglists 0 : { *(.debug_rnglists) }
    .ARM.attributes 0 : { KEEP(*(.ARM.attributes)) KEEP(*(.gnu.attributes)) }
}
