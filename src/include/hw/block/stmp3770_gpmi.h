/*
 * STMP3770 GPMI (General Purpose Memory Interface) NAND controller
 *
 * Based on STMP3770 Reference Manual Chapter 13
 *
 * Copyright (C) 2024
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef STMP3770_GPMI_H
#define STMP3770_GPMI_H

#include "hw/sysbus.h"
#include "hw/block/block.h"
#include "hw/dma/stmp3770_dma.h"

#define TYPE_STMP3770_GPMI "stmp3770-gpmi"
#define TYPE_STMP3770_BCH  "stmp3770-bch"

#define STMP3770_GPMI_NUM_CS 4

OBJECT_DECLARE_SIMPLE_TYPE(STMP3770GPMIState, STMP3770_GPMI)
OBJECT_DECLARE_SIMPLE_TYPE(STMP3770BCHState, STMP3770_BCH)

/* GPMI register offsets */
#define GPMI_CTRL0      0x00
#define GPMI_COMPARE    0x10
#define GPMI_ECCCTRL    0x20
#define GPMI_ECCCOUNT   0x30
#define GPMI_PAYLOAD    0x40
#define GPMI_AUXILIARY  0x50
#define GPMI_CTRL1      0x60
#define GPMI_TIMING0    0x70
#define GPMI_TIMING1    0x80
#define GPMI_TIMING2    0x90
#define GPMI_DATA       0xA0
#define GPMI_STAT       0xB0
#define GPMI_DEBUG      0xC0
#define GPMI_VERSION    0xD0

/* CTRL0 bits */
#define GPMI_CTRL0_SFTRST         (1U << 31)
#define GPMI_CTRL0_CLKGATE        (1U << 30)
#define GPMI_CTRL0_RUN            (1U << 29)
#define GPMI_CTRL0_DEV_IRQ_EN     (1U << 28)
#define GPMI_CTRL0_TIME_IRQ_EN    (1U << 27)
#define GPMI_CTRL0_COMMAND_MODE_SHIFT 24
#define GPMI_CTRL0_COMMAND_MODE_MASK  0x3
#define GPMI_CTRL0_WORD_LENGTH    (1U << 23)
#define GPMI_CTRL0_LOCK_CS        (1U << 22)
#define GPMI_CTRL0_CS_SHIFT       20
#define GPMI_CTRL0_CS_MASK        0x3
#define GPMI_CTRL0_ADDRESS_SHIFT  17
#define GPMI_CTRL0_ADDRESS_MASK   0x7
#define GPMI_CTRL0_ADDRESS_INCREMENT (1U << 16)
#define GPMI_CTRL0_XFER_COUNT_MASK 0xFFFF

/* COMMAND_MODE values */
#define GPMI_COMMAND_MODE_WRITE             0
#define GPMI_COMMAND_MODE_READ              1
#define GPMI_COMMAND_MODE_READ_AND_COMPARE  2
#define GPMI_COMMAND_MODE_WAIT_FOR_READY    3

/* ADDRESS values */
#define GPMI_ADDRESS_DATA 0
#define GPMI_ADDRESS_CLE  1
#define GPMI_ADDRESS_ALE  2

/* CTRL1 bits */
#define GPMI_CTRL1_GPMI_MODE          (1U << 0)
#define GPMI_CTRL1_CAMERA_MODE        (1U << 1)
#define GPMI_CTRL1_ATA_IRQRDY_POLARITY (1U << 2)
#define GPMI_CTRL1_DEV_RESET          (1U << 3)
#define GPMI_CTRL1_ABORT_WAIT_FOR_READY_MASK (0xFU << 4)
#define GPMI_CTRL1_BURST_EN           (1U << 8)
#define GPMI_CTRL1_TIMEOUT_IRQ        (1U << 9)
#define GPMI_CTRL1_DEV_IRQ            (1U << 10)
#define GPMI_CTRL1_DMA2ECC_MODE       (1U << 11)
#define GPMI_CTRL1_DSAMPLE_TIME_MASK  (0x7U << 12)
#define GPMI_CTRL1_IRQ_MASK           (GPMI_CTRL1_TIMEOUT_IRQ | GPMI_CTRL1_DEV_IRQ)
#define GPMI_CTRL1_CONFIG_MASK        0x000079FFU

/* STAT bits */
#define GPMI_STAT_PRESENT          (1U << 31)
#define GPMI_STAT_RDY_TIMEOUT_SHIFT 8
#define GPMI_STAT_RDY_TIMEOUT_MASK  0xF
#define GPMI_STAT_ATA_IRQ          (1U << 7)
#define GPMI_STAT_INVALID_BUFFER_MASK (1U << 6)
#define GPMI_STAT_FIFO_EMPTY       (1U << 5)
#define GPMI_STAT_FIFO_FULL        (1U << 4)
#define GPMI_STAT_DEV_ERROR_MASK   0xF

