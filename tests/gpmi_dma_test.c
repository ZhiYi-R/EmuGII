/*
 * GPMI/NAND DMA smoke test for STMP3770 QEMU
 *
 * Builds a small APBH DMA descriptor chain to perform READ ID and prints
 * the result.  The descriptor directions use MXS APBH terminology:
 * DMA_READ moves memory to the peripheral, DMA_WRITE moves the peripheral
 * result back to memory.
 */

#define UART_BASE 0x80070000
#define UART_DR   (*(volatile unsigned int *)(UART_BASE + 0x00))
#define UART_FR   (*(volatile unsigned int *)(UART_BASE + 0x18))
#define UART_CR   (*(volatile unsigned int *)(UART_BASE + 0x30))

#define UART_FR_TXFF (1 << 5)
#define CR_UARTEN (1 << 0)
#define CR_TXE    (1 << 8)

#define APBH_BASE 0x80004000
#define APBH_CTRL0  (*(volatile unsigned int *)(APBH_BASE + 0x00))
#define APBH_CTRL0_CLR (*(volatile unsigned int *)(APBH_BASE + 0x08))
#define APBH_CH_SEMA(ch) (*(volatile unsigned int *)(APBH_BASE + 0x040 + (ch)*0x70 + 0x40))
#define APBH_CH_NXTCMDAR(ch) (*(volatile unsigned int *)(APBH_BASE + 0x040 + (ch)*0x70 + 0x10))

#define GPMI_DMA_CH 4

#define GPMI_BASE 0x8000C000
#define GPMI_CTRL0     (*(volatile unsigned int *)(GPMI_BASE + 0x00))

#define CTRL0_COMMAND_MODE_SHIFT 24
#define CTRL0_ADDRESS_SHIFT 17
#define CTRL0_XFER_COUNT_MASK 0xFFFF

#define COMMAND_MODE_WRITE 0
#define COMMAND_MODE_READ  1
#define ADDRESS_CLE 1
#define ADDRESS_ALE 2
#define ADDRESS_DATA 0

#define NAND_CMD_READ_ID 0x90

/* APBH DMA command word bits */
#define DMA_CMD_COMMAND_SHIFT 0
#define DMA_CMD_COMMAND_NO_DMA_XFER 0
#define DMA_CMD_COMMAND_DMA_WRITE   1
#define DMA_CMD_COMMAND_DMA_READ    2
#define DMA_CMD_COMMAND_DMA_SENSE   3
#define DMA_CMD_IRQONCMPLT  (1U << 3)
#define DMA_CMD_CHAIN       (1U << 2)
#define DMA_CMD_SEMAPHORE   (1U << 6)
#define DMA_CMD_WAIT4ENDCMD (1U << 7)
#define DMA_CMD_NANDWAIT4READY (1U << 5)
#define DMA_CMD_CMDWORDS_SHIFT 12
#define DMA_CMD_XFER_COUNT_SHIFT 16

struct dma_desc {
    unsigned int next;
    unsigned int cmd;
    unsigned int bar;
    unsigned int pio[15];
};

static struct dma_desc descs[4] __attribute__((aligned(32)));
static unsigned char cmd_byte;
static unsigned char addr_byte;
static unsigned char id_buf[8];

static void uart_putc(char c) {
    while (UART_FR & UART_FR_TXFF);
    UART_DR = c;
}

static void uart_puts(const char *s) {
    while (*s) {
        if (*s == '\n') uart_putc('\r');
        uart_putc(*s++);
    }
}

static void uart_puthex(unsigned int v) {
    int i;
    uart_puts("0x");
    for (i = 28; i >= 0; i -= 4) {
        unsigned int n = (v >> i) & 0xF;
        uart_putc(n < 10 ? '0' + n : 'A' + n - 10);
    }
}

static unsigned int make_ctrl0(unsigned int mode, unsigned int addr,
                               unsigned int count) {
    return (mode << CTRL0_COMMAND_MODE_SHIFT) |
           (addr << CTRL0_ADDRESS_SHIFT) |
           (count & CTRL0_XFER_COUNT_MASK);
}

