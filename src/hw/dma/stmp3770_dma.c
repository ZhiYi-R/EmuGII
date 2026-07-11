/*
 * STMP3770 DMA controllers (APBH + APBX)
 *
 * Based on STMP3770 Reference Manual Chapters 11 (APBH) and 12 (APBX)
 *
 * Features:
 * - 8 channels per controller
 * - Linked-list command descriptors
 * - PIO word transfers, memory-to-peripheral and peripheral-to-memory
 * - Command completion interrupts
 * - SET/CLR/TOG register variants on global registers
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
#include "hw/dma/stmp3770_dma.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/address-spaces.h"

/* Register offsets */
#define REG_CTRL0           0x000
#define REG_CTRL1           0x010
#define REG_DEVSEL          0x020

#define CH_BASE(n)          (0x040 + ((n) * 0x70))
#define REG_CH_CURCMDAR(n)  (CH_BASE(n) + 0x00)
#define REG_CH_NXTCMDAR(n)  (CH_BASE(n) + 0x10)
#define REG_CH_CMD(n)       (CH_BASE(n) + 0x20)
#define REG_CH_BAR(n)       (CH_BASE(n) + 0x30)
#define REG_CH_SEMA(n)      (CH_BASE(n) + 0x40)
#define REG_CH_DEBUG1(n)    (CH_BASE(n) + 0x50)
#define REG_CH_DEBUG2(n)    (CH_BASE(n) + 0x60)

#define REG_VERSION         0x3F0

/* SET/CLR/TOG selector values decoded from register offset bits [3:2]. */
#define SCT_SET             1
#define SCT_CLR             2
#define SCT_TOG             3

/* CTRL0 bits */
#define CTRL0_SFTRST                (1U << 31)
#define CTRL0_CLKGATE               (1U << 30)
#define CTRL0_RESET_CHANNEL_SHIFT   16
#define CTRL0_RESET_CHANNEL_MASK    0xFF
#define CTRL0_CLKGATE_CHANNEL_SHIFT 8
#define CTRL0_CLKGATE_CHANNEL_MASK  0xFF
#define CTRL0_FREEZE_CHANNEL_SHIFT  0
#define CTRL0_FREEZE_CHANNEL_MASK   0xFF

/* CTRL1 bits */
#define CTRL1_AHB_ERROR_IRQ_SHIFT   16
#define CTRL1_AHB_ERROR_IRQ_MASK    0xFF
#define CTRL1_CMDCMPLT_IRQ_EN_SHIFT 8
#define CTRL1_CMDCMPLT_IRQ_EN_MASK  0xFF
#define CTRL1_CMDCMPLT_IRQ_SHIFT    0
#define CTRL1_CMDCMPLT_IRQ_MASK     0xFF

/* DEVSEL bit positions (APBX only) */
#define DEVSEL_CH7_SHIFT    28
#define DEVSEL_CH6_SHIFT    24
#define DEVSEL_CH2_SHIFT    8

/*
 * APBX DEVSEL writable mask: CH7 (31:28), CH6 (27:24), CH2 (11:8).
 * APBH DEVSEL is entirely read-only.
 */
#define DEVSEL_APBX_WRITABLE_MASK  0xFF000F00u

/* Channel command word bits */
#define CMD_XFER_COUNT_SHIFT    16
#define CMD_XFER_COUNT_MASK     0xFFFF
#define CMD_CMDWORDS_SHIFT      12
#define CMD_CMDWORDS_MASK       0xF
#define CMD_HALTONTERMINATE     (1U << 8)
#define CMD_WAIT4ENDCMD         (1U << 7)
#define CMD_SEMAPHORE           (1U << 6)
#define CMD_NANDWAIT4READY      (1U << 5)
#define CMD_NANDLOCK            (1U << 4)
#define CMD_IRQONCMPLT          (1U << 3)
#define CMD_CHAIN               (1U << 2)
#define CMD_COMMAND_SHIFT       0
#define CMD_COMMAND_MASK        0x3

