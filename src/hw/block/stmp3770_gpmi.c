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

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/block/block.h"
#include "hw/block/stmp3770_gpmi.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/block-backend.h"
#include "qapi/error.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "trace.h"

#define GPMI_VERSION_VALUE  0x02000000

/* Default NAND geometry (2K page + 64B OOB, 64 pages/block, 1024 blocks) */
#define DEFAULT_PAGE_SIZE       2048
#define DEFAULT_PAGES_PER_BLOCK 64
#define DEFAULT_NUM_BLOCKS      1024
#define DEFAULT_OOB_SIZE        64
#define DEFAULT_PAGE_BUF_SIZE   (DEFAULT_PAGE_SIZE + DEFAULT_OOB_SIZE)

static const uint8_t gpmi_appended_oob_magic[16] = "STMP3770GPMIOOB1";

static inline bool gpmi_enabled(STMP3770GPMIState *s)
{
    return (s->ctrl0 & (GPMI_CTRL0_SFTRST | GPMI_CTRL0_CLKGATE)) == 0;
}

static void gpmi_apply_sct(uint32_t *reg, uint32_t value, int sct)
{
    switch (sct) {
    case 0:
        *reg = value;
        break;
    case 1:
        *reg |= value;
        break;
    case 2:
        *reg &= ~value;
        break;
    case 3:
        *reg ^= value;
        break;
    }
}

static void bch_apply_sct(uint32_t *reg, uint32_t value, int sct)
{
    switch (sct) {
    case 0:
        *reg = value;
        break;
    case 1:
        *reg |= value;
        break;
    case 2:
        *reg &= ~value;
        break;
    case 3:
        *reg ^= value;
        break;
    }
}

static void gpmi_update_irq(STMP3770GPMIState *s)
{
    bool pending = false;

    /* Only DEV_IRQ and TIMEOUT_IRQ are routed to the GPMI top-level IRQ */
    if ((s->ctrl0 & GPMI_CTRL0_DEV_IRQ_EN) && (s->ctrl1 & GPMI_CTRL1_DEV_IRQ)) {
        pending = true;
    }
    if ((s->ctrl0 & GPMI_CTRL0_TIME_IRQ_EN) && (s->ctrl1 & GPMI_CTRL1_TIMEOUT_IRQ)) {
        pending = true;
    }

    qemu_set_irq(s->irq, pending);
}

static void bch_update_irq(STMP3770BCHState *s)
{
    bool pending = (s->ctrl & BCH_CTRL_COMPLETE_IRQ) &&
                   (s->ctrl & BCH_CTRL_COMPLETE_IRQ_EN);
    qemu_set_irq(s->irq, pending);
}

/*
 * Progress callback: log NAND operation progress to stderr so the user can
 * monitor FTL initialisation (dhara journal scan) without modifying firmware.
 */
static void gpmi_log_progress(STMP3770GPMIState *s, const char *op,
                              uint32_t row)
{
    uint64_t cnt = s->page_read_cnt + s->page_write_cnt + s->block_erase_cnt;

    /*
     * Log the first few operations and every 256th thereafter.
     * The first two operations report the total page count so the user can
     * estimate how long a full scan will take.
     */
    if (cnt <= 64 || (cnt & 255) == 0) {
        fprintf(stderr,
                "emu-gpmi: [%7" PRIu64 " rd / %7" PRIu64 " wr / %5" PRIu64 " er] "
                "%s row=%u\n",
                s->page_read_cnt, s->page_write_cnt, s->block_erase_cnt,
                op, row);
    }
}

/*
 * Simulate an ECC8 read completion: copy the page data and metadata into the
 * guest buffers pointed to by GPMI_PAYLOAD/GPMI_AUXILIARY, write the aux
 * status byte, and raise the BCH completion interrupt.
 */
static void gpmi_bch_complete_read(STMP3770GPMIState *s)
{
    STMP3770BCHState *bch = s->bch;

    if (!bch) {
        return;
    }

    if (s->payload) {
        address_space_write(&address_space_memory, s->payload,
                            MEMTXATTRS_UNSPECIFIED,
                            s->page_buf, s->page_size);
    }
    if (s->auxiliary) {
        uint8_t aux_meta[19];
        memset(aux_meta, 0xFF, sizeof(aux_meta));
        if (s->page_buf_size > s->page_size) {
            memcpy(aux_meta, s->page_buf + s->page_size,
                   MIN(sizeof(aux_meta), s->page_buf_size - s->page_size));
        }
        address_space_write(&address_space_memory, s->auxiliary,
                            MEMTXATTRS_UNSPECIFIED,
                            aux_meta, sizeof(aux_meta));

        /* Status byte at offset 16: 0x00 means no error. */
        {
            uint8_t status = 0x00;
            address_space_write(&address_space_memory, s->auxiliary + 16,
                                MEMTXATTRS_UNSPECIFIED,
                                &status, 1);
        }
    }

    /* Report successful completion */
    bch->status0 = BCH_STATUS0_COMPLETED_CE;
    bch->status1 = 0x00000000;   /* all payloads: zero corrections */
    bch->ctrl |= BCH_CTRL_COMPLETE_IRQ;
    bch_update_irq(bch);

    s->ecc_complete_cnt++;
    s->page_read_cnt++;
    gpmi_log_progress(s, "ECC-read", s->nand_row);
}

static void gpmi_nand_reset_state(STMP3770GPMIState *s)
{
    s->nand_state = NAND_STATE_IDLE;
    s->nand_cmd = 0;
    s->nand_addr = 0;
    s->nand_addr_cycles = 0;
    s->nand_column = 0;
    s->nand_page = 0;
    s->nand_block = 0;
    s->nand_row = 0;
    s->data_count = 0;
    s->data_ptr = 0;
    s->data_dir_write = false;
    s->write_cmd_sent = false;
}