static unsigned int make_cmd(unsigned int command, unsigned int cmdwords,
                             unsigned int xfer_count, unsigned int flags) {
    return (command << DMA_CMD_COMMAND_SHIFT) |
           flags |
           (cmdwords << DMA_CMD_CMDWORDS_SHIFT) |
           (xfer_count << DMA_CMD_XFER_COUNT_SHIFT);
}

static void run_dma(void) {
    /* Take APBH DMA out of reset and clear clock gate */
    APBH_CTRL0 = 0;

    APBH_CTRL0_CLR = (1U << 16) << GPMI_DMA_CH; /* reset channel */
    APBH_CH_NXTCMDAR(GPMI_DMA_CH) = (unsigned int)&descs[0];
    APBH_CH_SEMA(GPMI_DMA_CH) = 4;

    /* Wait for channel to go idle */
    while (APBH_CH_SEMA(GPMI_DMA_CH) & 0xFF0000);
}

void _start(void) __attribute__((section(".text.startup"), naked));
void _start(void) {
    __asm__ volatile (
        "ldr sp, =0x00080000\n\t"
        "bl main\n\t"
        "b .\n\t"
    );
}

void main(void) {
    UART_CR = CR_UARTEN | CR_TXE;
    uart_puts("GPMI NAND DMA READ ID test\n");

    /* Take GPMI out of reset */
    GPMI_CTRL0 = 0;

    cmd_byte = NAND_CMD_READ_ID;
    addr_byte = 0x00;

    /* Descriptor 0: send command byte from memory to GPMI */
    descs[0].next = (unsigned int)&descs[1];
    descs[0].cmd = make_cmd(DMA_CMD_COMMAND_DMA_READ, 3, 1,
                            DMA_CMD_SEMAPHORE | DMA_CMD_WAIT4ENDCMD |
                            DMA_CMD_CHAIN);
    descs[0].bar = (unsigned int)&cmd_byte;
    descs[0].pio[0] = make_ctrl0(COMMAND_MODE_WRITE, ADDRESS_CLE, 1);
    descs[0].pio[1] = 0;
    descs[0].pio[2] = 0;

    /* Descriptor 1: send address byte from memory to GPMI */
    descs[1].next = (unsigned int)&descs[2];
    descs[1].cmd = make_cmd(DMA_CMD_COMMAND_DMA_READ, 3, 1,
                            DMA_CMD_SEMAPHORE | DMA_CMD_WAIT4ENDCMD |
                            DMA_CMD_CHAIN);
    descs[1].bar = (unsigned int)&addr_byte;
    descs[1].pio[0] = make_ctrl0(COMMAND_MODE_WRITE, ADDRESS_ALE, 1);
    descs[1].pio[1] = 0;
    descs[1].pio[2] = 0;

    /* Descriptor 2: wait for ready */
    descs[2].next = (unsigned int)&descs[3];
    descs[2].cmd = make_cmd(DMA_CMD_COMMAND_DMA_SENSE, 0, 0,
                            DMA_CMD_SEMAPHORE | DMA_CMD_CHAIN);
    descs[2].bar = 0;

    /* Descriptor 3: read 5 ID bytes to id_buf */
    descs[3].next = 0;
    descs[3].cmd = make_cmd(DMA_CMD_COMMAND_DMA_WRITE, 1, 5,
                            DMA_CMD_SEMAPHORE | DMA_CMD_IRQONCMPLT);
    descs[3].bar = (unsigned int)id_buf;
    descs[3].pio[0] = make_ctrl0(COMMAND_MODE_READ, ADDRESS_DATA, 5);

    /* Ensure all descriptor writes are visible before kicking the DMA. */
    __asm__ volatile ("" ::: "memory");

    run_dma();

    uart_puts("ID: ");
    uart_puthex(id_buf[0]); uart_putc(' ');
    uart_puthex(id_buf[1]); uart_putc(' ');
    uart_puthex(id_buf[2]); uart_putc(' ');
    uart_puthex(id_buf[3]); uart_putc(' ');
    uart_puthex(id_buf[4]); uart_puts("\n");

    if (id_buf[0] == 0xEC) {
        uart_puts("PASS\n");
    } else {
        uart_puts("FAIL\n");
    }
}