/* COMMAND values */
#define COMMAND_NO_DMA_XFER     0
#define COMMAND_DMA_WRITE       1
#define COMMAND_DMA_READ        2
#define COMMAND_DMA_SENSE       3

/* SEMA register bits */
#define SEMA_PHORE_SHIFT        16
#define SEMA_PHORE_MASK         0xFF
#define SEMA_INCREMENT_SHIFT    0
#define SEMA_INCREMENT_MASK     0xFF

/* DEBUG1 bits */
#define DEBUG1_NEXTCMDADDRVALID (1U << 24)
#define DEBUG1_RD_FIFO_EMPTY    (1U << 23)
#define DEBUG1_WR_FIFO_EMPTY    (1U << 21)
#define DEBUG1_STATEMACHINE_SHIFT 0
#define DEBUG1_STATEMACHINE_MASK  0x1F

/* State machine values */
#define STATEMACHINE_IDLE       0x00
#define STATEMACHINE_XFER_COMPLETE 0x0F

/* VERSION value */
#define DMA_VERSION             0x01010000  /* v1.1 */

static inline bool stmp3770_dma_enabled(STMP3770DMAState *s)
{
    return (s->ctrl0 & (CTRL0_SFTRST | CTRL0_CLKGATE)) == 0;
}

static void stmp3770_dma_update_irq(STMP3770DMAState *s, int ch)
{
    bool cmdcmplt = (s->ctrl1 & (1U << ch)) &&
                    (s->ctrl1 & (1U << (ch + CTRL1_CMDCMPLT_IRQ_EN_SHIFT)));
    bool ahb_error = (s->ctrl1 & (1U << (ch + CTRL1_AHB_ERROR_IRQ_SHIFT))) != 0;
    qemu_set_irq(s->irq[ch], cmdcmplt || ahb_error);
}

static void stmp3770_dma_update_all_irqs(STMP3770DMAState *s)
{
    int i;

    for (i = 0; i < STMP3770_DMA_NUM_CHANNELS; i++) {
        stmp3770_dma_update_irq(s, i);
    }
}

void stmp3770_dma_set_channel_handler(STMP3770DMAState *s, int channel,
                                      STMP3770DMAHandler handler, void *opaque)
{
    if (channel < 0 || channel >= STMP3770_DMA_NUM_CHANNELS) {
        return;
    }
    s->ch_handler[channel].handler = handler;
    s->ch_handler[channel].opaque = opaque;
}

void stmp3770_dma_set_channel_sense_capable(STMP3770DMAState *s,
                                            int channel, bool capable)
{
    if (channel < 0 || channel >= STMP3770_DMA_NUM_CHANNELS) {
        return;
    }
    s->ch_handler[channel].sense_capable = capable;
}

void stmp3770_dma_set_channel_completion_callback(STMP3770DMAState *s,
                                                  int channel,
                                                  STMP3770DMACompletionFn cb,
                                                  void *opaque)
{
    if (channel < 0 || channel >= STMP3770_DMA_NUM_CHANNELS) {
        return;
    }
    s->completion_cb[channel] = cb;
    s->completion_opaque[channel] = opaque;
}

/*
 * Load a command descriptor from guest memory.  Returns false if the read
 * fails (bad address).  On success, the loaded_* fields of the channel are
 * updated and the visible CMD/BAR registers reflect the loaded command.
 */