static void gpmi_update_rdy_timeout(STMP3770GPMIState *s)
{
    /* For MVP, all chip selects are always ready */
    s->stat &= ~(GPMI_STAT_RDY_TIMEOUT_MASK << GPMI_STAT_RDY_TIMEOUT_SHIFT);
}

static void gpmi_decode_address(STMP3770GPMIState *s)
{
    s->nand_column = s->nand_addr & 0xFFFF;
    s->nand_row = (s->nand_addr >> 16) & 0xFFFF;
    s->nand_page = s->nand_row % s->pages_per_block;
    s->nand_block = s->nand_row / s->pages_per_block;
}

static uint64_t gpmi_total_pages(STMP3770GPMIState *s)
{
    return (uint64_t)s->pages_per_block * s->num_blocks;
}

static uint64_t gpmi_data_area_size(STMP3770GPMIState *s)
{
    return gpmi_total_pages(s) * s->page_size;
}

static uint64_t gpmi_oob_area_size(STMP3770GPMIState *s)
{
    return gpmi_total_pages(s) * s->oob_size;
}

static uint64_t gpmi_interleaved_area_size(STMP3770GPMIState *s)
{
    return gpmi_total_pages(s) * s->page_buf_size;
}

static uint64_t gpmi_appended_oob_offset(STMP3770GPMIState *s)
{
    return gpmi_data_area_size(s);
}

static uint64_t gpmi_appended_image_size(STMP3770GPMIState *s)
{
    return gpmi_data_area_size(s) + gpmi_oob_area_size(s);
}

static bool gpmi_nand_page_offsets(STMP3770GPMIState *s, uint64_t *data_offset,
                                   uint64_t *oob_offset)
{
    if (s->nand_row >= gpmi_total_pages(s)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-gpmi: page %u out of range\n", s->nand_row);
        return false;
    }

    if (s->storage_layout == GPMI_STORAGE_LAYOUT_INTERLEAVED_OOB) {
        *data_offset = (uint64_t)s->nand_row * s->page_buf_size;
        *oob_offset = *data_offset + s->page_size;
    } else {
        *data_offset = (uint64_t)s->nand_row * s->page_size;
        *oob_offset = gpmi_appended_oob_offset(s) +
                      (uint64_t)s->nand_row * s->oob_size;
    }

    if (*data_offset + s->page_size > s->storage_size ||
        *oob_offset + s->oob_size > s->storage_size) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-gpmi: page %u outside backing storage\n",
                      s->nand_row);
        return false;
    }

    return true;
}

static bool gpmi_nand_load_page(STMP3770GPMIState *s)
{
    uint64_t data_offset;
    uint64_t oob_offset;

    if (!s->storage) {
        return false;
    }

    if (!gpmi_nand_page_offsets(s, &data_offset, &oob_offset)) {
        return false;
    }

    memcpy(s->page_buf, s->storage + data_offset, s->page_size);
    memcpy(s->page_buf + s->page_size, s->storage + oob_offset, s->oob_size);
    return true;
}

static bool gpmi_nand_store_page(STMP3770GPMIState *s)
{
    uint64_t data_offset;
    uint64_t oob_offset;

    if (!s->storage) {
        return false;
    }

    if (!gpmi_nand_page_offsets(s, &data_offset, &oob_offset)) {
        return false;
    }

    memcpy(s->storage + data_offset, s->page_buf, s->page_size);
    memcpy(s->storage + oob_offset, s->page_buf + s->page_size, s->oob_size);

    if (s->blk) {
        int ret = blk_pwrite(s->blk, data_offset, s->page_size, s->page_buf, 0);
        if (ret < 0) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "stmp3770-gpmi: failed to write page %u\n",
                          s->nand_row);
            return false;
        }
        ret = blk_pwrite(s->blk, oob_offset, s->oob_size,
                         s->page_buf + s->page_size, 0);
        if (ret < 0) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "stmp3770-gpmi: failed to write OOB %u\n",
                          s->nand_row);
            return false;
        }
    }

    s->page_write_cnt++;
    gpmi_log_progress(s, "write", s->nand_row);
    return true;
}

static bool gpmi_nand_erase_block(STMP3770GPMIState *s)
{
    uint64_t data_offset;
    uint64_t data_size;
    uint64_t oob_offset;
    uint64_t erase_oob_size;

    if (!s->storage) {
        return false;
    }
    if (s->nand_block >= s->num_blocks) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-gpmi: block %u out of range\n",
                      s->nand_block);
        return false;
    }

    if (s->storage_layout == GPMI_STORAGE_LAYOUT_INTERLEAVED_OOB) {
        data_offset = (uint64_t)s->nand_block * s->pages_per_block *
                      s->page_buf_size;
        data_size = (uint64_t)s->pages_per_block * s->page_buf_size;
        oob_offset = 0;
        erase_oob_size = 0;
    } else {
        data_offset = (uint64_t)s->nand_block * s->pages_per_block *
                      s->page_size;
        data_size = (uint64_t)s->pages_per_block * s->page_size;
        oob_offset = gpmi_appended_oob_offset(s) +
                     (uint64_t)s->nand_block * s->pages_per_block *
                     s->oob_size;
        erase_oob_size = (uint64_t)s->pages_per_block * s->oob_size;
    }

    if (data_offset + data_size > s->storage_size ||
        (erase_oob_size &&
         oob_offset + erase_oob_size > s->storage_size)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-gpmi: erase block %u outside backing storage\n",
                      s->nand_block);
        return false;
    }

    memset(s->storage + data_offset, 0xFF, data_size);
    if (erase_oob_size) {
        memset(s->storage + oob_offset, 0xFF, erase_oob_size);
    }

    if (s->blk) {
        int ret = blk_pwrite(s->blk, data_offset, data_size,
                             s->storage + data_offset, 0);
        if (ret < 0) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "stmp3770-gpmi: failed to erase block %u\n",
                          s->nand_block);
            return false;
        }
        if (erase_oob_size) {
            ret = blk_pwrite(s->blk, oob_offset, erase_oob_size,
                             s->storage + oob_offset, 0);
            if (ret < 0) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "stmp3770-gpmi: failed to erase OOB block %u\n",
                              s->nand_block);
                return false;
            }
        }
    }

    return true;
}

