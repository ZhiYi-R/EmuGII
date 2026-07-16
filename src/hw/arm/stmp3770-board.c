/*
 * STMP3770 Development Board (HP 39gII Calculator)
 *
 * Mimics the real hardware environment including Boot ROM initialization.
 *
 * Hardware configuration based on ExistOS-For-HP39GII BSP:
 * - 512KB on-chip SRAM (no external DRAM)
 * - NAND Flash (Samsung K9F1G08U0D: 128MB, 2KB page, 64 pages/block)
 * - 131×64 monochrome LCD (grayscale capable)
 * - Matrix keyboard (6×9)
 * - USB 2.0 OTG
 * - Audio DAC/ADC
 *
 * Memory architecture:
 * - Physical SRAM: 512KB @ 0x00000000
 * - Virtual memory: ExistOS uses NAND Flash as swap space
 *   - VM ROM: 0x00100000-0x006FFFFF (6MB virtual, mapped to Flash)
 *   - VM RAM: 0x02000000-0x022FFFFF (3MB virtual, mapped to Flash)
 *
 * Copyright (C) 2024
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/arm/stmp3770.h"
#include "hw/boards.h"
#include "hw/qdev-properties.h"
#include "qemu/error-report.h"
#include "hw/loader.h"
#include "system/system.h"
#include "system/address-spaces.h"
#include "qemu/units.h"
#include "chardev/char.h"
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "system/block-backend-global-state.h"
#include "block/block-common.h"
#include "qobject/qdict.h"
#include "qemu/cutils.h"
#include "target/arm/cpu.h"
#include "exec/cputlb.h"
#include "block/aio.h"

/* HP 39gII hardware: 512KB SRAM only, no external DRAM */
#define STMP3770_BOARD_RAM_DEFAULT  (0)
#define STMP3770_DEFAULT_ROM_NAME   "rom.bin"
#define STMP3770_DEFAULT_FLASH_NAME "flash.bin"

#define TYPE_STMP3770_BOARD MACHINE_TYPE_NAME("stmp3770")
OBJECT_DECLARE_SIMPLE_TYPE(STMP3770BoardState, STMP3770_BOARD)

struct STMP3770BoardState {
    MachineState parent_obj;
    STMP3770State soc;

    /* Boot strap pins (modeled as board-level properties). */
    uint8_t boot_lcd_rs;
    uint8_t boot_lcd_data;

    /* Boot state machine state, exposed for introspection. */
    uint32_t boot_state;
    uint32_t boot_mode;
};

static char *stmp3770_exec_dir_file(const char *name)
{
    g_autofree char *dir = get_relocated_path(".");

    return g_build_filename(dir, name, NULL);
}

static char *stmp3770_default_file(const char *name)
{
    g_autofree char *direct = stmp3770_exec_dir_file(name);

    if (g_file_test(direct, G_FILE_TEST_IS_REGULAR)) {
        return g_steal_pointer(&direct);
    }

    g_autofree char *parent = stmp3770_exec_dir_file("..");
    g_autofree char *parent_firmware =
        g_build_filename(parent, "firmware", name, NULL);

    if (g_file_test(parent_firmware, G_FILE_TEST_IS_REGULAR)) {
        return g_steal_pointer(&parent_firmware);
    }

    return g_build_filename(parent, name, NULL);
}

static bool stmp3770_file_exists(const char *path)
{
    return g_file_test(path, G_FILE_TEST_IS_REGULAR);
}

/*
 * Simulate a peripheral register write as the boot ROM would do.
 * Uses address_space_write to go through the system address space,
 * ensuring all side effects (state transitions, IRQ updates) are
 * properly handled.
 */
static void stmp3770_rom_write(MemoryRegion *mr, hwaddr offset,
                               uint64_t value)
{
    memory_region_dispatch_write(mr, offset, value,
                                 MO_32, MEMTXATTRS_UNSPECIFIED);
}