static bool stmp3770_dma_load_command(STMP3770DMAState *s, int ch_idx)
{
    STMP3770DMAChannel *ch = &s->ch[ch_idx];
    hwaddr addr = ch->nxtcmdar;
    uint32_t words[4];
    int i;

    if (addr == 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: channel %d loaded null command address\n",
                      s->is_apbx ? "stmp3770-apbx-dma" : "stmp3770-apbh-dma",
                      ch_idx);
        return false;
    }

    if (address_space_rw(&address_space_memory, addr,
                         MEMTXATTRS_UNSPECIFIED, words, sizeof(words), 0) !=
        MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: channel %d failed to read command at "
                      HWADDR_FMT_plx "\n",
                      s->is_apbx ? "stmp3770-apbx-dma" : "stmp3770-apbh-dma",
                      ch_idx, addr);
        return false;
    }

    ch->loaded_nxtcmdar = words[0];
    ch->loaded_cmd = words[1];
    ch->loaded_bar = words[2];

    ch->num_pio_words = (ch->loaded_cmd >> CMD_CMDWORDS_SHIFT) &
                        CMD_CMDWORDS_MASK;
    if (ch->num_pio_words > 15) {
        ch->num_pio_words = 15;
    }

    if (ch->num_pio_words > 0) {
        size_t pio_size = ch->num_pio_words * sizeof(uint32_t);
        if (address_space_rw(&address_space_memory,
                             addr + 3 * sizeof(uint32_t),
                             MEMTXATTRS_UNSPECIFIED,
                             ch->pio_words, pio_size, 0) != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: channel %d failed to read PIO words at "
                          HWADDR_FMT_plx "\n",
                          s->is_apbx ? "stmp3770-apbx-dma" :
                                       "stmp3770-apbh-dma",
                          ch_idx, addr + 12);
            return false;
        }
    }

    /* Reflect loaded command in visible registers */
    ch->curcmdar = addr;
    ch->cmd = ch->loaded_cmd;
    ch->bar = ch->loaded_bar;

    /*
     * Forward PIO words to the registered peripheral handler.  If no handler
     * is installed, just log them for debugging.
     */
    if (ch->num_pio_words > 0) {
        if (s->ch_handler[ch_idx].handler) {
            s->ch_handler[ch_idx].handler(s, ch_idx,
                                          STMP3770_DMA_EVENT_PIO,
                                          ch->pio_words,
                                          ch->num_pio_words * sizeof(uint32_t),
                                          s->ch_handler[ch_idx].opaque);
        } else {
            qemu_log_mask(LOG_UNIMP,
                          "%s: channel %d PIO words not yet forwarded\n",
                          s->is_apbx ? "stmp3770-apbx-dma" : "stmp3770-apbh-dma",
                          ch_idx);
            for (i = 0; i < ch->num_pio_words; i++) {
                qemu_log_mask(LOG_UNIMP, "  pio[%d] = 0x%08x\n", i,
                              ch->pio_words[i]);
            }
        }
    }

    return true;
}

/*
 * Execute one loaded command and advance to the next.  This is a Phase 0
 * stub: it completes immediately without performing real data movement.
 *
 * The hardware continues processing a chained list as long as the next
 * command does not require a semaphore decrement when the count is zero.
 * We therefore check the SEMAPHORE bit of the command we are about to run
 * and stall only when that bit is set and the count has reached zero.
 */