static uint64_t gpmi_appended_magic_offset(STMP3770GPMIState *s)
{
    return gpmi_appended_image_size(s);
}

static bool gpmi_has_appended_oob_magic(STMP3770GPMIState *s,
                                        uint64_t file_size)
{
    uint8_t magic[sizeof(gpmi_appended_oob_magic)];
    uint64_t offset = gpmi_appended_magic_offset(s);

    if (!s->blk || file_size < offset + sizeof(magic)) {
        return false;
    }

    if (blk_pread(s->blk, offset, sizeof(magic), magic, 0) < 0) {
        return false;
    }

    return memcmp(magic, gpmi_appended_oob_magic, sizeof(magic)) == 0;
}

static void gpmi_persist_appended_oob_layout(STMP3770GPMIState *s)
{
    uint64_t oob_offset = gpmi_appended_oob_offset(s);
    uint64_t oob_size = gpmi_oob_area_size(s);
    uint64_t magic_offset = gpmi_appended_magic_offset(s);
    Error *local_err = NULL;
    int ret;

    if (!s->blk ||
        s->storage_layout != GPMI_STORAGE_LAYOUT_APPENDED_OOB) {
        return;
    }

    ret = blk_truncate(s->blk,
                       magic_offset + sizeof(gpmi_appended_oob_magic),
                       true, PREALLOC_MODE_OFF, 0, &local_err);
    if (ret < 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-gpmi: failed to resize NAND image: %s\n",
                      local_err ? error_get_pretty(local_err) : "unknown");
        if (local_err) {
            error_free(local_err);
        }
        return;
    }

    ret = blk_pwrite(s->blk, oob_offset, oob_size,
                     s->storage + oob_offset, 0);
    if (ret < 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-gpmi: failed to initialise appended OOB\n");
        return;
    }

    ret = blk_pwrite(s->blk, magic_offset, sizeof(gpmi_appended_oob_magic),
                     gpmi_appended_oob_magic, 0);
    if (ret < 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-gpmi: failed to write appended OOB marker\n");
        return;
    }

    s->storage_file_size = magic_offset + sizeof(gpmi_appended_oob_magic);
    blk_flush(s->blk);
}

static void gpmi_nand_command(STMP3770GPMIState *s, uint8_t cmd)
{
    s->nand_cmd = cmd;
    s->nand_addr = 0;
    s->nand_addr_cycles = 0;

    switch (cmd) {
    case NAND_CMD_RESET:
        s->nand_state = NAND_STATE_WAIT_READY;
        break;

    case NAND_CMD_READ_ID:
        s->nand_state = NAND_STATE_ADDR;
        break;

    case NAND_CMD_STATUS:
        s->nand_state = NAND_STATE_WAIT_READY;
        break;

    case NAND_CMD_READ_1ST:
    case NAND_CMD_PROGRAM_1ST:
    case NAND_CMD_ERASE_1ST:
        s->nand_state = NAND_STATE_ADDR;
        break;

    default:
        qemu_log_mask(LOG_UNIMP, "stmp3770-gpmi: unimplemented NAND command 0x%02x\n", cmd);
        s->nand_state = NAND_STATE_IDLE;
        break;
    }
}

static void gpmi_nand_address(STMP3770GPMIState *s, uint8_t addr)
{
    if (s->nand_addr_cycles < 4) {
        s->nand_addr |= (uint32_t)addr << (s->nand_addr_cycles * 8);
    }
    s->nand_addr_cycles++;

    switch (s->nand_cmd) {
    case NAND_CMD_READ_ID:
        /* Address 0x00 is sent once, then switch to data input */
        s->nand_state = NAND_STATE_DATA_OUT;
        s->data_count = 6;  /* 6 bytes ID (firmware reads byte 4 for geometry) */
        s->data_ptr = 0;
        break;

    case NAND_CMD_READ_1ST:
        /*
         * This board has 64K pages total, so two row cycles are sufficient.
         * Some firmware still sends a fifth zero row byte; keep accepting it.
         */
        if (s->nand_addr_cycles >= 4) {
            gpmi_decode_address(s);
            s->nand_state = NAND_STATE_CMD;  /* waiting for READ_2ND */
        }
        break;

    case NAND_CMD_PROGRAM_1ST:
        /* 2 column + 2 row cycles are enough for the modeled NAND geometry. */
        if (s->nand_addr_cycles >= 4) {
            gpmi_decode_address(s);
            s->nand_state = NAND_STATE_DATA_IN;
            s->data_count = s->page_size;
            s->data_ptr = 0;
            s->data_dir_write = true;
            memset(s->page_buf, 0xFF, s->page_buf_size);
        }
        break;

    case NAND_CMD_ERASE_1ST:
        /* Expect 3 row address cycles */
        if (s->nand_addr_cycles >= 3) {
            s->nand_row = s->nand_addr & 0xFFFFFF;
            s->nand_block = s->nand_row / s->pages_per_block;
            s->nand_page = s->nand_row % s->pages_per_block;
            s->nand_state = NAND_STATE_CMD;  /* waiting for ERASE_2ND */
        }
        break;

    default:
        break;
    }
}

