# AROS: A Preemptive Kernel with Stack Overflow Detection for ATmega328p

A from-scratch preemptive multitasking kernel for the Arduino Uno, with crash
detection and post-mortem reporting — on a chip that has no memory protection
hardware of any kind.

**Target:** ATmega328p @ 16 MHz, 2 KB SRAM
**Toolchain:** avr-gcc, avrdude, pySerial miniterm
**Language:** C with inline AVR assembly

---

## What it does

Three tasks run concurrently, preempted by a 1 ms timer interrupt. Each has its
own stack. On every tick the scheduler saves all 32 registers plus SREG to the
outgoing task's stack, swaps the stack pointer, and restores the incoming task's
context.

Two independent stack overflow detectors run on each tick:

- **SP bounds check** — compares the stack pointer against `stack_base +
  GUARD_BYTES`. Fires *before* memory outside the stack is touched, so the
  offending task can be killed while the rest of the system keeps running.
- **Canary** — a painted `0x55`/`0xAA` pattern at the low end of each stack.
  A broken canary means corruption has *already* happened, so the only safe
  response is a reset.

On detection, a crash record (task ID, stack pointer, reason) is written to a
`.noinit` section that survives reset, then the watchdog is armed. The next boot
reads the record and reports it over UART.

---

## Design decisions

**Why two detectors instead of one.** They answer different questions. The SP
check asks "is this task about to overflow?" The canary asks "did something
already overflow?" The answers imply different recovery policies — kill vs.
reset — and conflating them means either killing tasks whose memory is already
corrupt, or resetting when you didn't have to.

**Why `.noinit` instead of EEPROM.** EEPROM writes take ~3.3 ms per byte and
have finite endurance — both bad properties inside a fault path. `.noinit` is
SRAM the C runtime skips at startup, so it survives a reset (though not a power
cycle), and writes are free. The tradeoff: crash logs are lost if the board is
unplugged.

**Why magic bytes.** `.noinit` is uninitialized by definition, so on cold boot
it holds whatever SRAM powers up as. Without a sentinel value there's no way to
distinguish a real crash record from power-on garbage. `0xC0DE` is written last,
after the other fields, so a reset landing mid-write can't produce a
valid-looking record over junk.

**Why report on the next boot rather than in the handler.** The fault handler
runs on a possibly-corrupt stack with interrupts disabled. Anything that
allocates, blocks, or depends on interrupt-driven I/O is unavailable there.
Store-and-reset moves the reporting to a context where the stack is known good.

---

## Debugging timeline

The interesting part of this project was not writing it. It was the four hours
of wrong hypotheses that followed.

### 1. Nothing happened

Initial symptom: board flashed successfully, no LEDs. Cause was mundane — the
ISR saved and restored context without ever swapping the stack pointer, making
it an expensive no-op, and `main()` never created any tasks. Fixing both got two
LEDs blinking at independent rates, which proved the context switch worked.

### 2. Overflow detection outran by the overflow

First test task recursed with a 32-byte frame and no delay. It exhausted its
entire 256-byte stack in roughly seven calls — microseconds — then continued
straight through the other two tasks' stacks and the task table itself, all
between two timer ticks.

**Lesson learned the hard way:** tick-based detection can only catch overflows
slower than the tick period. This isn't a bug in the implementation, it's the
fundamental limit of polling. Hardware with an MPU traps on the offending
instruction; a 1 ms poll cannot. Slowing the test task's descent made the
detection observable.

### 3. Crash reports full of impossible values

Detection appeared to fire, but the reports read `pid=CE`, `sp=2FDC`. The 328p
has SRAM at `0x0100`–`0x08FF`; `0x2FDC` is not a physical address on the chip.

Three hypotheses were tested and discarded before the real cause:

- *RAM exhaustion* — ruled out by halving `STACK_SIZE` with no change.
- *Boot loop from a mis-armed watchdog* — ruled out by marker prints showing
  `main()` completing every pass.
- *The naked ISR corrupting SP* — the leading theory for some time. The
  reasoning was that calling a C function from an `ISR_NAKED` handler lets the
  compiler emit frame setup that collides with manual SP manipulation. Plausible,
  and wrong.