static void stmp3770_dma_run_channel(STMP3770DMAState *s, int ch_idx)
{
    STMP3770DMAChannel *ch = &s->ch[ch_idx];
    uint32_t cmd = ch->loaded_cmd;
    unsigned int command = (cmd >> CMD_COMMAND_SHIFT) & CMD_COMMAND_MASK;
    bool irq_on_cmplt = (cmd & CMD_IRQONCMPLT) != 0;
    bool decrement_sema = (cmd & CMD_SEMAPHORE) != 0;
    bool chain = (cmd & CMD_CHAIN) != 0;
    bool wait4endcmd = (cmd & CMD_WAIT4ENDCMD) != 0;
    bool sense_branch = false;
    uint32_t phore = (ch->sema >> SEMA_PHORE_SHIFT) & SEMA_PHORE_MASK;

    /* Stall if this command would need to decrement a zero semaphore. */
    if (decrement_sema && phore == 0) {
        return;
    }

    if (command == COMMAND_DMA_SENSE && !s->is_apbx) {
        bool sense = false;

        if (s->ch_handler[ch_idx].sense_capable) {
            sense = s->ch_handler[ch_idx].handler(
                s, ch_idx, STMP3770_DMA_EVENT_SENSE, NULL, 0,
                s->ch_handler[ch_idx].opaque) > 0;
            ch->nxtcmdar = sense ? ch->loaded_bar : ch->loaded_nxtcmdar;
            sense_branch = true;
        }
        command = COMMAND_NO_DMA_XFER;
    }

    /*
     * Perform data transfer when requested.  In MXS DMA terminology the
     * command name describes the peripheral's action: WRITE means the
     * peripheral writes to memory, READ means it reads from memory.
     */
    if (command == COMMAND_DMA_WRITE || command == COMMAND_DMA_READ) {
        uint32_t xfer_count = (cmd >> CMD_XFER_COUNT_SHIFT) & CMD_XFER_COUNT_MASK;
        hwaddr bar = ch->loaded_bar;

        /*
         * XFER_COUNT is a 16-bit byte count; a value of zero requests a
         * 64-Kbyte transfer.  BAR is a byte address, so zero is valid.
         */
        if (xfer_count == 0) {
            xfer_count = 1U << 16;
        }

        if (xfer_count > 0) {
            g_autofree uint8_t *buf = g_malloc0(xfer_count);
            bool transfer_ok = false;

            if (command == COMMAND_DMA_WRITE) {
                /*
                 * WRITE (MXS convention): peripheral -> memory.  Ask the
                 * handler to fill the buffer, then write it to guest memory
                 * at BAR.
                 */
                int handled = 0;
                if (s->ch_handler[ch_idx].handler) {
                    handled = s->ch_handler[ch_idx].handler(
                        s, ch_idx, STMP3770_DMA_EVENT_DATA_READ,
                        buf, xfer_count, s->ch_handler[ch_idx].opaque);
                }
                if (handled < 0) {
                    handled = 0;
                }
                if ((uint32_t)handled < xfer_count) {
                    qemu_log_mask(LOG_UNIMP,
                                  "%s: channel %d only handled %u/%u write bytes\n",
                                  s->is_apbx ? "stmp3770-apbx-dma" :
                                               "stmp3770-apbh-dma",
                                  ch_idx, handled, xfer_count);
                }
                transfer_ok = address_space_rw(&address_space_memory, bar,
                                               MEMTXATTRS_UNSPECIFIED,
                                               buf, xfer_count, 1) == MEMTX_OK;
            } else {
                /*
                 * READ (MXS convention): memory -> peripheral.  Read the
                 * buffer from guest memory at BAR and pass it to the handler.
                 */
                transfer_ok = address_space_rw(&address_space_memory, bar,
                                               MEMTXATTRS_UNSPECIFIED,
                                               buf, xfer_count, 0) == MEMTX_OK;
                if (transfer_ok && s->ch_handler[ch_idx].handler) {
                    s->ch_handler[ch_idx].handler(
                        s, ch_idx, STMP3770_DMA_EVENT_DATA_WRITE,
                        buf, xfer_count, s->ch_handler[ch_idx].opaque);
                }
            }

            if (!transfer_ok) {
                s->ctrl1 |= (1U << (ch_idx + CTRL1_AHB_ERROR_IRQ_SHIFT));
                stmp3770_dma_update_irq(s, ch_idx);
                qemu_log_mask(LOG_GUEST_ERROR,
                              "%s: channel %d failed to %s %u bytes at "
                              HWADDR_FMT_plx "\n",
                              s->is_apbx ? "stmp3770-apbx-dma" :
                                           "stmp3770-apbh-dma",
                              ch_idx,
                              command == COMMAND_DMA_WRITE ? "write" : "read",
                              xfer_count, bar);
            }
        }
    }

    /* Advance command chain */
    if (!sense_branch && chain) {
        ch->nxtcmdar = ch->loaded_nxtcmdar;
    } else if (!sense_branch) {
        ch->nxtcmdar = 0;
    }

    if (wait4endcmd && s->completion_cb[ch_idx]) {
        s->completion_cb[ch_idx](s, ch_idx, s->completion_opaque[ch_idx]);
        return;
    }

    /* Update semaphore after the command completes. */
    if (decrement_sema) {
        if (phore > 0) {
            phore--;
        }
        ch->sema = (phore << SEMA_PHORE_SHIFT);
    }

    /* Set completion IRQ */
    if (irq_on_cmplt) {
        s->ctrl1 |= (1U << ch_idx);
        stmp3770_dma_update_irq(s, ch_idx);
    }

    /* Load and run the next command if the chain continues. */
    if (chain || sense_branch) {
        if (stmp3770_dma_load_command(s, ch_idx)) {
            stmp3770_dma_run_channel(s, ch_idx);
        }
    }
}