static uint8_t gpmi_nand_read_data_byte(STMP3770GPMIState *s)
{
    uint8_t val = 0xFF;

    switch (s->nand_cmd) {
    case NAND_CMD_READ_ID:
        {
            /* Fake ID: Manufacturer=0xEC (Samsung), Device=0xDC, etc. */
            static const uint8_t nand_id[5] = { 0xEC, 0xDC, 0x10, 0x95, 0x54 };
            if (s->data_ptr < sizeof(nand_id)) {
                val = nand_id[s->data_ptr];
            }
            s->data_ptr++;
            if (s->data_ptr >= s->data_count) {
                s->nand_state = NAND_STATE_IDLE;
            }
        }
        break;

    case NAND_CMD_STATUS:
        /* Ready, no errors */
        val = 0xE0;
        s->nand_state = NAND_STATE_IDLE;
        break;

    case NAND_CMD_READ_1ST:
        if (s->data_ptr < s->page_size) {
            val = s->page_buf[s->data_ptr];
        }
        s->data_ptr++;
        if (s->data_ptr >= s->data_count) {
            s->nand_state = NAND_STATE_IDLE;
        }
        break;

    default:
        break;
    }

    return val;
}

static void gpmi_nand_write_data_byte(STMP3770GPMIState *s, uint8_t val)
{
    switch (s->nand_cmd) {
    case NAND_CMD_PROGRAM_1ST:
        if (s->data_ptr < s->page_buf_size) {
            s->page_buf[s->data_ptr] = val;
        }
        s->data_ptr++;
        if (s->data_ptr >= s->data_count) {
            s->nand_state = NAND_STATE_CMD;
        }
        break;

    default:
        break;
    }
}

static void gpmi_nand_second_command(STMP3770GPMIState *s, uint8_t cmd)
{
    switch (s->nand_cmd) {
    case NAND_CMD_READ_1ST:
        if (cmd == NAND_CMD_READ_2ND) {
            gpmi_decode_address(s);
            if (gpmi_nand_load_page(s)) {
                s->nand_state = NAND_STATE_WAIT_READY;
                s->data_count = s->page_size;
                s->data_ptr = 0;
            } else {
                s->nand_state = NAND_STATE_IDLE;
            }
        }
        break;

    case NAND_CMD_PROGRAM_1ST:
        if (cmd == NAND_CMD_PROGRAM_2ND) {
            gpmi_nand_store_page(s);
            s->nand_state = NAND_STATE_WAIT_READY;
        }
        break;

    case NAND_CMD_ERASE_1ST:
        if (cmd == NAND_CMD_ERASE_2ND) {
            gpmi_nand_erase_block(s);
            s->nand_state = NAND_STATE_WAIT_READY;
            s->block_erase_cnt++;
            gpmi_log_progress(s, "erase", s->nand_block * s->pages_per_block);
        }
        break;

    default:
        break;
    }
}

static void gpmi_handle_write_byte(STMP3770GPMIState *s, uint8_t byte)
{
    unsigned int address = (s->ctrl0 >> GPMI_CTRL0_ADDRESS_SHIFT) &
                           GPMI_CTRL0_ADDRESS_MASK;
    bool address_increment = (s->ctrl0 & GPMI_CTRL0_ADDRESS_INCREMENT) != 0;

    switch (address) {
    case GPMI_ADDRESS_CLE:
        if (!s->write_cmd_sent) {
            /* First byte on CLE is the command. */
            if (s->nand_state == NAND_STATE_CMD &&
                (s->nand_cmd == NAND_CMD_READ_1ST ||
                 s->nand_cmd == NAND_CMD_PROGRAM_1ST ||
                 s->nand_cmd == NAND_CMD_ERASE_1ST)) {
                gpmi_nand_second_command(s, byte);
            } else {
                gpmi_nand_command(s, byte);
            }
            s->write_cmd_sent = true;
        } else if (address_increment) {
            /* Subsequent bytes are address cycles when ADDRESS_INCREMENT is set. */
            gpmi_nand_address(s, byte);
        }
        break;

    case GPMI_ADDRESS_ALE:
        gpmi_nand_address(s, byte);
        break;

    case GPMI_ADDRESS_DATA:
        gpmi_nand_write_data_byte(s, byte);
        break;

    default:
        break;
    }
}

