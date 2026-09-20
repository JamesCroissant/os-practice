#include <stdarg.h>
#include <stddef.h>  // NULL only -- freestanding-safe, no libc functions

typedef unsigned char uint8_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
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

// Timer interrupts: the SBI legacy "Set Timer" extension (EID 0) arms a
// one-shot interrupt for an absolute `time` value; OpenSBI delivers it as
// a supervisor timer interrupt (scause 0x80000005) once the clock reaches
// that value. There's no periodic mode -- each interrupt has to re-arm
// the next one itself, which handle_trap does below.
#define TIMER_INTERVAL 1000000  // ~0.1s, going by this machine's reported 10MHz mtimer

uint64_t read_time(void) {
    // RV32 has no single 64-bit read of `time`; reading the 32-bit low
    // and high halves separately risks catching a rollover between the
    // two reads, so re-check the high half afterward and retry if it
    // moved.
    uint32_t lo, hi;
    do {
        hi = (uint32_t)READ_CSR(timeh);
        lo = (uint32_t)READ_CSR(time);
    } while ((uint32_t)READ_CSR(timeh) != hi);
    return ((uint64_t)hi << 32) | lo;
}

void arm_timer(void) {
    uint64_t next = read_time() + TIMER_INTERVAL;
    sbi_call((long)(uint32_t)next, (long)(uint32_t)(next >> 32), 0, 0, 0, 0, 0, 0);
}

// A CPU normally just keeps executing the next instruction, but on a
// syscall, an interrupt from hardware, or an invalid instruction, it
// instead "traps": control transfers to whatever address is in stvec.
//
// Unlike Step 3's handler (which only ever panicked and halted), a timer
// interrupt has to be resumable: the interrupted thread hasn't finished,
// it just got preempted, so kernel_entry must save *every* register (not
// just the callee-saved ones switch_context cares about -- an interrupt
// can land in the middle of any instruction sequence, not just at a
// function-call boundary) and end with `sret` instead of falling off.
struct trap_frame {
    uint32_t ra, gp, tp, t0, t1, t2, s0, s1;
    uint32_t a0, a1, a2, a3, a4, a5, a6, a7;
    uint32_t s2, s3, s4, s5, s6, s7, s8, s9, s10, s11;
    uint32_t t3, t4, t5, t6;
};

#define SCAUSE_SUPERVISOR_TIMER_INTERRUPT 0x80000005

// Defined later, once the thread pool exists -- forward-declared here so
// handle_trap can call it on a timer interrupt.
void yield(void);

void handle_trap(struct trap_frame *f) {
    (void)f;  // not inspected yet -- only scause/sepc drive any decision so far
    uint32_t scause = READ_CSR(scause);
    uint32_t sepc = READ_CSR(sepc);

    if (scause == SCAUSE_SUPERVISOR_TIMER_INTERRUPT) {
        arm_timer();
        // yield() may switch_context() into a completely different
        // thread; this call only returns once the scheduler picks *this*
        // thread again, at which point kernel_entry resumes exactly as
        // if nothing happened.
        yield();
        return;
    }

    printf("\nPANIC: unexpected trap scause=%x, sepc=%x\n", scause, sepc);
    for (;;) {
        __asm__ __volatile__("wfi");
    }
}