void stmp3770_dma_complete_channel_command(STMP3770DMAState *s, int ch_idx)
{
    STMP3770DMAChannel *ch;
    uint32_t cmd;
    bool irq_on_cmplt;
    bool decrement_sema;
    bool chain;
    uint32_t phore;

    if (!s || ch_idx < 0 || ch_idx >= STMP3770_DMA_NUM_CHANNELS) {
        return;
    }

    ch = &s->ch[ch_idx];
    cmd = ch->loaded_cmd;
    irq_on_cmplt = (cmd & CMD_IRQONCMPLT) != 0;
    decrement_sema = (cmd & CMD_SEMAPHORE) != 0;
    chain = (cmd & CMD_CHAIN) != 0;
    phore = (ch->sema >> SEMA_PHORE_SHIFT) & SEMA_PHORE_MASK;

    if (decrement_sema) {
        if (phore > 0) {
            phore--;
        }
        ch->sema = (phore << SEMA_PHORE_SHIFT);
    }

    if (irq_on_cmplt) {
        s->ctrl1 |= (1U << ch_idx);
        stmp3770_dma_update_irq(s, ch_idx);
    }

    if (chain && stmp3770_dma_load_command(s, ch_idx)) {
        stmp3770_dma_run_channel(s, ch_idx);
    }
}

static void stmp3770_dma_kick_channel(STMP3770DMAState *s, int ch_idx)
{
    STMP3770DMAChannel *ch = &s->ch[ch_idx];
    uint32_t phore;

    if (!stmp3770_dma_enabled(s)) {
        return;
    }

    phore = (ch->sema >> SEMA_PHORE_SHIFT) & SEMA_PHORE_MASK;
    if (phore == 0 || ch->nxtcmdar == 0) {
        return;
    }

    if (stmp3770_dma_load_command(s, ch_idx)) {
        stmp3770_dma_run_channel(s, ch_idx);
    }
}

static uint64_t dma_read_subword(uint32_t value, hwaddr offset, unsigned size)
{
    unsigned shift = (offset & 3) * 8;
    uint32_t mask = (size >= 4) ? 0xFFFFFFFFu : ((1u << (size * 8)) - 1u);

    return (value >> shift) & mask;
}