/*
 * Simulate the STMP3770 mask ROM boot initialization sequence.
 *
 * This replicates the observable side effects of the real SigmaTel mask ROM
 * (extracted from a 64 KiB OCROM dump) reset handler and early init functions:
 *
 *  1. Reset handler (0xFFFF2338, ARM mode):
 *     - Ungate RTC, PINCTRL (SFTRST + CLKGATE clear)
 *     - Ungate POWER (CLKGATE clear only; SFTRST is not asserted at reset)
 *
 *  2. init_func_1 (0xFFFF15A4): OCOTP bank open/read/close cycle
 *
 *  3. init_func_2 (0xFFFF1858): Debug UART configuration
 *     - IBRD=0x0D, FBRD=0x01 (24 MHz / (16 * 115200) ≈ 13.02)
 *     - LCR_H=0x70 (8-bit, FIFO enabled)
 *     - CR=0x301 (UARTEN | TXE | RXE)
 *
 *  4. init_func_3/4 (0xFFFF1518/0xFFFF1538): CP15 TLB + I-cache invalidate
 *  5. init_func_5/6 (0xFFFF154C/0xFFFF1554): CP15 SCTLR configuration
 *     - Enable alignment checking (SCTLR_A, bit 1)
 *     - Configure ARM926EJ-S control bits
 */
static void stmp3770_rom_boot_init(void *opaque)
{
    STMP3770State *soc = opaque;

    /* Get memory regions for each device we need to write to */
    MemoryRegion *rtc_mr = sysbus_mmio_get_region(
        SYS_BUS_DEVICE(soc->rtc), 0);
    MemoryRegion *pinctrl_mr = sysbus_mmio_get_region(
        SYS_BUS_DEVICE(soc->pinctrl), 0);
    MemoryRegion *power_mr = sysbus_mmio_get_region(
        SYS_BUS_DEVICE(soc->power), 0);
    MemoryRegion *ocotp_mr = sysbus_mmio_get_region(
        SYS_BUS_DEVICE(soc->ocotp), 0);

    /*
     * 1. Reset handler: release peripheral soft-reset and clock gates.
     *
     * RTC CTRL_CLR (0x8005C008) = 0xC0000000  (SFTRST | CLKGATE)
     * PINCTRL CTRL_CLR (0x80018008) = 0xC0000000  (SFTRST | CLKGATE)
     * POWER CTRL_CLR (0x80044008) = 0x40000000  (CLKGATE only)
     */
    stmp3770_rom_write(rtc_mr, 0x008, 0xC0000000);
    stmp3770_rom_write(pinctrl_mr, 0x008, 0xC0000000);
    stmp3770_rom_write(power_mr, 0x008, 0x40000000);

    /*
     * 2. OCOTP init: open bank for shadow read, then close.
     *
     * OCOTP CTRL_SET (0x8002C004) = 0x00001000  (RD_BANK_OPEN, bit 12)
     * OCOTP CTRL_CLR (0x8002C008) = 0x00001000  (close bank)
     *
     * The real ROM waits for the bank-ready status between set and close,
     * but QEMU's OCOTP shadow read is synchronous so no polling is needed.
     */
    stmp3770_rom_write(ocotp_mr, 0x004, 0x00001000);
    stmp3770_rom_write(ocotp_mr, 0x008, 0x00001000);

    /*
     * 3. Debug UART configuration (init_func_2).
     *
     * The ROM configures the debug UART for 115200 baud at 24 MHz XTAL:
     *   IBRD = 13, FBRD = 1  →  24e6 / (16 * (13 + 1/64)) = 115384 Hz
     *   LCR_H = 0x70         →  8-bit word, FIFO enabled
     *   CR = 0x301           →  UARTEN | TXE | RXE
     *
     * Directly set the UART state registers because the UART device
     * reset must run first (to initialize FIFOs and timers) and our
     * bottom-half callback runs after reset completes.
     */
    {
        STMP3770UARTDebugState *uart = soc->uartdbg;
        uart->ibrd = 0x00D;
        uart->fbrd = 0x001;
        uart->lcr = 0x070;
        uart->cr = 0x301;
    }

    /*
     * 4. CP15 initialization (init_func_3/4/5/6).
     *
     * The ROM performs:
     *   MCR p15, r0, c8, c7, #0  — invalidate entire TLB
     *   MCR p15, r0, c7, c5, #0  — invalidate entire I-cache
     *   MRC p15, r0, c1, c0, #0  — read SCTLR
     *   ORR r0, r0, #2            — set SCTLR_A (alignment check enable)
     *   AND r0, r0, mask          — clear unused/reserved bits
     *   ORR r0, r0, value         — set ARM926 control bits
     *   MCR p15, r0, c1, c0, #0  — write SCTLR
     *
     * In QEMU, TLB invalidation maps to tlb_flush().  I-cache invalidation
     * is a no-op (QEMU manages translation caching internally).  The SCTLR
     * is modified in-place and hflags are rebuilt.
     */
    {
        CPUARMState *env = &soc->cpu.env;
        uint64_t sctlr = env->cp15.sctlr_ns;

        /* Flush TLB (MCR p15, c8, c7, #0) */
        tlb_flush(CPU(&soc->cpu));

        /* Modify SCTLR: enable alignment checking, configure control bits */
        sctlr |= SCTLR_A;          /* bit 1: alignment check enable */
        sctlr &= 0x0005F3FFULL;    /* clear upper bits and reserved fields */
        sctlr |= 0x00050078ULL;    /* set ARM926 control bits */
        env->cp15.sctlr_ns = sctlr;
        arm_rebuild_hflags(env);
    }
}

