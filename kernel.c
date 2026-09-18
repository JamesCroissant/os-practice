#include <stdarg.h>
#include <stddef.h>  // NULL only -- freestanding-safe, no libc functions

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

// A context switch saves the CPU state of the thread giving up the CPU
// and restores the state of the one taking over -- that's the entire
// mechanism multiple threads sharing one CPU are built on. "CPU state"
// here means the callee-saved registers (ra, s0-s11: the ones a function
// is required to preserve across a call) -- caller-saved registers and
// globals need no help, since the caller already protects its own and
// code/globals are shared between threads anyway.
#define THREAD_STACK_SIZE 8192

enum thread_state {
    THREAD_UNUSED,
    THREAD_RUNNABLE,
};

struct thread {
    uint32_t sp;
    enum thread_state state;
    uint8_t stack[THREAD_STACK_SIZE];
};

// Pushes the 13 callee-saved words onto the current stack, records where
// they ended up (*prev_sp), then does the reverse for the incoming
// thread: loads its saved sp, pops its 13 words back into the same
// registers, and returns -- landing at whatever address the incoming
// thread's saved `ra` holds.
// prev_sp/next_sp are read directly via a0/a1 in the asm below, not as C
// parameters -- naked functions shouldn't contain ordinary C statements
// (no guaranteed stack frame), so __attribute__((unused)) silences the
// warning instead of a (void) cast in the body.
__attribute__((naked)) void switch_context(uint32_t *prev_sp __attribute__((unused)),
                                            uint32_t *next_sp __attribute__((unused))) {
    __asm__ __volatile__(
        "addi sp, sp, -13 * 4\n"
        "sw ra,   0 * 4(sp)\n"
        "sw s0,   1 * 4(sp)\n"
        "sw s1,   2 * 4(sp)\n"
        "sw s2,   3 * 4(sp)\n"
        "sw s3,   4 * 4(sp)\n"
        "sw s4,   5 * 4(sp)\n"
        "sw s5,   6 * 4(sp)\n"
        "sw s6,   7 * 4(sp)\n"
        "sw s7,   8 * 4(sp)\n"
        "sw s8,   9 * 4(sp)\n"
        "sw s9,  10 * 4(sp)\n"
        "sw s10, 11 * 4(sp)\n"
        "sw s11, 12 * 4(sp)\n"

        "sw sp, (a0)\n"
        "lw sp, (a1)\n"

        "lw ra,   0 * 4(sp)\n"
        "lw s0,   1 * 4(sp)\n"
        "lw s1,   2 * 4(sp)\n"
        "lw s2,   3 * 4(sp)\n"
        "lw s3,   4 * 4(sp)\n"
        "lw s4,   5 * 4(sp)\n"
        "lw s5,   6 * 4(sp)\n"
        "lw s6,   7 * 4(sp)\n"
        "lw s7,   8 * 4(sp)\n"
        "lw s8,   9 * 4(sp)\n"
        "lw s9,  10 * 4(sp)\n"
        "lw s10, 11 * 4(sp)\n"
        "lw s11, 12 * 4(sp)\n"
        "addi sp, sp, 13 * 4\n"
        "ret\n"
    );
}

// Prepares a thread that has never run yet: switch_context's restore
// side always pops 13 words expecting ra, s0..s11 in that order, so a
// brand new thread needs that exact frame pre-built on its own stack --
// only `ra` (the entry point) matters, since s0-s11 haven't been read by
// anyone yet.
void thread_init(struct thread *th, void (*entry)(void)) {
    uint32_t *sp = (uint32_t *)(th->stack + sizeof(th->stack));
    sp -= 13;
    memset(sp, 0, 13 * sizeof(uint32_t));
    sp[0] = (uint32_t)entry;  // ra
    th->sp = (uint32_t)sp;
}

// A fixed-size pool instead of thread_a/thread_b as named globals: with a
// hardcoded pair, each thread can only ever hand off to "the other one",
// hardcoded by name. That stops working the moment there's a third
// thread -- with N threads, *something* has to decide who runs next.
// That something is the scheduler below.
#define MAX_THREADS 8

struct thread threads[MAX_THREADS];

// Represents kernel_main's own execution while it's not running any of
// the threads above. It's never in the pool and never picked by yield()
// as a destination -- only ever a place for its own state to be saved
// while some thread runs -- so its `state` field is simply never read.
struct thread idle_thread;
struct thread *current_thread = &idle_thread;

struct thread *thread_create(void (*entry)(void)) {
    for (int i = 0; i < MAX_THREADS; i++) {
        if (threads[i].state == THREAD_UNUSED) {
            thread_init(&threads[i], entry);
            threads[i].state = THREAD_RUNNABLE;
            return &threads[i];
        }
    }
    printf("PANIC: no free thread slots\n");
    for (;;) {
        __asm__ __volatile__("wfi");
    }
}

// Round-robin: starting just after the current thread's slot, find the
// next RUNNABLE one and switch to it. A thread never needs to know who
// it's handing off to -- it just calls yield() and trusts the scheduler.
void yield(void) {
    int current_index = (int)(current_thread - threads);  // negative/out-of-range for idle_thread, which is fine: it's never a valid match below

    struct thread *next = NULL;
    for (int offset = 1; offset <= MAX_THREADS; offset++) {
        int i = (current_index + offset) % MAX_THREADS;
        if (i < 0) {
            i += MAX_THREADS;
        }
        if (threads[i].state == THREAD_RUNNABLE) {
            next = &threads[i];
            break;
        }
    }

    if (next == NULL || next == current_thread) {
        return;  // nothing else runnable; keep running (or stay idle)
    }

    struct thread *prev = current_thread;
    current_thread = next;
    switch_context(&prev->sp, &next->sp);
}

void thread_a_entry(void) {
    for (;;) {
        printf("A");
        yield();
    }
}

void thread_b_entry(void) {
    for (;;) {
        printf("B");
        yield();
    }
}

void thread_c_entry(void) {
    for (;;) {
        printf("C");
        yield();
    }
}

void kernel_main(void) {
    // The linker only reserves space for .bss; nothing has zeroed it yet.
    memset(__bss, 0, (size_t)__bss_end - (size_t)__bss);

    WRITE_CSR(stvec, (uint32_t)kernel_entry);

    printf("\n\nHello World!\n");
    printf("1 + 2 = %d, 0x%x\n", 1 + 2, 0x1234abcd);

    // Three threads, not two -- proving the scheduler actually decides
    // who runs next instead of two threads just hardcoding each other.
    thread_create(thread_a_entry);
    thread_create(thread_b_entry);
    thread_create(thread_c_entry);

    // kernel_main is current_thread's placeholder (idle_thread) until
    // this first yield() hands off to whichever thread the scheduler
    // picks; nothing ever switches back to idle_thread afterwards.
    yield();

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
