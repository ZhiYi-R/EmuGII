/*
 * GPMI ECC write metadata regression test.
 *
 * ExistOS writes FTL pages with a 2048-byte payload followed by a 19-byte
 * auxiliary metadata buffer in the same NAND program operation.  Dhara uses
 * that metadata when resuming the FTL map after reboot, so the emulator must
 * persist both payload and auxiliary bytes.
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
#define NAND_PROGRAM_BYTES          (2048U + 19U)

#define NAND_CMD_READ0              0x00
#define NAND_CMD_READSTART          0x30
#define NAND_CMD_SEQIN              0x80
#define NAND_CMD_PAGEPROG           0x10

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

static struct gpmi_dma_desc write_desc[6] __attribute__((aligned(32)));
static struct gpmi_dma_desc read_desc[6] __attribute__((aligned(32)));
static uint8_t cmd_buf[8] __attribute__((aligned(4)));
static uint8_t data_buf[2048] __attribute__((aligned(4)));
static uint8_t meta_buf[19] __attribute__((aligned(4)));
static uint8_t read_data[2048] __attribute__((aligned(4)));
static uint8_t read_aux[64] __attribute__((aligned(4)));

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

static int run_chain(struct gpmi_dma_desc *chain)
{
    uint32_t timeout;

    __asm__ volatile ("" ::: "memory");

    APBH_CH4_NXTCMDAR = (uint32_t)chain;
    APBH_CH4_SEMA = 1;

    timeout = 1000000;
    while ((APBH_CH4_SEMA & 0xFFU) && timeout--) {
    }

    return timeout == 0 ? -1 : 0;
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
    uint32_t row = 40U * 64U;

    UART_CR = CR_UARTEN | CR_TXE;
    uart_puts("GPMI write metadata test\n");

    APBH_CTRL0 = 0;
    GPMI_CTRL0 = 0;
    BCH_CTRL = 0;

    for (uint32_t i = 0; i < sizeof(data_buf); i++) {
        data_buf[i] = (uint8_t)(0x40U + (i & 0x3FU));
        read_data[i] = 0;
    }
    for (uint32_t i = 0; i < sizeof(meta_buf); i++) {
        meta_buf[i] = 0xFF;
    }
    for (uint32_t i = 0; i < sizeof(read_aux); i++) {
        read_aux[i] = 0xA5;
    }
    meta_buf[0] = 0x78;
    meta_buf[1] = 0x56;
    meta_buf[2] = 0x34;
    meta_buf[3] = 0x12;

    cmd_buf[0] = NAND_CMD_SEQIN;
    cmd_buf[1] = 0;
    cmd_buf[2] = 0;
    cmd_buf[3] = (uint8_t)(row & 0xFF);
    cmd_buf[4] = (uint8_t)((row >> 8) & 0xFF);
    cmd_buf[5] = NAND_CMD_PAGEPROG;

    write_desc[0].next = (uint32_t)&write_desc[1];
    write_desc[0].cmd = make_cmd(DMA_CMD_COMMAND_DMA_READ, 3, 5,
                                 DMA_CMD_WAIT4ENDCMD | DMA_CMD_NANDLOCK |
                                 DMA_CMD_CHAIN);
    write_desc[0].bar = (uint32_t)cmd_buf;
    write_desc[0].ctrl0 = make_ctrl0(COMMAND_MODE_WRITE, ADDRESS_CLE, 5,
                                     CTRL0_LOCK_CS | CTRL0_ADDRESS_INCREMENT);
    write_desc[0].eccctrl = 0;

    write_desc[1].next = (uint32_t)&write_desc[2];
    write_desc[1].cmd = make_cmd(DMA_CMD_COMMAND_DMA_READ, 4, 2048,
                                 DMA_CMD_NANDLOCK | DMA_CMD_CHAIN);
    write_desc[1].bar = (uint32_t)data_buf;
    write_desc[1].ctrl0 = make_ctrl0(COMMAND_MODE_WRITE, ADDRESS_DATA,
                                     NAND_PROGRAM_BYTES, CTRL0_LOCK_CS);
    write_desc[1].compare = 0;
    write_desc[1].eccctrl = GPMI_ECCCTRL_ENABLE_ECC | 0x10F;
    write_desc[1].ecccount = GPMI_ECC_BYTES;

    write_desc[2].next = (uint32_t)&write_desc[3];
    write_desc[2].cmd = make_cmd(DMA_CMD_COMMAND_DMA_READ, 0, 19,
                                 DMA_CMD_WAIT4ENDCMD | DMA_CMD_NANDLOCK |
                                 DMA_CMD_CHAIN);
    write_desc[2].bar = (uint32_t)meta_buf;

    write_desc[3].next = (uint32_t)&write_desc[4];
    write_desc[3].cmd = make_cmd(DMA_CMD_COMMAND_DMA_READ, 3, 1,
                                 DMA_CMD_WAIT4ENDCMD | DMA_CMD_NANDLOCK |
                                 DMA_CMD_CHAIN);
    write_desc[3].bar = (uint32_t)&cmd_buf[5];
    write_desc[3].ctrl0 = make_ctrl0(COMMAND_MODE_WRITE, ADDRESS_CLE, 1,
                                     CTRL0_LOCK_CS);
    write_desc[3].eccctrl = 0;

    write_desc[4].next = (uint32_t)&write_desc[5];
    write_desc[4].cmd = make_cmd(DMA_CMD_COMMAND_NO_DMA_XFER, 1, 0,
                                 DMA_CMD_WAIT4ENDCMD | DMA_CMD_NANDWAIT4READY |
                                 DMA_CMD_CHAIN);
    write_desc[4].ctrl0 = make_ctrl0(COMMAND_MODE_WAIT_READY, ADDRESS_DATA, 0,
                                     0);

    write_desc[5].next = 0;
    write_desc[5].cmd = DMA_CMD_IRQONCMPLT | DMA_CMD_SEMAPHORE;

    if (run_chain(write_desc) != 0) {
        uart_puts("GPMI WRITE META TEST TIMEOUT\n");
        return;
    }

    cmd_buf[0] = NAND_CMD_READ0;
    cmd_buf[1] = 0;
    cmd_buf[2] = 0;
    cmd_buf[3] = (uint8_t)(row & 0xFF);
    cmd_buf[4] = (uint8_t)((row >> 8) & 0xFF);
    cmd_buf[5] = NAND_CMD_READSTART;

    read_desc[0].next = (uint32_t)&read_desc[1];
    read_desc[0].cmd = make_cmd(DMA_CMD_COMMAND_DMA_READ, 3, 5,
                                DMA_CMD_WAIT4ENDCMD | DMA_CMD_NANDLOCK |
                                DMA_CMD_CHAIN);
    read_desc[0].bar = (uint32_t)cmd_buf;
    read_desc[0].ctrl0 = make_ctrl0(COMMAND_MODE_WRITE, ADDRESS_CLE, 5,
                                    CTRL0_LOCK_CS | CTRL0_ADDRESS_INCREMENT);
    read_desc[0].eccctrl = 0;

    read_desc[1].next = (uint32_t)&read_desc[2];
    read_desc[1].cmd = make_cmd(DMA_CMD_COMMAND_DMA_READ, 3, 1,
                                DMA_CMD_WAIT4ENDCMD | DMA_CMD_NANDLOCK |
                                DMA_CMD_CHAIN);
    read_desc[1].bar = (uint32_t)&cmd_buf[5];
    read_desc[1].ctrl0 = make_ctrl0(COMMAND_MODE_WRITE, ADDRESS_CLE, 1,
                                    CTRL0_LOCK_CS);
    read_desc[1].eccctrl = 0;

    read_desc[2].next = (uint32_t)&read_desc[3];
    read_desc[2].cmd = make_cmd(DMA_CMD_COMMAND_NO_DMA_XFER, 1, 0,
                                DMA_CMD_WAIT4ENDCMD | DMA_CMD_NANDWAIT4READY |
                                DMA_CMD_CHAIN);
    read_desc[2].ctrl0 = make_ctrl0(COMMAND_MODE_WAIT_READY, ADDRESS_DATA, 0,
                                    0);

    read_desc[3].next = (uint32_t)&read_desc[4];
    read_desc[3].cmd = make_cmd(DMA_CMD_COMMAND_DMA_SENSE, 0, 0,
                                DMA_CMD_CHAIN);
    read_desc[3].bar = (uint32_t)&read_desc[5];

    read_desc[4].next = (uint32_t)&read_desc[5];
    read_desc[4].cmd = make_cmd(DMA_CMD_COMMAND_NO_DMA_XFER, 6, 0,
                                DMA_CMD_WAIT4ENDCMD | DMA_CMD_NANDLOCK |
                                DMA_CMD_CHAIN);
    read_desc[4].ctrl0 = make_ctrl0(COMMAND_MODE_READ, ADDRESS_DATA,
                                    GPMI_ECC_BYTES, 0);
    read_desc[4].compare = 0;
    read_desc[4].eccctrl = GPMI_ECCCTRL_ENABLE_ECC | 0x10F;
    read_desc[4].ecccount = GPMI_ECC_BYTES;
    read_desc[4].payload = (uint32_t)read_data;
    read_desc[4].auxiliary = (uint32_t)read_aux;

    read_desc[5].next = 0;
    read_desc[5].cmd = DMA_CMD_IRQONCMPLT | DMA_CMD_SEMAPHORE;

    if (run_chain(read_desc) != 0) {
        uart_puts("GPMI WRITE META TEST READ TIMEOUT\n");
        return;
    }

    uart_puts("data0=");
    uart_hex8(read_data[0]);
    uart_puts(" aux=");
    uart_hex8(read_aux[0]);
    uart_hex8(read_aux[1]);
    uart_hex8(read_aux[2]);
    uart_hex8(read_aux[3]);
    uart_puts(" aux16=");
    uart_hex8(read_aux[16]);
    uart_puts("\n");

    if (read_data[0] == data_buf[0] &&
        read_aux[0] == meta_buf[0] &&
        read_aux[1] == meta_buf[1] &&
        read_aux[2] == meta_buf[2] &&
        read_aux[3] == meta_buf[3] &&
        read_aux[16] == 0x00) {
        uart_puts("GPMI WRITE META TEST PASS\n");
    } else {
        uart_puts("GPMI WRITE META TEST FAIL\n");
    }
}