static BlockBackend *stmp3770_open_default_flash(const char *path)
{
    Error *local_err = NULL;
    QDict *options = qdict_new();
    BlockBackend *blk;

    qdict_put_str(options, "driver", "raw");
    blk = blk_new_open(path, NULL, options, BDRV_O_RDWR | BDRV_O_RESIZE,
                       &local_err);
    if (!blk) {
        warn_report("Could not open default flash '%s': %s",
                    path, error_get_pretty(local_err));
        error_free(local_err);
        return NULL;
    }

    return blk;
}

typedef enum {
    STMP3770_BOOT_RESULT_WAIT,
    STMP3770_BOOT_RESULT_LOADED,
    STMP3770_BOOT_RESULT_FAILED,
} STMP3770BootResult;

typedef enum {
    STMP3770_BOOT_MODE_USB,
    STMP3770_BOOT_MODE_I2C,
    STMP3770_BOOT_MODE_SPI1_FLASH,
    STMP3770_BOOT_MODE_SPI2_FLASH,
    STMP3770_BOOT_MODE_GPMI_ECC4,
    STMP3770_BOOT_MODE_JTAG_WAIT,
    STMP3770_BOOT_MODE_SPI2_EEPROM,
    STMP3770_BOOT_MODE_SSP1_MMC,
    STMP3770_BOOT_MODE_SSP2_MMC,
    STMP3770_BOOT_MODE_GPMI_ECC8,
    STMP3770_BOOT_MODE_RECOVERY,
    STMP3770_BOOT_MODE_UNKNOWN,
    STMP3770_BOOT_MODE_MAX,
} STMP3770BootMode;

typedef enum {
    STMP3770_BOOT_STATE_INIT,
    STMP3770_BOOT_STATE_SELECT,
    STMP3770_BOOT_STATE_PORT_INIT,
    STMP3770_BOOT_STATE_LOAD,
    STMP3770_BOOT_STATE_EXEC,
    STMP3770_BOOT_STATE_JTAG_WAIT,
    STMP3770_BOOT_STATE_FAILED,
} STMP3770BootState;

static const char * const stmp3770_boot_mode_name[STMP3770_BOOT_MODE_MAX] = {
    [STMP3770_BOOT_MODE_USB]         = "USB",
    [STMP3770_BOOT_MODE_I2C]         = "I2C",
    [STMP3770_BOOT_MODE_SPI1_FLASH]  = "SPI1 Flash",
    [STMP3770_BOOT_MODE_SPI2_FLASH]  = "SPI2 Flash",
    [STMP3770_BOOT_MODE_GPMI_ECC4]   = "GPMI ECC4",
    [STMP3770_BOOT_MODE_JTAG_WAIT]   = "JTAG_WAIT",
    [STMP3770_BOOT_MODE_SPI2_EEPROM] = "SPI2 EEPROM",
    [STMP3770_BOOT_MODE_SSP1_MMC]    = "SSP1 MMC",
    [STMP3770_BOOT_MODE_SSP2_MMC]    = "SSP2 MMC",
    [STMP3770_BOOT_MODE_GPMI_ECC8]   = "GPMI ECC8",
    [STMP3770_BOOT_MODE_RECOVERY]    = "Recovery",
    [STMP3770_BOOT_MODE_UNKNOWN]     = "Unknown",
};

