ENTRY(boot)

SECTIONS {
    /* OpenSBI (fw_dynamic) jumps to S-mode here on the QEMU "virt" machine. */
    . = 0x80200000;

    .text :{
        KEEP(*(.text.boot));
        *(.text .text.*);
    }

    .rodata : ALIGN(4) {
        *(.rodata .rodata.*);
    }

    .data : ALIGN(4) {
        *(.data .data.*);
    }

    .bss : ALIGN(4) {
        __bss = .;
        *(.bss .bss.* .sbss .sbss.*);
        __bss_end = .;
    }

    . = ALIGN(4);
    . += 128 * 1024; /* 128KB kernel stack */
    __stack_top = .;

    /* Everything past here, up to the end of RAM (see Makefile's -m 128M),
       is handed out by alloc_pages() -- a page allocator has to own a
       range it's sure nothing else is using. */
    . = ALIGN(4096);
    __free_ram = .;
    . += 64 * 1024 * 1024; /* 64MB, well inside the 128MB RAM given to QEMU */
    __free_ram_end = .;
}
