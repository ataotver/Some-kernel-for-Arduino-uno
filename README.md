# aroskrnl — a minimal preemptive kernel for the ATmega328P

A tiny preemptive multitasking kernel for the Arduino UNO, written
bare-metal in C with no Arduino libraries. It runs two tasks
"simultaneously" on a single core by switching between them on a timer
interrupt — the same mechanism a real OS uses, stripped to its essentials.

## How it works

A hardware timer fires every 1 ms. On each interrupt, the kernel saves
the running task's CPU registers and stack pointer, loads the other
task's saved state, and returns into it — so each task runs as if it
had the CPU to itself.

New tasks are started with a trick: `setup_task` forges a stack frame
that looks exactly like a task that was interrupted mid-run, planting
the task function's address where the return-from-interrupt instruction
expects it. The normal restore path then "resumes" a task that never
actually ran.

## Build & flash

Tested on Windows 10, ATmega328P (Arduino UNO), 16 MHz.

    avr-gcc -mmcu=atmega328p -DF_CPU=16000000UL -Os -o aroskrnl.elf aroskrnl.c
    avr-objcopy -O ihex -R .eeprom aroskrnl.elf aroskrnl.hex
    avrdude -C "<path-to>/avrdude.conf" -c arduino -p atmega328p -P COM8 -b 115200 -U flash:w:aroskrnl.hex:i

Adjust the avrdude.conf path and COM port for your system.

Expected result: the onboard LED (pin 13) and an LED on pin 12 blink
at different rates, at the same time.

## Known limitations

- Only two tasks, hardcoded (no dynamic task table yet).
- Fixed stack size per task; no overflow detection.
- No memory protection — a task's stray pointer can corrupt another's
  stack silently (the ATmega328P has no MMU).
- The initial task launch in `main` relies on compiler behaviour that
  isn't guaranteed; robust would be a dedicated asm launch routine.

## Planned next

- Task table with real IDs, dynamic add/remove.
- Priority-based scheduling.
