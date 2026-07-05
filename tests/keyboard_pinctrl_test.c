/*
 * HP39GII keyboard GPIO idle-level test for STMP3770 QEMU.
 *
 * ExistOS configures the keyboard matrix columns as input GPIOs and treats
 * a high column level as "not pressed". With no modeled key press, all
 * column inputs must therefore read high.
 */

#define UART_BASE 0x80070000
#define UART_DR   (*(volatile unsigned int *)(UART_BASE + 0x00))
#define UART_FR   (*(volatile unsigned int *)(UART_BASE + 0x18))
#define UART_CR   (*(volatile unsigned int *)(UART_BASE + 0x30))

#define UART_FR_TXFF (1 << 5)
#define CR_UARTEN    (1 << 0)
#define CR_TXE       (1 << 8)

#define PINCTRL_BASE 0x80018000
#define PINCTRL_DOUT0 (*(volatile unsigned int *)(PINCTRL_BASE + 0x400))
#define PINCTRL_DOUT1 (*(volatile unsigned int *)(PINCTRL_BASE + 0x410))
#define PINCTRL_DOUT2 (*(volatile unsigned int *)(PINCTRL_BASE + 0x420))
#define PINCTRL_DIN0  (*(volatile unsigned int *)(PINCTRL_BASE + 0x500))
#define PINCTRL_DIN1  (*(volatile unsigned int *)(PINCTRL_BASE + 0x510))
#define PINCTRL_DOE0  (*(volatile unsigned int *)(PINCTRL_BASE + 0x600))
#define PINCTRL_DOE1  (*(volatile unsigned int *)(PINCTRL_BASE + 0x610))
#define PINCTRL_DOE2  (*(volatile unsigned int *)(PINCTRL_BASE + 0x620))

#define KEY_COL_MASK ((1U << 22) | (1U << 23) | (1U << 25) | \
                      (1U << 26) | (1U << 27))
#define KEY_ROW2_MASK ((1U << 14) | (1U << 8) | (1U << 7) | \
                       (1U << 6) | (1U << 5) | (1U << 4) | \
                       (1U << 3) | (1U << 2))

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
    unsigned int din1;
    unsigned int pass;

    UART_CR = CR_UARTEN | CR_TXE;
    uart_puts("Keyboard PINCTRL idle test\n");

    /*
     * Match ExistOS Keyboard::gpioInit() electrical intent:
     * - bank1 column pins are inputs with DOUT high
     * - rows are GPIO outputs; no key is modeled as pressed
     */
    PINCTRL_DOUT1 |= KEY_COL_MASK;
    PINCTRL_DOE1 &= ~KEY_COL_MASK;

    PINCTRL_DOUT2 &= ~KEY_ROW2_MASK;
    PINCTRL_DOE2 |= KEY_ROW2_MASK;

    PINCTRL_DOUT1 &= ~(1U << 24);
    PINCTRL_DOE1 |= (1U << 24);

    PINCTRL_DOUT0 &= ~(1U << 20);
    PINCTRL_DOE0 |= (1U << 20);

    PINCTRL_DOUT0 |= (1U << 14);
    PINCTRL_DOE0 &= ~(1U << 14);

    din1 = PINCTRL_DIN1;
    uart_puts("DIN1: ");
    uart_puthex(din1);
    uart_puts("\n");

    pass = (din1 & KEY_COL_MASK) == KEY_COL_MASK;
    if (pass) {
        uart_puts("KEYBOARD PINCTRL TEST PASS\n");
    } else {
        uart_puts("KEYBOARD PINCTRL TEST FAIL\n");
    }
}