static uint64_t stmp3770_dma_read(void *opaque, hwaddr offset, unsigned size)
{
    STMP3770DMAState *s = STMP3770_DMA(opaque);
    hwaddr base = offset & ~0xFULL;
    int ch_idx;
    uint32_t ret;

    switch (base) {
    case REG_CTRL0:
        ret = s->ctrl0;
        return dma_read_subword(ret, offset, size);
    case REG_CTRL1:
        ret = s->ctrl1;
        return dma_read_subword(ret, offset, size);
    case REG_DEVSEL:
        ret = s->devsel;
        return dma_read_subword(ret, offset, size);
    case REG_VERSION:
        ret = DMA_VERSION;
        return dma_read_subword(ret, offset, size);
    default:
        break;
    }

    for (ch_idx = 0; ch_idx < STMP3770_DMA_NUM_CHANNELS; ch_idx++) {
        STMP3770DMAChannel *ch = &s->ch[ch_idx];
        if (base == REG_CH_CURCMDAR(ch_idx)) {
            return dma_read_subword(ch->curcmdar, offset, size);
        }
        if (base == REG_CH_NXTCMDAR(ch_idx)) {
            return dma_read_subword(ch->nxtcmdar, offset, size);
        }
        if (base == REG_CH_CMD(ch_idx)) {
            return dma_read_subword(ch->cmd, offset, size);
        }
        if (base == REG_CH_BAR(ch_idx)) {
            return dma_read_subword(ch->bar, offset, size);
        }
        if (base == REG_CH_SEMA(ch_idx)) {
            /*
             * SEMA register layout (STMP3770/i.MX23):
             * - Write: bit[7:0] = INCREMENT_SEMA (value to add to PHORE)
             * - Read:  bit[7:0] = PHORE (current semaphore count)
             *          bit[23:16] = PHORE (mirrored, legacy field)
             *
             * Firmware polls INCREMENT_SEMA bit[7:0] to check if DMA is done
             * (waits for it to decrement to 0). We must return PHORE in both
             * bit[7:0] AND bit[23:16] on read.
             */
            uint32_t phore = (ch->sema >> SEMA_PHORE_SHIFT) & SEMA_PHORE_MASK;
            uint32_t sema_read = (phore << SEMA_PHORE_SHIFT) | (phore << SEMA_INCREMENT_SHIFT);
            return dma_read_subword(sema_read, offset, size);
        }
        if (base == REG_CH_DEBUG1(ch_idx)) {
            uint32_t phore = (ch->sema >> SEMA_PHORE_SHIFT) & SEMA_PHORE_MASK;
            ret = DEBUG1_RD_FIFO_EMPTY | DEBUG1_WR_FIFO_EMPTY |
                  (phore ? STATEMACHINE_XFER_COMPLETE : STATEMACHINE_IDLE) |
                  (ch->nxtcmdar ? DEBUG1_NEXTCMDADDRVALID : 0);
            return dma_read_subword(ret, offset, size);
        }
        if (base == REG_CH_DEBUG2(ch_idx)) {
            return dma_read_subword(ch->debug2, offset, size);
        }
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "%s: read from unimplemented offset " HWADDR_FMT_plx "\n",
                  s->is_apbx ? "stmp3770-apbx-dma" : "stmp3770-apbh-dma",
                  offset);
    return 0;
}

static void stmp3770_dma_apply_sct(uint32_t *reg, uint32_t value, int sct)
{
    switch (sct) {
    case SCT_SET:
        *reg |= value;
        break;
    case SCT_CLR:
        *reg &= ~value;
        break;
    case SCT_TOG:
        *reg ^= value;
        break;
    default:
        *reg = value;
        break;
    }
}