static STMP3770BootMode stmp3770_boot_mode_from_bm(uint8_t bm)
{
    switch (bm) {
    case 0x0:
        return STMP3770_BOOT_MODE_USB;
    case 0x1:
        return STMP3770_BOOT_MODE_I2C;
    case 0x2:
        return STMP3770_BOOT_MODE_SPI1_FLASH;
    case 0x3:
        return STMP3770_BOOT_MODE_SPI2_FLASH;
    case 0x4:
        return STMP3770_BOOT_MODE_GPMI_ECC4;
    case 0x6:
        return STMP3770_BOOT_MODE_JTAG_WAIT;
    case 0x8:
        return STMP3770_BOOT_MODE_SPI2_EEPROM;
    case 0x9:
        return STMP3770_BOOT_MODE_SSP1_MMC;
    case 0xA:
        return STMP3770_BOOT_MODE_SSP2_MMC;
    case 0xC:
        return STMP3770_BOOT_MODE_GPMI_ECC8;
    default:
        return STMP3770_BOOT_MODE_UNKNOWN;
    }
}

static STMP3770BootMode stmp3770_boot_select(STMP3770BoardState *s)
{
    STMP3770State *soc = &s->soc;
    uint32_t ocotp_rom0 = soc->ocotp->rom[0];
    uint32_t rtc_persistent1 = soc->rtc->persistent[1];
    bool force_recovery = rtc_persistent1 & 0x1;
    bool disable_recovery = ocotp_rom0 & (1U << 2);
    uint8_t bm;

    if (force_recovery) {
        /* ROM consumes the recovery latch once it has read it. */
        soc->rtc->persistent[1] &= ~1U;
    }

    if (force_recovery && !disable_recovery) {
        return STMP3770_BOOT_MODE_RECOVERY;
    }

    if (s->boot_lcd_rs & 0x1) {
        bm = s->boot_lcd_data & 0xF;
    } else {
        bm = (ocotp_rom0 >> 24) & 0xF;
    }

    return stmp3770_boot_mode_from_bm(bm);
}

static STMP3770BootResult stmp3770_boot_load_usb(STMP3770BoardState *s,
                                                 hwaddr *entry)
{
    return STMP3770_BOOT_RESULT_FAILED;
}

static STMP3770BootResult stmp3770_boot_load_i2c(STMP3770BoardState *s,
                                                 hwaddr *entry)
{
    return STMP3770_BOOT_RESULT_FAILED;
}

static STMP3770BootResult stmp3770_boot_load_spi(STMP3770BoardState *s,
                                                 hwaddr *entry, int port)
{
    return STMP3770_BOOT_RESULT_FAILED;
}

static STMP3770BootResult stmp3770_boot_load_ssp(STMP3770BoardState *s,
                                                 hwaddr *entry, int port)
{
    return STMP3770_BOOT_RESULT_FAILED;
}

static STMP3770BootResult stmp3770_boot_load_gpmi(STMP3770BoardState *s,
                                                  hwaddr *entry, bool ecc8)
{
    return STMP3770_BOOT_RESULT_FAILED;
}