static void gpmi_execute_command(STMP3770GPMIState *s)
{
    unsigned int mode = (s->ctrl0 >> GPMI_CTRL0_COMMAND_MODE_SHIFT) &
                        GPMI_CTRL0_COMMAND_MODE_MASK;
    unsigned int address = (s->ctrl0 >> GPMI_CTRL0_ADDRESS_SHIFT) &
                           GPMI_CTRL0_ADDRESS_MASK;
    unsigned int cs = (s->ctrl0 >> GPMI_CTRL0_CS_SHIFT) & GPMI_CTRL0_CS_MASK;
    uint32_t count = s->ctrl0 & GPMI_CTRL0_XFER_COUNT_MASK;

    if (!gpmi_enabled(s)) {
        return;
    }

    /* Clear RUN bit after command execution */
    s->ctrl0 &= ~GPMI_CTRL0_RUN;

    switch (mode) {
    case GPMI_COMMAND_MODE_WRITE:
        if (address == GPMI_ADDRESS_CLE) {
            /* Command byte(s).  When the data is supplied by DMA, the FIFO is
             * empty here and the bytes are routed through gpmi_handle_write_byte()
             * as they arrive.  Only process PIO-supplied bytes immediately. */
            if (s->fifo_count > 0) {
                if (s->nand_state == NAND_STATE_CMD &&
                    (s->nand_cmd == NAND_CMD_READ_1ST ||
                     s->nand_cmd == NAND_CMD_PROGRAM_1ST ||
                     s->nand_cmd == NAND_CMD_ERASE_1ST)) {
                    /* Second cycle command */
                    gpmi_nand_second_command(s, s->fifo[0] & 0xFF);
                } else {
                    gpmi_nand_command(s, s->fifo[0] & 0xFF);
                }
            }
        } else if (address == GPMI_ADDRESS_ALE) {
            if (s->fifo_count > 0) {
                gpmi_nand_address(s, s->fifo[0] & 0xFF);
            }
        } else if (address == GPMI_ADDRESS_DATA) {
            /* PIO data write - used mainly for programming */
            uint32_t i;
            for (i = 0; i < count && i < s->fifo_count; i++) {
                gpmi_nand_write_data_byte(s, s->fifo[i] & 0xFF);
            }
        }
        break;

    case GPMI_COMMAND_MODE_READ:
        if (address == GPMI_ADDRESS_DATA) {
            if (s->eccctrl & GPMI_ECCCTRL_ENABLE_ECC) {
                /* ECC read: data is routed through the BCH engine directly
                 * into guest memory. */
                gpmi_nand_load_page(s);
                gpmi_bch_complete_read(s);
            } else if (count <= ARRAY_SIZE(s->fifo)) {
                /* Small PIO read: fill the FIFO so software can pop bytes. */
                uint32_t i;
                s->fifo_count = count;
                for (i = 0; i < count; i++) {
                    s->fifo[i] = gpmi_nand_read_data_byte(s);
                }
            } else {
                /* Large DMA read: leave data in the page buffer; the DMA
                 * handler will copy it out directly. */
                s->data_count = count;
                s->data_ptr = 0;
            }
        }
        break;

    case GPMI_COMMAND_MODE_WAIT_FOR_READY:
        /* MVP: NAND is always ready immediately */
        s->nand_state = NAND_STATE_IDLE;
        break;

    case GPMI_COMMAND_MODE_READ_AND_COMPARE:
        qemu_log_mask(LOG_UNIMP,
                      "stmp3770-gpmi: READ_AND_COMPARE not implemented\n");
        break;

    default:
        break;
    }

    s->debug = (cs << 28) | 0x10;  /* ready sense for selected CS */
    gpmi_update_rdy_timeout(s);

    /* Trigger DMA completion interrupt if requested */
    if (s->dma) {
        /* The APBH DMA engine will handle its own IRQ based on descriptor */
    }
}

static uint64_t gpmi_read(void *opaque, hwaddr offset, unsigned size)
{
    STMP3770GPMIState *s = STMP3770_GPMI(opaque);

    if (size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-gpmi: unsupported read size %u at offset "
                      HWADDR_FMT_plx "\n", size, offset);
        return 0;
    }

    switch (offset) {
    case GPMI_CTRL0:
        return s->ctrl0;
    case GPMI_COMPARE:
        return s->compare;
    case GPMI_ECCCTRL:
        return s->eccctrl;
    case GPMI_ECCCOUNT:
        return s->ecccount;
    case GPMI_PAYLOAD:
        return s->payload;
    case GPMI_AUXILIARY:
        return s->auxiliary;
    case GPMI_CTRL1:
        return s->ctrl1;
    case GPMI_TIMING0:
        return s->timing0;
    case GPMI_TIMING1:
        return s->timing1;
    case GPMI_DATA:
        /* Pop one word from FIFO */
        if (s->fifo_count > 0) {
            uint32_t val = s->fifo[0];
            memmove(&s->fifo[0], &s->fifo[1],
                    (s->fifo_count - 1) * sizeof(s->fifo[0]));
            s->fifo_count--;
            return val;
        }
        return 0xFFFFFFFF;
    case GPMI_STAT:
        return s->stat;
    case GPMI_DEBUG:
        return s->debug;
    case GPMI_VERSION:
        return GPMI_VERSION_VALUE;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-gpmi: read from unimplemented offset "
                      HWADDR_FMT_plx "\n", offset);
        return 0;
    }
}

static void gpmi_write(void *opaque, hwaddr offset,
                       uint64_t value, unsigned size)
{
    STMP3770GPMIState *s = STMP3770_GPMI(opaque);
    int sct = (offset >> 2) & 3;
    hwaddr base = offset & ~0xC;

    if (size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-gpmi: unsupported write size %u at offset "
                      HWADDR_FMT_plx "\n", size, offset);
        return;
    }

    switch (base) {
    case GPMI_CTRL0:
        gpmi_apply_sct(&s->ctrl0, (uint32_t)value, sct);
        /* Hardware ties CLKGATE to SFTRST */
        if (s->ctrl0 & GPMI_CTRL0_SFTRST) {
            s->ctrl0 |= GPMI_CTRL0_CLKGATE;
            gpmi_nand_reset_state(s);
        } else {
            s->ctrl0 &= ~GPMI_CTRL0_CLKGATE;
        }
        if (s->ctrl0 & GPMI_CTRL0_RUN) {
            gpmi_execute_command(s);
        }
        break;
    case GPMI_COMPARE:
        s->compare = (uint32_t)value;
        break;
    case GPMI_ECCCTRL:
        s->eccctrl = (uint32_t)value;
        break;
    case GPMI_ECCCOUNT:
        s->ecccount = (uint32_t)value;
        break;
    case GPMI_PAYLOAD:
        s->payload = (uint32_t)value;
        break;
    case GPMI_AUXILIARY:
        s->auxiliary = (uint32_t)value;
        break;
    case GPMI_CTRL1:
        gpmi_apply_sct(&s->ctrl1, (uint32_t)value, sct);
        gpmi_update_irq(s);
        break;
    case GPMI_TIMING0:
        s->timing0 = (uint32_t)value;
        break;
    case GPMI_TIMING1:
        s->timing1 = (uint32_t)value;
        break;
    case GPMI_DATA:
        /* Push one word to FIFO */
        if (s->fifo_count < ARRAY_SIZE(s->fifo)) {
            s->fifo[s->fifo_count++] = (uint32_t)value;
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-gpmi: write to unimplemented offset "
                      HWADDR_FMT_plx "\n", offset);
        break;
    }
}

