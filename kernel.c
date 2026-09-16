#include <stdarg.h>

typedef unsigned char uint8_t;
typedef unsigned int uint32_t;
typedef uint32_t size_t;

// Provided by kernel.ld: the bounds of the zero-initialized data segment,
// and the top of the stack region reserved after it.
extern char __bss[], __bss_end[], __stack_top[];

void *memset(void *buf, char c, size_t n) {
    uint8_t *p = (uint8_t *)buf;
    while (n--)
        *p++ = c;
    return buf;
}

// An SBI call is the RISC-V analog of a syscall, but from kernel (S-mode)
// to firmware (M-mode) instead of from userspace to the kernel: put the
// extension ID in a7, the function ID in a6, arguments in a0-a5, and
// execute `ecall`. Firmware handles it and returns with a0/a1 holding
// the result.
struct sbiret {
    long error;
    long value;
};

struct sbiret sbi_call(long arg0, long arg1, long arg2, long arg3, long arg4,
                        long arg5, long fid, long eid) {
    register long a0 __asm__("a0") = arg0;
    register long a1 __asm__("a1") = arg1;
    register long a2 __asm__("a2") = arg2;
    register long a3 __asm__("a3") = arg3;
    register long a4 __asm__("a4") = arg4;
    register long a5 __asm__("a5") = arg5;
    register long a6 __asm__("a6") = fid;
    register long a7 __asm__("a7") = eid;

    __asm__ __volatile__("ecall"
                          : "=r"(a0), "=r"(a1)
                          : "r"(a0), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a6), "r"(a7)
                          : "memory");

    return (struct sbiret){.error = a0, .value = a1};
}

// Legacy extension 0x01 ("Console Putchar"): writes one character to the
// debug console. There's no modern replacement extension we need yet, so
// this is the whole of our output story for now.
void putchar(char ch) {
    sbi_call(ch, 0, 0, 0, 0, 0, 0, 1);
}

// A small printf: %s, %d, %x, %%. This is more than the single-format
// demo it started as, since the upcoming trap handler wants %d (for
// scause) and %s (for messages) too -- not worth writing three ad-hoc
// print helpers instead of one small, real printf.
void printf(const char *fmt, ...) {
    va_list vararg;
    va_start(vararg, fmt);

    while (*fmt) {
        if (*fmt != '%') {
            putchar(*fmt);
            fmt++;
            continue;
        }

        fmt++;
        switch (*fmt) {
            case '\0':
                putchar('%');
                goto end;
            case '%':
                putchar('%');
                break;
            case 's': {
                const char *s = va_arg(vararg, const char *);
                while (*s)
                    putchar(*s++);
                break;
            }
            case 'd': {
                int value = va_arg(vararg, int);
                unsigned magnitude = (unsigned)value;
                if (value < 0) {
                    putchar('-');
                    magnitude = (unsigned)(-value);
                }
                unsigned divisor = 1;
                while (magnitude / divisor > 9)
                    divisor *= 10;
                while (divisor > 0) {
                    putchar((char)('0' + magnitude / divisor));
                    magnitude %= divisor;
                    divisor /= 10;
                }
                break;
            }
            case 'x': {
                unsigned value = va_arg(vararg, unsigned);
                for (int i = 7; i >= 0; i--)
                    putchar("0123456789abcdef"[(value >> (i * 4)) & 0xf]);
                break;
            }
        }
        fmt++;
    }
end:
    va_end(vararg);
}

#define READ_CSR(reg)                                             \
    ({                                                            \
        unsigned long __tmp;                                      \
        __asm__ __volatile__("csrr %0, " #reg : "=r"(__tmp));     \
        __tmp;                                                    \
    })

#define WRITE_CSR(reg, value) __asm__ __volatile__("csrw " #reg ", %0" ::"r"(value))

// A CPU normally just keeps executing the next instruction, but on a
// syscall, an interrupt from hardware, or (here) an invalid instruction,
// it instead "traps": control transfers to whatever address is in stvec.
// This handler doesn't do anything to fix the situation -- it just
// reports it and halts, which is enough to prove the trap plumbing
// itself works. Handling traps well enough to *resume* the interrupted
// code (needed once threads/syscalls exist) is Step 4's job.
void handle_trap(uint32_t scause, uint32_t sepc) {
    printf("\nPANIC: unexpected trap scause=%x, sepc=%x\n", scause, sepc);
    for (;;) {
        __asm__ __volatile__("wfi");
    }
}

// scause/sepc are read directly into a0/a1 here so they land exactly
// where handle_trap(scause, sepc) expects its arguments -- no need to
// save every register first since this handler never returns to the
// trapping code. RISC-V requires stvec (which holds this function's
// address) to be 4-byte aligned.
__attribute__((naked))
__attribute__((aligned(4)))
void kernel_entry(void) {
    __asm__ __volatile__(
        "csrr a0, scause\n"
        "csrr a1, sepc\n"
        "call handle_trap\n"
    );
}

void kernel_main(void) {
    // The linker only reserves space for .bss; nothing has zeroed it yet.
    memset(__bss, 0, (size_t)__bss_end - (size_t)__bss);

    WRITE_CSR(stvec, (uint32_t)kernel_entry);

    printf("\n\nHello World!\n");
    printf("1 + 2 = %d, 0x%x\n", 1 + 2, 0x1234abcd);

    // Deliberately trigger a trap to prove the handler above actually
    // runs: `unimp` is a reserved all-zero instruction encoding RISC-V
    // guarantees will always be illegal.
    __asm__ __volatile__("unimp");

    for (;;) {
        __asm__ __volatile__("wfi");
    }
}

// OpenSBI jumps here with no stack set up -- naked + inline asm means
// this function is exactly the instructions below, nothing the compiler
// might insert (like a prologue that touches the stack before we've set
// sp to anything valid).
__attribute__((section(".text.boot")))
__attribute__((naked))
void boot(void) {
    __asm__ __volatile__(
        "mv sp, %[stack_top]\n"
        "j kernel_main\n"
        :
        : [stack_top] "r" (__stack_top)
    );
}