static STMP3770BootResult stmp3770_boot_load(STMP3770BoardState *s,
                                             hwaddr *entry)
{
    STMP3770BootMode mode = s->boot_mode;

    switch (mode) {
    case STMP3770_BOOT_MODE_USB:
    case STMP3770_BOOT_MODE_RECOVERY:
        return stmp3770_boot_load_usb(s, entry);
    case STMP3770_BOOT_MODE_I2C:
        return stmp3770_boot_load_i2c(s, entry);
    case STMP3770_BOOT_MODE_SPI1_FLASH:
        return stmp3770_boot_load_spi(s, entry, 1);
    case STMP3770_BOOT_MODE_SPI2_FLASH:
    case STMP3770_BOOT_MODE_SPI2_EEPROM:
        return stmp3770_boot_load_spi(s, entry, 2);
    case STMP3770_BOOT_MODE_SSP1_MMC:
        return stmp3770_boot_load_ssp(s, entry, 1);
    case STMP3770_BOOT_MODE_SSP2_MMC:
        return stmp3770_boot_load_ssp(s, entry, 2);
    case STMP3770_BOOT_MODE_GPMI_ECC4:
        return stmp3770_boot_load_gpmi(s, entry, false);
    case STMP3770_BOOT_MODE_GPMI_ECC8:
        return stmp3770_boot_load_gpmi(s, entry, true);
    case STMP3770_BOOT_MODE_JTAG_WAIT:
        info_report("STMP3770 boot: JTAG_WAIT mode, waiting for debugger");
        return STMP3770_BOOT_RESULT_WAIT;
    case STMP3770_BOOT_MODE_UNKNOWN:
    default:
        return STMP3770_BOOT_RESULT_FAILED;
    }
}

static STMP3770BootResult stmp3770_boot_run(STMP3770BoardState *s)
{
    hwaddr entry = STMP3770_SRAM_ADDR;

    s->boot_state = STMP3770_BOOT_STATE_INIT;
    s->boot_state = STMP3770_BOOT_STATE_SELECT;
    s->boot_mode = stmp3770_boot_select(s);
    info_report("STMP3770 boot: selected mode %s",
                stmp3770_boot_mode_name[s->boot_mode]);
    s->boot_state = STMP3770_BOOT_STATE_PORT_INIT;
    s->boot_state = STMP3770_BOOT_STATE_LOAD;

    STMP3770BootResult result = stmp3770_boot_load(s, &entry);
    if (result == STMP3770_BOOT_RESULT_LOADED) {
        s->boot_state = STMP3770_BOOT_STATE_EXEC;
        s->soc.cpu.env.regs[15] = entry;
    } else if (result == STMP3770_BOOT_RESULT_WAIT) {
        s->boot_state = STMP3770_BOOT_STATE_JTAG_WAIT;
    } else {
        s->boot_state = STMP3770_BOOT_STATE_FAILED;
    }

    return result;
}

static void stmp3770_board_get_boot_lcd_rs(Object *obj, Visitor *v,
                                           const char *name, void *opaque,
                                           Error **errp)
{
    STMP3770BoardState *s = STMP3770_BOARD(obj);
    uint8_t value = s->boot_lcd_rs;

    visit_type_uint8(v, name, &value, errp);
}

static void stmp3770_board_set_boot_lcd_rs(Object *obj, Visitor *v,
                                           const char *name, void *opaque,
                                           Error **errp)
{
    STMP3770BoardState *s = STMP3770_BOARD(obj);

    visit_type_uint8(v, name, &s->boot_lcd_rs, errp);
}

static void stmp3770_board_get_boot_lcd_data(Object *obj, Visitor *v,
                                             const char *name, void *opaque,
                                             Error **errp)
{
    STMP3770BoardState *s = STMP3770_BOARD(obj);
    uint8_t value = s->boot_lcd_data;

    visit_type_uint8(v, name, &value, errp);
}

static void stmp3770_board_set_boot_lcd_data(Object *obj, Visitor *v,
                                             const char *name, void *opaque,
                                             Error **errp)
{
    STMP3770BoardState *s = STMP3770_BOARD(obj);

    visit_type_uint8(v, name, &s->boot_lcd_data, errp);
}

