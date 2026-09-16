# os-practice

A tiny RISC-V (RV32) operating system, built from scratch and incrementally,
as a hands-on companion to
["45分でゼロから作る！OS自作ライブコーディング"](https://youtu.be/dPEsTeL2F98)
(OSC2023 Online/Kyoto) and the ideas behind
[*Operating System in 1,000 Lines*](https://operating-system-in-1000-lines.vercel.app/).

The point isn't to build a usable OS. It's the same idea as the talk: an OS
looks intimidatingly huge, but its first steps (boot, talk to the firmware,
handle a trap, switch between two execution contexts) are small and
understandable — you can write and run them yourself in an afternoon.

## What's implemented

- **Boot**: linker script + a hand-written stack setup, jumping into a C
  `kernel_main`.
- **SBI calls**: the RISC-V equivalent of a syscall, from kernel (S-mode) to
  firmware (M-mode) — used to implement `putchar` and, on top of that, a
  small `printf` (`%s`, `%d`, `%x`, `%%`).
- **Trap handling**: saving full CPU state on entry, reading `scause`/`sepc`
  to report what happened (deliberately triggered here with an illegal
  instruction, matching the talk's demo).
- **Context switching**: two kernel threads that yield the CPU to each other
  by saving/restoring callee-saved registers on their own stacks.

See [`docs/JOURNAL.md`](docs/JOURNAL.md) for what each step does and why,
including one real bug hit and fixed while building this (a PIE/build-id
linking issue that made QEMU jump into non-code bytes).

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
