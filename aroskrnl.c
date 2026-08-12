#include <avr/io.h>     //memory adress and pin macros purely for adress translations
#include <avr/interrupt.h>   //the interrupt function



#define STACK_SIZE 256


//inline assembly for saving every single register values of a program on the stack
#define SAVE_CONTEXT()  \
    asm volatile (      \
        "push r0\n\t" "in r0, __SREG__\n\t" "push r0\n\t" \
        "push r1\n\t" "clr r1\n\t" \
        "push r2\n\t" "push r3\n\t" "push r4\n\t" "push r5\n\t" \
        "push r6\n\t" "push r7\n\t" "push r8\n\t" "push r9\n\t" \
        "push r10\n\t" "push r11\n\t" "push r12\n\t" "push r13\n\t" \
        "push r14\n\t" "push r15\n\t" "push r16\n\t" "push r17\n\t" \
        "push r18\n\t" "push r19\n\t" "push r20\n\t" "push r21\n\t" \
        "push r22\n\t" "push r23\n\t" "push r24\n\t" "push r25\n\t" \
        "push r26\n\t" "push r27\n\t" "push r28\n\t" "push r29\n\t" \
        "push r30\n\t" "push r31\n\t" )

//another inline assembly for restoring the previously saved register values of another program

#define RESTORE_CONTEXT() \
    asm volatile (        \
        "pop r31\n\t" "pop r30\n\t" "pop r29\n\t" "pop r28\n\t" \
        "pop r27\n\t" "pop r26\n\t" "pop r25\n\t" "pop r24\n\t" \
        "pop r23\n\t" "pop r22\n\t" "pop r21\n\t" "pop r20\n\t" \
        "pop r19\n\t" "pop r18\n\t" "pop r17\n\t" "pop r16\n\t" \
        "pop r15\n\t" "pop r14\n\t" "pop r13\n\t" "pop r12\n\t" \
        "pop r11\n\t" "pop r10\n\t" "pop r9\n\t" "pop r8\n\t" \
        "pop r7\n\t" "pop r6\n\t" "pop r5\n\t" "pop r4\n\t" \
        "pop r3\n\t" "pop r2\n\t" \
        "pop r1\n\t" \
        "pop r0\n\t" "out __SREG__, r0\n\t" "pop r0\n\t" \
        "reti\n\t" )

//every loaded program are literally bytes, same for every stack frame
uint8_t stackA[STACK_SIZE];      // pre allocated stacks for both tasks--|
uint8_t stackB[STACK_SIZE];      // <------------------------------------|

uint16_t spA;                    // saved stack pointer value for A
uint16_t spB;                    // saved stack pointer value for B
uint8_t current = 0;             // which task is running now(more like a crude PID)


//program A
void taskA(void) 
{
    DDRB |= (1 << PB5);
    while (1) 
    {
        PORTB ^= (1 << PB5);     // toggl LED
        for (volatile long i = 0; i < 30000; i++);  // delay a bit
    }
}

//program B
void taskB(void) 
{
    DDRB |= (1 << PB4);
    while (1) 
    {
        PORTB ^= (1 << PB4);     // toggle another pin with more time duration
        for (volatile long i = 0; i < 8000; i++);  //delay a bit more
    }
}


void setup_task(uint8_t *stack, uint16_t *sp_slot, void (*task_func)(void))
{
    uint8_t *top = stack + STACK_SIZE - 1;   // stacks grows down so start at top

    uint16_t addr = (uint16_t)task_func;     // program adress

    // position the address where reti(return from interrupt) will find it since AVR requires high byte, then low, something like a big endian bytes
    *top = addr & 0xFF;          // low byte
    top--;
    *top = (addr >> 8) & 0xFF;   // high byte
    top--;

    // leave room for the 32 registers + SREGister that the switch will pop on first restore
    top -= 33;

    *sp_slot = (uint16_t)top;    // save where this process's stack pointer to the top
}

ISR(TIMER1_COMPA_vect, ISR_NAKED) 
{
    SAVE_CONTEXT();          // push all registers onto current process's stack

    // save current SPointer into the running task's slot, load the other task's SPointer, flip 'current(aka PID)'
    if (current == 0) { spA = SP; SP = spB; current = 1; }
    else{ spB = SP; SP = spA; current = 0; }

    RESTORE_CONTEXT();       // pop all registers from new task's stack, then retinterrupt 
}


int main(void)
{
    
    setup_task(stackA, &spA, taskA);  
    setup_task(stackB, &spB, taskB);

    // --- timer setup ---
    OCR1A  = 249;                          // count to this, then interrupt(via every 1mseconds), every tick = 4 microseconds(in this case)
    TCCR1B |= (1 << WGM12);                  // CTC mode: reset counter on match
    TCCR1B |= (1 << CS11) | (1 << CS10);     // prescaler 64?
    TIMSK1 |= (1 << OCIE1A);                 // enable the compare-match interrupt


    current = 0;              // we're about to become program A
    SP = spA;                // teleport to program A's stack
    sei();                   // enable interrupts after
    RESTORE_CONTEXT();      //pop the main's stack since we dont need it anymore
    while(1)
    {
        //some say something is nothing, and nothing is something...
    }
}
