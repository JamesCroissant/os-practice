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
