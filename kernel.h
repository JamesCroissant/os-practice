// Declarations for everything kernel.c implements -- split out following
// the convention of the project this kernel is modeled on
// (github.com/nuta/operating-system-in-1000-lines, cited in README.md),
// whose own kernel.h/kernel.c split separates "what exists" from "how it
// works." kernel.c has grown past the point where skimming it top to
// bottom was the only way to see the shape of the kernel; this is that
// shape. The "why" behind each piece stays in kernel.c, next to its
// implementation, not duplicated here.

#ifndef KERNEL_H
#define KERNEL_H

#include <stdarg.h>
#include <stddef.h>  // NULL only -- freestanding-safe, no libc functions

typedef unsigned char uint8_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef uint32_t size_t;

// A physical address -- distinct from an ordinary pointer once virtual
// memory exists (Step 9).
typedef uint32_t paddr_t;

// Provided by kernel.ld: where the kernel image starts, the bounds of the
// zero-initialized data segment, the top of the stack region reserved
// after it, the page-aligned range holding just user_entry's own code,
// and the range handed out by alloc_pages().
extern char __kernel_base[], __bss[], __bss_end[], __stack_top[], __free_ram[],
    __free_ram_end[], __user_text_start[], __user_text_end[];

void *memset(void *buf, char c, size_t n);

// An SBI call's result: error code plus a return value, matching the
// modern SBI calling convention (see sbi_call()'s own comment in
// kernel.c for why legacy extensions still get interpreted this way).
struct sbiret {
    long error;
    long value;
};

struct sbiret sbi_call(long arg0, long arg1, long arg2, long arg3, long arg4,
                        long arg5, long fid, long eid);
void putchar(char ch);
void printf(const char *fmt, ...);

#define PAGE_SIZE 4096

paddr_t alloc_pages(uint32_t n);

// Both carry a "memory" clobber: csrr/csrw touch machine state (sie,
// sstatus, satp, stvec, ...) that memory accesses can depend on in ways
// the compiler has no other way to know about, so without it, nothing
// stops the optimizer from treating these as pure, memory-independent
// operations and reordering ordinary loads/stores across them -- see
// docs/JOURNAL.md Steps 17-18 for the sfence.vma instance that first
// surfaced this and the generalization to these two macros.
#define READ_CSR(reg)                                                   \
    ({                                                                  \
        unsigned long __tmp;                                            \
        __asm__ __volatile__("csrr %0, " #reg : "=r"(__tmp) :: "memory"); \
        __tmp;                                                          \
    })

#define WRITE_CSR(reg, value) \
    __asm__ __volatile__("csrw " #reg ", %0" :: "r"(value) : "memory")

#define TIMER_INTERVAL 1000000  // ~0.1s, going by this machine's reported 10MHz mtimer

uint64_t read_time(void);
void arm_timer(void);

// Sv32 PTE flag bits.
#define PAGE_V (1 << 0)  // valid
#define PAGE_R (1 << 1)  // readable
#define PAGE_W (1 << 2)  // writable
#define PAGE_X (1 << 3)  // executable
#define PAGE_U (1 << 4)  // user-mode accessible

void map_page(uint32_t *table1, uint32_t vaddr, paddr_t paddr, uint32_t flags);

// There's only one page table so far (no per-process address spaces
// yet) -- built once in kernel_main, global so user_launcher_entry() can
// also call map_page() against it.
extern uint32_t *kernel_page_table;

#define SATP_SV32 (1u << 31)

void enable_paging(uint32_t *table1);

// Register state saved/restored around a trap -- see kernel.c's own
// comment on kernel_entry for why sepc/sstatus ride along with the GPRs.
struct trap_frame {
    uint32_t ra, gp, tp, t0, t1, t2, s0, s1;
    uint32_t a0, a1, a2, a3, a4, a5, a6, a7;
    uint32_t s2, s3, s4, s5, s6, s7, s8, s9, s10, s11;
    uint32_t t3, t4, t5, t6;
    uint32_t sepc, sstatus;
};

#define SCAUSE_SUPERVISOR_TIMER_INTERRUPT 0x80000005
#define SCAUSE_ECALL_FROM_U_MODE 8

// Our own syscall ABI -- not SBI's (see handle_trap's comment in kernel.c).
#define SYS_PUTCHAR 1
#define SYS_EXIT 2

void handle_trap(struct trap_frame *f);
void kernel_entry(void);

#define THREAD_STACK_SIZE 8192

enum thread_state {
    THREAD_UNUSED,
    THREAD_RUNNABLE,
};

struct thread {
    uint32_t sp;
    enum thread_state state;
};

void switch_context(uint32_t *prev_sp, uint32_t *next_sp);
void thread_trampoline(void);
void thread_init(struct thread *th, void (*entry)(void));

#define MAX_THREADS 8

extern struct thread threads[MAX_THREADS];
extern struct thread idle_thread;
extern struct thread *current_thread;

struct thread *thread_create(void (*entry)(void));
void yield(void);
__attribute__((noreturn)) void thread_exit(void);

void thread_a_entry(void);
void thread_b_entry(void);
void thread_c_entry(void);
void thread_d_entry(void);
void thread_e_entry(void);

#define SSTATUS_SPP (1 << 8)   // trap-return privilege: 0 = U-mode, 1 = S-mode
#define SSTATUS_SPIE (1 << 5)  // restored into SIE by sret
#define SSTATUS_SUM (1 << 18)  // permit S-mode loads/stores to PAGE_U pages

__attribute__((noreturn)) void enter_user_mode(uint32_t entry, uint32_t user_sp);
void user_entry(void);
void user_launcher_entry(void);

void kernel_main(void);
void boot(void);

#endif  // KERNEL_H
