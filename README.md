# aroskrnl — a minimal preemptive kernel for the ATmega328P

A tiny preemptive multitasking kernel for the Arduino UNO, written
bare-metal in C with no Arduino libraries. It runs three tasks(or any amount you configure it to)
"simultaneously" on a single core by switching between them on a timer
interrupt — the same mechanism a real OS uses, stripped to its essentials.

## How it works

A hardware timer fires every 1 ms. On each interrupt, the kernel saves
the running task's CPU registers and stack pointer, loads the next
task's saved state, and returns into it — so each task runs as if it
had the CPU to itself.

New tasks are started with a trick: `setup_task` forges a stack frame
that looks exactly like a task that was interrupted mid-run, planting
the task function's address where the return-from-interrupt instruction
expects it. The normal restore path then "resumes" a task that never
actually ran.

## Crash detection

The ATmega328P has no MMU or MPU, so nothing traps a stack overflow.
The kernel detects it in software on every timer tick, two ways:

- **Stack pointer bounds** — SP is compared against the task's stack
  base plus a guard margin. This fires before memory outside the stack
  is touched, so the offending task can be killed while the rest of the
  system keeps running.
- **Canary bytes** — a 0x55/0xAA pattern painted at the low end of each
  stack. A broken canary means corruption has already happened, so the
  kernel resets rather than trying to continue.

Either way a crash record (task ID, stack pointer, cause) is written to
a `.noinit` section that survives reset, the watchdog is armed, and the
next boot reads the record and reports it over UART at 9600 baud.

## Build & flash

Tested on Windows 10, ATmega328P (Arduino UNO), 16 MHz.

    avr-gcc -mmcu=atmega328p -DF_CPU=16000000UL -Os -o aroskrnl.elf aroskrnl.c
    avr-objcopy -O ihex -R .eeprom aroskrnl.elf aroskrnl.hex
    avrdude -C "<path-to>/avrdude.conf" -c arduino -p atmega328p -P COM8 -b 115200 -U flash:w:aroskrnl.hex:i

Adjust the avrdude.conf path and COM port for your system.

Expected result: the onboard LED (pin 13) and an LED on pin 12 blink
at different rates, at the same time (or whatever program you would want to run).

To see crash reports, open any serial monitor at 9600 baud:
eg:
    python -m serial.tools.miniterm COM8 9600

Only one program can hold the serial port at a time — close the monitor
before flashing, or avrdude will fail with "unable to open port".

    boot magic=C0DE pid=02 sp=028F reason=Canary

## Known limitations
- Task count is a compile-time constant (`MAX_TASKS`); no dynamic
  add/remove yet.
- Fixed stack size per task, set by `STACK_SIZE` for all tasks alike.
- Overflow detection runs once per timer tick, so an overflow faster
  than 1 ms can corrupt memory before it is caught. This is the
  fundamental limit of polling — hardware with an MPU traps on the
  offending instruction instead.
- No memory protection. The ATmega328P has no MMU or MPU, so a stray
  pointer can still corrupt another task's stack silently; the guard
  bytes only catch overflow through the stack's own low end.
- No idle task. If every task is killed the scheduler has no valid
  context to restore and resets. An always-runnable idle task would
  remove this path.
- Crash logs live in `.noinit`, which survives a reset but not a power
  cycle — unplugging the board loses the log.
- The context-switch macros declare no clobber lists, so the compiler
  is not told they modify all 32 registers and move the stack pointer.
  This works because they sit at the exact start and end of the handler,
  but it is not robust to changes in optimization level.
- The initial task launch in `main` relies on compiler behaviour that
  isn't guaranteed; robust would be a dedicated asm launch routine.

## Debugging log

See [DEBUGGING.md](DEBUGGING.md) — three wrong hypotheses before the
real cause turned up in the generated assembly: a dead store the
compiler had eliminated because `g_crash` wasn't declared `volatile`.

## Planned next

- Dynamic add/remove tasks.
- Priority-based scheduling.
