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

/* HP 39gII hardware: 512KB SRAM only, no external DRAM */
#define STMP3770_BOARD_RAM_DEFAULT  (0)
#define STMP3770_DEFAULT_ROM_NAME   "rom.bin"
#define STMP3770_DEFAULT_FLASH_NAME "flash.bin"

#define TYPE_STMP3770_BOARD MACHINE_TYPE_NAME("stmp3770")
OBJECT_DECLARE_SIMPLE_TYPE(STMP3770BoardState, STMP3770_BOARD)

struct STMP3770BoardState {
    MachineState parent_obj;
    STMP3770State soc;
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
     * Real STMP3770 Boot ROM configures basic peripherals before firmware runs.
     * Based on ExistOS-For-HP39GII BSP analysis.
     */

    /* 1. Pre-configure Debug UART
     *
     * ExistOS Uart::init() is a no-op because Boot ROM already configured UART.
     * UARTDBG Control Register (UARTDBGCR) @ offset 0x30:
     *   Bit 0: UARTEN (UART enable)
     *   Bit 8: TXE (transmit enable)
     *   Bit 9: RXE (receive enable)
     */
    {
        uint32_t uartcr_val = (1 << 0) | (1 << 8) | (1 << 9);
        MemoryRegion *uart_mr = sysbus_mmio_get_region(
            SYS_BUS_DEVICE(s->soc.uartdbg), 0);
        memory_region_dispatch_write(uart_mr, 0x30, uartcr_val,
                                      MO_32, MEMTXATTRS_UNSPECIFIED);
    }

    /*
     * 2. CPU starts at 24MHz XTAL (not PLL)
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
