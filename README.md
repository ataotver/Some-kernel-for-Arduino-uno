# Some-kernel-for-Arduino-UNO
A small scheduler program that allows an Arduino UNO run multiple programs at once through interrupts.
It may be prone to bugs or unexpected issues. Keep in mind that this is the exact setup where it worked:
On Windows 10, with the following commands:

avr-gcc -mmcu=atmega328p -DF_CPU=16000000UL -Os -o aroskrnl.elf aroskrnl.c
avr-objcopy -O ihex -R .eeprom aroskrnl.elf aroskrnl.hex
avrdude -C "C:\Users\<username>\AppData\Local\Arduino15\packages\arduino\tools\avrdude\8.0.0-arduino1\etc\avrdude.conf" -c arduino -p atmega328p -P COM8 -b 115200 -U flash:w:aroskrnl.hex:i
