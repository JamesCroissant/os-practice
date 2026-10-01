ENTRY(boot)

SECTIONS {
    /* OpenSBI (fw_dynamic) jumps to S-mode here on the QEMU "virt" machine. */
    . = 0x80200000;
    __kernel_base = .;

    .text :{
        KEEP(*(.text.boot));
        *(.text);
    }

    /* user_entry (section .text.user) gets its own page-aligned output
       section instead of matching the generic .text above (which, on
       purpose, no longer globs .text.* -- it needs the PAGE_U bit set on
       its mapping, and S-mode can never execute a PAGE_U page (see
       Step 11's journal entry), so it must not share a page with any
       code the kernel itself runs. */
    . = ALIGN(4096);
    __user_text_start = .;
    .text.user : {
        *(.text.user)
    }
    . = ALIGN(4096);
    __user_text_end = .;

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
