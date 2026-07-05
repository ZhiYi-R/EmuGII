/*
 * GPMI ECC aux completion regression test.
 *
 * ExistOS uses APBH DMA descriptors whose GPMI CTRL0 PIO word does not set
 * RUN explicitly. Hardware still starts the GPMI command from the DMA PIO
 * transfer. The ECC read must write the aux buffer status byte at offset 16.
 */

#include <stdint.h>

#define UART_BASE 0x80070000
#define UART_DR   (*(volatile uint32_t *)(UART_BASE + 0x00))
#define UART_FR   (*(volatile uint32_t *)(UART_BASE + 0x18))
#define UART_CR   (*(volatile uint32_t *)(UART_BASE + 0x30))

#define UART_FR_TXFF (1U << 5)
#define CR_UARTEN    (1U << 0)
#define CR_TXE       (1U << 8)

#define APBH_BASE 0x80004000
#define APBH_CTRL0        (*(volatile uint32_t *)(APBH_BASE + 0x000))
#define APBH_CH4_NXTCMDAR (*(volatile uint32_t *)(APBH_BASE + 0x210))
#define APBH_CH4_SEMA     (*(volatile uint32_t *)(APBH_BASE + 0x240))

#define GPMI_BASE 0x8000C000
#define GPMI_CTRL0 (*(volatile uint32_t *)(GPMI_BASE + 0x00))

#define BCH_BASE 0x80008000
#define BCH_CTRL (*(volatile uint32_t *)(BCH_BASE + 0x00))

#define DMA_CMD_COMMAND_NO_DMA_XFER 0
#define DMA_CMD_COMMAND_DMA_WRITE   1
#define DMA_CMD_COMMAND_DMA_READ    2
#define DMA_CMD_COMMAND_DMA_SENSE   3
#define DMA_CMD_IRQONCMPLT          (1U << 3)
#define DMA_CMD_CHAIN               (1U << 2)
#define DMA_CMD_SEMAPHORE           (1U << 6)
#define DMA_CMD_WAIT4ENDCMD         (1U << 7)
#define DMA_CMD_NANDWAIT4READY      (1U << 5)
#define DMA_CMD_NANDLOCK            (1U << 4)
#define DMA_CMD_CMDWORDS_SHIFT      12
#define DMA_CMD_XFER_COUNT_SHIFT    16

#define CTRL0_WORD_LENGTH           (1U << 23)
#define CTRL0_LOCK_CS               (1U << 22)
#define CTRL0_COMMAND_MODE_SHIFT    24
#define CTRL0_ADDRESS_SHIFT         17
#define CTRL0_ADDRESS_INCREMENT     (1U << 16)

#define COMMAND_MODE_WRITE          0
#define COMMAND_MODE_READ           1
#define COMMAND_MODE_WAIT_READY     3
#define ADDRESS_DATA                0
#define ADDRESS_CLE                 1

#define GPMI_ECCCTRL_ENABLE_ECC     (1U << 12)
#define GPMI_ECC_BYTES              (4U * (512U + 9U) + (19U + 9U))

#define NAND_CMD_READ0              0x00
#define NAND_CMD_READSTART          0x30
#define EXPECTED_DATA0              0x5A

struct gpmi_dma_desc {
    uint32_t next;
    uint32_t cmd;
    uint32_t bar;
    uint32_t ctrl0;
    uint32_t compare;
    uint32_t eccctrl;
    uint32_t ecccount;
    uint32_t payload;
    uint32_t auxiliary;
};

static struct gpmi_dma_desc desc[6] __attribute__((aligned(32)));
static uint8_t cmd_buf[8] __attribute__((aligned(4)));
static uint8_t data_buf[2048] __attribute__((aligned(4)));
static uint8_t aux_buf[64] __attribute__((aligned(4)));

