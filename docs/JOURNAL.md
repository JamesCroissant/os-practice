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