// Saves all 30 non-zero, non-sp registers (sp itself is preserved simply
// by the push/pop being balanced -- nothing needs to record its value)
// onto the interrupted thread's own stack, calls handle_trap with a
// pointer to that saved frame, then restores everything and `sret`s.
// RISC-V requires stvec (which holds this function's address) to be
// 4-byte aligned.
__attribute__((naked))
__attribute__((aligned(4)))
void kernel_entry(void) {
    __asm__ __volatile__(
        "addi sp, sp, -4 * 30\n"
        "sw ra,   4 * 0(sp)\n"
        "sw gp,   4 * 1(sp)\n"
        "sw tp,   4 * 2(sp)\n"
        "sw t0,   4 * 3(sp)\n"
        "sw t1,   4 * 4(sp)\n"
        "sw t2,   4 * 5(sp)\n"
        "sw s0,   4 * 6(sp)\n"
        "sw s1,   4 * 7(sp)\n"
        "sw a0,   4 * 8(sp)\n"
        "sw a1,   4 * 9(sp)\n"
        "sw a2,   4 * 10(sp)\n"
        "sw a3,   4 * 11(sp)\n"
        "sw a4,   4 * 12(sp)\n"
        "sw a5,   4 * 13(sp)\n"
        "sw a6,   4 * 14(sp)\n"
        "sw a7,   4 * 15(sp)\n"
        "sw s2,   4 * 16(sp)\n"
        "sw s3,   4 * 17(sp)\n"
        "sw s4,   4 * 18(sp)\n"
        "sw s5,   4 * 19(sp)\n"
        "sw s6,   4 * 20(sp)\n"
        "sw s7,   4 * 21(sp)\n"
        "sw s8,   4 * 22(sp)\n"
        "sw s9,   4 * 23(sp)\n"
        "sw s10,  4 * 24(sp)\n"
        "sw s11,  4 * 25(sp)\n"
        "sw t3,   4 * 26(sp)\n"
        "sw t4,   4 * 27(sp)\n"
        "sw t5,   4 * 28(sp)\n"
        "sw t6,   4 * 29(sp)\n"

        "mv a0, sp\n"
        "call handle_trap\n"

        "lw ra,   4 * 0(sp)\n"
        "lw gp,   4 * 1(sp)\n"
        "lw tp,   4 * 2(sp)\n"
        "lw t0,   4 * 3(sp)\n"
        "lw t1,   4 * 4(sp)\n"
        "lw t2,   4 * 5(sp)\n"
        "lw s0,   4 * 6(sp)\n"
        "lw s1,   4 * 7(sp)\n"
        "lw a0,   4 * 8(sp)\n"
        "lw a1,   4 * 9(sp)\n"
        "lw a2,   4 * 10(sp)\n"
        "lw a3,   4 * 11(sp)\n"
        "lw a4,   4 * 12(sp)\n"
        "lw a5,   4 * 13(sp)\n"
        "lw a6,   4 * 14(sp)\n"
        "lw a7,   4 * 15(sp)\n"
        "lw s2,   4 * 16(sp)\n"
        "lw s3,   4 * 17(sp)\n"
        "lw s4,   4 * 18(sp)\n"
        "lw s5,   4 * 19(sp)\n"
        "lw s6,   4 * 20(sp)\n"
        "lw s7,   4 * 21(sp)\n"
        "lw s8,   4 * 22(sp)\n"
        "lw s9,   4 * 23(sp)\n"
        "lw s10,  4 * 24(sp)\n"
        "lw s11,  4 * 25(sp)\n"
        "lw t3,   4 * 26(sp)\n"
        "lw t4,   4 * 27(sp)\n"
        "lw t5,   4 * 28(sp)\n"
        "lw t6,   4 * 29(sp)\n"
        "addi sp, sp, 4 * 30\n"
        "sret\n"
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

// Forward-declared so thread_trampoline can call it below -- defined
// later, once current_thread and yield() exist.
__attribute__((noreturn)) void thread_exit(void);

// A thread switched to via a timer interrupt normally resumes through
// kernel_entry's own `sret`, which is what re-enables interrupts
// (hardware automatically clears sstatus.SIE on trap entry and restores
// it from sstatus.SPIE on sret). A thread running for the very first
// time never goes through that `sret` at all -- switch_context's `ret`
// jumps straight to its entry point -- so it would otherwise start
// running with interrupts still disabled from whatever trap led here,
// silently killing all future preemption. This trampoline is what every
// new thread's saved "ra" actually points to, so that gap has exactly
// one place to be closed: enable interrupts explicitly, then call the
// real entry point (left in s0 by thread_init, and reloaded into s0 by
// switch_context's own restore before we get here) -- `jalr`, not `jr`,
// so a thread whose entry function returns normally (unlike thread_a/b/c,
// which never do) falls into thread_exit instead of jumping into
// whatever garbage instructions happen to sit after this function.
__attribute__((naked)) void thread_trampoline(void) {
    __asm__ __volatile__(
        "csrsi sstatus, 2\n"  // sstatus.SIE (bit 1) = 1
        "jalr s0\n"
        "call thread_exit\n"
    );
}

// Prepares a thread that has never run yet: switch_context's restore
// side always pops 13 words expecting ra, s0..s11 in that order, so a
// brand new thread needs that exact frame pre-built on its own stack.
void thread_init(struct thread *th, void (*entry)(void)) {
    uint32_t *sp = (uint32_t *)(th->stack + sizeof(th->stack));
    sp -= 13;
    memset(sp, 0, 13 * sizeof(uint32_t));
    sp[0] = (uint32_t)thread_trampoline;  // ra
    sp[1] = (uint32_t)entry;              // s0 -- read by the trampoline above
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

// Reached when a thread's entry function returns, via thread_trampoline's
// `call` after `jalr s0` (see above) -- not just on explicit request.
// There's nowhere to "return" to (the trampoline that got it here wasn't
// called from anywhere resumable), so this is the only sane thing left to
// do: mark the slot free, so thread_create can reuse it, and give up the
// CPU for good. The trailing loop never actually runs a second iteration
// in practice -- once state is THREAD_UNUSED, yield() can never pick this
// thread again, so the first yield() call here is also the last time this
// stack is ever touched.
__attribute__((noreturn)) void thread_exit(void) {
    current_thread->state = THREAD_UNUSED;
    for (;;) {
        yield();
    }
}

// No yield() calls in these bodies anymore -- on purpose. If interleaved
// A/B/C output still shows up, that proves the timer interrupt is
// genuinely preempting a thread that never asks to give up the CPU,
// rather than just observing cooperative yielding again.
void thread_a_entry(void) {
    for (;;) {
        printf("A");
    }
}

void thread_b_entry(void) {
    for (;;) {
        printf("B");
    }
}

void thread_c_entry(void) {
    for (;;) {
        printf("C");
    }
}

// Unlike thread_a/b/c (deliberately infinite, to prove preemption keeps
// working on a thread that never cooperates), this one does finite work
// and returns -- to prove thread_exit's path through thread_trampoline
// actually runs, not just that it compiles.
void thread_d_entry(void) {
    for (int i = 0; i < 5; i++) {
        printf("D");
    }
}

void kernel_main(void) {
    // The linker only reserves space for .bss; nothing has zeroed it yet.
    memset(__bss, 0, (size_t)__bss_end - (size_t)__bss);

    WRITE_CSR(stvec, (uint32_t)kernel_entry);

    // Enable supervisor timer interrupts (sie bit 5, STIE) and global
    // S-mode interrupt delivery (sstatus bit 1, SIE), then arm the first
    // one -- without this, an OS is stuck with only cooperative
    // scheduling: a thread that never calls yield() keeps the CPU
    // forever.
    WRITE_CSR(sie, READ_CSR(sie) | (1 << 5));
    WRITE_CSR(sstatus, READ_CSR(sstatus) | (1 << 1));
    arm_timer();

    printf("\n\nHello World!\n");
    printf("1 + 2 = %d, 0x%x\n", 1 + 2, 0x1234abcd);

    // Three threads, not two -- proving the scheduler actually decides
    // who runs next instead of two threads just hardcoding each other.
    thread_create(thread_a_entry);
    thread_create(thread_b_entry);
    thread_create(thread_c_entry);
    thread_create(thread_d_entry);

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