static void stmp3770_dma_write(void *opaque, hwaddr offset,
                               uint64_t value, unsigned size)
{
    STMP3770DMAState *s = STMP3770_DMA(opaque);
    int sct = (offset >> 2) & 3;
    hwaddr base = offset & ~0xFULL;
    unsigned shift = (offset & 3) * 8;
    uint32_t mask = (size >= 4) ? 0xFFFFFFFFu : ((1u << (size * 8)) - 1u);
    uint32_t full_mask = mask << shift;
    uint32_t cur;
    int ch_idx;

    switch (base) {
    case REG_CTRL0: {
        /*
         * APBH: SFTRST(31), CLKGATE(30), RESET_CHANNEL(23:16),
         *        CLKGATE_CHANNEL(15:8), FREEZE_CHANNEL(7:0)
         * APBX: SFTRST(31), CLKGATE(30), RESET_CHANNEL(23:16),
         *        RSVD(15:8), FREEZE_CHANNEL(7:0)
         */
        uint32_t ctrl0_writable = CTRL0_SFTRST | CTRL0_CLKGATE |
                                  (CTRL0_RESET_CHANNEL_MASK <<
                                   CTRL0_RESET_CHANNEL_SHIFT) |
                                  (CTRL0_FREEZE_CHANNEL_MASK <<
                                   CTRL0_FREEZE_CHANNEL_SHIFT);
        if (!s->is_apbx) {
            ctrl0_writable |= (CTRL0_CLKGATE_CHANNEL_MASK <<
                               CTRL0_CLKGATE_CHANNEL_SHIFT);
        }
        uint32_t wmask = ctrl0_writable & full_mask;
        cur = s->ctrl0;
        stmp3770_dma_apply_sct(&cur, (uint32_t)value << shift, sct);
        s->ctrl0 = (s->ctrl0 & ~wmask) | (cur & wmask);
        /*
         * Hardware ties CLKGATE to SFTRST: asserting reset automatically
         * gates the clock, so firmware polls CLKGATE after setting SFTRST.
         */
        if (s->ctrl0 & CTRL0_SFTRST) {
            s->ctrl0 |= CTRL0_CLKGATE;
        } else {
            s->ctrl0 &= ~CTRL0_CLKGATE;
        }
        return;
    }
    case REG_CTRL1: {
        /*
         * Per PDF Table 346: bits 31:24 are RSVD RO, bits 23:0 are all RW
         * (AHB_ERROR_IRQ + CMDCMPLT_IRQ_EN + CMDCMPLT_IRQ).
         */
        uint32_t ctrl1_writable = (CTRL1_AHB_ERROR_IRQ_MASK <<
                                   CTRL1_AHB_ERROR_IRQ_SHIFT) |
                                  (CTRL1_CMDCMPLT_IRQ_EN_MASK <<
                                   CTRL1_CMDCMPLT_IRQ_EN_SHIFT) |
                                  (CTRL1_CMDCMPLT_IRQ_MASK <<
                                   CTRL1_CMDCMPLT_IRQ_SHIFT);
        uint32_t wmask = ctrl1_writable & full_mask;
        cur = s->ctrl1;
        stmp3770_dma_apply_sct(&cur, (uint32_t)value << shift, sct);
        s->ctrl1 = (s->ctrl1 & ~wmask) | (cur & wmask);
        stmp3770_dma_update_all_irqs(s);
        return;
    }
    case REG_DEVSEL: {
        uint32_t devsel_writable = s->is_apbx ? DEVSEL_APBX_WRITABLE_MASK : 0;
        uint32_t wmask = devsel_writable & full_mask;
        cur = s->devsel;
        stmp3770_dma_apply_sct(&cur, (uint32_t)value << shift, sct);
        s->devsel = (s->devsel & ~wmask) | (cur & wmask);
        return;
    }
    default:
        break;
    }

    for (ch_idx = 0; ch_idx < STMP3770_DMA_NUM_CHANNELS; ch_idx++) {
        STMP3770DMAChannel *ch = &s->ch[ch_idx];
        if (base == REG_CH_NXTCMDAR(ch_idx)) {
            cur = (ch->nxtcmdar >> shift) & mask;
            stmp3770_dma_apply_sct(&cur, (uint32_t)value, sct);
            ch->nxtcmdar = (ch->nxtcmdar & ~full_mask) |
                           ((cur & mask) << shift);
            return;
        }
        if (base == REG_CH_SEMA(ch_idx)) {
            uint32_t increment = ((uint32_t)value << shift) & SEMA_INCREMENT_MASK;
            uint32_t phore = (ch->sema >> SEMA_PHORE_SHIFT) & SEMA_PHORE_MASK;
            phore += increment;
            if (phore > SEMA_PHORE_MASK) {
                phore = SEMA_PHORE_MASK;
            }
            ch->sema = (phore << SEMA_PHORE_SHIFT);
            if (increment > 0) {
                stmp3770_dma_kick_channel(s, ch_idx);
            }
            return;
        }
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "%s: write to unimplemented offset " HWADDR_FMT_plx "\n",
                  s->is_apbx ? "stmp3770-apbx-dma" : "stmp3770-apbh-dma",
                  offset);
}

