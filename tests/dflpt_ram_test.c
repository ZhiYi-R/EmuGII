/*
 * Hardware DFLPT RAM regression test.
 *
 * ExistOS uses the STMP37xx hardware first-level page-table RAM at
 * 0x800C0000 when USE_HARDWARE_DFLPT is enabled.  The board model must expose
 * this range as writable RAM so the hypervisor can maintain VM mappings.
 */

#define UART_BASE 0x80070000
#define UART_DR   (*(volatile unsigned int *)(UART_BASE + 0x00))
#define UART_FR   (*(volatile unsigned int *)(UART_BASE + 0x18))
#define UART_CR   (*(volatile unsigned int *)(UART_BASE + 0x30))

#define UART_FR_TXFF (1 << 5)
#define CR_UARTEN    (1 << 0)
#define CR_TXE       (1 << 8)

#define DFLPT_BASE 0x800C0000
#define DFLPT_WORD(n) (*(volatile unsigned int *)(DFLPT_BASE + (n) * 4))

static void uart_putc(char c)
{
    while (UART_FR & UART_FR_TXFF) {
    }
    UART_DR = c;
}

static void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') {
            uart_putc('\r');
        }
        uart_putc(*s++);
    }
}

static void uart_puthex(unsigned int v)
{
    int i;

    uart_puts("0x");
    for (i = 28; i >= 0; i -= 4) {
        unsigned int n = (v >> i) & 0xF;
        uart_putc(n < 10 ? '0' + n : 'A' + n - 10);
    }
}

void _start(void) __attribute__((section(".text.startup"), naked));

void _start(void)
{
    __asm__ volatile (
        "ldr sp, =0x00080000\n\t"
        "bl main\n\t"
        "b .\n\t"
    );
}

void main(void)
{
    unsigned int first;
    unsigned int last;

    UART_CR = CR_UARTEN | CR_TXE;
    uart_puts("DFLPT RAM test\n");

    DFLPT_WORD(0) = 0xA5A55A5A;
    DFLPT_WORD(4095) = 0x5AA5A55A;

    first = DFLPT_WORD(0);
    last = DFLPT_WORD(4095);

    uart_puts("first=");
    uart_puthex(first);
    uart_puts("\n");
    uart_puts("last=");
    uart_puthex(last);
    uart_puts("\n");

    if (first == 0xA5A55A5A && last == 0x5AA5A55A) {
        uart_puts("DFLPT RAM TEST PASS\n");
    } else {
        uart_puts("DFLPT RAM TEST FAIL\n");
    }
}