/* ECCCTRL bits */
#define GPMI_ECCCTRL_ECC_CMD_SHIFT 13
#define GPMI_ECCCTRL_ECC_CMD_MASK  0x3
#define GPMI_ECCCTRL_ENABLE_ECC    (1U << 12)
#define GPMI_ECCCTRL_BUFFER_MASK_SHIFT 0
#define GPMI_ECCCTRL_BUFFER_MASK_MASK  0x1FF

/* ECC_CMD values */
#define GPMI_ECC_CMD_DECODE_4_BIT  0
#define GPMI_ECC_CMD_DECODE_8_BIT  2
#define GPMI_ECC_CMD_ENCODE_8_BIT  3

/* BCH register offsets */
#define BCH_CTRL        0x00
#define BCH_STATUS0     0x10
#define BCH_STATUS1     0x20

/* BCH CTRL bits */
#define BCH_CTRL_SFTRST         (1U << 31)
#define BCH_CTRL_CLKGATE        (1U << 30)
#define BCH_CTRL_AHBM_SFTRST    (1U << 29)
#define BCH_CTRL_COMPLETE_IRQ   (1U << 0)
#define BCH_CTRL_COMPLETE_IRQ_EN (1U << 8)

/* BCH STATUS0 bits */
#define BCH_STATUS0_ALLONES       (1U << 4)
#define BCH_STATUS0_CORRECTED     (1U << 3)
#define BCH_STATUS0_UNCORRECTABLE (1U << 2)
#define BCH_STATUS0_COMPLETED_CE  0x3
#define BCH_STATUS0_STATUS_AUX_SHIFT 8
#define BCH_STATUS0_STATUS_AUX_MASK  0xF
#define BCH_STATUS0_HANDLE_SHIFT     16
#define BCH_STATUS0_HANDLE_MASK      0xFFFF

/* NAND command bytes */
#define NAND_CMD_READ_1ST   0x00
#define NAND_CMD_READ_2ND   0x30
#define NAND_CMD_READ_ID    0x90
#define NAND_CMD_RESET      0xFF
#define NAND_CMD_STATUS     0x70
#define NAND_CMD_PROGRAM_1ST 0x80
#define NAND_CMD_PROGRAM_2ND 0x10
#define NAND_CMD_ERASE_1ST  0x60
#define NAND_CMD_ERASE_2ND  0xD0

/* Internal NAND state */
typedef enum {
    NAND_STATE_IDLE,
    NAND_STATE_CMD,
    NAND_STATE_ADDR,
    NAND_STATE_DATA_IN,
    NAND_STATE_DATA_OUT,
    NAND_STATE_WAIT_READY,
} STMP3770NandState;

typedef enum {
    GPMI_STORAGE_LAYOUT_APPENDED_OOB = 0,
    GPMI_STORAGE_LAYOUT_INTERLEAVED_OOB = 1,
} STMP3770GPMIStorageLayout;

struct STMP3770GPMIState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

    BlockBackend *blk;
    uint8_t *storage;
    uint64_t storage_size;
    uint64_t storage_file_size;
    STMP3770GPMIStorageLayout storage_layout;

    /* Configuration */
    uint32_t page_size;
    uint32_t pages_per_block;
    uint32_t num_blocks;
    uint32_t oob_size;
    uint32_t block_size;
    uint32_t total_size;

    /* GPMI registers */
    uint32_t ctrl0;
    uint32_t compare;
    uint32_t eccctrl;
    uint32_t ecccount;
    uint32_t payload;
    uint32_t auxiliary;
    uint32_t ctrl1;
    uint32_t timing0;
    uint32_t timing1;
    uint32_t timing2;
    uint32_t stat;
    uint32_t debug;

    /* FIFO for PIO data transfers */
    uint32_t fifo[16];
    uint32_t fifo_count;

    /* Tracks whether the command byte in a CLE+ADDRESS_INCREMENT write has
     * already been sent; reset at the start of each DMA transfer. */
    bool write_cmd_sent;

    /* NAND state machine */
    STMP3770NandState nand_state;
    uint8_t  nand_cmd;
    uint32_t nand_addr;
    uint32_t nand_addr_cycles;
    uint32_t nand_column;
    uint32_t nand_page;
    uint32_t nand_block;
    uint32_t nand_row;
    uint32_t data_count;
    uint32_t data_ptr;
    bool     data_dir_write;

    /* Page buffer */
    uint8_t *page_buf;
    uint32_t page_buf_size;

    /* DMA handler registration */
    STMP3770DMAState *dma;
    int dma_channel_base;

    /* BCH back-pointer for ECC completion IRQ/status */
    STMP3770BCHState *bch;

    /* Progress counters for NAND operations */
    uint64_t page_read_cnt;
    uint64_t page_write_cnt;
    uint64_t block_erase_cnt;
    uint64_t ecc_complete_cnt;
};

struct STMP3770BCHState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t ctrl;
    uint32_t status0;
    uint32_t status1;
};

/* GPMI API for SoC integration */
void stmp3770_gpmi_set_dma(STMP3770GPMIState *s, STMP3770DMAState *dma,
                           int channel_base);
void stmp3770_gpmi_set_bch(STMP3770GPMIState *s, STMP3770BCHState *bch);

#endif /* STMP3770_GPMI_H */