static const MemoryRegionOps gpmi_ops = {
    .read = gpmi_read,
    .write = gpmi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static bool gpmi_dma_pio_starts_command(uint32_t ctrl0)
{
    unsigned int mode = (ctrl0 >> GPMI_CTRL0_COMMAND_MODE_SHIFT) &
                        GPMI_CTRL0_COMMAND_MODE_MASK;

    /*
     * MXS APBH DMA PIO descriptors launch the GPMI command after the PIO
     * register words are staged. WRITE commands whose bytes come from the DMA
     * data phase are handled as those bytes arrive, so starting here would be
     * too early for them.
     */
    return mode != GPMI_COMMAND_MODE_WRITE;
}

static void gpmi_reset(DeviceState *dev)
{
    STMP3770GPMIState *s = STMP3770_GPMI(dev);

    s->ctrl0 = GPMI_CTRL0_SFTRST | GPMI_CTRL0_CLKGATE;
    s->compare = 0;
    s->eccctrl = 0;
    s->ecccount = 0;
    s->payload = 0;
    s->auxiliary = 0;
    s->ctrl1 = 0;
    s->timing0 = 0x00010203;
    s->timing1 = 0;
    s->stat = GPMI_STAT_PRESENT | GPMI_STAT_FIFO_EMPTY;
    s->debug = 0;
    s->fifo_count = 0;

    gpmi_nand_reset_state(s);
    gpmi_update_rdy_timeout(s);
    gpmi_update_irq(s);

    /* Progress counters are NOT reset on soft reset so they accumulate
     * across the entire QEMU session. */
}

static void gpmi_init(Object *obj)
{
    STMP3770GPMIState *s = STMP3770_GPMI(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &gpmi_ops, s,
                          TYPE_STMP3770_GPMI, 0x2000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    /* Initialize progress counters */
    s->page_read_cnt = 0;
    s->page_write_cnt = 0;
    s->block_erase_cnt = 0;
    s->ecc_complete_cnt = 0;
}

static void gpmi_realize(DeviceState *dev, Error **errp)
{
    STMP3770GPMIState *s = STMP3770_GPMI(dev);

    s->page_size = DEFAULT_PAGE_SIZE;
    s->pages_per_block = DEFAULT_PAGES_PER_BLOCK;
    s->num_blocks = DEFAULT_NUM_BLOCKS;
    s->oob_size = DEFAULT_OOB_SIZE;
    s->block_size = s->page_size * s->pages_per_block;
    s->total_size = s->block_size * s->num_blocks;
    s->page_buf_size = s->page_size + s->oob_size;

    fprintf(stderr, "stmp3770-gpmi: NAND geometry: %u blocks × %u pages × %u bytes "
            "(total %u MB), progress tracking enabled\n",
            s->num_blocks, s->pages_per_block, s->page_size,
            (s->total_size) >> 20);

    if (s->blk) {
        uint64_t perm = BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE |
                        BLK_PERM_RESIZE;
        int64_t file_size;
        uint64_t read_size;
        bool has_appended_oob;

        if (blk_set_perm(s->blk, perm, BLK_PERM_ALL, errp) < 0) {
            return;
        }

        file_size = blk_getlength(s->blk);
        if (file_size < 0) {
            error_setg(errp, "stmp3770-gpmi: failed to get NAND image size");
            return;
        }
        s->storage_file_size = file_size;
        has_appended_oob =
            gpmi_has_appended_oob_magic(s, s->storage_file_size);

        if (has_appended_oob) {
            s->storage_layout = GPMI_STORAGE_LAYOUT_APPENDED_OOB;
            s->storage_size = gpmi_appended_image_size(s);
            read_size = MIN(s->storage_file_size, s->storage_size);
        } else if (s->storage_file_size >= gpmi_interleaved_area_size(s)) {
            s->storage_layout = GPMI_STORAGE_LAYOUT_INTERLEAVED_OOB;
            s->storage_size = s->storage_file_size;
            read_size = s->storage_size;
        } else {
            s->storage_layout = GPMI_STORAGE_LAYOUT_APPENDED_OOB;
            s->storage_size = gpmi_appended_image_size(s);
            read_size = MIN(s->storage_file_size, gpmi_data_area_size(s));
        }

        fprintf(stderr, "stmp3770-gpmi: NAND image layout: %s (%" PRIu64
                " bytes backing)\n",
                s->storage_layout == GPMI_STORAGE_LAYOUT_INTERLEAVED_OOB ?
                "interleaved-oob" : "appended-oob",
                s->storage_size);

        s->storage = blk_blockalign(s->blk, s->storage_size);
        memset(s->storage, 0xFF, s->storage_size);
        if (read_size > 0 &&
            blk_pread(s->blk, 0, read_size, s->storage, 0) < 0) {
            error_setg(errp, "stmp3770-gpmi: failed to read NAND image");
            return;
        }
        if (!has_appended_oob) {
            gpmi_persist_appended_oob_layout(s);
        }
    } else {
        s->storage_file_size = 0;
        s->storage_layout = GPMI_STORAGE_LAYOUT_APPENDED_OOB;
        s->storage_size = gpmi_appended_image_size(s);
        s->storage = g_malloc(s->storage_size);
        memset(s->storage, 0xFF, s->storage_size);
    }

    s->page_buf = g_malloc0(s->page_buf_size);
}

static void gpmi_unrealize(DeviceState *dev)
{
    STMP3770GPMIState *s = STMP3770_GPMI(dev);

    g_free(s->page_buf);
    s->page_buf = NULL;

    if (s->blk) {
        s->storage = NULL;
    } else {
        g_free(s->storage);
        s->storage = NULL;
    }
}

/*
 * APBH DMA handler for GPMI channels.  The PIO words in the DMA descriptor
 * are written to GPMI registers starting at offset 0x00 (CTRL0) and stepping
 * by 0x10, matching the GPMI register layout.  DMA PIO descriptors start
 * non-WRITE GPMI commands even when firmware leaves CTRL0.RUN clear.  Data
 * read/write events copy bytes between the DMA buffer and the GPMI data FIFO.
 */
static int stmp3770_gpmi_dma_handler(STMP3770DMAState *dma,
                                     int channel, STMP3770DMAEvent event,
                                     void *buf, size_t len, void *opaque)
{
    STMP3770GPMIState *s = STMP3770_GPMI(opaque);
    STMP3770DMAChannel *ch = &dma->ch[channel];
    int i;

    if (event == STMP3770_DMA_EVENT_PIO) {
        uint32_t ctrl0 = 0;
        int nwords = (int)ch->num_pio_words;
        /*
         * PIO words are laid out to match the GPMI register window:
         *   word 0 -> CTRL0     (0x00)
         *   word 1 -> COMPARE   (0x10)
         *   word 2 -> ECCCTRL   (0x20)
         *   word 3 -> ECCCOUNT  (0x30)
         *   word 4 -> PAYLOAD   (0x40)
         *   word 5 -> AUXILIARY (0x50)
         */
        static const hwaddr pio_offsets[] = {
            GPMI_CTRL0, GPMI_COMPARE, GPMI_ECCCTRL, GPMI_ECCCOUNT,
            GPMI_PAYLOAD, GPMI_AUXILIARY
        };
        const int max_pio = ARRAY_SIZE(pio_offsets);

        /* Each descriptor's PIO data is independent; clear stale FIFO bytes. */
        s->fifo_count = 0;
        s->write_cmd_sent = false;

        if (nwords > 0) {
            ctrl0 = ch->pio_words[0];
            /* Defer execution until all PIO data words are staged */
            gpmi_write(s, GPMI_CTRL0, ctrl0 & ~GPMI_CTRL0_RUN, 4);
        }
        for (i = 1; i < nwords; i++) {
            if (i < max_pio) {
                gpmi_write(s, pio_offsets[i], ch->pio_words[i], 4);
            } else {
                qemu_log_mask(LOG_UNIMP,
                              "stmp3770-gpmi: unexpected PIO word %d = 0x%08x\n",
                              i, ch->pio_words[i]);
            }
        }
        if (nwords > 0) {
            if (ctrl0 & GPMI_CTRL0_RUN) {
                gpmi_write(s, GPMI_CTRL0, ctrl0, 4);
            } else if (gpmi_dma_pio_starts_command(ctrl0)) {
                gpmi_execute_command(s);
            }
        }
        return nwords * sizeof(uint32_t);
    }

    if (event == STMP3770_DMA_EVENT_DATA_READ) {
        uint8_t *dst = buf;
        size_t n;
        for (n = 0; n < len; n++) {
            if (s->fifo_count > 0) {
                dst[n] = s->fifo[0] & 0xFF;
                memmove(&s->fifo[0], &s->fifo[1],
                        (s->fifo_count - 1) * sizeof(s->fifo[0]));
                s->fifo_count--;
            } else if (s->data_ptr < s->data_count) {
                dst[n] = gpmi_nand_read_data_byte(s);
            } else {
                break;
            }
        }
        return n;
    }

    if (event == STMP3770_DMA_EVENT_DATA_WRITE) {
        const uint8_t *src = buf;
        unsigned int mode = (s->ctrl0 >> GPMI_CTRL0_COMMAND_MODE_SHIFT) &
                            GPMI_CTRL0_COMMAND_MODE_MASK;
        unsigned int address = (s->ctrl0 >> GPMI_CTRL0_ADDRESS_SHIFT) &
                               GPMI_CTRL0_ADDRESS_MASK;
        uint32_t count = s->ctrl0 & GPMI_CTRL0_XFER_COUNT_MASK;
        size_t n;

        if (mode == GPMI_COMMAND_MODE_WRITE &&
            address == GPMI_ADDRESS_DATA &&
            s->nand_cmd == NAND_CMD_PROGRAM_1ST &&
            s->data_dir_write && count > s->data_count) {
            s->data_count = MIN(count, s->page_buf_size);
        }

        for (n = 0; n < len; n++) {
            gpmi_handle_write_byte(s, src[n]);
        }
        return n;
    }

    return 0;
}

void stmp3770_gpmi_set_dma(STMP3770GPMIState *s, STMP3770DMAState *dma,
                           int channel_base)
{
    int i;

    s->dma = dma;
    s->dma_channel_base = channel_base;

    if (!dma) {
        return;
    }

    for (i = 0; i < 4; i++) {
        stmp3770_dma_set_channel_handler(dma, channel_base + i,
                                         stmp3770_gpmi_dma_handler, s);
    }
}

void stmp3770_gpmi_set_bch(STMP3770GPMIState *s, STMP3770BCHState *bch)
{
    s->bch = bch;
}

static const VMStateDescription vmstate_gpmi = {
    .name = "stmp3770-gpmi",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl0, STMP3770GPMIState),
        VMSTATE_UINT32(compare, STMP3770GPMIState),
        VMSTATE_UINT32(eccctrl, STMP3770GPMIState),
        VMSTATE_UINT32(ecccount, STMP3770GPMIState),
        VMSTATE_UINT32(payload, STMP3770GPMIState),
        VMSTATE_UINT32(auxiliary, STMP3770GPMIState),
        VMSTATE_UINT32(ctrl1, STMP3770GPMIState),
        VMSTATE_UINT32(timing0, STMP3770GPMIState),
        VMSTATE_UINT32(timing1, STMP3770GPMIState),
        VMSTATE_UINT32(stat, STMP3770GPMIState),
        VMSTATE_UINT32(debug, STMP3770GPMIState),
        VMSTATE_UINT32(fifo_count, STMP3770GPMIState),
        VMSTATE_UINT32_ARRAY(fifo, STMP3770GPMIState, 16),
        VMSTATE_BOOL(write_cmd_sent, STMP3770GPMIState),
        VMSTATE_UINT8(nand_cmd, STMP3770GPMIState),
        VMSTATE_UINT32(nand_addr, STMP3770GPMIState),
        VMSTATE_UINT32(nand_addr_cycles, STMP3770GPMIState),
        VMSTATE_UINT32(nand_column, STMP3770GPMIState),
        VMSTATE_UINT32(nand_page, STMP3770GPMIState),
        VMSTATE_UINT32(nand_block, STMP3770GPMIState),
        VMSTATE_UINT32(nand_row, STMP3770GPMIState),
        VMSTATE_UINT32(data_count, STMP3770GPMIState),
        VMSTATE_UINT32(data_ptr, STMP3770GPMIState),
        VMSTATE_BOOL(data_dir_write, STMP3770GPMIState),
        VMSTATE_VBUFFER_ALLOC_UINT32(page_buf, STMP3770GPMIState, 0, NULL,
                                     page_buf_size),
        VMSTATE_UINT64(page_read_cnt, STMP3770GPMIState),
        VMSTATE_UINT64(page_write_cnt, STMP3770GPMIState),
        VMSTATE_UINT64(block_erase_cnt, STMP3770GPMIState),
        VMSTATE_UINT64(ecc_complete_cnt, STMP3770GPMIState),
        VMSTATE_END_OF_LIST()
    }
};