static void stmp3770_board_init(MachineState *machine)
{
    STMP3770BoardState *s = STMP3770_BOARD(machine);
    MemoryRegion *sysmem = get_system_memory();
    Chardev *chr;
    g_autofree char *default_rom = stmp3770_default_file(STMP3770_DEFAULT_ROM_NAME);
    g_autofree char *default_flash =
        stmp3770_default_file(STMP3770_DEFAULT_FLASH_NAME);
    const char *firmware = machine->firmware;
    BlockBackend *default_flash_blk = NULL;

    /* Initialize the SoC */
    object_initialize_child(OBJECT(machine), "soc", &s->soc, TYPE_STMP3770);

    /* Connect debug UART to the first serial chardev if provided */
    chr = serial_hd(0);
    if (!chr) {
        /* In -nographic mode qemu_chr_new may not be wired to serial_hd(0) */
        chr = qemu_chr_new("stdio", "stdio", NULL);
    }
    if (chr) {
        qdev_prop_set_chr(DEVICE(s->soc.uartdbg), "chardev", chr);
    }

    /* Connect application UART to the second serial chardev if provided */
    chr = serial_hd(1);
    if (chr) {
        qdev_prop_set_chr(DEVICE(s->soc.uartapp), "chardev", chr);
    }

    /* Connect NAND drive to GPMI if one was provided with -drive if=none */
    {
        DriveInfo *dinfo = drive_get(IF_NONE, 0, 0);
        if (dinfo) {
            qdev_prop_set_drive(DEVICE(s->soc.gpmi), "drive",
                                blk_by_legacy_dinfo(dinfo));
        } else if (stmp3770_file_exists(default_flash)) {
            default_flash_blk = stmp3770_open_default_flash(default_flash);
            if (default_flash_blk) {
                qdev_prop_set_drive(DEVICE(s->soc.gpmi), "drive",
                                    default_flash_blk);
                fprintf(stderr, "stmp3770: using default flash %s\n",
                        default_flash);
            }
        }
    }

    if (!qdev_realize(DEVICE(&s->soc), NULL, &error_fatal)) {
        error_report("Failed to realize STMP3770 SoC");
        exit(1);
    }

    /*
     * Simulate Boot ROM initialization.
     *
     * The real STMP3770 mask ROM (SigmaTel OCROM, 64 KiB @ 0xFFFF0000)
     * performs a sequence of peripheral initialization before handing
     * control to the boot loader.  The sequence below replicates the
     * observable side effects extracted from the actual ROM dump:
     *
     *   - Release soft-reset/clock-gate on RTC, PINCTRL, POWER
     *   - Open/close OCOTP bank for shadow read
     *   - Configure Debug UART (115200 8N1 at 24 MHz XTAL)
     *   - Invalidate TLB and configure CP15 SCTLR (alignment check, etc.)
     *
     * This is registered as a reset handler so it runs AFTER all device
     * reset handlers, matching the real hardware where the ROM runs after
     * the reset deassertion sequence.
     */
    /* Schedule the ROM boot init to run after all device reset handlers
     * complete.  Using a bottom half ensures the init runs on the next
     * main loop iteration, which is after the reset phase.  This matches
     * real hardware where the mask ROM runs after reset deassertion. */
    aio_bh_schedule_oneshot(qemu_get_aio_context(),
                            stmp3770_rom_boot_init, &s->soc);

    /*
     * CPU starts at 24MHz XTAL (not PLL)
     *
     * Real hardware: CPU runs at 24MHz until firmware enables PLL and switches
     * to high-frequency domain. CLKCTRL reset values:
     *   - CLKCTRL_CLKSEQ.BYPASS_CPU = 1 (use 24MHz XTAL, not PLL/480MHz)
     *   - CLKCTRL_PLLCTRL0.POWER = 0 (PLL disabled)
     *   - CLKCTRL_FRAC.CLKGATECPU = 1 (CPU clock gated by default)
     *
     * ExistOS Clk::init() sequence:
     *   1. Enable PLL (PLLCTRL0.POWER=1)
     *   2. Ungate CPU clock (FRAC.CLKGATECPU=0)
     *   3. Set temporary dividers (CPU=5, HBUS=4 → 96MHz/24MHz)
     *   4. Switch to PLL domain (CLKSEQ.BYPASS_CPU=0)
     *   5. Set final dividers (CPU frac=22, HBUS=2 → 392.7MHz/240MHz)
     *
     * Note: CLKCTRL reset() should handle these values. This comment documents
     * expected Boot ROM / reset state for reference.
     */

    /*
     * Run the ROM boot state machine.  It consumes LCD_RS/LCD_DATA[5:0],
     * OCOTP_ROM0 and RTC_PERSISTENT1.FORCE_RECOVERY to select a boot port
     * and, when a port loader succeeds, sets the CPU entry point.
     */
    {
        STMP3770BootResult result = stmp3770_boot_run(s);
        if (result == STMP3770_BOOT_RESULT_LOADED) {
            return;
        }
        if (result == STMP3770_BOOT_RESULT_WAIT) {
            return;
        }
    }

    /*
     * 3. No external DRAM on HP 39gII
     *
     * HP 39gII has NO external DRAM - only 512KB on-chip SRAM.
     * STMP3770 SoC has a DRAM controller (EMI) but it's unused on this board.
     *
     * ExistOS virtual memory (VM_RAM_BASE @ 0x02000000, 3MB) is mapped to
     * NAND Flash via MMU, not to physical DRAM. The DRAM address range
     * (0x40000000+) is unmapped and will cause data abort if accessed.
     *
     * For generic firmware that expects DRAM, machine->ram can be optionally
     * provided and mapped to 0x40000000, but this doesn't match HP 39gII
     * hardware.
     */
    if (machine->ram_size > 0) {
        /* Optional external DRAM for non-HP39gII firmware */
        memory_region_add_subregion(sysmem, STMP3770_DRAM_ADDR, machine->ram);
    }

    if (!firmware && !machine->kernel_filename) {
        if (stmp3770_file_exists(default_rom)) {
            firmware = default_rom;
            fprintf(stderr, "stmp3770: using default ROM %s\n", default_rom);
        } else {
            warn_report("No kernel specified and default ROM '%s' was not found",
                        default_rom);
        }
    }

    /* Load kernel or firmware if provided */
    if (firmware) {
        /* Firmware (e.g. HP39GII Hypervisor rom.bin) is linked to run from SRAM */
        if (load_image_targphys(firmware,
                                STMP3770_SRAM_ADDR,
                                STMP3770_SRAM_SIZE) < 0) {
            error_report("Failed to load firmware '%s'", firmware);
            exit(1);
        }

        /* Set entry point */
        s->soc.cpu.env.regs[15] = STMP3770_SRAM_ADDR;
    } else if (machine->kernel_filename) {
        /* Load kernel to SRAM (HP 39gII has no DRAM) */
        if (load_image_targphys(machine->kernel_filename,
                                STMP3770_SRAM_ADDR,
                                STMP3770_SRAM_SIZE) < 0) {
            error_report("Failed to load kernel '%s'", machine->kernel_filename);
            exit(1);
        }

        /* Set entry point */
        s->soc.cpu.env.regs[15] = STMP3770_SRAM_ADDR;
    } else {
        /* No kernel - CPU will try to boot from ROM/SRAM */
        warn_report("No kernel specified, CPU will execute from 0x0 (SRAM)");
    }
}

