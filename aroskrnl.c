#include <avr/io.h>     //memory adress and pin macros purely for adress translations
#include <avr/interrupt.h>   //the interrupt function
#include <avr/wdt.h>  // for watchdog timer
#define BAUD 9600
#include <util/setbaud.h>  //for the serail monitor crash reporting


#define STACK_SIZE 256
#define MAX_TASKS 3
#define GUARD_BYTES 16
#define CRASH_MAGIC 0xC0DE

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
/*============================================================================================================*/






















/*============================================================================================*/
/*Process metadata and setup, with he interrupt handler*/
/*OS \/*/


















/*=============================================================================================*/
/*Crash data and reporting*/

typedef struct {
    uint16_t magic;     //to distinguish if we are reading a valid crash report
    uint8_t  pid;       //which process has crashed
    uint16_t sp;        //the stack where the crash happened
    uint8_t  reason;   // 1 = SPointer bound, 2 = canary
} crash_log_t;


volatile crash_log_t g_crash __attribute__((section(".noinit"))); //this is a protected non zeroed section within the AVR chip, so we use it to store crash logs

static void paint_guard(uint8_t *stack) 
{
    for (uint8_t i = 0; i < GUARD_BYTES; i++)
        stack[i] = (i & 1) ? 0xAA : 0x55;      //write some bytes that we will check if they are overwritten
}

static uint8_t guard_intact(uint8_t *stack) 
{
    for (uint8_t i = 0; i < GUARD_BYTES; i++)
        if (stack[i] != ((i & 1) ? 0xAA : 0x55)) return 0;     //check if any of the guard bytes are overwritten
    return 1;
}

/*Report crashes through the serial monitor*/
static void uart_init(void) 
{
    UBRR0H = UBRRH_VALUE;
    UBRR0L = UBRRL_VALUE;
    #if USE_2X
        UCSR0A |= (1 << U2X0);
    #else
        UCSR0A &= ~(1 << U2X0);
    #endif
    UCSR0B = (1 << TXEN0);                    // enable transmitter only
    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);   // 8 data bits, no parity, 1 stop
}

static void uart_putc(char c) 
{
    while (!(UCSR0A & (1 << UDRE0)));  // wait until the buffer is free
    UDR0 = c;
}

static void uart_puts(const char *s) 
{   //send characters in form of a string
    while (*s) uart_putc(*s++);
}

static void uart_hex8(uint8_t v) 
{
    const char *d = "0123456789ABCDEF";   //lookup the given number as hex
    uart_putc(d[v >> 4]);
    uart_putc(d[v & 0x0F]);
}

static void uart_hex16(uint16_t v)   //similiar with dividing it between 2 bytes
{
    uart_hex8(v >> 8);
    uart_hex8(v & 0xFF);
}

static void report_crash(const volatile crash_log_t *log)
{
    uart_init();
    uart_puts("CRASH pid=");
    uart_hex8(log->pid);
    uart_puts(" sp=");
    uart_hex16(log->sp);
    uart_puts(" reason=");
    if (log->reason == 1) uart_puts("Pointer bound");
    else if (log->reason == 2) uart_puts("Canary");
    else uart_puts("No tasks left");
    uart_puts("\r\n");
}

/*================================================================================================*/
















typedef struct
{
    uint16_t sp;
    uint8_t active;
    uint8_t PID;
    uint8_t *stack_base;
}task_t;

task_t tasks[MAX_TASKS];
volatile uint8_t current_task = 0;


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

int create_task(void (*func)(void), uint8_t *stack) 
{  //IMPORTANT: must preallocate a buffer for a stack 
    for (int i = 0; i < MAX_TASKS; i++) 
    {
        if (!tasks[i].active) 
        {
            setup_task(stack, &tasks[i].sp, func);
            tasks[i].active     = 1;
            tasks[i].PID         = i;
            tasks[i].stack_base = stack;
            paint_guard(stack);
            return i;
        }
    }
    return -1;
}


ISR(TIMER1_COMPA_vect, ISR_NAKED) 
{
    SAVE_CONTEXT();          // push all registers onto current process's stack

    uint16_t sp = SP;
    tasks[current_task].sp = sp;

    uint8_t *base = tasks[current_task].stack_base;

    if(!guard_intact(base))   //is the guard bytes are overwritten then we must setup the crash report...
    {
        g_crash.magic = CRASH_MAGIC;
        g_crash.pid = current_task;
        g_crash.sp = sp;
        g_crash.reason = 2;
        wdt_enable(WDTO_15MS);  //...and enable the watchdog timer
        while (1);  //also must reset right away since the corruption already happened
    }

      if (sp < (uint16_t)base + GUARD_BYTES) //if the stack pointer went below the allocated buffer's borderline....
    {
        g_crash.magic = CRASH_MAGIC;
        g_crash.pid = current_task;
        g_crash.sp = sp;
        g_crash.reason = 1;
        tasks[current_task].active = 0;   //....that means there hasnt been corruption on other programs yet, so just kill it and move on
    }

  
    uint8_t next = current_task;   //next is just where we start at
    for (uint8_t i = 0; i < MAX_TASKS; i++) //look for the next active task to start executing
    {
        next = (next + 1) % MAX_TASKS;
        if (tasks[next].active) break;
    }
    
    if (!tasks[next].active) {
        g_crash.pid = current_task;
        g_crash.sp = sp;
        g_crash.reason = 3;
        g_crash.magic = CRASH_MAGIC;   // magic last
        wdt_enable(WDTO_15MS);
        while (1);
    }

    current_task = next;
    SP = tasks[next].sp;

    RESTORE_CONTEXT();       // pop all registers from new task's stack, then retinterrupt 
}

/*=====================================================================================================*/


















/*======================================================================================================*/
/*Programs to be run*/

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


/*===================================================================================================*/

























uint8_t stackA[STACK_SIZE];
uint8_t stackB[STACK_SIZE];

int main(void)
{
    uint8_t mcusr = MCUSR;
    MCUSR = 0;
    wdt_disable();            // must be immediate or you boot-loop

    uart_init();
    uart_puts("boot mcusr=");
    uart_hex8(mcusr);
    uart_puts(" magic=");
    uart_hex16(g_crash.magic);
    uart_puts("\r\n");

    if (g_crash.magic == CRASH_MAGIC) {
        report_crash(&g_crash);
        g_crash.magic = 0;
    }
   uart_puts("A\r\n");
create_task(taskA, stackA);
create_task(taskB, stackB);
uart_puts("B\r\n");

    OCR1A  = 249;
    TCCR1B |= (1 << WGM12) | (1 << CS11) | (1 << CS10);
    TIMSK1 |= (1 << OCIE1A);

    // start task 0 by hand
    uart_puts("C\r\n");
    SP = tasks[0].sp;
    sei();
    RESTORE_CONTEXT();
    while(1);
}