static Property gpmi_properties[] = {
    DEFINE_PROP_DRIVE("drive", STMP3770GPMIState, blk),
};

static void gpmi_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = gpmi_realize;
    dc->unrealize = gpmi_unrealize;
    device_class_set_legacy_reset(dc, gpmi_reset);
    dc->vmsd = &vmstate_gpmi;
    device_class_set_props(dc, gpmi_properties);
}

static const TypeInfo gpmi_type_info = {
    .name          = TYPE_STMP3770_GPMI,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(STMP3770GPMIState),
    .instance_init = gpmi_init,
    .class_init    = gpmi_class_init,
};

/* BCH implementation */

static uint64_t bch_read(void *opaque, hwaddr offset, unsigned size)
{
    STMP3770BCHState *s = STMP3770_BCH(opaque);

    if (size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-bch: unsupported read size %u\n", size);
        return 0;
    }

    switch (offset) {
    case BCH_CTRL:
        return s->ctrl;
    case BCH_STATUS0:
        return s->status0;
    case BCH_STATUS1:
        return s->status1;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-bch: read from unimplemented offset "
                      HWADDR_FMT_plx "\n", offset);
        return 0;
    }
}

static void bch_write(void *opaque, hwaddr offset,
                      uint64_t value, unsigned size)
{
    STMP3770BCHState *s = STMP3770_BCH(opaque);
    int sct = (offset >> 2) & 3;
    hwaddr base = offset & ~0xC;

    if (size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-bch: unsupported write size %u\n", size);
        return;
    }

    switch (base) {
    case BCH_CTRL:
        bch_apply_sct(&s->ctrl, (uint32_t)value, sct);
        /* Hardware ties CLKGATE to SFTRST */
        if (s->ctrl & BCH_CTRL_SFTRST) {
            s->ctrl |= BCH_CTRL_CLKGATE;
            s->status0 = 0x00001C01;
            s->status1 = 0xCCCCCCCC;
        } else {
            s->ctrl &= ~BCH_CTRL_CLKGATE;
        }
        bch_update_irq(s);
        break;
    case BCH_STATUS0:
        /* Write-1-to-clear bits (works for base and CLR alias) */
        s->status0 &= ~((uint32_t)value & (BCH_STATUS0_ALLONES |
                                            BCH_STATUS0_CORRECTED |
                                            BCH_STATUS0_UNCORRECTABLE |
                                            BCH_STATUS0_COMPLETED_CE));
        break;
    case BCH_STATUS1:
        /* Read-only */
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-bch: write to unimplemented offset "
                      HWADDR_FMT_plx "\n", offset);
        break;
    }
}