static const MemoryRegionOps stmp3770_dma_ops = {
    .read = stmp3770_dma_read,
    .write = stmp3770_dma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void stmp3770_dma_reset(DeviceState *dev)
{
    STMP3770DMAState *s = STMP3770_DMA(dev);
    int i;

    s->ctrl0 = CTRL0_SFTRST | CTRL0_CLKGATE;
    s->ctrl1 = 0;
    s->devsel = 0;

    for (i = 0; i < STMP3770_DMA_NUM_CHANNELS; i++) {
        STMP3770DMAChannel *ch = &s->ch[i];
        ch->curcmdar = 0;
        ch->nxtcmdar = 0;
        ch->cmd = 0;
        ch->bar = 0;
        ch->sema = 0;
        ch->debug1 = DEBUG1_RD_FIFO_EMPTY | DEBUG1_WR_FIFO_EMPTY |
                     STATEMACHINE_IDLE;
        ch->debug2 = 0;
        ch->loaded_nxtcmdar = 0;
        ch->loaded_cmd = 0;
        ch->loaded_bar = 0;
        ch->num_pio_words = 0;
    }

    stmp3770_dma_update_all_irqs(s);
}

static void stmp3770_dma_init(Object *obj)
{
    STMP3770DMAState *s = STMP3770_DMA(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    int i;

    memory_region_init_io(&s->iomem, obj, &stmp3770_dma_ops, s,
                          object_get_typename(obj), 0x2000);
    sysbus_init_mmio(sbd, &s->iomem);

    for (i = 0; i < STMP3770_DMA_NUM_CHANNELS; i++) {
        sysbus_init_irq(sbd, &s->irq[i]);
    }
}

static const VMStateDescription vmstate_stmp3770_dma_channel = {
    .name = "stmp3770-dma-channel",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(curcmdar, STMP3770DMAChannel),
        VMSTATE_UINT32(nxtcmdar, STMP3770DMAChannel),
        VMSTATE_UINT32(cmd, STMP3770DMAChannel),
        VMSTATE_UINT32(bar, STMP3770DMAChannel),
        VMSTATE_UINT32(sema, STMP3770DMAChannel),
        VMSTATE_UINT32(debug1, STMP3770DMAChannel),
        VMSTATE_UINT32(debug2, STMP3770DMAChannel),
        VMSTATE_UINT32(loaded_nxtcmdar, STMP3770DMAChannel),
        VMSTATE_UINT32(loaded_cmd, STMP3770DMAChannel),
        VMSTATE_UINT32(loaded_bar, STMP3770DMAChannel),
        VMSTATE_UINT32_ARRAY(pio_words, STMP3770DMAChannel, 15),
        VMSTATE_UINT32(num_pio_words, STMP3770DMAChannel),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_stmp3770_dma = {
    .name = "stmp3770-dma",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl0, STMP3770DMAState),
        VMSTATE_UINT32(ctrl1, STMP3770DMAState),
        VMSTATE_UINT32(devsel, STMP3770DMAState),
        VMSTATE_STRUCT_ARRAY(ch, STMP3770DMAState,
                             STMP3770_DMA_NUM_CHANNELS, 0,
                             vmstate_stmp3770_dma_channel,
                             STMP3770DMAChannel),
        VMSTATE_BOOL(is_apbx, STMP3770DMAState),
        VMSTATE_END_OF_LIST()
    }
};

static void stmp3770_dma_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, stmp3770_dma_reset);
    dc->vmsd = &vmstate_stmp3770_dma;
}

static void stmp3770_apbh_dma_instance_init(Object *obj)
{
    STMP3770DMAState *s = STMP3770_DMA(obj);
    s->is_apbx = false;
}

static void stmp3770_apbx_dma_instance_init(Object *obj)
{
    STMP3770DMAState *s = STMP3770_DMA(obj);
    s->is_apbx = true;
}

static const TypeInfo stmp3770_dma_type_info = {
    .name          = TYPE_STMP3770_DMA,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(STMP3770DMAState),
    .instance_init = stmp3770_dma_init,
    .class_init    = stmp3770_dma_class_init,
    .abstract      = true,
};

static const TypeInfo stmp3770_apbh_dma_type_info = {
    .name          = TYPE_STMP3770_APBH_DMA,
    .parent        = TYPE_STMP3770_DMA,
    .instance_init = stmp3770_apbh_dma_instance_init,
};

static const TypeInfo stmp3770_apbx_dma_type_info = {
    .name          = TYPE_STMP3770_APBX_DMA,
    .parent        = TYPE_STMP3770_DMA,
    .instance_init = stmp3770_apbx_dma_instance_init,
};

static void stmp3770_dma_register_types(void)
{
    type_register_static(&stmp3770_dma_type_info);
    type_register_static(&stmp3770_apbh_dma_type_info);
    type_register_static(&stmp3770_apbx_dma_type_info);
}

type_init(stmp3770_dma_register_types)
