# Build journal

What each step adds, why, and what actually broke while building it.

## Step 1+2: boot, SBI calls, and "Hello World"

### Boot

QEMU's `virt` machine, given `-bios <opensbi>`, starts OpenSBI in M-mode.
OpenSBI does minimal setup and jumps to S-mode at the kernel's load
address. At that point there is no valid stack yet — `boot()` in
`kernel.c` is a `naked` function containing nothing but inline assembly
that sets `sp` to `__stack_top` (computed by `kernel.ld`, 128KB above
`.bss`) and jumps to `kernel_main`. `naked` matters here: a normal
function's compiler-generated prologue might touch the stack before we've
pointed `sp` anywhere valid.

`kernel_main` immediately zeroes `.bss` (`__bss` to `__bss_end`, also from
`kernel.ld`) — the linker only reserves that space, nothing has
initialized it yet.

### SBI calls

SBI (Supervisor Binary Interface) is the RISC-V analog of a syscall, but
one level down: kernel (S-mode) asking firmware (M-mode) to do something,
via `ecall`. The legacy "Console Putchar" extension (EID `0x01`) takes one
argument (the character) in `a0` and needs no function ID. `sbi_call()`
puts the extension ID in `a7`, function ID in `a6`, and up to 6 arguments
in `a0`-`a5`, matching the C calling convention closely enough that at
`-O2` the compiler mostly just moves the *already-correctly-positioned*
function arguments straight into the `ecall`.

`putchar()` wraps that for EID 1. `printf()` (`%s`, `%d`, `%x`, `%%`) is
built on top of `putchar()` alone — more format specifiers than the talk's
own demo (which only needed `%x`), added because the upcoming trap handler
wants `%d` for `scause` and `%s` for messages, and a real minimal `printf`
isn't much more code than a hex-only one.

### The bug: QEMU jumped into non-code bytes