static void stmp3770_board_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "STMP3770 Development Board (HP 39gII Calculator)";
    mc->init = stmp3770_board_init;
    mc->max_cpus = 1;
    mc->min_cpus = 1;
    mc->default_cpus = 1;
    mc->is_default = true;
    mc->default_ram_size = STMP3770_BOARD_RAM_DEFAULT;
    mc->default_ram_id = "stmp3770.dram";

    object_class_property_add(oc, "boot-lcd-rs", "uint8",
                              stmp3770_board_get_boot_lcd_rs,
                              stmp3770_board_set_boot_lcd_rs,
                              NULL, NULL);
    object_class_property_set_description(oc, "boot-lcd-rs",
        "LCD_RS boot strap pin (0 = use OCOTP, 1 = use LCD_DATA[5:0])");

    object_class_property_add(oc, "boot-lcd-data", "uint8",
                              stmp3770_board_get_boot_lcd_data,
                              stmp3770_board_set_boot_lcd_data,
                              NULL, NULL);
    object_class_property_set_description(oc, "boot-lcd-data",
        "LCD_DATA[5:0] boot strap vector when boot-lcd-rs is 1");
}

static const TypeInfo stmp3770_board_type = {
    .name = TYPE_STMP3770_BOARD,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(STMP3770BoardState),
    .class_init = stmp3770_board_class_init,
};

static void stmp3770_board_register_types(void)
{
    type_register_static(&stmp3770_board_type);
}

type_init(stmp3770_board_register_types)