static void uart_putc(char c)
{
    while (UART_FR & UART_FR_TXFF) {
    }
    UART_DR = (uint32_t)c;
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

static void uart_hex8(uint8_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    uart_putc(hex[(v >> 4) & 0xF]);
    uart_putc(hex[v & 0xF]);
}

static uint32_t make_cmd(uint32_t command, uint32_t cmdwords,
                         uint32_t xfer_count, uint32_t flags)
{
    return command | flags |
           (cmdwords << DMA_CMD_CMDWORDS_SHIFT) |
           (xfer_count << DMA_CMD_XFER_COUNT_SHIFT);
}

static uint32_t make_ctrl0(uint32_t mode, uint32_t address, uint32_t count,
                           uint32_t flags)
{
    return (mode << CTRL0_COMMAND_MODE_SHIFT) |
           (address << CTRL0_ADDRESS_SHIFT) |
           CTRL0_WORD_LENGTH | flags | count;
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
    uint32_t timeout;
    uint32_t row = 31U * 64U;

    UART_CR = CR_UARTEN | CR_TXE;
    uart_puts("GPMI ECC AUX test\n");

    APBH_CTRL0 = 0;
    GPMI_CTRL0 = 0;
    BCH_CTRL = 0;

    cmd_buf[0] = NAND_CMD_READ0;
    cmd_buf[1] = 0;
    cmd_buf[2] = 0;
    cmd_buf[3] = (uint8_t)(row & 0xFF);
    cmd_buf[4] = (uint8_t)((row >> 8) & 0xFF);
    cmd_buf[5] = NAND_CMD_READSTART;

    for (int i = 0; i < (int)sizeof(aux_buf); i++) {
        aux_buf[i] = 0xA5;
    }
    aux_buf[16] = 0x23;

    desc[0].next = (uint32_t)&desc[1];
    desc[0].cmd = make_cmd(DMA_CMD_COMMAND_DMA_READ, 3, 5,
                           DMA_CMD_WAIT4ENDCMD | DMA_CMD_NANDLOCK |
                           DMA_CMD_CHAIN);
    desc[0].bar = (uint32_t)cmd_buf;
    desc[0].ctrl0 = make_ctrl0(COMMAND_MODE_WRITE, ADDRESS_CLE, 5,
                               CTRL0_LOCK_CS | CTRL0_ADDRESS_INCREMENT);
    desc[0].eccctrl = 0;

    desc[1].next = (uint32_t)&desc[2];
    desc[1].cmd = make_cmd(DMA_CMD_COMMAND_DMA_READ, 3, 1,
                           DMA_CMD_WAIT4ENDCMD | DMA_CMD_NANDLOCK |
                           DMA_CMD_CHAIN);
    desc[1].bar = (uint32_t)&cmd_buf[5];
    desc[1].ctrl0 = make_ctrl0(COMMAND_MODE_WRITE, ADDRESS_CLE, 1,
                               CTRL0_LOCK_CS);
    desc[1].eccctrl = 0;

    desc[2].next = (uint32_t)&desc[3];
    desc[2].cmd = make_cmd(DMA_CMD_COMMAND_NO_DMA_XFER, 1, 0,
                           DMA_CMD_WAIT4ENDCMD | DMA_CMD_NANDWAIT4READY |
                           DMA_CMD_CHAIN);
    desc[2].ctrl0 = make_ctrl0(COMMAND_MODE_WAIT_READY, ADDRESS_DATA, 0, 0);

    desc[3].next = (uint32_t)&desc[4];
    desc[3].cmd = make_cmd(DMA_CMD_COMMAND_DMA_SENSE, 0, 0, DMA_CMD_CHAIN);
    desc[3].bar = (uint32_t)&desc[5];

    desc[4].next = (uint32_t)&desc[5];
    desc[4].cmd = make_cmd(DMA_CMD_COMMAND_NO_DMA_XFER, 6, 0,
                           DMA_CMD_WAIT4ENDCMD | DMA_CMD_NANDLOCK |
                           DMA_CMD_CHAIN);
    desc[4].ctrl0 = make_ctrl0(COMMAND_MODE_READ, ADDRESS_DATA,
                               GPMI_ECC_BYTES, 0);
    desc[4].compare = 0;
    desc[4].eccctrl = GPMI_ECCCTRL_ENABLE_ECC | 0x10F;
    desc[4].ecccount = GPMI_ECC_BYTES;
    desc[4].payload = (uint32_t)data_buf;
    desc[4].auxiliary = (uint32_t)aux_buf;

    desc[5].next = 0;
    desc[5].cmd = DMA_CMD_IRQONCMPLT | DMA_CMD_SEMAPHORE;

    __asm__ volatile ("" ::: "memory");

    APBH_CH4_NXTCMDAR = (uint32_t)&desc[0];
    APBH_CH4_SEMA = 1;

    timeout = 1000000;
    while ((APBH_CH4_SEMA & 0xFFU) && timeout--) {
    }

    uart_puts("aux16=");
    uart_hex8(aux_buf[16]);
    uart_puts("\n");
    uart_puts("data0=");
    uart_hex8(data_buf[0]);
    uart_puts("\n");

    if (timeout == 0) {
        uart_puts("GPMI ECC AUX TEST TIMEOUT\n");
    } else if (aux_buf[16] == 0x00 && data_buf[0] == EXPECTED_DATA0) {
        uart_puts("GPMI ECC AUX TEST PASS\n");
    } else {
        uart_puts("GPMI ECC AUX TEST FAIL\n");
    }
}