First build produced *zero* output — not even a crash, just silence after
the OpenSBI banner. `-d int -D trace.log` showed the real story: QEMU was
repeatedly raising an illegal-instruction trap at `epc:0x80200000` (our
kernel's supposed entry address) and never getting anywhere.

`readelf -l` explained why: the build was producing a **PIE, dynamically-
linked** executable (an `INTERP` segment requesting
`/lib/ld-linux-riscv32-ilp32.so.1`, a `DYNAMIC` segment, `.dynsym`/`.got`/
`.plt`) — Ubuntu's GCC defaults to PIE, which is completely wrong for a
freestanding kernel with no loader. That pushed the first `LOAD` segment's
address to start *before* `.text` (ELF/program headers first), and —
this was the actual surprising part — QEMU's kernel loader jumps to the
**first LOAD segment's address**, not the ELF's `e_entry` field. Once
those two addresses differ, QEMU executes whatever bytes happen to sit at
the segment's start, code or not.

Two fixes, both in `Makefile`:
- `-static -fno-pie -no-pie -Wl,--no-dynamic-linker` to kill the PIE/
  dynamic-linking segments entirely.
- `-Wl,--build-id=none` because even after that fix, a `.note.gnu.build-id`
  section was still landing *before* `.text.boot` at the segment's start
  (build-id notes get emitted early by default) — 36 bytes of ELF note
  data, decoded as instructions, is exactly the kind of thing that looks
  like an illegal instruction.

After both, `readelf -h`'s entry point and `readelf -l`'s first `LOAD`
segment address matched (`0x80200000`), and `boot` — not a stray note —
was the first thing at that address. "Hello World!" and the `%d`/`%x`
test line printed immediately.

**Verified**: `make run`'s serial output shows the OpenSBI banner followed
immediately by `Hello World!` and `1 + 2 = 3, 0x1234abcd` — confirming
boot, the stack, `.bss` zeroing, `sbi_call`, `putchar`, and `printf`'s
`%d`/`%x` paths all work together.

## Step 3: trap handler

A CPU trap (syscall, hardware interrupt, or — the case exercised here —
an invalid instruction) transfers control to whatever address is in the
`stvec` CSR. `kernel_entry` reads `scause` (why) and `sepc` (where)
directly into `a0`/`a1` and calls `handle_trap(scause, sepc)`, which
prints both and halts. No register-saving beyond that: this handler never
resumes the code that trapped, so there's nothing to restore later. RISC-V
requires `stvec` to be 4-byte aligned, hence
`__attribute__((aligned(4)))` on `kernel_entry`.

Tested by deliberately executing `unimp` — a reserved, all-zero
instruction encoding that RISC-V guarantees is always illegal — right
after the Step 2 printf calls.

**Verified**: output shows `scause=00000002` (RISC-V's standard "illegal
instruction" exception code) and `sepc=8020023e`. Cross-checked against
`objdump -d`: `8020023e` is exactly the `unimp` instruction's address,
confirming the trap fired for the right reason at the right place, not
just that *some* trap happened to occur.

## Step 4: context switching

Multiple threads sharing one CPU comes down to one mechanism: save the
CPU state of the thread giving up the CPU, restore the state of the one
taking over. "CPU state" here means just the callee-saved registers
(`ra`, `s0`-`s11` — the ones a function is contractually required to
preserve across a call); caller-saved registers need no help since the
caller already protects its own, and code/globals are shared between
threads regardless.

`switch_context(prev_sp, next_sp)` pushes those 13 words onto the
*current* stack, stashes the resulting `sp` into `*prev_sp`, then does
the exact reverse using `*next_sp`: load the incoming thread's saved
`sp`, pop the 13 words back into the same registers, `ret`. That `ret`
is the whole trick — it jumps to whatever `ra` was just popped, which for
a thread's first-ever switch is wherever `thread_init` put its entry
point.

`thread_init` has to build that 13-word frame by hand for a thread that
has never run, since `switch_context`'s restore side unconditionally pops
13 words no matter what's actually there — only slot 0 (`ra`) is
meaningful the first time; `s0`-`s11` start zeroed since nothing's read
them yet.

Two cooperative threads (`thread_a`/`thread_b`) each print one character
and immediately yield to the other, forever.

**Verified**: captured 2 seconds of serial output — the section following
`Hello World!`/the `%d`/`%x` line is 437,192 characters of exactly
`ABABAB...` (checked programmatically, not just eyeballed), followed only
by the harness's own timeout-kill message. Confirms both directions of
the switch work repeatedly (not just once) and that no register/stack
corruption creeps in over ~200k round trips.

## Step 5: scheduler

The talk itself stops at two threads directly naming each other
(`thread_a` calls `switch_context` on `thread_b` and vice versa) and
says, in closing, that this doesn't scale: a third or fourth thread has
nowhere hardcoded to hand off to, so *something* has to decide who runs
next. That something is a scheduler.

`threads[MAX_THREADS]` replaces the two named globals with a pool, each
slot tagged `THREAD_UNUSED` or `THREAD_RUNNABLE`. `thread_create(entry)`
claims the first free slot and initializes it exactly as `thread_init`
did before. `yield()` is the scheduler itself: starting just after
`current_thread`'s own slot, it scans round-robin for the next
`THREAD_RUNNABLE` thread and switches to it. Threads no longer call
`switch_context` directly or know who they're yielding to — they just
call `yield()`.

`idle_thread` stands in for `kernel_main`'s own execution before the
first `yield()` hands off to whichever thread the scheduler picks; it's
never in the pool and never selected as a destination, only ever a place
for `current_thread` to point at momentarily.

Three threads now (`thread_a`/`thread_b`/`thread_c`, printing A/B/C) —
deliberately more than the two the direct-handoff version could ever
generalize beyond.

**Verified**: over 400,000 characters of serial output following the
Step 1-2 lines are exactly `ABCABCABC...`, confirming round-robin order
across three independently-created threads holds over hundreds of
thousands of switches, not just for the first few.

(Side note on this step's own debugging: `-serial mon:stdio` -- used for
every earlier step's verification -- started silently producing zero
output when re-tested in this session, unrelated to any kernel code
change (a bare OpenSBI boot with no kernel at all also produced nothing).
Swapping to `-serial stdio -monitor none` for the verification commands
in this environment resolved it immediately. `Makefile`'s `make run`
target still uses `mon:stdio`, since it's meant for a real interactive
terminal, where that combination is standard and gives access to the
QEMU monitor via `Ctrl-A C`.)

## Step 6: preemptive scheduling via timer interrupt

Everything so far is cooperative: a thread only ever gives up the CPU by
calling `yield()` itself. The talk's own closing remarks point at what's
missing without saying it outright -- a thread that never calls `yield()`
would simply hang onto the CPU forever. Fixing that needs a hardware
timer interrupt to force a switch, tying the trap handler (Step 3) and
the scheduler (Step 5) together for the first time.

**Enabling it**: `sie` bit 5 (STIE, supervisor timer interrupt enable),
`sstatus` bit 1 (SIE, global S-mode interrupt enable), then arm the first
interrupt via the SBI legacy "Set Timer" extension (EID 0) with an
absolute `time` value read via the `time`/`timeh` CSRs (RV32 exposes the
64-bit mtimer as two 32-bit halves; `read_time()` re-checks the high half
after reading the low one to avoid catching a rollover mid-read). There's
no periodic mode — each timer interrupt has to re-arm the next one
itself.

**Full register save, and `sret`**: Step 3's trap handler only ever
panicked, so it never needed to resume anything. A timer interrupt is
different — it has to return exactly to whatever the interrupted thread
was doing, mid-instruction-sequence, not just at a function-call
boundary — so `kernel_entry` now saves all 30 non-zero, non-sp registers
(not just the 13 callee-saved ones `switch_context` cares about) and
ends with `sret` instead of halting. `handle_trap` takes a
`struct trap_frame *` matching that layout; on a supervisor timer
interrupt (`scause == 0x80000005`) it re-arms the timer and calls
`yield()`, which may switch to a completely different thread's stack.
Handling that correctly means a `switch_context` call nested *inside* a
trap frame, on the same physical stack — this only works because
`switch_context` was already written generically enough to not care what
called it.

**The bug this step actually hit**: after wiring all that up, the timer
only ever fired once in 3 seconds of runtime, not the expected ~28.
`-d int` showed why: thousands of `supervisor_ecall` traps (every
`putchar`) but only a single `s_timer` interrupt, total. RISC-V trap
entry automatically clears `sstatus.SIE` and `sret` automatically
restores it from `sstatus.SPIE` — but a *freshly created* thread's first
run never executes an `sret` at all: `switch_context`'s `ret` jumps
straight to the thread's entry point, since there's no suspended trap to
resume. The first timer interrupt (while `thread_a` was running solo)
preempted into `thread_b`'s brand-new context — which then ran with
interrupts silently still disabled from that trap, so the *next* timer
interrupt fired the hardware timer but was never delivered.

Fix: every new thread's saved `ra` now points at `thread_trampoline`
(with the real entry function stashed in the saved `s0` slot instead),
which explicitly does `csrsi sstatus, 2` before jumping to the real
entry — closing exactly the gap that skipping `sret` left open.

**Verified**: three threads (still A/B/C, still no `yield()` calls in
their bodies at all) produce output in clean, single-letter runs of
roughly 10,000-15,000 characters each, in strict `A, B, C, A, B, C, ...`
order — 29 runs matching 28 traced timer interrupts. Confirms preemption
now recurs correctly (not just once), stays fair across threads that
never cooperate, and correctly resumes a previously-interrupted thread's
exact register state each time it comes back around.

## Step 7: thread exit

Every thread so far (`thread_a`/`b`/`c`) is written as an infinite loop,
so the question of what happens when a thread's entry function actually
*returns* never came up -- but nothing stopped a thread from being written
that way, and `switch_context`'s `ret` landing in `thread_trampoline`
followed by a bare `jr s0` meant a returning entry function would fall
into whatever instructions happen to sit right after `thread_trampoline`
in memory. Not a hang, not a crash with a useful `scause` -- just silent
execution of garbage.

Fix: `thread_trampoline` now uses `jalr s0` (a call, not a tail-jump) so
`s0`'s return address is `thread_trampoline`'s own next instruction,
followed by `call thread_exit`. `thread_exit` marks the current thread's
slot `THREAD_UNUSED` and calls `yield()` in a loop -- it can't free its
own stack while still running on it, so it just gives up the CPU
permanently instead; since a `THREAD_UNUSED` slot is never selected by
`yield()`'s scan, that first `yield()` call is also the last time this
thread ever runs.

A fourth thread, `thread_d`, exists solely to exercise this path: unlike
`a`/`b`/`c` it does finite work (prints `D` five times) and returns
normally.

**Verified**: captured 3 seconds of serial output. The five `D`s appear
as exactly one contiguous run (`DDDDD`, confirmed by scanning byte
offsets, not just eyeballing) partway through the capture, and never
again afterward, while `A`/`B`/`C` keep running in the same interleaved
pattern as before for the rest of the run — confirming `thread_d` ran
once, returned through the new trampoline path without corrupting
anything, and was correctly excluded from the scheduler's rotation from
then on.

## Step 8: physical memory allocation

Every thread's stack so far has been a `uint8_t stack[THREAD_STACK_SIZE]`
array embedded directly in its `struct thread` slot — reserved in `.bss`
for all `MAX_THREADS` slots whether or not a thread is ever created in
them. That was fine as long as the only thing anyone needed memory for
*was* a thread stack; it stops being fine the moment something else
(a page table, a buffer for a driver) also needs RAM, since there's
nowhere to ask for it from.

`alloc_pages(n)` is a minimal fix: a bump allocator over a region
(`__free_ram` to `__free_ram_end`) that `kernel.ld` now reserves in the
`.bss`-and-stack tail, sized at 64MB against the 128MB QEMU is now told
to give the machine explicitly (`-m 128M`, previously left at whatever
QEMU's default happened to be — the allocator's region has to be sized
against a number that's actually guaranteed, not assumed). It never
frees, so no free-list is needed yet: nothing in this kernel has a
lifetime shorter than "forever" so far (`thread_exit` frees a *thread
slot* for `thread_create` to reuse, not the physical memory backing that
thread's stack). It zeroes what it hands out — stale bytes from OpenSBI
or an earlier boot aren't safe to hand a new thread as its stack.

`thread_init` now calls `alloc_pages(THREAD_STACK_SIZE / PAGE_SIZE)`
instead of pointing into its own embedded array, and `struct thread`
drops the array entirely. This is the allocator's first real caller, not
a toy demo bolted on beside the real code: if it handed back overlapping
or non-zeroed memory, thread stacks would corrupt each other immediately.

**Verified**: `readelf -s` confirms `__free_ram_end` (`0x84221000`) sits
comfortably below the 128MB ceiling (`0x88000000` from a `0x80000000`
base). Re-ran the same 3-second capture used to verify Step 7: `thread_d`
still prints exactly one contiguous run of 5 `D`s and exits cleanly,
`thread_a`/`b`/`c` keep interleaving correctly for the rest of the run
with no panic and no corruption — confirming stacks now allocated from
the page allocator behave identically to the previous embedded-array
ones, which is exactly what should happen when the allocation is correct.

## Step 9: virtual memory (Sv32 page tables)

Every address used so far -- code fetches, stack accesses, the pages
`alloc_pages()` hands out -- has been a physical address, straight into
RAM. That's fine for a kernel that's one flat address space, but it's
also exactly what has to stop being true before user mode can exist:
different processes need the *same* virtual address (say, where their
code starts) to mean different physical memory. Sv32 -- RV32's 2-level
page table format -- is the mechanism; it has to go in before user mode,
not after, since user mode has nothing to run in without it.

`map_page(table1, vaddr, paddr, flags)` builds one page's worth of
mapping: a 10-bit `vpn1` indexes the root table (itself exactly one
page -- 1024 4-byte PTEs) to find or lazily allocate a second-level
table, and a 10-bit `vpn0` indexes *that* to place the leaf PTE. Lazy
allocation matters here for the same reason `alloc_pages()` itself
matters: pre-allocating all 1024 possible second-level tables up front,
to cover a 4GB address space that (for this kernel) maps only ~66MB of
it, would waste far more memory than it saves.

`kernel_main` now builds one page table identity-mapping the entire
kernel image *and* the whole `alloc_pages()` range -- virtual address
equals physical address for every page mapped, deliberately, so nothing
written before this step (or since -- the thread stacks `alloc_pages()`
hands out later) has to change to keep working once translation turns
on. Mapping the *whole* free-RAM range up front, not just what's
allocated so far, is what makes that "later" safe: a thread created after
boot still gets a stack inside an already-mapped region. `enable_paging()`
writes the mode bit and root table's physical page number into `satp`,
then `sfence.vma` -- required, not optional, since the CPU is free to
have cached a translation (or the absence of one) from before the write.

**Verified**: `kernel_main` now prints `satp` right after enabling
paging: `satp=80080221`. Bit 31 set confirms Sv32 mode; the PPN field
(`0x80221`) times 4096 is `0x80221000` -- which `readelf -s` confirms is
exactly `__free_ram`, i.e. the very first page `alloc_pages()` ever
handed out (used for `kernel_page_table` itself, before any thread stack
was allocated). Re-ran the same 3-second capture again: `thread_d` still
prints exactly one contiguous run of 5 `D`s and exits cleanly, `thread_a`/
`b`/`c` keep interleaving with no panic -- confirming the kernel runs
identically with translation on, which is exactly what a correct identity
mapping should produce, and that traps (`stvec`), context switches, and
SBI calls all keep working through the switch to paged addressing.

## Step 10: per-thread `sepc`/`sstatus` (a latent scheduling bug)

While sketching out what user mode would need next, a real problem
turned up: `kernel_entry` saves and restores all 30 GPRs per thread, but
`sepc` and `sstatus` are CSRs, not GPRs -- `handle_trap` only ever reads
them into local variables, and nothing writes them back per-thread.
`sret`, at the end of `kernel_entry`, uses whatever the *hardware*
currently holds in those CSRs -- which is simply whatever the most
recent *actual* trap set them to.

That matters because `yield()` doesn't necessarily resume the thread
that just trapped. Trace: thread A traps (sepc = A's own interrupted
PC), `yield()` picks B, `switch_context` swaps onto B's stack and does a
plain `ret` -- landing back inside B's *own*, long-dormant call chain
(its earlier `yield()` → `handle_trap()` → `kernel_entry`'s tail),
entirely through ordinary C-level returns, no new hardware trap anywhere
in that unwind. By the time B's `kernel_entry` reaches its own `sret`,
the CSR `sepc` is still A's, not B's.

Confirmed with `qemu -d int`: every single `s_timer` trap, across an
unmodified capture, landed at the exact same `epc` (`0x8020014e`,
`objdump` places it right after `printf`'s SBI `ecall`). Not a
coincidence -- `thread_a`/`b`/`c`/`d`'s entire bodies live inside
`printf()`, one shared function, so *every* thread's own trap happens at
that same address anyway. The bug was real from the start; it just had
no way to become visible, since "the wrong thread's stale sepc" and
"this thread's own correct sepc" were, by construction, always equal.

Fix: `struct trap_frame` grows two fields, `sepc` and `sstatus`;
`kernel_entry` now `csrr`s both into the frame right after saving the 30
GPRs (reusing the already-saved `t0` as scratch -- its real value sits
safely in slot 3 by then) and `csrw`s them back from the frame before
restoring the GPRs and `sret`ing. Each thread's own stack now carries
its own correct values, independent of whatever the CSRs most recently
held.

**Verified**: added `thread_e`, deliberately unlike `a`/`b`/`c`/`d` --
almost all of its time is a plain busy-loop with no `printf`/`ecall` at
all, printing `E` only once every million iterations. Its own trap PC
therefore has to land somewhere genuinely different from the others.
Re-ran the trace: two distinct `s_timer` epc values now appear
(`0x80200162`, `objdump`-confirmed still inside `printf`, for
`a`/`b`/`c`/`d`; `0x802002e0`, confirmed inside `thread_e_entry`'s own
loop, for `e`) -- proof the scheduler is now resuming threads at their
*own* trap-time PC, not silently reusing whichever thread trapped most
recently. 3-second capture: no panic, `thread_d` still exits cleanly
after exactly 5 `D`s, and `A`/`B`/`C`/`E` all keep interleaving
correctly for the rest of the run.

## Step 11: user mode and syscalls

Everything so far runs in S-mode. Sv32 (Step 9) exists specifically to
make U-mode possible, and Step 10's fix specifically removes the one
thing that would have made a U-mode thread unsafe to preempt and resume
through the existing scheduler. This step is the payoff: an actual
U-mode thread.

**The mechanism**: `enter_user_mode(entry, user_sp)` is a one-way trip,
the same shape as `thread_trampoline`'s jump into a brand-new thread,
just crossing a privilege boundary instead of a function-call one --
write `sepc` (where `sret` lands), clear `sstatus.SPP` (which privilege
`sret` drops into -- 0 is U), set `sstatus.SPIE` (restored into `SIE` by
that same `sret`, or this thread would run in U-mode permanently
un-preemptible), point `sp` at a separate user stack, `sret`.

**The syscall convention is ours, not SBI's.** U-mode's `ecall` traps
straight to *this* kernel (OpenSBI's `medeleg` delegates "environment
call from U-mode", cause 8, down to S-mode) -- it never reaches M-mode
at all, so `sbi_call()` itself is simply unusable from U-mode. `handle_trap`
dispatches on `f->a3` (syscall number) with one implemented so far,
`SYS_PUTCHAR`, reading the character from `f->a0`. `ecall` doesn't
advance `pc` on its own, so the handler sets `f->sepc = sepc + 4` --
written into the *frame*, not the live CSR, since Step 10 means
`kernel_entry` now restores `sepc` from there.

**The bug this step actually hit**: the first attempt gave *every*
mapped page -- kernel code included -- `PAGE_U`, reasoning that real
per-process isolation was a problem for a later step. The kernel never
even reached its own first `printf` after enabling paging: `qemu -d int`
showed an `exec_page_fault` at `0x80200000` (the kernel's own entry
point), forever, with firmware re-jumping to `Domain0 Next Address`
(also `0x80200000`) each time and faulting again immediately. The actual
rule, independent of `sstatus.SUM` (which only governs S-mode
loads/stores): **S-mode can never execute an instruction fetched from a
page with `PAGE_U` set, full stop.** Marking the kernel's own code
`PAGE_U` meant the kernel could no longer execute itself the instant
translation turned on.

Fix: `PAGE_U` only goes on the specific pages that need it.
`user_entry`'s code gets a dedicated, page-aligned linker output section
(`.text.user`, with `__user_text_start`/`__user_text_end`) specifically
so it can never end up sharing a page with code the kernel itself runs
-- `kernel.ld`'s main `.text` was narrowed from `*(.text .text.*)` to
`*(.text)` so its wildcard can't sweep `.text.user` in first. The user
stack (allocated at runtime via `alloc_pages()`, so it can't be handled
in the linker script) gets a second, explicit `map_page()` call adding
`PAGE_U` just to that range. Everything else the main identity-map loop
covers keeps the Step 9 flags, unchanged (`PAGE_R | PAGE_W | PAGE_X`, no
`U`).

**Verified**: 3-second capture, no panic. `qemu -d int` shows 80,192
`user_ecall` (cause 8) events, every one at `epc=0x80201006` --
`objdump`-confirmed as `user_entry`'s own `ecall` instruction -- and the
serial output has exactly that many `U`s. `s_timer` events now show
*three* distinct epc values: `0x80200162` (inside `printf`, for
`a`/`b`/`c`/`d`), `0x802002e0` (inside `thread_e`'s loop), and
`0x8020100a` (inside `user_entry`'s own loop, right after its `ecall`) --
confirming the U-mode thread genuinely gets caught by the timer mid-loop
and correctly resumes in U-mode afterward, not just that its syscalls
work. `thread_d` still exits cleanly after exactly 5 `D`s; `A`/`B`/`C`/`E`
keep interleaving correctly for the whole run.

No real isolation yet -- one page table, shared by every thread, kernel
and user alike; a U-mode thread could still read or write any *other*
page that happens to lack `PAGE_U` just by having the kernel map it that
way, since nothing stops `map_page()` from being called against the same
table for anything. Per-process page tables are the next thing this
design was always leaving for later, not a gap found by accident this
time.

## Step 12: `SYS_EXIT`

Step 11's `user_entry` only ever looped forever -- there was no way for
a U-mode program to actually finish. The same question already has an
answer on the kernel-thread side: `thread_exit()` (Step 7), reached when
a thread's entry function returns. A U-mode program has no C-level
"return into the kernel" to hook, so it needs a syscall instead, but the
destination is the same function: `SYS_EXIT`'s case in `handle_trap`
just calls `thread_exit()` directly. `current_thread` is still whichever
thread issued the `ecall` (nothing switches it before this point), so
`thread_exit()` frees exactly that thread's slot and never returns, the
same way it does when reached from `thread_trampoline`'s `call` after a
kernel thread's own entry function returns.

`user_entry` now prints `U` a fixed 5 times (mirroring `thread_d`'s
pattern from Step 7 exactly) and then calls `SYS_EXIT`, instead of
looping forever.

**Verified**: 5-second capture, no panic. Serial output has exactly one
contiguous run of 5 `U`s, same shape as `thread_d`'s 5 `D`s, never
repeating afterward. `qemu -d int` shows exactly 6 `user_ecall` events
total across the whole capture -- 5 at `epc=0x80201008`
(`SYS_PUTCHAR`'s `ecall`) and exactly 1 at `epc=0x80201014`
(`SYS_EXIT`'s, a different instruction address, confirming it's a
genuinely separate call) -- and not one more after that, confirming the
thread actually stopped rather than merely going quiet. `A`/`B`/`C`/`E`
keep running correctly for the rest of the capture, same as every step
since Step 7.

## Step 13: `sstatus.SUM` (found reviewing Step 11/12, not by a visible failure)

Nothing was broken when this step started -- it came from re-reading
Step 11's own reasoning rather than from a crash. That comment claimed
`PAGE_U` only ever restricts *execution* in S-mode, never load/store,
since only `PAGE_X` governs that. True for U-mode, false for S-mode: the
RISC-V privileged spec also gates S-mode *loads and stores* to a
`PAGE_U` page behind `sstatus.SUM` ("permit Supervisor User Memory
access") -- SUM=0 means those accesses fault too, not just execution.

That should matter a lot here: `kernel_entry`'s very first instructions
on any trap push the 32-word frame onto whatever `sp` currently is, and
for a U-mode thread that's the `PAGE_U`-marked user stack `user_launcher_entry`
mapped (Step 11) -- no sscratch-based kernel-stack switch exists yet, a
gap already called out in that step's journal entry. Checking
`sstatus` directly after `enable_paging()` showed `SUM` reading back
`0` -- never set anywhere in this kernel -- and yet Step 11 and Step
12's own verification both passed repeatedly, with the trap frame
pushed and popped from that exact stack dozens of times. QEMU's RV32
model simply isn't enforcing the SUM check here, at least not in this
configuration.

Not something to leave relying on an emulator's leniency: fixed by
setting `sstatus.SUM` once in `kernel_main`, alongside the existing
`SIE` setup, before any thread (kernel or user) ever runs.
`enter_user_mode`'s clear/set of `SPP`/`SPIE` only ever touches those
two bits (`and`/`or` against a mask, not a wholesale overwrite), so
`SUM` -- set once, globally, before it matters -- survives into every
thread's own saved `sstatus` through Step 10's per-thread save/restore
untouched, including a thread that's currently running in U-mode.

**Verified**: a temporary debug print showed `sstatus` reading back
`0x80006000` (bit 18 clear) right after `enable_paging()`, before the
fix; `0x80046002` (bit 18 set) right after the new `WRITE_CSR`, after
it -- confirming the write actually lands, not just that the code
compiles. Re-ran the full Step 12 verification afterward: no panic,
`thread_d` and `user_entry` each still produce exactly one clean run of
5 characters (`DDDDD`, `UUUUU`), and `A`/`B`/`C`/`E` keep interleaving
correctly -- confirming the fix changes nothing observable (as it
shouldn't, since nothing was failing), just makes the one access this
kernel actually depends on -- S-mode's own trap-frame push onto a
U-mode thread's stack -- correct by the spec rather than correct by
coincidence of which emulator happens to be running it.

## Step 14: `current_thread - threads` (another review-found issue)

Same kind of find as Step 13 -- not a crash, a second pass over
`yield()` with Step 13's "correct by spec, not by luck" question in
mind. `int current_index = (int)(current_thread - threads);` subtracts
two pointers to compute where `current_thread` sits in the pool -- but
the very first call, from `kernel_main`, has `current_thread ==
&idle_thread`, a *separate* global, not an element of `threads[]`. The
C standard only defines pointer subtraction between pointers into the
same array (or one past its end); subtracting pointers to two unrelated
objects is undefined behavior, not just "a negative number that happens
to fall out of the math." The existing comment already knew the result
needed to land out of range for the loop below to behave -- it just
didn't notice that *getting* that value was itself the undefined part.

Fix: compare addresses instead of subtracting them for the one case
that isn't actually an array access -- `(current_thread == &idle_thread)
? -1 : (int)(current_thread - threads)`. Pointer *comparison* between
any two pointers is always well-defined, so this produces the exact
same `-1` the old code relied on, by a route the standard actually
permits, and the subtraction itself now only ever executes when
`current_thread` genuinely does point into `threads[]`.

**Verified**: no behavior to change (same value, different route to
it), so verification is that nothing broke -- re-ran the full QEMU
capture: no panic, `thread_d`/`user_entry` each still produce exactly
one clean run of 5 characters, `A`/`B`/`C`/`E` keep interleaving
correctly.

## Step 15: `printf`'s `%d` and `INT_MIN` (a third review-found issue)

A third pass, same question as Steps 13-14: is this correct by the
language's rules, or correct by luck? `printf`'s `%d` case negates a
negative `value` to get its magnitude: `magnitude = (unsigned)(-value);`.
For `value == INT_MIN`, `-value` is signed overflow -- `INT_MIN`'s
magnitude (2147483648) doesn't fit in an `int`, and `int` has no
"wrap around" defined for overflow the way unsigned types do. No
caller in this kernel happens to pass `INT_MIN` today, so nothing was
visibly broken; it's a latent bug in a function every future `printf`
call trusts.

Fix: skip the signed negation entirely. `0u - (unsigned)value` gets the
exact same magnitude through unsigned arithmetic alone -- converting a
negative `int` to `unsigned` and subtracting are both modular
operations the standard defines completely, for every possible `int`
value including `INT_MIN`, unlike negating a signed `int`.

**Verified**: added a one-off `printf("INT_MIN = %d\n", -2147483647 - 1)`
call (written that way, not as a literal `-2147483648`, since that
token would first parse as negating `2147483648` -- already too big
for an `int` before the fix even applies) right after the existing
`1 + 2 = ...` sanity line. Output: `INT_MIN = -2147483648`, exactly
correct, no panic. Re-ran the full capture afterward: `thread_d`/
`user_entry` still each produce exactly one clean run of 5 characters,
`A`/`B`/`C`/`E` keep interleaving correctly.

## Step 16: `__stack_top` wasn't 16-byte aligned (a fourth review-found issue)

A fourth pass, this time over `kernel.ld` rather than `kernel.c`. The
RISC-V calling convention requires `sp` to be 16-byte aligned at
function entry -- not just word-aligned -- and `boot()` loads
`__stack_top` straight into `sp` before jumping into `kernel_main`, a
perfectly ordinary (non-naked) C function. `kernel.ld` computed
`__stack_top` as `ALIGN(4)` plus a 128KB reservation. 128KB is itself a
multiple of 16, so it preserves whatever alignment the *base* already
had -- and that base is only guaranteed 4-byte aligned, which leaves
`__stack_top`'s alignment mod 16 basically up to how big `.bss` happens
to be on any given build.

`readelf -s` on the actual build confirmed it: `__stack_top` =
`0x8022241c`, which is `12 mod 16`, not `0`. Every kernel thread's own
stack top (from `thread_init`, built on `alloc_pages()`'s page-aligned
output) was already correctly 16-aligned -- this was specifically the
one-time boot stack that wasn't, and it's used exactly once, for
exactly one function entry (`kernel_main`), which is exactly why it
went unnoticed: nothing in this kernel allocates large aligned stack
objects or uses atomics/SIMD that would actually break visibly on a
4-byte-off stack.

Fix: align the *base* to 16 instead of 4 before reserving the 128KB,
so adding a multiple of 16 can't un-fix what alignment already held.

**Verified**: `readelf -s` after the fix: `__stack_top` =
`0x80222420` -- `0 mod 16`. Re-ran the full capture: no panic,
`INT_MIN = -2147483648` still prints correctly, `thread_d`/`user_entry`
each still produce exactly one clean run of 5 characters, `A`/`B`/`C`/`E`
keep interleaving correctly.