static const MemoryRegionOps bch_ops = {
    .read = bch_read,
    .write = bch_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void bch_reset(DeviceState *dev)
{
    STMP3770BCHState *s = STMP3770_BCH(dev);

    s->ctrl = BCH_CTRL_SFTRST | BCH_CTRL_CLKGATE;
    s->status0 = 0x00001C01;
    s->status1 = 0xCCCCCCCC;
    qemu_set_irq(s->irq, false);
}

static void bch_init(Object *obj)
{
    STMP3770BCHState *s = STMP3770_BCH(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &bch_ops, s,
                          TYPE_STMP3770_BCH, 0x2000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static const VMStateDescription vmstate_bch = {
    .name = "stmp3770-bch",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl, STMP3770BCHState),
        VMSTATE_UINT32(status0, STMP3770BCHState),
        VMSTATE_UINT32(status1, STMP3770BCHState),
        VMSTATE_END_OF_LIST()
    }
};

static void bch_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, bch_reset);
    dc->vmsd = &vmstate_bch;
}

static const TypeInfo bch_type_info = {
    .name          = TYPE_STMP3770_BCH,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(STMP3770BCHState),
    .instance_init = bch_init,
    .class_init    = bch_class_init,
};

static void gpmi_register_types(void)
{
    type_register_static(&gpmi_type_info);
    type_register_static(&bch_type_info);
}

type_init(gpmi_register_types)