### 4. Reading the generated assembly

`avr-gcc -S` settled it. The ISR body contained no compiler-generated frame
setup, no phantom `push r28/r29`, and exactly one `out __SP_L__` — the intended
one. The naked-ISR theory was dead.

What the assembly *did* show, in the canary-failure branch:

```asm
sts g_crash+1,r25      ; magic = 0xC0DE
sts g_crash,r24
lds r24,current_task   ; load current_task...
ldi r24,lo8(24)        ; ...clobbered on the very next instruction
```

The stores to `pid`, `sp`, and `reason` were **absent from the binary**. The
compiler had proven them dead — `g_crash` was not declared `volatile`, and the
stores were followed by `while(1);` with no intervening reads, so dead store
elimination removed them. Only `magic` survived. The "impossible" field values
were uninitialized SRAM, faithfully reported.

**Fix:** `volatile crash_log_t g_crash __attribute__((section(".noinit")));`

### 5. The crash report that never printed

Second bug, independent of the first. The boot-time check gated on
`MCUSR & (1 << WDRF)` — the watchdog reset flag. It was always zero.

Cause: Optiboot, the Uno's bootloader, reads and clears `MCUSR` before handing
control to the application. The flag is gone before `main()` ever runs. Gating on
the magic value alone is the correct approach on this platform.

### 6. Working

```
boot mcusr=00 magic=0000
A
B
C
boot mcusr=00 magic=C0DE pid=02 sp=028F reason=Canary
```

First line: cold boot, no record, magic correctly rejected. Every line after:
task 2 overflowed, canary caught it, watchdog reset, record survived, report
printed. End to end.

---

## Things learned that aren't in tutorials(probably)

**Dead store elimination applies to memory you think is special.** A `.noinit`
section is still ordinary memory to the optimizer. If nothing reads a store and
control flow can't escape, the store is removed — regardless of your intent to
read it after a reset. `volatile` is what communicates "something outside this
program's control flow will observe this."

**Bootloaders modify machine state before your code runs.** Optiboot clearing
`MCUSR` is invisible from source and silently breaks reset-cause detection on
every Arduino board.

**The stack grows down, and every check follows from that.** Overflow means SP
*decreasing* past a threshold, which is why the bounds check reads
`sp < base + GUARD_BYTES` rather than the reverse.

**Watchdog state survives watchdog reset.** After a WDT reset the watchdog is
still enabled at the shortest timeout. Anything slow before `wdt_disable()`
produces an infinite reboot loop that looks exactly like a dead board.

**IntelliSense is not a compiler.** Several apparent errors were the language
server failing to resolve AVR headers; several real errors (implicit
declarations, link failures) produced no squiggles at all. `-Wall` catches what
the editor doesn't.

**When the theory and the hardware disagree, read the assembly.** Three
reasonable hypotheses were wrong. `avr-gcc -S` gave the answer in one pass. The
generated code is the ground truth; source is a suggestion the optimizer is free
to reinterpret.

---

## Known limitations

- **Detection granularity is one timer tick.** Overflows faster than 1 ms are
  not caught before damage. Fundamental to polling; unavoidable without an MPU.
- **No idle task.** If every task is killed the scheduler has no valid context to
  restore and resets. An always-runnable idle task (which is what FreeRTOS
  creates automatically) would eliminate this path.
- **Inline assembly declares no clobbers.** The context-switch macros modify all
  32 registers and move SP by 33 bytes; the compiler is told none of this. It
  works because the macros sit at the exact start and end of the handler, but it
  is not robust to changes in optimization level or handler structure.
- **Round-robin only.** No priorities, no blocking, no sleep. Tasks are
  time-sliced equally.
- **`.noinit` does not survive power loss.** Crash logs are readable only after a
  reset with power maintained.

---

## Possible next steps

- Idle task, removing the no-tasks-left reset path
- Priority scheduling
- A `yield()` for voluntary switching (note: writing SP outside an ISR is a
  two-byte non-atomic operation and needs interrupts disabled)
- Clobber lists on the context-switch macros
- Reporting `stack_base` alongside `sp` to measure actual overflow margin and
  tune `GUARD_BYTES` empirically
