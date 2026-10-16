# os-practice

A tiny RISC-V (RV32) operating system, built from scratch and incrementally,
as a hands-on companion to
["45分でゼロから作る！OS自作ライブコーディング"](https://youtu.be/dPEsTeL2F98)
(OSC2023 Online/Kyoto) and the ideas behind
[*Operating System in 1,000 Lines*](https://operating-system-in-1000-lines.vercel.app/).

The point isn't to build a usable OS. It's the same idea as the talk: an OS
looks intimidatingly huge, but its first steps (boot, talk to the firmware,
handle a trap, switch between execution contexts) are small and
understandable — you can write and run them yourself in an afternoon.

## What's implemented

- **Boot**: linker script + a hand-written stack setup, jumping into a C
  `kernel_main`.
- **SBI calls**: the RISC-V equivalent of a syscall, from kernel (S-mode) to
  firmware (M-mode) — used to implement `putchar` and, on top of that, a
  small `printf` (`%s`, `%d`, `%x`, `%%`).
- **Trap handling**: full register save/restore, so a trap can either
  panic (reporting `scause`/`sepc`, matching the talk's illegal-
  instruction demo) or actually resume whatever it interrupted.
- **Context switching**: kernel threads that give up the CPU by
  saving/restoring callee-saved registers on their own stacks.
- **Scheduler**: a fixed-size thread pool and round-robin `yield()` —
  extending past the talk's own two hardcoded threads to three
  independently-created ones.
- **Preemption**: a timer interrupt forces a switch even for threads that
  never call `yield()` themselves — ties the trap handler and scheduler
  together.
- **Thread exit**: a thread whose entry function returns (instead of
  looping forever, like the demo threads) frees its slot for reuse
  instead of jumping into whatever happens to follow it in memory.
- **Physical memory allocation**: a bump page allocator hands out zeroed
  RAM from a region reserved in the linker script — thread stacks now
  come from it instead of being embedded, unused-or-not, in every pool
  slot's `.bss`.
- **Virtual memory**: Sv32 page tables, built and switched on at boot —
  identity-mapped for now (no user mode exists yet to need anything
  else), but the addressing every later process needs is real from here
  on, not retrofitted.
- **Per-thread `sepc`/`sstatus`**: fixes a latent bug where a thread
  resumed by a *different* thread's trap would `sret` using that other
  trap's leftover CSR values — invisible until a thread with a genuinely
  different code shape existed to expose it.
- **User mode and syscalls**: a thread that drops to U-mode and talks to
  the kernel only through `ecall` (its own syscall convention, not
  SBI's) — preemptible and resumable like any other thread, now that
  `sepc`/`sstatus` are saved per-thread.
- **`SYS_EXIT`**: a U-mode program can terminate itself through the same
  `thread_exit()` a kernel thread's entry function returning into (Step
  7) already uses — a syscall away rather than a plain C `return`, since
  U-mode has no such thing as "returning into the kernel".
- **`sstatus.SUM`** *(superseded by Step 22 below — kept here for the
  project's own history, not as a description of current behavior)*:
  set correctly, rather than relying on QEMU not enforcing it — S-mode
  needed it to read/write a `PAGE_U` page, which `kernel_entry`'s own
  trap-frame push did the moment a U-mode thread trapped onto its own
  (`PAGE_U`-marked) stack. Removed once Step 22's dedicated kernel stack
  meant S-mode never touched a `PAGE_U` page at all.
- **Well-defined scheduler bookkeeping**: `yield()` no longer subtracts
  pointers to two unrelated objects (undefined behavior, even though it
  happened to produce a usable value) to detect the one case — the very
  first call — where `current_thread` isn't in the thread pool at all.
- **`printf`'s `%d` and `INT_MIN`**: fixed a signed-overflow bug (negating
  `INT_MIN` doesn't fit in an `int`) that no current caller happened to
  trigger, by computing the magnitude through unsigned arithmetic
  instead of negation.
- **16-byte stack alignment at boot**: `__stack_top` — loaded straight
  into `sp` before `kernel_main` ever runs — is now actually aligned to
  the 16 bytes the RISC-V calling convention requires at function entry,
  not just 4.
- **`sfence.vma` as a real compiler barrier**: it now carries a `memory`
  clobber, so the one instruction whose whole job is a memory-translation
  fence can't be treated by the optimizer as having no effect on memory
  at all.
- **`READ_CSR`/`WRITE_CSR` as real compiler barriers too**: every
  `csrr`/`csrw` in this kernel now carries the same `memory` clobber, for
  the same reason — nothing else stops the optimizer from treating a
  write to `satp`, `sstatus`, or `stvec` as having no effect on memory.
- **`kernel.h` include guard**: inert today (one translation unit
  includes it), but a standard `#ifndef`/`#define`/`#endif` guard either
  way, closing a gap Step 19's split left open.
- **Housekeeping**: removed `Makefile`'s dead `OBJCOPY` variable and
  fixed a stale comment in `kernel.h` pointing at a `kernel.c`
  definition that Step 19's split had already moved into the header.
- **A dedicated kernel stack for U-mode traps**: `kernel_entry_from_user`
  swaps to each thread's own kernel stack via `sscratch` before building
  its trap frame, instead of landing that frame on the U-mode program's
  own (`PAGE_U`-marked) stack -- a real gap left open since Step 11,
  closed without needing a spare register to track it. `sstatus.SUM`
  (Step 13) is gone with it: nothing left for it to permit.
- **`kernel_entry_from_user` re-arms `sscratch` on every return path**:
  not just the one that happens to call `yield()` -- a plain `SYS_PUTCHAR`
  ecall was leaving `sscratch` pointing at the wrong thing for the next
  trap, invisible only because `user_entry` never moves its own stack
  pointer between syscalls.
- **One shared register list for both trap entry points**: `kernel_entry`
  and `kernel_entry_from_user` used to carry the same 30-register
  save/restore sequence twice, verbatim -- the exact shape of risk that
  let Step 23's bug happen in the first place. Now defined once and
  reused by both, confirmed byte-identical in the generated code.
- **A second, independent U-mode thread**: `user_launcher_entry2`/
  `user_entry2`, printing `'V'` instead of `'U'` -- proving
  `kernel_entry_from_user`'s per-thread kernel stack and `stvec` target
  (Step 22) actually hold up with two U-mode threads getting preempted
  and resumed independently, not just the one that existed when that
  design was written and verified.

## Source layout

`kernel.h` declares everything (types, macros, struct layouts, function
prototypes); `kernel.c` implements it, following the split used by
[*Operating System in 1,000 Lines*](https://github.com/nuta/operating-system-in-1000-lines)
itself (its own `kernel.h`/`kernel.c`, alongside `common.*` and `user.*`
for the pieces this project doesn't need a separate file for yet). The
"why" behind each piece stays as a comment in `kernel.c`, next to the
code it explains, not duplicated into the header.

See [`docs/JOURNAL.md`](docs/JOURNAL.md) for what each step does and why,
including two real bugs hit and fixed while building this (a PIE/
build-id linking issue that made QEMU jump into non-code bytes, and an
interrupt-enable bit that silently stayed cleared after switching into a
never-before-run thread).

`make run` now passes QEMU `-m 128M` explicitly: the page allocator's
region, reserved in `kernel.ld`, is sized against that assumption.

## Building and running

Everything runs inside QEMU's RISC-V `virt` machine — no real hardware
needed.

```sh
# Debian/Ubuntu
sudo apt-get install qemu-system-misc gcc-riscv64-linux-gnu \
    binutils-riscv64-linux-gnu opensbi

make run
```

Exit QEMU with `Ctrl-A X`.

## Why RV32 with a `riscv64-linux-gnu` compiler?

There's no separately-packaged bare-metal `riscv64-unknown-elf-gcc` in
Ubuntu's repos, but `gcc-riscv64-linux-gnu`'s RISC-V backend accepts
`-march=rv32imac_zicsr_zifencei -mabi=ilp32` and happily emits valid RV32
code — we just never link against its (rv64-only) libc, since the kernel is
built `-ffreestanding -nostdlib` throughout.
